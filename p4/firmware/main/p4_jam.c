// P4 board: the microSD card and the jam tracks on it.
//
// Card: the 7B's TF slot on SDMMC slot 0, 4 bits; pins and the IO power (on-chip LDO channel 4) as in Waveshare's
// SD example for this board (examples/esp-idf/04_sdmmc): CLK GPIO43, CMD GPIO44, D0-D3 GPIO39-42. The card is only
// mounted, never formatted (a card that does not mount is left alone). FAT32 (SDHC up to 32 GB).
//
// Tracks: every *.wav in /sdcard/jam (the loose ones: the album "") and in its folders, one level (an album each,
// 2026-11-27: the Jam Player's folders on the Mac); listed loose first, then album by album, each by title. A track's
// name is its file relative to /sdcard/jam without ".wav" ("Album/Title", loose "Title"). Each is measured once in the
// background as the Jam Player on the Mac does it (loudness.h: LUFS, the loudness that counts against a guitar, peak)
// and kept in /sdcard/jam/loudness.txt ("<file><TAB><size><TAB><lufs><TAB><play><TAB><peak>", <file> relative as
// the name: the files from before albums keep working); it then plays at -21 LUFS (peaks below -1 dBFS). A correction
// by ear per track is added and kept in /sdcard/jam/levels.txt ("<file><TAB><dB>"). /sdcard/jam/sources.txt
// ("<file><TAB><stamp>") holds the stamp of the Mac's file each track was made from, so the Jam Player sends a song
// again only when it changed there. The three files are only ever appended to, a line at a time, and the later line of
// a name wins (a power cut costs one line at most, and nothing is written again from a list that may be stale while
// files come over Wi-Fi); when they hold many old lines they are made smaller from the file itself (compact_keep).
// Files can come over Wi-Fi (p4_net.c): during a transfer playing and measuring stop, then the folder is read again.
// A track ends: the next one of its album follows; at the album's end playing stops.
// A file task reads ahead into the player's buffer (6 s in PSRAM), so a slow moment of the card is not heard.
//
// STATUS: builds; not yet run on hardware (pins and LDO from Waveshare's example, not measured).
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "loudness.h"
#include "p4_audio.h"
#include "wav.h"

static const char *TAG = "p4_jam";

#define MOUNT "/sdcard"
#define MAX_TRACKS 256             // (both lists in PSRAM)
#define LINE 320                   // a line of the text files: "<Album/Title>.wav" and its values
#define CHUNK 4096                 // frames read at a time
#define RING_FRAMES 262144         // 6 s at 44.1 kHz, 1 MB
#define MEASURE_BLOCKS 12000       // 100 ms blocks: tracks up to 20 minutes are measured in full

typedef struct {
    char name[P4_JAM_NAME];        // "Album/Title", loose "Title"
    char source[P4_JAM_SOURCE];    // the Mac's stamp of the file it was made from ("" = none)
    uint8_t album;                 // bytes of the album in name (0 = loose)
    uint32_t size;                 // bytes of the file (a new file of the same name is measured again)
    float trim;                    // correction by ear (dB)
    float lufs, play, peak;        // measured (play NAN = not yet)
} jam_track_t;
typedef enum { JAM_PLAY, JAM_SEEK, JAM_STOP, JAM_RESCAN } jam_cmd_kind_t;
typedef struct { jam_cmd_kind_t kind; int value; } jam_cmd_t;

static bool s_card, s_card_unreadable;
static jam_track_t *s_lists[2], *s_tracks;   // read twice: the list being shown stays valid while a new one is read
static volatile int s_count;
static volatile int s_left_out;    // tracks on the card the list had no room for (whole albums where it can)
static volatile int s_current = -1;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;   // the tracks' values (measuring task, file task, calls)
static SemaphoreHandle_t s_files;  // the three text files: written by the measuring task, the calls and Wi-Fi
                                   // (taken before s_lock, never the other way round)
static float s_volume_db;          // the jam volume (mixer gain)
static volatile bool s_transfer;   // files are coming over Wi-Fi: no playing, no measuring
static volatile int s_measuring;   // tracks still to measure
static TaskHandle_t s_measure_task;

// ---- card ----

// The chip has one SD host with two slots: the Wi-Fi part (esp_hosted, slot 1) sets it up before app_main, and a
// second set-up is refused ("no available sd host controller", seen on the board 2026-11-18). So the card's slot 0
// joins the host that is there - and never takes it down again, the Wi-Fi part lives on it.
static esp_err_t host_shared(void)
{
    return ESP_OK;
}

static bool mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#ifdef CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED
    host.init = host_shared;
    host.deinit = host_shared;
    host.flags &= ~SDMMC_HOST_FLAG_DEINIT_ARG;
#else
    (void)host_shared;
#endif
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr) == ESP_OK) host.pwr_ctrl_handle = pwr;
    else ESP_LOGW(TAG, "card: IO power (LDO 4) not set up");

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 6,
                                                  .allocation_unit_size = 16 * 1024 };
    sdmmc_card_t *card = NULL;
    esp_err_t e = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &card);
    if (e != ESP_OK) {
        // The card answered but nothing mounts (ESP_FAIL): no FAT32 on it - most often exFAT, which cards above
        // 32 GB come with and ESP-IDF's file system does not read. It is left as it is (never formatted here).
        s_card_unreadable = e == ESP_FAIL;
        if (s_card_unreadable) ESP_LOGW(TAG, "a microSD card is in the slot, but the board cannot read it: it needs FAT32 (\"MS-DOS (FAT)\" on the Mac; exFAT is not read)");
        else ESP_LOGW(TAG, "no microSD card (%s)", esp_err_to_name(e));
        return false;
    }
    sdmmc_card_print_info(stdout, card);
    mkdir(P4_JAM_DIR, 0775);   // (an existing folder stays as it is)
    return true;
}

static const char *title_of(const jam_track_t *t) { return t->name + (t->album ? t->album + 1 : 0); }

