// P4 board: the microSD card and the jam tracks on it.
//
// Card: the 7B's TF slot on SDMMC slot 0, 4 bits; pins and the IO power (on-chip LDO channel 4) as in Waveshare's
// SD example for this board (examples/esp-idf/04_sdmmc): CLK GPIO43, CMD GPIO44, D0-D3 GPIO39-42. The card is only
// mounted, never formatted (a card that does not mount is left alone). FAT32 (SDHC up to 32 GB).
//
// Tracks: every *.wav in /sdcard/jam, in name order. Each is measured once in the background as the Jam Player on
// the Mac does it (loudness.h: LUFS, the loudness that counts against a guitar, peak) and kept in
// /sdcard/jam/loudness.txt ("<file><TAB><size><TAB><lufs><TAB><play><TAB><peak>"); it then plays at -21 LUFS
// (peaks below -1 dBFS). A correction by ear per track is added and kept in /sdcard/jam/levels.txt
// ("<file><TAB><dB>"). Files can come over Wi-Fi (p4_net.c): during a transfer playing and measuring stop, then the
// folder is read again.
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
#define MAX_TRACKS 64
#define CHUNK 4096                 // frames read at a time
#define RING_FRAMES 262144         // 6 s at 44.1 kHz, 1 MB
#define MEASURE_BLOCKS 12000       // 100 ms blocks: tracks up to 20 minutes are measured in full

typedef struct {
    char name[P4_JAM_NAME];
    uint32_t size;                 // bytes of the file (a new file of the same name is measured again)
    float trim;                    // correction by ear (dB)
    float lufs, play, peak;        // measured (play NAN = not yet)
} jam_track_t;
typedef enum { JAM_PLAY, JAM_SEEK, JAM_STOP, JAM_RESCAN } jam_cmd_kind_t;
typedef struct { jam_cmd_kind_t kind; int value; } jam_cmd_t;

static bool s_card, s_card_unreadable;
static jam_track_t *s_lists[2], *s_tracks;   // read twice: the list being shown stays valid while a new one is read
static volatile int s_count;
static volatile int s_current = -1;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;   // the tracks' values (measuring task, file task, calls)
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

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const jam_track_t *)a)->name, ((const jam_track_t *)b)->name);
}

static jam_track_t *find(jam_track_t *list, int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(list[i].name, name)) return &list[i];
    return NULL;
}

// "<name>.wav" in a line of one of the two files: the name without ".wav", or NULL
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

// Reads the folder into the list not shown, then shows it (the track playing keeps playing under its new number).
static void scan(void)
{
    jam_track_t *list = s_tracks == s_lists[0] ? s_lists[1] : s_lists[0];
    int count = 0;
    DIR *d = opendir(P4_JAM_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && count < MAX_TRACKS) {
            size_t n = strlen(e->d_name);
            if (n < 5 || n - 4 >= P4_JAM_NAME || strcasecmp(e->d_name + n - 4, ".wav") || e->d_name[0] == '.') continue;
            jam_track_t *t = &list[count++];
            memset(t, 0, sizeof(*t));
            memcpy(t->name, e->d_name, n - 4);
            t->play = t->lufs = NAN;
            char path[sizeof(P4_JAM_DIR) + P4_JAM_NAME + 8];
            snprintf(path, sizeof(path), P4_JAM_DIR "/%s", e->d_name);
            struct stat st;
            if (stat(path, &st) == 0) t->size = (uint32_t)st.st_size;
        }
        closedir(d);
    }
    qsort(list, count, sizeof(jam_track_t), by_name);

    char line[200], *rest;
    FILE *f = fopen(P4_JAM_DIR "/levels.txt", "r");
    while (f && fgets(line, sizeof(line), f)) {
        char *name = line_name(line, &rest);
        jam_track_t *t = name ? find(list, count, name) : NULL;
        if (t) t->trim = strtof(rest, NULL);
    }
    if (f) fclose(f);
    f = fopen(P4_JAM_DIR "/loudness.txt", "r");
    while (f && fgets(line, sizeof(line), f)) {
        char *name = line_name(line, &rest);
        jam_track_t *t = name ? find(list, count, name) : NULL;
        unsigned size;
        float lufs, play, peak;
        if (t && sscanf(rest, "%u\t%f\t%f\t%f", &size, &lufs, &play, &peak) == 4 && size == t->size) {
            t->lufs = lufs;
            t->play = play;
            t->peak = peak;
        }
    }
    if (f) fclose(f);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *playing = s_current >= 0 && s_current < s_count ? s_tracks[s_current].name : NULL;
    int now = -1;
    for (int i = 0; playing && i < count; i++)
        if (!strcmp(list[i].name, playing)) now = i;
    s_tracks = list;
    s_count = count;
    s_current = now;
    int left = 0;
    for (int i = 0; i < count; i++) left += isnan(list[i].play);
    s_measuring = left;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d jam tracks on the card, %d to measure", count, left);
    if (s_measure_task) xTaskNotifyGive(s_measure_task);
}

