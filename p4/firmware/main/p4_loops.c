// P4 board: the looper's loops saved to the microSD card (2026-11-28) - each part that holds a loop as a WAV file in
// /sdcard/loops (48 kHz, 16 bit, stereo: the take with its overdub, as it plays from its next pass on), for the Jam
// Player on the Mac to fetch over Wi-Fi (p4_net.c: GET /api/loops). The screen's "Save loops" (the drum menu) starts it.
//
// The sound never waits for it. A task of its own - low, on the mixer's core, which takes the core from it whenever it
// renders - copies the loop in pieces while it plays on: looper_save_* (looper.h) only reads, the audio side merely
// counts its changes of the loop (no lock, no wait). Nothing of the saving is in the sound's path: no file, no
// memory taken, no log line. A piece of 2048 frames is read from PSRAM (the loop and its overdub), pieces are
// gathered into one 32 KB buffer that goes to the card in one write - whole sectors, as the header is one sector of
// its own, so the card takes them by DMA straight from the buffer - and the task sleeps a tick between two pieces.
// A take, an overdub, undo / redo or clear on a part while it is copied make its file go: it is thrown away and the
// screen says so. A part that is being recorded is waited for up to 3 s, then left out.
//
// Nothing is written to the settings store: the number of a save is the highest on the card + 1, and the "saved"
// mark (the looper not changed since it was saved: the button says so, a second tap saves nothing twice) lives in RAM.
// A file is written as save.tmp and named when it is whole, so the list never shows half a loop. No card, a card
// the board cannot read, a full card or a write that fails end with a message on the screen; nothing is half done.
//
// Names (loop_wav.h): "Loop 0007 A, 120 BPM, 4 bars.wav", free length "Loop 0007 A, 8.3 s.wav"; once the Jam Player
// has told the board the time (POST /api/time) also ", 2026-11-28 14.05". The numbers go up with every save, so the
// names sort in the order the loops were made. With bars, the WAV also says its tempo (an "acid" chunk, for DAWs).
//
// The sound while saving is looked at and logged (p4_audio_marks): frames that went out late, packets skipped,
// frames missing in the capture, the longest render of the mix.
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loop_wav.h"
#include "p4_audio.h"
#include "ui.h"

static const char *TAG = "p4_loops";

#define RATE 48000
#define PIECE 2048                    // frames read from PSRAM at a time (8 KB of the loop, 8 KB of its overdub)
#define OUT_BYTES (32 * 1024)         // gathered, then one write to the card
#define WAIT_QUIET_MS 3000            // a part being recorded or merged is waited for this long
#define CARD_SPARE (1024 * 1024)      // left free on the card
#define TMP_PATH P4_LOOPS_DIR "/save.tmp"

static atomic_int s_busy, s_pct;
static atomic_bool s_saved[2];        // the part was saved ...
static atomic_uint s_saved_edit[2];   // ... at this change count (looper_edit_count: unchanged since = the same loop)
static atomic_uint s_saved_number[2];
static atomic_bool s_time_known;

enum { JOB_SAVE, JOB_EMPTY, JOB_BUSY, JOB_SAME, JOB_SAVED, JOB_CHANGED, JOB_FAILED };

typedef struct {
    int result;                       // JOB_*
    looper_save_t s;
    loop_name_t name;
    char file[LOOP_WAV_NAME_MAX];
} job_t;

// The Jam Player's clock: kept as the board's time in local time (so the card's file times are the Mac's too).
void p4_loops_set_time(int64_t unix_s, int utc_minutes)
{
    if (unix_s < 1735689600 || unix_s > 4102444800LL || utc_minutes < -14 * 60 || utc_minutes > 14 * 60) return;   // (2025 .. 2100)
    struct timeval tv = { .tv_sec = (time_t)(unix_s + utc_minutes * 60), .tv_usec = 0 };
    if (settimeofday(&tv, NULL) == 0) atomic_store(&s_time_known, true);
}