// The loose tracks first, then the albums by name, inside an album by title (case does not count; FAT keeps no two
// names that differ in case alone).
static int by_album(const void *a, const void *b)
{
    const jam_track_t *x = a, *y = b;
    if (!x->album != !y->album) return x->album ? 1 : -1;
    size_t n = x->album < y->album ? x->album : y->album;
    int c = strncasecmp(x->name, y->name, n);
    if (!c) c = (int)x->album - (int)y->album;
    if (!c) c = strcasecmp(title_of(x), title_of(y));
    return c ? c : strcmp(x->name, y->name);
}

static bool same_album(const jam_track_t *list, int a, int b)
{
    return list[a].album == list[b].album && !strncmp(list[a].name, list[b].name, list[a].album);
}

static jam_track_t *find(jam_track_t *list, int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(list[i].name, name)) return &list[i];
    return NULL;
}

// "<name>.wav" in a line of one of the text files: the name without ".wav", or NULL
static char *line_name(char *line, char **rest)
{
    char *tab = strchr(line, '\t');
    if (!tab) return NULL;
    *tab = 0;
    size_t n = strlen(line);
    if (n > 4 && !strcasecmp(line + n - 4, ".wav")) line[n - 4] = 0;
    *rest = tab + 1;
    return line;
}

// A whole line of one of the text files into line, without its end (false: no more). A line longer than line is
// dropped as a whole, and so is a last one without its '\n': cut by a power cut while it was written (every line
// written here ends in one) - "-1" of a cut "-12.5" would pass for a value.
static bool read_line(FILE *f, char *line, size_t size)
{
    while (fgets(line, (int)size, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') {
            line[--n] = 0;
            if (n && line[n - 1] == '\r') line[n - 1] = 0;
            return true;
        }
        int c;   // (the rest of a long one skipped)
        while ((c = fgetc(f)) != EOF && c != '\n') {}
    }
    return false;
}

// The next line of a text that came over Wi-Fi into line (cut off: false for it); p moves past it.
static bool next_line(const char **p, char *line, size_t size)
{
    const char *end = strchr(*p, '\n');
    size_t len = end ? (size_t)(end - *p) : strlen(*p);
    bool fits = len < size;
    if (fits) {
        memcpy(line, *p, len);
        line[len] = 0;
        line[strcspn(line, "\r")] = 0;
    }
    *p += len + (end ? 1 : 0);
    return fits;
}

// A file of the jam folder (album "") or of an album's folder that is a track: a WAV, not hidden, a name the list
// can hold.
static bool track_file(const char *album, const char *file)
{
    size_t a = strlen(album), n = strlen(file);
    return n >= 5 && file[0] != '.' && !strcasecmp(file + n - 4, ".wav") && a + 1 + n - 4 < P4_JAM_NAME;
}

// A track_file into the list (t: its place, room checked by the caller).
static void add_track(jam_track_t *t, const char *album, const char *file)
{
    size_t a = strlen(album), n = strlen(file);
    memset(t, 0, sizeof(*t));
    if (a) {
        memcpy(t->name, album, a);
        t->name[a] = '/';
    }
    memcpy(t->name + (a ? a + 1 : 0), file, n - 4);
    t->album = (uint8_t)a;
    t->play = t->lufs = NAN;
    char path[P4_JAM_PATH];
    snprintf(path, sizeof(path), P4_JAM_DIR "/%s.wav", t->name);
    struct stat st;
    if (stat(path, &st) == 0) t->size = (uint32_t)st.st_size;
}

static bool is_folder(const struct dirent *e) { return e->d_type == DT_DIR; }   // (ESP-IDF's FAT driver always says)

static int by_folder_name(const void *a, const void *b) { return strcasecmp(a, b); }

typedef char jam_album_t[P4_JAM_ALBUM_MAX + 1];

// The WAVs in /sdcard/jam and in its folders (one level: an album's own folders are left alone, and so are folders
// whose name is too long or starts with '.', as the Mac's .Trashes / .fseventsd do). More than the list holds: an
// album that does not fit whole is left out whole - half an album on the screen would pass for all of it - and the
// albums go in by name, so which ones are shown does not hang on the order FAT keeps them in; loose tracks past the
// end are left out one by one. *left_out counts the tracks left out (GET /api/tracks tells the Jam Player).
static int read_folder(jam_track_t *list, int *left_out)
{
    enum { MOST_ALBUMS = MAX_TRACKS };
    int count = 0, out = 0, albums = 0;
    *left_out = 0;
    DIR *d = opendir(P4_JAM_DIR);
    if (!d) return 0;
    jam_album_t *album = heap_caps_malloc(MOST_ALBUMS * sizeof(jam_album_t), MALLOC_CAP_SPIRAM);
    if (!album) ESP_LOGE(TAG, "no memory for the album names: the loose tracks only");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!is_folder(e)) {
            if (!track_file("", e->d_name)) continue;
            if (count < MAX_TRACKS) add_track(&list[count++], "", e->d_name);
            else out++;
        } else if (strlen(e->d_name) > P4_JAM_ALBUM_MAX) {
            ESP_LOGW(TAG, "%.64s...: folder name too long for an album (%d bytes at most), left out", e->d_name, P4_JAM_ALBUM_MAX);
        } else if (album && albums < MOST_ALBUMS) {
            strlcpy(album[albums++], e->d_name, sizeof(jam_album_t));
        } else if (album) {
            ESP_LOGW(TAG, "%s: more than %d album folders, left out", e->d_name, MOST_ALBUMS);
        }
    }
    closedir(d);
    if (out) ESP_LOGW(TAG, "%d loose tracks left out: the list holds %d", out, MAX_TRACKS);
    if (albums) qsort(album, (size_t)albums, sizeof(jam_album_t), by_folder_name);
    for (int i = 0; i < albums; i++) {
        char path[sizeof(P4_JAM_DIR) + P4_JAM_ALBUM_MAX + 2];
        snprintf(path, sizeof(path), P4_JAM_DIR "/%s", album[i]);
        DIR *ad = opendir(path);
        if (!ad) {
            ESP_LOGW(TAG, "%s: album folder not readable", album[i]);
            continue;
        }
        int start = count, tracks = 0;
        struct dirent *ae;
        while ((ae = readdir(ad))) {
            if (is_folder(ae) || !track_file(album[i], ae->d_name)) continue;
            tracks++;
            if (count < MAX_TRACKS) add_track(&list[count++], album[i], ae->d_name);
        }
        closedir(ad);
        if (start + tracks > MAX_TRACKS) {   // (counted to its end: a smaller album after it may still fit)
            count = start;
            out += tracks;
            ESP_LOGW(TAG, "album %s (%d tracks) left out: the list holds %d", album[i], tracks, MAX_TRACKS);
        }
    }
    free(album);
    *left_out = out;
    return count;
}