// Both files written again from the list (a new file, then renamed - a power cut leaves the old one).
static void save_file(const char *name, bool loudness)
{
    char tmp[sizeof(P4_JAM_DIR) + 20], path[sizeof(P4_JAM_DIR) + 20];
    snprintf(tmp, sizeof(tmp), P4_JAM_DIR "/%s.tmp", name);
    snprintf(path, sizeof(path), P4_JAM_DIR "/%s.txt", name);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "%s.txt not written", name);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const jam_track_t *t = &s_tracks[i];
        if (loudness && !isnan(t->play))
            fprintf(f, "%s.wav\t%u\t%.2f\t%.2f\t%.5f\n", t->name, (unsigned)t->size, (double)t->lufs, (double)t->play, (double)t->peak);
        else if (!loudness && t->trim != 0)
            fprintf(f, "%s.wav\t%.1f\n", t->name, (double)t->trim);
    }
    xSemaphoreGive(s_lock);
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) ESP_LOGW(TAG, "%s.txt NOT written", name);
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

            char path[sizeof(P4_JAM_DIR) + P4_JAM_NAME + 8];
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
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "%s: %.1f LUFS, counts as %.1f, peak %.1f dBFS (%u ms)%s", name, (double)r.lufs, (double)r.play,
                     r.peak > 0 ? 20 * log10((double)r.peak) : -90.0, (unsigned)(esp_log_timestamp() - t0), ok ? "" : " - NOT READABLE");
            save_file("loudness", true);
            if (current) track_player_set_gain_db(&g_p4_track, track_gain(s_current));
        }
    }
}

// ---- file task ----