int p4_loops_state(int *percent)
{
    *percent = atomic_load(&s_pct);
    if (atomic_load(&s_busy)) return P4_SAVE_BUSY;
    loop_parts_t *P = p4_loop_parts();
    if (!P) return P4_SAVE_NONE;
    bool any = false, unsaved = false;
    for (int k = 0; k < 2; k++) {
        looper_info_t li;
        looper_get_info(&P->part[k], &li);
        if (li.state == LOOPER_EMPTY) continue;
        any = true;
        if (!atomic_load(&s_saved[k]) || looper_edit_count(&P->part[k]) != atomic_load(&s_saved_edit[k])) unsaved = true;
    }
    if (!any) return P4_SAVE_NONE;
    if (!p4_jam_card()) return P4_SAVE_NO_CARD;
    return unsaved ? P4_SAVE_READY : P4_SAVE_DONE;
}

// The parts to save: each one's copy begun (a part being recorded or merged: waited for, both at once).
static void take_parts(loop_parts_t *P, job_t *job)
{
    bool open[2] = { true, true };
    for (int waited = 0; open[0] || open[1]; waited += 20) {
        for (int k = 0; k < 2; k++) {
            if (!open[k]) continue;
            looper_info_t li;
            looper_get_info(&P->part[k], &li);
            if (li.state == LOOPER_EMPTY) job[k].result = JOB_EMPTY, open[k] = false;
            else if (looper_save_begin(&P->part[k], &job[k].s)) job[k].result = JOB_SAVE, open[k] = false;
            else if (waited >= WAIT_QUIET_MS) job[k].result = JOB_BUSY, open[k] = false;
        }
        if (open[0] || open[1]) vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Not changed since it was saved: not twice. But a save is the looper as it is - when one part changed, both go
    // under the new number (Loop 8 A and B belong together, even if A is the A of Loop 7).
    bool changed = false;
    for (int k = 0; k < 2; k++) {
        if (job[k].result != JOB_SAVE) continue;
        if (atomic_load(&s_saved[k]) && atomic_load(&s_saved_edit[k]) == job[k].s.edit) job[k].result = JOB_SAME;
        else changed = true;
    }
    for (int k = 0; k < 2 && changed; k++)
        if (job[k].result == JOB_SAME) job[k].result = JOB_SAVE;
}

// Bars and tempo: the bars the take was rounded to, else whole bars of the Nano's tempo now (within 10 ms, as the
// screen counts them); the tempo is the loop's own (its length over its beats).
static void describe(job_t *j, int k, unsigned number)
{
    const looper_save_t *s = &j->s;
    loop_name_t *nm = &j->name;
    memset(nm, 0, sizeof(*nm));
    nm->number = number;
    nm->part = k;
    nm->seconds = s->len / (float)RATE;
    unsigned bars = 0;
    uint32_t now = p4_looper_bar_now();
    if (s->bar && s->len % s->bar == 0) {
        bars = s->len / s->bar;
    } else if (now) {
        uint32_t b = (s->len + now / 2) / now;
        int64_t off = (int64_t)b * now - s->len;
        if (b && off >= -480 && off <= 480) bars = b;
    }
    int beats = p4_looper_beats();
    if (bars && bars <= 999) {
        nm->bars = bars;
        nm->bpm = (float)RATE * 60.0f * beats * bars / s->len;
    }
    if (atomic_load(&s_time_known)) {
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);   // (the board's time is local time: p4_loops_set_time)
        nm->dated = true;
        nm->year = tm.tm_year + 1900, nm->month = tm.tm_mon + 1, nm->day = tm.tm_mday, nm->hour = tm.tm_hour, nm->minute = tm.tm_min;
    }
    loop_wav_name(j->file, sizeof(j->file), nm);
}

static unsigned next_number(void)
{
    unsigned most = 0;
    DIR *d = opendir(P4_LOOPS_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        unsigned v = loop_wav_number(e->d_name);
        if (v > most) most = v;
    }
    if (d) closedir(d);
    return most + 1;
}

typedef struct { uint32_t done, total; int64_t read_us, write_us; uint32_t clipped; } progress_t;