// The text files kept beside the tracks. Lines are only appended (later ones win); compact_keep makes them smaller.
enum { KEEP_LEVELS, KEEP_LOUDNESS, KEEP_SOURCES };
static const char *const KEEP_NAME[] = { "levels", "loudness", "sources" };
#define KEEP_PATH (sizeof(P4_JAM_DIR) + 16)
#define KEEP_REST 96               // the values of a line (a measurement ~40 bytes, a stamp at most 40)
static int s_kept[3];              // lines each held when it was last made smaller
static void compact_keep(int what, jam_track_t *list, int count);

static void keep_path(char *path, size_t size, int what, const char *ext)
{
    snprintf(path, size, P4_JAM_DIR "/%s.%s", KEEP_NAME[what], ext);
}

static FILE *append_keep(int what)   // (s_files taken)
{
    char path[KEEP_PATH];
    keep_path(path, sizeof(path), what, "txt");
    FILE *f = fopen(path, "a");
    if (!f) ESP_LOGW(TAG, "%s.txt: a line NOT written", KEEP_NAME[what]);
    return f;
}

static void loudness_line(FILE *f, const char *name, uint32_t size, float lufs, float play, float peak)
{
    fprintf(f, "%s.wav\t%u\t%.2f\t%.2f\t%.5f\n", name, (unsigned)size, (double)lufs, (double)play, (double)peak);
}

// Reads the folder into the list not shown, then shows it (the track playing keeps playing under its new number).
static void scan(void)
{
    jam_track_t *list = s_tracks == s_lists[0] ? s_lists[1] : s_lists[0];
    int left_out, count = read_folder(list, &left_out);
    qsort(list, count, sizeof(jam_track_t), by_album);

    char line[LINE], *rest, path[KEEP_PATH];
    int lines[3] = { 0 };
    xSemaphoreTake(s_files, portMAX_DELAY);   // (no line written meanwhile is lost to a file made smaller below)
    keep_path(path, sizeof(path), KEEP_LEVELS, "txt");
    FILE *f = fopen(path, "r");
    while (f && read_line(f, line, sizeof(line))) {
        lines[KEEP_LEVELS]++;
        char *name = line_name(line, &rest);
        jam_track_t *t = name ? find(list, count, name) : NULL;
        if (t) t->trim = strtof(rest, NULL);
    }
    if (f) fclose(f);
    // The last line of a name decides: one of another size (a new version of the file, or the "not measured" line
    // of one that came without the Mac's measurement) means it is measured again - an older line of the same size
    // must not pass for it.
    keep_path(path, sizeof(path), KEEP_LOUDNESS, "txt");
    f = fopen(path, "r");
    while (f && read_line(f, line, sizeof(line))) {
        lines[KEEP_LOUDNESS]++;
        char *name = line_name(line, &rest);
        jam_track_t *t = name ? find(list, count, name) : NULL;
        if (!t) continue;
        unsigned size;
        float lufs, play, peak;
        if (sscanf(rest, "%u\t%f\t%f\t%f", &size, &lufs, &play, &peak) == 4 && size == t->size && isfinite(play)) {
            t->lufs = lufs;
            t->play = play;
            t->peak = peak;
        } else {
            t->lufs = t->play = NAN;
            t->peak = 0;
        }
    }
    if (f) fclose(f);
    keep_path(path, sizeof(path), KEEP_SOURCES, "txt");
    f = fopen(path, "r");
    while (f && read_line(f, line, sizeof(line))) {
        lines[KEEP_SOURCES]++;
        char *name = line_name(line, &rest);
        jam_track_t *t = name ? find(list, count, name) : NULL;
        if (t) strlcpy(t->source, rest, sizeof(t->source));
    }
    if (f) fclose(f);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *playing = s_current >= 0 && s_current < s_count ? s_tracks[s_current].name : NULL;
    int now = -1;
    for (int i = 0; playing && i < count; i++)
        if (!strcmp(list[i].name, playing)) now = i;
    s_tracks = list;
    s_count = count;
    s_left_out = left_out;
    s_current = now;
    int left = 0;
    for (int i = 0; i < count; i++) left += isnan(list[i].play);
    s_measuring = left;
    xSemaphoreGive(s_lock);
    // the files only ever grow (a line per change): made smaller when they hold many old lines
    for (int w = 0; w < 3; w++)
        if (lines[w] > (s_kept[w] > count ? s_kept[w] : count) + 32 && !s_transfer) compact_keep(w, list, count);
    xSemaphoreGive(s_files);
    if (left_out) ESP_LOGW(TAG, "%d jam tracks on the card, %d of them NOT in the list (it holds %d), %d to measure", count + left_out, left_out, MAX_TRACKS, left);
    else ESP_LOGI(TAG, "%d jam tracks on the card, %d to measure", count, left);
    if (s_measure_task) xTaskNotifyGive(s_measure_task);
}