static FILE *open_track(int i, wav_info_t *wi)
{
    char path[sizeof(P4_JAM_DIR) + P4_JAM_NAME + 8];
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
        if (!got_cmd && s_current >= 0 && !s_transfer) {   // after a track: the next one (at the end of the list: stop)
            track_player_info_t ti;
            track_player_get_info(&g_p4_track, &ti);
            if (ti.ended && s_current + 1 < s_count) {
                c = (jam_cmd_t){ JAM_PLAY, s_current + 1 };
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
    if (!ring || !s_lists[0] || !s_lists[1] || !s_lock) {
        ESP_LOGE(TAG, "no memory for the jam player");
        return;
    }
    s_tracks = s_lists[0];
    track_player_init(&g_p4_track, ring, RING_FRAMES, 48000);
    s_card = mount();
    if (s_card) scan();
    s_queue = xQueueCreate(4, sizeof(jam_cmd_t));
    xTaskCreatePinnedToCore(jam_task, "jam", 4096, NULL, 6, NULL, 1);
    if (s_card) xTaskCreatePinnedToCore(measure_task, "jam_measure", 4096, NULL, 2, &s_measure_task, 1);
}

bool p4_jam_card(void) { return s_card; }
bool p4_jam_card_unreadable(void) { return !s_card && s_card_unreadable; }
int p4_jam_count(void) { return s_count; }
int p4_jam_measuring(void) { return s_measuring; }
const char *p4_jam_name(int i) { return i >= 0 && i < s_count ? s_tracks[i].name : ""; }

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

void p4_jam_prev(void)
{
    track_player_info_t ti;
    track_player_get_info(&g_p4_track, &ti);
    int cur = s_current;
    if (cur < 0) { if (s_count) p4_jam_play(0); return; }
    if (ti.rate && ti.pos > 3 * ti.rate) p4_jam_seek(0);
    else p4_jam_play(cur > 0 ? cur - 1 : 0);
}

void p4_jam_next(void)
{
    int cur = s_current;
    if (cur + 1 < s_count) p4_jam_play(cur + 1);
}

// A correction by ear for the current track: used at once and written to levels.txt.
void p4_jam_set_track_level(float db)
{
    int cur = s_current;
    if (cur < 0 || cur >= s_count) return;
    if (db < -12) db = -12;   // (as the Jam Player: +-12 dB)
    if (db > 12) db = 12;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tracks[cur].trim = db;
    xSemaphoreGive(s_lock);
    track_player_set_gain_db(&g_p4_track, track_gain(cur));
    save_file("levels", false);
    ESP_LOGI(TAG, "%s: by ear %+.1f dB, gain %+.1f dB", s_tracks[cur].name, (double)db, (double)track_gain(cur));
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

// A measurement that came with a file (the Mac measured it): kept, so the board need not measure it.
void p4_jam_measured(const char *name, uint32_t size, float lufs, float play, float peak)
{
    FILE *f = fopen(P4_JAM_DIR "/loudness.txt", "a");
    if (!f) return;
    fprintf(f, "%s.wav\t%u\t%.2f\t%.2f\t%.5f\n", name, (unsigned)size, (double)lufs, (double)play, (double)peak);
    fclose(f);
}

void p4_jam_set_trim(const char *name, float db)   // a correction that came with a file
{
    if (fabsf(db) < 0.05f) return;
    FILE *f = fopen(P4_JAM_DIR "/levels.txt", "a");
    if (!f) return;
    fprintf(f, "%s.wav\t%.1f\n", name, (double)db);
    fclose(f);
}

// Corrections by ear from the Mac ("<file><TAB><dB>" per line): merged into levels.txt (a line with 0 removes one).
// Read in when the transfer ends.
void p4_jam_trims(const char *text)
{
    enum { MOST = 256 };
    typedef struct { char name[P4_JAM_NAME]; float db; } trim_t;
    trim_t *list = heap_caps_calloc(MOST, sizeof(trim_t), MALLOC_CAP_SPIRAM);
    if (!list) return;
    int n = 0;
    char line[200], *rest;
    FILE *f = fopen(P4_JAM_DIR "/levels.txt", "r");
    while (f && n < MOST && fgets(line, sizeof(line), f)) {
        char *name = line_name(line, &rest);
        if (!name) continue;
        strlcpy(list[n].name, name, sizeof(list[n].name));
        list[n++].db = strtof(rest, NULL);
    }
    if (f) fclose(f);
    for (const char *p = text; *p;) {   // the new ones: replace or add
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = 0;
            char *name = line_name(line, &rest);
            if (name && name[0]) {
                int k = 0;
                while (k < n && strcmp(list[k].name, name)) k++;
                if (k == n && n < MOST) strlcpy(list[n++].name, name, sizeof(list[0].name));
                if (k < n) list[k].db = strtof(rest, NULL);
            }
        }
        p += len + (end ? 1 : 0);
    }
    f = fopen(P4_JAM_DIR "/levels.tmp", "w");
    if (f) {
        for (int i = 0; i < n; i++)
            if (fabsf(list[i].db) >= 0.05f) fprintf(f, "%s.wav\t%.1f\n", list[i].name, (double)list[i].db);
        if (fclose(f) == 0) {
            remove(P4_JAM_DIR "/levels.txt");
            rename(P4_JAM_DIR "/levels.tmp", P4_JAM_DIR "/levels.txt");
        }
    }
    free(list);
    ESP_LOGI(TAG, "corrections by ear from the Mac taken");
}