// One part into its file: JOB_SAVED, JOB_CHANGED (thrown away) or JOB_FAILED (the card).
static int write_part(looper_t *lp, job_t *j, uint8_t *buf, progress_t *pg)
{
    const looper_save_t *s = &j->s;
    FILE *f = fopen(TMP_PATH, "wb");
    if (!f) return JOB_FAILED;
    setvbuf(f, NULL, _IONBF, 0);   // (the 32 KB pieces go to the file system as they are)
    int beats = p4_looper_beats();
    loop_wav_header(buf, s->len, RATE, j->name.bpm, j->name.bars * beats, beats);
    size_t fill = LOOP_WAV_HEAD;
    int result = JOB_SAVED;
    for (uint32_t from = 0; from < s->len;) {
        uint32_t n = s->len - from;
        if (n > PIECE) n = PIECE;
        if (n > (OUT_BYTES - fill) / 4) n = (OUT_BYTES - fill) / 4;
        int64_t t0 = esp_timer_get_time();
        looper_save_read(lp, s, from, n, (int16_t *)(buf + fill), &pg->clipped);
        pg->read_us += esp_timer_get_time() - t0;
        if (!looper_save_same(lp, s)) {
            result = JOB_CHANGED;
            break;
        }
        fill += (size_t)n * 4;
        from += n;
        if (fill == OUT_BYTES || from == s->len) {
            t0 = esp_timer_get_time();
            bool ok = fwrite(buf, 1, fill, f) == fill;
            pg->write_us += esp_timer_get_time() - t0;
            if (!ok) {
                result = JOB_FAILED;
                break;
            }
            fill = 0;
        }
        pg->done += n;
        atomic_store(&s_pct, (int)((uint64_t)pg->done * 100 / pg->total));
        vTaskDelay(1);   // (the screen and the rest of the core between two pieces)
    }
    if (fclose(f) != 0 && result == JOB_SAVED) result = JOB_FAILED;
    if (result == JOB_SAVED) {
        char path[sizeof(P4_LOOPS_DIR) + LOOP_WAV_NAME_MAX + 2];
        snprintf(path, sizeof(path), P4_LOOPS_DIR "/%s", j->file);
        if (rename(TMP_PATH, path) != 0) result = JOB_FAILED;   // (FAT's rename replaces nothing: never one of the same name)
    }
    if (result != JOB_SAVED) remove(TMP_PATH);
    return result;
}

// "A and B", "A"
static void parts_text(char *out, size_t n, const job_t *job, int result)
{
    out[0] = 0;
    for (int k = 0; k < 2; k++) {
        if (job[k].result != result) continue;
        size_t at = strlen(out);
        snprintf(out + at, n - at, "%s%c", at ? " and " : "", 'A' + k);
    }
}