// One of the text files made smaller: the last line of each name whose WAV is still on the card, taken from the file
// itself - not from the list, which can lack tracks (past MAX_TRACKS, in an album folder that could not be read)
// whose lines must stay: a lost stamp would let the Jam Player take a changed song for the one on the card. Lines
// that say nothing any more go too: a correction of 0, an empty stamp, a measurement of another size. Written to a
// new file, then renamed (recover_keep). Left as it is when a transfer begins meanwhile (a file being replaced is
// briefly not there) or when the file names more tracks than it can hold here. (s_files taken)
static void compact_keep(int what, jam_track_t *list, int count)
{
    enum { MOST = 4 * MAX_TRACKS };
    typedef struct { char name[P4_JAM_NAME], rest[KEEP_REST]; } keep_t;
    keep_t *keep = heap_caps_malloc(MOST * sizeof(keep_t), MALLOC_CAP_SPIRAM);
    char path[KEEP_PATH], tmp[KEEP_PATH], line[LINE], *rest;
    keep_path(path, sizeof(path), what, "txt");
    keep_path(tmp, sizeof(tmp), what, "tmp");
    FILE *f = keep ? fopen(path, "r") : NULL;
    if (!f) {
        free(keep);
        return;
    }
    int n = 0;
    bool full = false;
    while (!full && read_line(f, line, sizeof(line))) {
        char *name = line_name(line, &rest);
        if (!name || strlen(name) >= P4_JAM_NAME || strlen(rest) >= KEEP_REST) continue;   // (never written here)
        int k = 0;
        while (k < n && strcmp(keep[k].name, name)) k++;
        if (k == n && !(full = n == MOST)) strlcpy(keep[n++].name, name, sizeof(keep[0].name));
        if (k < n) strlcpy(keep[k].rest, rest, sizeof(keep[0].rest));
    }
    fclose(f);
    f = full ? NULL : fopen(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "%s.txt not made smaller%s", KEEP_NAME[what], full ? ": it names too many tracks" : "");
        free(keep);
        return;
    }
    int written = 0;
    for (int i = 0; i < n; i++) {
        const keep_t *k = &keep[i];
        if (what == KEEP_LEVELS && fabsf(strtof(k->rest, NULL)) < 0.05f) continue;
        if (what == KEEP_SOURCES && !k->rest[0]) continue;
        const jam_track_t *t = find(list, count, k->name);
        uint32_t size = t ? t->size : 0;
        if (!t) {   // not in the list: is its file there?
            char wav[P4_JAM_PATH];
            struct stat st;
            snprintf(wav, sizeof(wav), P4_JAM_DIR "/%s.wav", k->name);
            if (stat(wav, &st) != 0) continue;
            size = (uint32_t)st.st_size;
        }
        unsigned was;
        if (what == KEEP_LOUDNESS && (sscanf(k->rest, "%u", &was) != 1 || was != size)) continue;
        fprintf(f, "%s.wav\t%s\n", k->name, k->rest);
        written++;
    }
    free(keep);
    bool ok = fclose(f) == 0 && !s_transfer;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    } else {
        remove(tmp);
    }
    if (ok) s_kept[what] = written;
    if (ok) ESP_LOGI(TAG, "%s.txt made smaller: %d lines", KEEP_NAME[what], written);
    else ESP_LOGW(TAG, "%s.txt not made smaller", KEEP_NAME[what]);
}

// A power cut between compact_keep's remove and rename leaves only the new copy: it is whole (renamed only after a
// clean fclose), so it becomes the file again. (A .tmp beside its .txt was cut while written: left, the next
// compaction writes over it.)
static void recover_keep(void)
{
    for (int w = 0; w < 3; w++) {
        char path[KEEP_PATH], tmp[KEEP_PATH];
        struct stat st;
        keep_path(path, sizeof(path), w, "txt");
        keep_path(tmp, sizeof(tmp), w, "tmp");
        if (stat(path, &st) == 0 || stat(tmp, &st) != 0) continue;
        if (rename(tmp, path) == 0) ESP_LOGW(TAG, "%s.txt taken back from its new copy (a power cut while it was written)", KEEP_NAME[w]);
        else ESP_LOGW(TAG, "%s.txt NOT taken back from %s.tmp", KEEP_NAME[w], KEEP_NAME[w]);
    }
}

// The gain a track plays with: measured - to the target with its correction; not yet - its correction alone.
static float track_gain(int i)
{
    if (i < 0 || i >= s_count) return 0;
    const jam_track_t *t = &s_tracks[i];
    loudness_result_t r = { t->lufs, t->play, t->peak };
    return isnan(t->play) ? t->trim : loudness_gain_db(r, LOUD_TARGET_DB, t->trim);
}

// ---- measuring (low priority, in the background) ----

static void measure_task(void *arg)
{
    double *energy = heap_caps_malloc(MEASURE_BLOCKS * sizeof(double), MALLOC_CAP_SPIRAM);
    double *no_bass = heap_caps_malloc(MEASURE_BLOCKS * sizeof(double), MALLOC_CAP_SPIRAM);
    uint8_t *raw = heap_caps_malloc(CHUNK * 8, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(CHUNK * 4, MALLOC_CAP_SPIRAM);
    if (!energy || !no_bass || !raw || !pcm) {
        ESP_LOGE(TAG, "no memory for measuring");
        vTaskDelete(NULL);
    }
    static loudness_t m;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        while (!s_transfer) {
            char name[P4_JAM_NAME] = "";
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_count && !name[0]; i++)
                if (isnan(s_tracks[i].play) && s_tracks[i].lufs != -INFINITY) strlcpy(name, s_tracks[i].name, sizeof(name));
            xSemaphoreGive(s_lock);
            if (!name[0]) break;

            char path[P4_JAM_PATH];
            snprintf(path, sizeof(path), P4_JAM_DIR "/%s.wav", name);
            FILE *f = fopen(path, "rb");
            static uint8_t head[4096];
            wav_info_t wi;
            bool ok = f && wav_parse(head, fread(head, 1, sizeof(head), f), &wi);
            loudness_result_t r = { -INFINITY, -INFINITY, 0 };   // (unreadable: not tried again until the folder is read again)
            int64_t t0 = esp_log_timestamp();
            if (ok) {
                fseek(f, wi.data_offset, SEEK_SET);
                loudness_init(&m, wi.rate, energy, no_bass, MEASURE_BLOCKS);
                for (uint32_t left = wi.frames; left && !s_transfer;) {
                    size_t got = fread(raw, wi.block_align, left < CHUNK ? left : CHUNK, f);
                    if (!got) break;
                    wav_to_stereo16(&wi, raw, got, pcm);
                    loudness_feed(&m, pcm, got);
                    left -= got;
                    vTaskDelay(1);   // (the card and the cores belong to the music first)
                }
                if (!s_transfer) r = loudness_result(&m);
            }
            if (f) fclose(f);
            if (s_transfer) break;   // measured again after the transfer
            xSemaphoreTake(s_lock, portMAX_DELAY);
            jam_track_t *t = find(s_tracks, s_count, name);
            if (t) {
                t->lufs = r.lufs;
                t->play = isfinite(r.play) ? r.play : NAN;
                t->peak = r.peak;
                if (!isfinite(r.play)) t->lufs = -INFINITY;   // (marks it as tried)
            }
            int left = 0;
            for (int i = 0; i < s_count; i++) left += isnan(s_tracks[i].play) && s_tracks[i].lufs != -INFINITY;
            s_measuring = left;
            bool current = t && s_current >= 0 && &s_tracks[s_current] == t;
            uint32_t size = t ? t->size : 0;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "%s: %.1f LUFS, counts as %.1f, peak %.1f dBFS (%u ms)%s", name, (double)r.lufs, (double)r.play,
                     r.peak > 0 ? 20 * log10((double)r.peak) : -90.0, (unsigned)(esp_log_timestamp() - t0), ok ? "" : " - NOT READABLE");
            // One line appended. s_transfer is asked again under s_files: a file that came meanwhile writes its own
            // line after this one (its file was not replaced before s_transfer was set).
            xSemaphoreTake(s_files, portMAX_DELAY);
            FILE *keep = t && isfinite(r.play) && !s_transfer ? append_keep(KEEP_LOUDNESS) : NULL;
            if (keep) {
                loudness_line(keep, name, size, r.lufs, r.play, r.peak);
                fclose(keep);
            }
            xSemaphoreGive(s_files);
            if (current) track_player_set_gain_db(&g_p4_track, track_gain(s_current));
        }
    }
}

// ---- file task ----

static FILE *open_track(int i, wav_info_t *wi)
{
    char path[P4_JAM_PATH];
    snprintf(path, sizeof(path), P4_JAM_DIR "/%s.wav", s_tracks[i].name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static uint8_t head[4096];
    size_t n = fread(head, 1, sizeof(head), f);
    if (!wav_parse(head, n, wi)) {
        ESP_LOGW(TAG, "%s: not a WAV file the board can play (PCM / float, mono / stereo)", s_tracks[i].name);
        fclose(f);
        return NULL;
    }
    return f;
}

static bool wait_ready(void)
{
    for (int i = 0; i < 100; i++) {   // the audio side empties the buffer within a few ms (when USB runs)
        if (track_player_ready(&g_p4_track)) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static void jam_task(void *arg)
{
    uint8_t *raw = heap_caps_malloc(CHUNK * 8, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(CHUNK * 4, MALLOC_CAP_SPIRAM);
    FILE *f = NULL;
    wav_info_t wi = { 0 };
    uint32_t left = 0;
    while (true) {
        jam_cmd_t c;
        bool got_cmd = xQueueReceive(s_queue, &c, pdMS_TO_TICKS(f && left ? 20 : 200)) == pdTRUE;
        int cur = s_current;
        if (!got_cmd && cur >= 0 && !s_transfer) {   // after a track: the next one of its album (at the album's end: stop)
            track_player_info_t ti;
            track_player_get_info(&g_p4_track, &ti);
            if (ti.ended && cur + 1 < s_count && same_album(s_tracks, cur, cur + 1)) {
                c = (jam_cmd_t){ JAM_PLAY, cur + 1 };
                got_cmd = true;
            }
        }
        if (got_cmd && (c.kind == JAM_STOP || c.kind == JAM_RESCAN)) {
            if (f) {   // a transfer may replace the file: let it go
                fclose(f);
                f = NULL;
                left = 0;
            }
            track_player_set_playing(&g_p4_track, false);
            if (c.kind == JAM_RESCAN) scan();
            continue;
        }
        if (got_cmd && !s_transfer) {
            int track = c.kind == JAM_PLAY ? c.value : s_current;
            uint32_t start = 0;
            if (track < 0 || track >= s_count) continue;
            if (f) fclose(f);
            f = open_track(track, &wi);
            if (!f) { s_current = -1; continue; }
            if (c.kind == JAM_SEEK) {
                start = (uint32_t)c.value * wi.rate;
                if (start >= wi.frames) start = wi.frames ? wi.frames - 1 : 0;
            }
            fseek(f, wi.data_offset + (long)start * wi.block_align, SEEK_SET);
            track_player_begin(&g_p4_track, wi.rate, start, wi.frames);
            if (!wait_ready()) ESP_LOGW(TAG, "audio side not running (Nano not on USB?)");
            s_current = track;
            float gain = track_gain(track);
            track_player_set_gain_db(&g_p4_track, gain);
            left = wi.frames - start;
            if (c.kind == JAM_PLAY) track_player_set_playing(&g_p4_track, true);
            ESP_LOGI(TAG, "%s: %u Hz, %u ch, %u bit, %u s, gain %+.1f dB%s", s_tracks[track].name, (unsigned)wi.rate,
                     wi.channels, wi.bits, (unsigned)(wi.frames / wi.rate), (double)gain,
                     isnan(s_tracks[track].play) ? " (not measured yet)" : "");
        }
        // read ahead while there is room
        while (f && left && track_player_space(&g_p4_track) >= CHUNK && uxQueueMessagesWaiting(s_queue) == 0) {
            uint32_t want = left < CHUNK ? left : CHUNK;
            size_t got = fread(raw, wi.block_align, want, f);
            if (got == 0) { left = 0; break; }
            wav_to_stereo16(&wi, raw, got, pcm);
            track_player_write(&g_p4_track, pcm, got);
            left -= got;
        }
        if (f && !left) {
            track_player_end_of_data(&g_p4_track);
            fclose(f);
            f = NULL;
        }
    }
}

// ---- calls ----

void p4_jam_start(void)
{
    int16_t *ring = heap_caps_malloc(RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_lists[0] = heap_caps_calloc(MAX_TRACKS, sizeof(jam_track_t), MALLOC_CAP_SPIRAM);
    s_lists[1] = heap_caps_calloc(MAX_TRACKS, sizeof(jam_track_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    s_files = xSemaphoreCreateMutex();
    if (!ring || !s_lists[0] || !s_lists[1] || !s_lock || !s_files) {
        ESP_LOGE(TAG, "no memory for the jam player");
        return;
    }
    s_tracks = s_lists[0];
    track_player_init(&g_p4_track, ring, RING_FRAMES, 48000);
    s_card = mount();
    if (s_card) {
        recover_keep();
        scan();
    }
    s_queue = xQueueCreate(4, sizeof(jam_cmd_t));
    xTaskCreatePinnedToCore(jam_task, "jam", 6144, NULL, 6, NULL, 1);   // (reading the folder: paths and lines of the albums)
    if (s_card) xTaskCreatePinnedToCore(measure_task, "jam_measure", 4096, NULL, 2, &s_measure_task, 1);
}

bool p4_jam_card(void) { return s_card; }
bool p4_jam_card_unreadable(void) { return !s_card && s_card_unreadable; }
int p4_jam_count(void) { return s_count; }
int p4_jam_left_out(void) { return s_left_out; }
int p4_jam_measuring(void) { return s_measuring; }
const char *p4_jam_name(int i) { return i >= 0 && i < s_count ? s_tracks[i].name : ""; }
const char *p4_jam_source(int i) { return i >= 0 && i < s_count ? s_tracks[i].source : ""; }
uint32_t p4_jam_size(int i) { return i >= 0 && i < s_count ? s_tracks[i].size : 0; }

void p4_jam_note(int i, char *out, size_t n)
{
    out[0] = 0;
    if (i < 0 || i >= s_count) return;
    const jam_track_t *t = &s_tracks[i];
    char trim[24] = "";
    if (fabsf(t->trim) >= 0.05f) snprintf(trim, sizeof(trim), " \xC2\xB7 %s%.1f", t->trim < 0 ? "\xE2\x88\x92" : "+", fabsf(t->trim));
    if (t->lufs == -INFINITY) snprintf(out, n, "NOT READABLE");
    else if (isnan(t->play)) snprintf(out, n, "%s%s", s_transfer ? "" : "MEASURING", trim);
    else snprintf(out, n, "%s%.1f LUFS%s", t->lufs < 0 ? "\xE2\x88\x92" : "", fabsf(t->lufs), trim);
}

void p4_jam_play(int i)
{
    jam_cmd_t c = { JAM_PLAY, i };
    if (s_queue) xQueueSend(s_queue, &c, 0);
}

void p4_jam_seek(uint32_t seconds)
{
    jam_cmd_t c = { JAM_SEEK, (int)seconds };
    if (s_queue && s_current >= 0) xQueueSend(s_queue, &c, 0);
}

void p4_jam_toggle(void)
{
    track_player_info_t ti;
    track_player_get_info(&g_p4_track, &ti);
    if (ti.ended || s_current < 0) {
        if (s_count) p4_jam_play(s_current < 0 ? 0 : s_current);
        return;
    }
    track_player_set_playing(&g_p4_track, !ti.playing);
}

void p4_jam_info(int *current, track_player_info_t *out)
{
    *current = s_current;
    track_player_get_info(&g_p4_track, out);
}

void p4_jam_set_level_db(float db)
{
    if (db < -40) db = -40;
    if (db > 6) db = 6;
    s_volume_db = db;
    audio_mix_set_gain_db(&g_p4_mix, AUDIO_SRC_TRACK, db);
}

float p4_jam_level_db(void) { return s_volume_db; }
float p4_jam_level(int i) { return i >= 0 && i < s_count ? s_tracks[i].trim : 0; }

// Back and Next stay inside the album of the track (an album is one set; the next album is chosen in the list).
void p4_jam_prev(void)
{
    track_player_info_t ti;
    track_player_get_info(&g_p4_track, &ti);
    int cur = s_current;
    const jam_track_t *list = s_tracks;
    if (cur < 0) { if (s_count) p4_jam_play(0); return; }
    if (ti.rate && ti.pos > 3 * ti.rate) p4_jam_seek(0);
    else p4_jam_play(cur > 0 && cur < s_count && same_album(list, cur - 1, cur) ? cur - 1 : cur);
}

void p4_jam_next(void)
{
    int cur = s_current;
    const jam_track_t *list = s_tracks;
    if (cur >= 0 && cur + 1 < s_count && same_album(list, cur, cur + 1)) p4_jam_play(cur + 1);
}

// A correction by ear for the current track: used at once and appended to levels.txt - one line, never the whole
// file from the list: during a Wi-Fi transfer the list still holds the names and values from before it (a move, the
// Mac's corrections), and writing it out would undo them.
void p4_jam_set_track_level(float db)
{
    int cur = s_current;
    if (cur < 0 || cur >= s_count) return;
    if (db < -12) db = -12;   // (as the Jam Player: +-12 dB)
    if (db > 12) db = 12;
    char name[P4_JAM_NAME];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tracks[cur].trim = db;
    strlcpy(name, s_tracks[cur].name, sizeof(name));
    xSemaphoreGive(s_lock);
    track_player_set_gain_db(&g_p4_track, track_gain(cur));
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *f = append_keep(KEEP_LEVELS);
    if (f) {
        fprintf(f, "%s.wav\t%.1f\n", name, (double)db);   // (0 too: it wins over the correction before)
        fclose(f);
    }
    xSemaphoreGive(s_files);
    ESP_LOGI(TAG, "%s: by ear %+.1f dB, gain %+.1f dB", name, (double)db, (double)track_gain(cur));
}

// Wi-Fi transfer (p4_net.c): on = stop playing and measuring (files may be replaced); off = read the folder again.
void p4_jam_transfer(bool on)
{
    if (on == s_transfer) return;
    s_transfer = on;
    jam_cmd_t c = { on ? JAM_STOP : JAM_RESCAN, 0 };
    if (s_queue) xQueueSend(s_queue, &c, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "transfer %s", on ? "begins: playing and measuring stop" : "done: reading the folder again");
}

// A file came: the measurement it came with (the Mac measured it), so the board need not measure it - or, without
// one, a "not measured" line (size 0): the measurement of the version before must not pass for the new one, which
// often has exactly the same size (a song bounced again).
void p4_jam_measured(const char *name, uint32_t size, float lufs, float play, float peak)
{
    if (!(isfinite(lufs) && isfinite(play) && isfinite(peak) && peak > 0)) size = 0, lufs = play = peak = 0;
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *f = append_keep(KEEP_LOUDNESS);
    if (f) {
        loudness_line(f, name, size, lufs, play, peak);
        fclose(f);
    }
    xSemaphoreGive(s_files);
}

void p4_jam_set_trim(const char *name, float db)   // a correction that came with a file
{
    if (fabsf(db) < 0.05f) return;
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *f = append_keep(KEEP_LEVELS);
    if (f) {
        fprintf(f, "%s.wav\t%.1f\n", name, (double)db);
        fclose(f);
    }
    xSemaphoreGive(s_files);
}

// Corrections by ear from the Mac ("<file><TAB><dB>" per line): appended to levels.txt (later lines win, a line with
// 0 removes one). Read in when the transfer ends.
void p4_jam_trims(const char *text)
{
    char line[LINE], *rest;
    int n = 0;
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *f = append_keep(KEEP_LEVELS);
    for (const char *p = text; f && *p;) {
        if (!next_line(&p, line, sizeof(line))) continue;
        char *name = line_name(line, &rest), *end;
        if (!name || !p4_jam_clean_name(name)) continue;   // (as the file will be named on the card)
        float db = strtof(rest, &end);
        if (end == rest || !isfinite(db)) continue;
        fprintf(f, "%s.wav\t%.1f\n", name, (double)db);
        n++;
    }
    if (f) fclose(f);
    xSemaphoreGive(s_files);
    ESP_LOGI(TAG, "%d corrections by ear from the Mac taken", n);
}

// ---- albums and the Mac's stamps (Wi-Fi, p4_net.c) ----

static bool name_char_ok(unsigned char c) { return c >= 0x20 && c != 0x7F && !strchr("\\:*?\"<>|", c); }

// One part of a name as FAT keeps it: a folder without its trailing spaces and dots (FatFs's create_name cuts them
// from every part, so "Live./Song" would be stored as "Live/Song" and its keys and the Mac's name would never meet
// the file again). Leading spaces stay: FatFs keeps them in the long name (it drops them only from the 8.3 name), and
// the tracks sent before 2026-11-27 lie on the card with them. A title is followed by ".wav" / ".jpg", so its own
// trailing spaces and dots stay too.
static bool clean_part(char *s, bool folder, size_t most)
{
    size_t n = strlen(s);
    while (folder && n && (s[n - 1] == ' ' || s[n - 1] == '.')) s[--n] = 0;
    if (!n || n > most || s[0] == '.') return false;   // (".." and hidden files: never)
    for (const char *p = s; *p; p++)
        if (!name_char_ok((unsigned char)*p)) return false;
    return true;
}

bool p4_jam_clean_name(char *name)
{
    size_t len = strlen(name);
    char *slash = strchr(name, '/');
    if (len >= P4_JAM_NAME || (slash && strchr(slash + 1, '/'))) return false;   // one folder level only
    char album[P4_JAM_NAME] = "", title[P4_JAM_NAME];
    if (slash) {
        memcpy(album, name, (size_t)(slash - name));
        album[slash - name] = 0;
        if (!clean_part(album, true, P4_JAM_ALBUM_MAX)) return false;
    }
    strlcpy(title, slash ? slash + 1 : name, sizeof(title));
    if (!clean_part(title, false, P4_JAM_TITLE_MAX)) return false;
    size_t a = strlen(album), t = strlen(title);   // (never longer than before: it fits where it came from)
    if (a) {
        memcpy(name, album, a);
        name[a] = '/';
    }
    memcpy(name + (a ? a + 1 : 0), title, t + 1);
    return true;
}

static bool stamp_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= P4_JAM_SOURCE) return false;
    for (; *s; s++)
        if (*s <= 0x20 || *s == 0x7F) return false;
    return true;
}

// A stamp into sources.txt (f, open to append; s_files taken) and at once into the list shown, so the Mac's next
// look sees it without the folder read again.
static void keep_source(FILE *f, const char *name, const char *stamp)
{
    fprintf(f, "%s.wav\t%s\n", name, stamp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    jam_track_t *t = find(s_tracks, s_count, name);
    if (t) strlcpy(t->source, stamp, sizeof(t->source));
    xSemaphoreGive(s_lock);
}

// A file came: the stamp it came with - or none (the board's own upload page, an older Jam Player), which clears the
// one before, so a file from elsewhere never passes for the Mac's song.
void p4_jam_set_source(const char *name, const char *stamp)
{
    if (!stamp_ok(stamp)) stamp = "";
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *f = append_keep(KEEP_SOURCES);
    if (f) {
        keep_source(f, name, stamp);
        fclose(f);
    }
    xSemaphoreGive(s_files);
}

// Stamps from the Mac for tracks on the card already (sent before stamps existed: the same song).
void p4_jam_sources(const char *text)
{
    char line[LINE], *rest;
    int n = 0;
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *f = append_keep(KEEP_SOURCES);
    for (const char *p = text; f && *p;) {
        if (!next_line(&p, line, sizeof(line))) continue;
        char *name = line_name(line, &rest);
        if (!name || !p4_jam_clean_name(name) || !stamp_ok(rest)) continue;
        keep_source(f, name, rest);
        n++;
    }
    if (f) fclose(f);
    xSemaphoreGive(s_files);
    ESP_LOGI(TAG, "%d stamps of the Mac's files taken", n);
}

static void path_of(char *path, size_t size, const char *name, const char *ext)
{
    snprintf(path, size, P4_JAM_DIR "/%s.%s", name, ext);
}

// The album's folder of a name ("Album/Title"), made if it is not there.
void p4_jam_make_album(const char *name)
{
    const char *slash = strchr(name, '/');
    if (!slash) return;
    char path[sizeof(P4_JAM_DIR) + P4_JAM_ALBUM_MAX + 2];
    snprintf(path, sizeof(path), P4_JAM_DIR "/%.*s", (int)(slash - name), name);
    mkdir(path, 0775);   // (an existing folder stays as it is)
}

typedef struct { char from[P4_JAM_NAME], to[P4_JAM_NAME], rest[3][KEEP_REST]; } jam_move_t;

// The moved tracks' keys: the last line of each "from" in one of the text files appended again under its "to". A
// "from" without a line gets a line that says nothing for its "to" (no correction, not measured, no stamp), so an
// old line of a track of that name removed by hand does not pass for it. The lines of "from" stay until
// compact_keep drops them (their file is gone). False: not written. (s_files taken)
static bool carry_keys(int what, jam_move_t *moves, int n)
{
    static const char *const NOTHING[] = { "0.0", "0\t0.00\t0.00\t0.00000", "" };
    char path[KEEP_PATH], line[LINE], *rest;
    keep_path(path, sizeof(path), what, "txt");
    for (int i = 0; i < n; i++) moves[i].rest[what][0] = 0;
    FILE *f = fopen(path, "r");
    while (f && read_line(f, line, sizeof(line))) {
        char *name = line_name(line, &rest);
        for (int i = 0; name && i < n; i++)
            if (!strcmp(name, moves[i].from)) strlcpy(moves[i].rest[what], rest, KEEP_REST);
    }
    if (f) fclose(f);
    f = append_keep(what);
    if (!f) return false;
    for (int i = 0; i < n; i++)
        fprintf(f, "%s.wav\t%s\n", moves[i].to, moves[i].rest[what][0] ? moves[i].rest[what] : NOTHING[what]);
    return fclose(f) == 0;
}

// Tracks into an album ("<from>.wav<TAB><to>.wav" per line; the Mac's folders became albums, 2026-11-27). The WAV is
// renamed, its cover beside it goes along, the album's folder is made; the keys in the text files go along. "from"
// names a file the board itself listed: checked as a name, but taken as it is spelt (cleaning could change it, and
// then it would not meet its file); "to" is made as FAT will keep it. FAT's rename replaces nothing: a target that
// is there already leaves its line out. The keys are written under the new names first and the files renamed after
// that, so a power cut in between leaves moved files with their keys (or keys of files not yet moved: unused).
// Called during a transfer (nothing plays, nothing is measured); the list is read again when it ends. Returns how
// many were moved.
int p4_jam_move(const char *text)
{
    jam_move_t *moves = heap_caps_calloc(MAX_TRACKS, sizeof(jam_move_t), MALLOC_CAP_SPIRAM);
    if (!moves) return 0;
    static const char *const EXT[] = { "jpg", "jpeg" };
    int n = 0;
    char line[LINE], *rest, a[P4_JAM_PATH], b[P4_JAM_PATH], check[P4_JAM_NAME];
    struct stat st;
    for (const char *p = text; *p && n < MAX_TRACKS;) {
        if (!next_line(&p, line, sizeof(line))) continue;
        char *from = line_name(line, &rest);
        if (!from) continue;
        char *to = rest;
        size_t k = strlen(to);
        if (k > 4 && !strcasecmp(to + k - 4, ".wav")) to[k - 4] = 0;
        strlcpy(check, from, sizeof(check));
        if (!p4_jam_clean_name(check) || strcmp(check, from) || !p4_jam_clean_name(to) || !strcasecmp(from, to)) continue;
        int i = 0;   // (one track, or one target, twice in the lines: the first counts)
        while (i < n && strcasecmp(moves[i].from, from) && strcasecmp(moves[i].to, to)) i++;
        if (i < n) continue;
        path_of(a, sizeof(a), from, "wav");
        path_of(b, sizeof(b), to, "wav");
        if (stat(a, &st) != 0 || stat(b, &st) == 0) {
            ESP_LOGW(TAG, "%s -> %s: not moved (%s)", from, to, stat(b, &st) == 0 ? "the target is there already" : "no such track");
            continue;
        }
        strlcpy(moves[n].from, from, sizeof(moves[n].from));
        strlcpy(moves[n].to, to, sizeof(moves[n].to));
        n++;
    }
    bool keys = true;
    if (n) {
        xSemaphoreTake(s_files, portMAX_DELAY);
        for (int w = 0; w < 3; w++) keys = carry_keys(w, moves, n) && keys;
        xSemaphoreGive(s_files);
    }
    if (!keys) {   // (moved without them, a track would lose its correction and its stamp)
        ESP_LOGW(TAG, "the keys of the tracks to move NOT written: nothing moved");
        n = 0;
    }
    int moved = 0;
    for (int i = 0; i < n; i++) {
        const char *from = moves[i].from, *to = moves[i].to;
        path_of(a, sizeof(a), from, "wav");
        path_of(b, sizeof(b), to, "wav");
        p4_jam_make_album(to);
        if (rename(a, b) != 0) {
            ESP_LOGW(TAG, "%s -> %s: NOT moved (the card refused)", from, to);
            continue;
        }
        for (int e = 0; e < 2; e++) {   // its cover beside it (one left over at the target gives way)
            path_of(a, sizeof(a), from, EXT[e]);
            path_of(b, sizeof(b), to, EXT[e]);
            if (stat(a, &st) != 0) continue;
            remove(b);
            if (rename(a, b) != 0) ESP_LOGW(TAG, "%s.%s: the cover NOT moved", from, EXT[e]);
        }
        moved++;
        ESP_LOGI(TAG, "moved %s -> %s", from, to);
    }
    free(moves);
    return moved;
}

// "Album/Title" came: a loose "Title" (sent before the Mac's folders became albums) would be a second copy of it, so
// it goes, with its cover. (Its lines in the text files stay unused until compact_keep drops them: the file is gone.)
void p4_jam_remove_loose(const char *name)
{
    const char *slash = strchr(name, '/');
    if (!slash) return;
    char path[P4_JAM_PATH];
    path_of(path, sizeof(path), slash + 1, "wav");
    if (remove(path) != 0) return;
    path_of(path, sizeof(path), slash + 1, "jpg");
    remove(path);
    path_of(path, sizeof(path), slash + 1, "jpeg");
    remove(path);
    ESP_LOGI(TAG, "%s came: the loose copy of it removed", name);
}