static void run_save(char *msg, size_t n)
{
    loop_parts_t *P = p4_loop_parts();
    if (!P) {
        snprintf(msg, n, "The looper has no memory: nothing to save");
        return;
    }
    if (!p4_jam_card()) {
        snprintf(msg, n, p4_jam_card_unreadable() ? "The card needs FAT32 - loops not saved" : "No microSD card in the board - loops not saved");
        return;
    }
    job_t job[2];
    memset(job, 0, sizeof(job));
    take_parts(P, job);
    int todo = 0;
    uint64_t need = CARD_SPARE;
    for (int k = 0; k < 2; k++)
        if (job[k].result == JOB_SAVE) todo++, need += LOOP_WAV_HEAD + (uint64_t)job[k].s.len * 4;
    char which[16];
    if (!todo) {
        if (job[0].result == JOB_BUSY || job[1].result == JOB_BUSY) {
            parts_text(which, sizeof(which), job, JOB_BUSY);
            snprintf(msg, n, "Part %s is being recorded - save when it plays", which);
        } else if (job[0].result == JOB_SAME || job[1].result == JOB_SAME) {
            unsigned a = atomic_load(&s_saved_number[0]), b = atomic_load(&s_saved_number[1]);
            if (job[0].result == JOB_SAME && job[1].result == JOB_SAME && a != b)
                snprintf(msg, n, "Already on the card: Loop %u A, Loop %u B", a, b);
            else {
                parts_text(which, sizeof(which), job, JOB_SAME);
                snprintf(msg, n, "Already on the card: Loop %u %s", job[0].result == JOB_SAME ? a : b, which);
            }
        } else {
            snprintf(msg, n, "Nothing to save: both parts are empty");
        }
        return;
    }
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &free_b) != ESP_OK) {
        snprintf(msg, n, "The card cannot be read - loops not saved");
        return;
    }
    if (free_b < need) {
        snprintf(msg, n, "The card is full (%u MB free, %u MB needed) - not saved", (unsigned)(free_b >> 20), (unsigned)((need + (1 << 20) - 1) >> 20));
        return;
    }
    mkdir(P4_LOOPS_DIR, 0775);   // (an existing folder stays as it is)
    struct stat st;
    uint8_t *buf = heap_caps_aligned_alloc(64, OUT_BYTES, MALLOC_CAP_SPIRAM);   // (cache-line aligned: the card's DMA reads it)
    if (stat(P4_LOOPS_DIR, &st) != 0 || !S_ISDIR(st.st_mode) || !buf) {
        free(buf);
        snprintf(msg, n, buf ? "Could not write to the card - loops not saved" : "The board is short of memory - loops not saved");
        return;
    }
    unsigned number = next_number();
    progress_t pg = { 0 };
    for (int k = 0; k < 2; k++) if (job[k].result == JOB_SAVE) pg.total += job[k].s.len;
    p4_sound_marks_t m0, m1;
    p4_audio_marks(&m0, true);
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < 2; k++) {
        if (job[k].result != JOB_SAVE) continue;
        describe(&job[k], k, number);
        int64_t r0 = pg.read_us, w0 = pg.write_us, p0 = esp_timer_get_time();
        job[k].result = write_part(&P->part[k], &job[k], buf, &pg);
        unsigned ms = (unsigned)((esp_timer_get_time() - p0) / 1000), kb = (unsigned)((LOOP_WAV_HEAD + job[k].s.len * 4ull) / 1024);
        if (job[k].result == JOB_SAVED) {
            atomic_store(&s_saved_edit[k], job[k].s.edit);
            atomic_store(&s_saved_number[k], number);
            atomic_store(&s_saved[k], true);
            ESP_LOGI(TAG, "%s: %u KB in %u ms (reading the loop %u ms, writing the card %u ms: %u KB/s)", job[k].file, kb, ms,
                     (unsigned)((pg.read_us - r0) / 1000), (unsigned)((pg.write_us - w0) / 1000),
                     (unsigned)(kb * 1000000ull / (uint64_t)(pg.write_us - w0 + 1)));
        } else {
            ESP_LOGW(TAG, "part %c NOT saved: %s", 'A' + k, job[k].result == JOB_CHANGED ? "it changed while it was copied" : "the card refused the file");
        }
    }
    free(buf);
    p4_audio_marks(&m1, false);
    ESP_LOGI(TAG, "the sound while saving (%u ms): %u frames late, %u packets skipped, %u capture frames missing; "
             "longest mix render %u us (it renders 20 ms ahead); %u samples clipped in the files",
             (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)(m1.late_frames - m0.late_frames), (unsigned)(m1.skipped - m0.skipped),
             (unsigned)(m1.short_frames - m0.short_frames), (unsigned)m1.mix_max_us, (unsigned)pg.clipped);

    // what the screen says
    int saved = 0;
    for (int k = 0; k < 2; k++) saved += job[k].result == JOB_SAVED;
    if (!saved) {
        if (job[0].result == JOB_FAILED || job[1].result == JOB_FAILED) snprintf(msg, n, "Could not write to the card - loops not saved");
        else snprintf(msg, n, "The loop changed while it was saved - save again");
        return;
    }
    parts_text(which, sizeof(which), job, JOB_SAVED);
    int at = snprintf(msg, n, "Saved to the card: Loop %u %s", number, which);
    for (int k = 0; k < 2 && at > 0 && (size_t)at < n; k++) {
        const char *why = job[k].result == JOB_BUSY ? "being recorded" : job[k].result == JOB_CHANGED ? "changed while saving"
                        : job[k].result == JOB_FAILED ? "not written" : NULL;
        if (why) at += snprintf(msg + at, n - (size_t)at, " \xC2\xB7 %c %s", 'A' + k, why);
    }
}

static void save_task(void *arg)
{
    (void)arg;
    char msg[120];
    run_save(msg, sizeof(msg));
    ESP_LOGI(TAG, "%s", msg);
    atomic_store(&s_busy, 0);
    ui_show_message(msg);
    vTaskDelete(NULL);
}

void p4_loops_save(void)
{
    int idle = 0;
    if (!atomic_compare_exchange_strong(&s_busy, &idle, 1)) return;   // (saving already)
    atomic_store(&s_pct, 0);
    // low, on core 1 with the mixer: it renders whenever it has to, the copy only fills the gaps
    if (xTaskCreatePinnedToCore(save_task, "loop_save", 6144, NULL, 2, NULL, 1) != pdPASS) {
        atomic_store(&s_busy, 0);
        ui_show_message("The board is short of memory - loops not saved");
    }
}
