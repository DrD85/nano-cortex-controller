// P4 board: audio over the Nano's USB - the phone's audio, the jam track and the looper (mixer, audio_mix.h) into
// the Nano's playback; the Nano's output (capture channels 3 / 4) goes to the looper and is measured for the OUT field of the signal chain (momentary loudness, ITU-R BS.1770 K-weighting,
// 400 ms). The phone comes from the Bluetooth bridge (a2dp-bridge/, a classic ESP32): its audio over I2S (the P4 is
// the slave), what plays and the buttons over a UART line (protocol in a2dp-bridge/main/main.c).
//
// Pins (PLAN.md, header P1): I2S BCLK GPIO46, WS GPIO47, DATA GPIO48; UART TX GPIO49, RX GPIO50; ESP32 EN GPIO51.
//
// STATUS: builds; not yet run on hardware.
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_cache.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "nvs.h"
#include "audio_pipe.h"
#include "board.h"
#include "p4_audio.h"
#include "p4_cover.h"
#include "p4_net.h"
#include "p4_spectrum.h"
#include "loudness.h"
#include "uac2_host.h"
#include "ui.h"

static const char *TAG = "p4_audio";
static double s_ms_pub[2];        // the Nano's output: mean square of the last 400 ms, written by the audio task
static atomic_uint s_ms_seq;       // (which slot was written last; the screen makes the LUFS of it)

#define RING_FRAMES 4096
#define LOOPER_MAX_S 45
// The looper's round trip in frames: from the capture frame it works on to the capture frame in which a sound played
// now comes back. Measured on the board with clicks into the Nano's re-amp input (USB playback 3/4, back on capture
// 3/4 through the preset). 2026-11-18: 567-771 frames, wandering between connections and once within one. Since
// 2026-11-19 the playback follows the capture packet by packet (uac2_pacer.h) and the mixer keeps its cushion
// (audio_mix.h): 620-650 frames over two board starts and eight reconnects, the same within each connection.
// The Nano's own processing is part of that measurement but not of the looper's way (the loop plays into the
// Nano's output, USB playback 1/2, which does not come back in the capture): taken as about 1.3 ms (62 frames,
// the difference seen on 2026-11-18) - to be confirmed by ear. Changes with MIX_CUSHION.
// + LIM_AHEAD (2026-11-19): the mixer's look-ahead limiter sends the board's playback 48 frames later, so the player
// hears the loop and the drums that much later and plays that much later.
// + LEAD_FRAMES (2026-11-26): the mix is rendered that much ahead of the USB playback (see "the playback's lead").
#define LEAD_RING 1024               // frames (power of two)
#define LEAD_FRAMES 960              // 20 ms
#define LOOPER_LATENCY (578 - 192 + MIX_CUSHION + MIX_OUT_DELAY + LEAD_FRAMES)   // (578 was measured with a cushion of 192)
#define OUT_LEFT 2            // capture channel 3 (0-based 2): the Nano's processed sound
#define OUT_RIGHT 3
#define WINDOW_FRAMES 19200   // 400 ms at 48 kHz
#define SHOW_EVERY_US 100000
#define BRIDGE_BCLK GPIO_NUM_46
#define BRIDGE_WS GPIO_NUM_47
#define BRIDGE_DATA GPIO_NUM_48
#define BRIDGE_TX GPIO_NUM_49
#define BRIDGE_RX GPIO_NUM_50
#define BRIDGE_EN GPIO_NUM_51
#define BRIDGE_UART UART_NUM_1
#define BRIDGE_I2S I2S_NUM_1   // (I2S 0 belongs to the board's own codec)

static audio_pipe_t s_pipe;
audio_mix_t g_p4_mix;
track_player_t g_p4_track;
// The looper is two parts (A and B, loop_parts.h); s_looper is the part that plays.
static loop_parts_t s_parts;
#define s_looper (s_parts.part[atomic_load(&s_parts.i_active) & 1])
static bool s_looper_ready;
static drum_machine_t s_drums;
static bool s_drums_ready;
static int s_drum_groove = 12;          // "Funk" (the groove, its level and the sync are kept: NVS "p4")
static float s_drum_level_db = -9;
// The kit itself is turned down by this much (the screen's level stays as it was): at 0 dB its hits together reached
// 1.8 times full scale and, with a loop, kept the limiter busy all the time (3000-6000 frames in every 5 s, 2026-11-19).
#define DRUM_TRIM_DB (-6.0f)
static bool s_drum_sync = true;
static bool s_count_in;
static bool s_quantize = true, s_arm;   // takes on the drums' bar lines; Rec arms
static int s_drum_kit = DRUM_KIT_ACOUSTIC;   // (kept: NVS "p4" / "dkit", the DRUM_KIT_* number)
static bool s_acoustic_kit;                  // its samples were found in the flash
static atomic_bool s_lofi_asked;             // the Lo-Fi kit is made or being made (once: see make_lofi)
static void drums_store(void)
{
    nvs_handle_t nvs;
    if (nvs_open("p4", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "dgroove", (uint8_t)s_drum_groove);
    nvs_set_i8(nvs, "dlevel", (int8_t)lroundf(s_drum_level_db));
    nvs_set_u8(nvs, "dsync", s_drum_sync);
    nvs_set_u8(nvs, "dcount", s_count_in);
    nvs_set_u8(nvs, "dquant", s_quantize);
    nvs_set_u8(nvs, "darm", s_arm);
    nvs_set_u8(nvs, "dkit", (uint8_t)s_drum_kit);
    nvs_commit(nvs);
    nvs_close(nvs);
}
static void drums_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("p4", NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t g, y;
    int8_t l;
    if (nvs_get_u8(nvs, "dgroove", &g) == ESP_OK && g < DRUM_GROOVE_COUNT) s_drum_groove = g;
    if (nvs_get_i8(nvs, "dlevel", &l) == ESP_OK) s_drum_level_db = l;
    if (nvs_get_u8(nvs, "dsync", &y) == ESP_OK) s_drum_sync = y != 0;
    if (nvs_get_u8(nvs, "dcount", &y) == ESP_OK) s_count_in = y != 0;
    if (nvs_get_u8(nvs, "dquant", &y) == ESP_OK) s_quantize = y != 0;
    if (nvs_get_u8(nvs, "darm", &y) == ESP_OK) s_arm = y != 0;
    if (nvs_get_u8(nvs, "dkit", &y) == ESP_OK && y < DRUM_KITS) s_drum_kit = y;
    nvs_close(nvs);
}

// The looper parts' own grooves (NVS "p4" / "pgroove", two int8, -1 = the common groove): handed to the parts
// resolved - a part without its own plays the common groove, but only once one of them has its own.
static int s_part_groove[2] = { -1, -1 };
static void parts_groove_apply(void)
{
    bool any = s_part_groove[0] >= 0 || s_part_groove[1] >= 0;
    for (int i = 0; i < 2; i++) loop_parts_set_part_groove(&s_parts, i, s_part_groove[i] >= 0 ? s_part_groove[i] : any ? s_drum_groove : -1);
}
static void parts_groove_store(void)
{
    nvs_handle_t nvs;
    if (nvs_open("p4", NVS_READWRITE, &nvs) != ESP_OK) return;
    int8_t v[2] = { (int8_t)s_part_groove[0], (int8_t)s_part_groove[1] };
    nvs_set_blob(nvs, "pgroove", v, sizeof(v));
    nvs_commit(nvs);
    nvs_close(nvs);
}
static void parts_groove_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("p4", NVS_READONLY, &nvs) != ESP_OK) return;
    int8_t v[2];
    size_t n = sizeof(v);
    if (nvs_get_blob(nvs, "pgroove", v, &n) == ESP_OK && n == sizeof(v))
        for (int i = 0; i < 2; i++) s_part_groove[i] = v[i] >= 0 && v[i] < DRUM_GROOVE_COUNT ? v[i] : -1;
    nvs_close(nvs);
}

// The acoustic kit's samples (the QC board's tools/gen_drum_kit.c, "NDK1"): in the partition "drumkit" (at 26 MB, where
// the QC board's firmware keeps the same file), read into PSRAM once (about 4 MB) - false when they are not there.
#define DRUMKIT_MAX 0x400000
static bool load_acoustic_kit(drum_machine_t *d)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x43, "drumkit");
    uint32_t head[4];
    if (!part || esp_partition_read(part, 0, head, 16) != ESP_OK ||
        memcmp(head, "NDK1", 4) || head[2] > DRUM_PADS * DRUM_LAYERS * DRUM_TAKES || head[3] != 16 + 16 * head[2]) {
        ESP_LOGI(TAG, "drums: no acoustic kit in the flash memory");
        return false;
    }
    uint8_t *table = malloc(head[3]);
    if (!table || esp_partition_read(part, 0, table, head[3]) != ESP_OK) { free(table); return false; }
    size_t size = head[3];
    for (uint32_t i = 0; i < head[2]; i++) {   // the file's end: the furthest take
        uint32_t off, frames;
        memcpy(&off, table + 16 + 16 * i + 4, 4);
        memcpy(&frames, table + 16 + 16 * i + 8, 4);
        size_t end = head[3] + (size_t)off + (size_t)frames * table[16 + 16 * i + 3] * 2;
        if (end > size) size = end;
    }
    free(table);
    if (size > DRUMKIT_MAX || size > part->size) return false;
    uint8_t *file = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!file) { ESP_LOGE(TAG, "drums: no memory for the acoustic kit (%u KB)", (unsigned)(size / 1024)); return false; }
    for (size_t at = 0; at < size; at += 65536) {   // (in pieces: the other tasks run between them)
        size_t n = size - at < 65536 ? size - at : 65536;
        if (esp_partition_read(part, at, file + at, n) != ESP_OK) { free(file); return false; }
        vTaskDelay(pdMS_TO_TICKS(20));   // (one tick was not enough: the idle task's watchdog spoke twice while it read, 2026-11-26)
    }
    if (!drum_machine_load_kit(d, file, size)) { free(file); ESP_LOGW(TAG, "drums: the acoustic kit is broken"); return false; }
    ESP_LOGI(TAG, "drums: acoustic kit, %u takes, %u KB", (unsigned)head[2], (unsigned)(size / 1024));
    return true;   // (the file stays: the kit plays from it)
}
static void *psram_alloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
// The Lo-Fi kit (drum_lofi.c, about 1.1 MB in PSRAM): made once, when it is first chosen - in drums_task when it is the
// kept kit (before the Nano's sound runs), otherwise in a task of its own; the computed kit plays until it is there.
static void make_lofi(void)
{
    if (atomic_exchange(&s_lofi_asked, true)) return;
    int64_t t0 = esp_timer_get_time();
    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (!drum_machine_make_lofi(&s_drums, psram_alloc)) {
        ESP_LOGE(TAG, "drums: no memory for the Lo-Fi kit");
        atomic_store(&s_lofi_asked, false);   // (tried again when it is chosen again)
        return;
    }
    ESP_LOGI(TAG, "drums: Lo-Fi kit made in %d ms, %u KB PSRAM, %u KB left", (int)((esp_timer_get_time() - t0) / 1000),
             (unsigned)((before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
static void lofi_task(void *arg)
{
    make_lofi();
    vTaskDelete(NULL);
}
static int kit_to_play(int kit) { return kit == DRUM_KIT_ACOUSTIC && !s_acoustic_kit ? DRUM_KIT_COMPUTED : kit; }
static void drums_task(void *arg);
static bool s_bar_lines;
static portMUX_TYPE s_looper_lock = portMUX_INITIALIZER_UNLOCKED;   // commands come from the footswitch and app tasks
static int s_beats = 4;
static ui_phone_t s_phone = { .device = "Phone", .volume_db = -8.0f, .mute_on_tuner = true };

// ---- the Nano's output: momentary loudness ----

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;
static biquad_t s_shelf[2] = {
    { 1.53512485958697f, -2.69169618940638f, 1.19839281085285f, -1.69065929318241f, 0.73248077421585f, 0, 0 },
    { 1.53512485958697f, -2.69169618940638f, 1.19839281085285f, -1.69065929318241f, 0.73248077421585f, 0, 0 },
};
static biquad_t s_high[2] = {
    { 1.0f, -2.0f, 1.0f, -1.99004745483398f, 0.99007225036621f, 0, 0 },
    { 1.0f, -2.0f, 1.0f, -1.99004745483398f, 0.99007225036621f, 0, 0 },
};
// The capture callback's constants in internal RAM (2026-11-28): as literals they sat in the file's constant pool in
// flash - IRAM_ATTR moves only the code - and every packet read them through the cache.
static DRAM_ATTR float s_k_full = 1.0f / 8388608.0f, s_k_block = 1.0f / 960.0f;
static double s_sum[WINDOW_FRAMES / 960];   // mean squares per 20 ms block, for a sliding 400 ms window
static int s_block, s_block_frames;
static float s_block_sum;   // (float: the P4 has no double unit - a double here cost a library call per frame)
static int64_t s_shown_us;
// 100 ms blocks for the Level sheet (2026-11-26, after the QC board): K-weighted mean square of both channels
// (BS.1770: LUFS = -0.691 + 10 log10 e) and the sample peak; written by the USB audio task, read by the screen's
// task in loop_source.
#define LOUD_RING 64
typedef struct { float e, peak; } loud_block_t;
static loud_block_t s_loud[LOUD_RING];
static atomic_uint s_loud_n;
static float s_acc_e, s_acc_peak, s_peak20;
static int s_acc_k;

static inline float run(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

#ifdef NANO_UITEST
// ---- development: a click into one playback pair, looked for on every capture channel ----
// Answers what the looper has to know about the Nano: which playback pair it plays, whether it sends the board's
// playback back in the capture (and how loud), and how many frames later - counted as the looper counts: from the
// capture frame it was working on when the click went out to the capture frame the click comes back in.
#define CLICK_FRAMES 96            // 2 ms of 1 kHz
#define CLICK_WAIT 24000           // half a second to come back
#define CLICK_NONE INT32_MIN
static struct {
    atomic_int request;            // 1 + playback pair to send a click to (0 = none)
    atomic_int done;
    float amp;
    bool active;
    int pair, channels;
    uint32_t sent, seen;
    uint32_t r0, cr0, fill;        // render index / capture frame in work / frames waiting when the click went out
    int loop_state;                // the looper while the click's first frame was rendered: its state,
    uint32_t loop_pos;             // ... the position it played (the frames recorded, while the first take runs)
    float quiet[8], floor[8], peak[8];
    int32_t first[8];              // capture frame of the onset minus cr0
} s_click;

static void click_render(int32_t *frames, int count, int channels)
{
    if (!s_click.active) {
        int req = atomic_exchange(&s_click.request, 0);
        if (!req || (req - 1) * 2 + 1 >= channels) return;
        s_click.pair = req - 1;
        s_click.sent = s_click.seen = 0;
        s_click.r0 = g_p4_mix.render_index - (uint32_t)count;
        uint32_t cr = atomic_load(&g_p4_mix.cr);
        s_click.cr0 = cr - (uint32_t)count;
        s_click.fill = atomic_load(&g_p4_mix.cw) - cr;
        s_click.loop_state = s_looper_ready ? (int)s_looper.st : -1;   // (same task as looper_process: exact)
        s_click.loop_pos = s_looper_ready ? (s_looper.st == LOOPER_REC ? s_looper.rec_n : s_looper.pos) - (uint32_t)count : 0;
        for (int c = 0; c < 8; c++) {
            s_click.floor[c] = s_click.quiet[c];
            s_click.peak[c] = 0;
            s_click.first[c] = CLICK_NONE;
        }
        s_click.active = true;
    }
    for (int i = 0; i < count && s_click.sent < CLICK_FRAMES; i++, s_click.sent++) {
        float v = s_click.amp * sinf(2 * (float)M_PI * 1000 * s_click.sent / 48000);
        int32_t slot = (int32_t)((uint32_t)(int32_t)(v * 8388607.0f) << 8);
        frames[i * channels + s_click.pair * 2] = slot;
        frames[i * channels + s_click.pair * 2 + 1] = slot;
    }
}

static void click_capture(const int32_t *frames, int count, int channels, uint32_t index)
{
    s_click.channels = channels;
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < channels && c < 8; c++) {
            float v = fabsf(frames[i * channels + c] / 2147483648.0f);
            if (!s_click.active) {
                s_click.quiet[c] = fmaxf(v, s_click.quiet[c] * 0.9999f);   // a slowly falling peak: the noise before
                continue;
            }
            if (v > s_click.peak[c]) s_click.peak[c] = v;
            if (s_click.first[c] == CLICK_NONE && v > fmaxf(8 * s_click.floor[c], 0.002f))
                s_click.first[c] = (int32_t)(index + (uint32_t)i - s_click.cr0);
        }
    }
    if (s_click.active && (s_click.seen += (uint32_t)count) > CLICK_WAIT) {
        s_click.active = false;
        atomic_store(&s_click.done, 1);
    }
}

// One click at `db` dBFS into playback pair 0-3; the result as log lines (takes about 0.7 s).
void p4_audio_click(int pair, float db)
{
    s_click.amp = powf(10.0f, db / 20.0f);
    atomic_store(&s_click.done, 0);
    atomic_store(&s_click.request, 1 + pair);
    for (int i = 0; i < 150 && !atomic_load(&s_click.done); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!atomic_load(&s_click.done)) {
        atomic_store(&s_click.request, 0);
        ESP_LOGW(TAG, "CLICK playback %d/%d: no audio stream running - nothing sent", pair * 2 + 1, pair * 2 + 2);
        return;
    }
    ESP_LOGI(TAG, "CLICK playback %d/%d at %.0f dBFS: %u capture frames were waiting in the mixer, capture - render index %ld, looper state %d at frame %u",
             pair * 2 + 1, pair * 2 + 2, db, (unsigned)s_click.fill, (long)(int32_t)(s_click.cr0 - s_click.r0),
             s_click.loop_state, (unsigned)s_click.loop_pos);
    for (int c = 0; c < s_click.channels && c < 8; c++) {
        float floor_db = 20 * log10f(fmaxf(s_click.floor[c], 1e-7f)), peak_db = 20 * log10f(fmaxf(s_click.peak[c], 1e-7f));
        if (s_click.first[c] == CLICK_NONE)
            ESP_LOGI(TAG, "CLICK   capture %d: not found (noise before %.0f dBFS, peak after %.0f dBFS)", c + 1, floor_db, peak_db);
        else
            ESP_LOGI(TAG, "CLICK   capture %d: back after %ld frames (%.2f ms), %.1f dB against what was sent (noise before %.0f dBFS)",
                     c + 1, (long)s_click.first[c], s_click.first[c] / 48.0f, peak_db - db, floor_db);
    }
}
#endif

static void mix_held(uint32_t us);
// ---- the playback's lead (2026-11-26) ----
// The Nano's audio goes wrong for good ("robotic": frames a second time on every channel) when the USB playback
// stumbles - and it stumbled whenever the render was held up: the mix reads loops and drum sounds from PSRAM, and
// while the screen's driver writes a drawn frame out of the cache (up to 11-16 ms for a large area) such a read
// waits. Rendering inside the USB task, that held the USB transfers up as well (capture gaps of the same length).
// Now the USB task only copies: the mix is rendered LEAD_FRAMES ahead by a task of its own (core 1, above the
// screen's task: between the pieces of a write-back it runs unhindered) into a ring in internal RAM. Frames are
// counted from the start on both sides, so the render stays exactly LEAD_FRAMES ahead whatever happens: a render
// that falls behind makes silence go out for the missing frames and then leaves those out (nothing shifts).
static DRAM_ATTR int32_t s_lead[LEAD_RING * 2];   // left, right as they go out
static atomic_uint s_lead_w, s_lead_r;            // frames rendered / sent since the start
static atomic_uint s_lead_missing;                // frames that went out as silence (the render was behind)
// For a look at the sound over a span of time (p4_audio_marks: saving the loops, 2026-11-28): the same frames since
// the start, and the longest render of the mix since the last look.
static atomic_uint s_lead_missing_all, s_mix_max_us;

// The test click (console ':c', 2026-11-27): 10 ms of 1 kHz every 500 ms into playback 3/4, the Nano's re-amp input -
// it comes back on capture 3/4 through the preset, so the looper records a known signal (p4-loop-test.py). From a
// table in internal RAM: this is the USB task, no libm (flash) here.
#define CLICK_TAB 48                  // one period of 1 kHz
#define CLICK_LEN 480                 // 10 periods: starts and ends at zero
#define CLICK_EVERY 24000
static DRAM_ATTR int32_t s_click_tab[CLICK_TAB];
static atomic_int s_click_db;         // its level below full scale, 0 = off
static uint32_t s_click_at;           // (USB task only)

static IRAM_ATTR void on_render(int32_t *frames, int count, int channels, void *arg)   // (USB task; internal RAM only)
{
    uint32_t r = atomic_load_explicit(&s_lead_r, memory_order_relaxed), w = atomic_load_explicit(&s_lead_w, memory_order_acquire);
    uint32_t missing = 0;
    memset(frames, 0, sizeof(int32_t) * (size_t)count * channels);
    for (int i = 0; i < count; i++, r++) {
        if ((int32_t)(w - r) <= 0) { missing++; continue; }
        frames[i * channels + 0] = s_lead[2 * (r & (LEAD_RING - 1))];   // playback channels 1/2
        frames[i * channels + 1] = s_lead[2 * (r & (LEAD_RING - 1)) + 1];
    }
    atomic_store_explicit(&s_lead_r, r, memory_order_release);
    if (missing) {
        atomic_fetch_add_explicit(&s_lead_missing, missing, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_lead_missing_all, missing, memory_order_relaxed);
    }
    if (atomic_load_explicit(&s_click_db, memory_order_acquire) && channels >= 4) {
        for (int i = 0; i < count; i++) {
            if (s_click_at < CLICK_LEN) frames[i * channels + 2] = frames[i * channels + 3] = s_click_tab[s_click_at % CLICK_TAB];
            if (++s_click_at == CLICK_EVERY) s_click_at = 0;
        }
    }
#ifdef NANO_UITEST
    click_render(frames, count, channels);
#endif
}

static IRAM_ATTR void mix_task(void *arg)
{
    static int32_t block[MIX_BLOCK * 2];
    for (;;) {
        uint32_t w = atomic_load_explicit(&s_lead_w, memory_order_relaxed);
        int32_t need = (int32_t)(atomic_load_explicit(&s_lead_r, memory_order_acquire) + LEAD_FRAMES - w);
        if (need <= 0) { vTaskDelay(1); continue; }
        uint32_t n = need > MIX_BLOCK ? MIX_BLOCK : (uint32_t)need;
        int64_t t0 = esp_timer_get_time();
        audio_mix_render(&g_p4_mix, block, n, 2, 0, 1);
        uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
        if (took > 3000) mix_held(took);   // (measuring: 'M' in the trace)
        if (took > atomic_load_explicit(&s_mix_max_us, memory_order_relaxed)) atomic_store_explicit(&s_mix_max_us, took, memory_order_relaxed);
        for (uint32_t i = 0; i < n; i++) {
            s_lead[2 * ((w + i) & (LEAD_RING - 1))] = block[2 * i];
            s_lead[2 * ((w + i) & (LEAD_RING - 1)) + 1] = block[2 * i + 1];
        }
        atomic_store_explicit(&s_lead_w, w + n, memory_order_release);
    }
}

// The sound's (capture 3/4) squares since input_task last looked: its 'played' gate. Written by on_capture.
static float s_in_sq;
static uint32_t s_in_n;
static float s_in_db = -120;   // 3/4's mean level in input_task's last 5 s (the console's REPORT)
// The levels a compressor sees, for the gain reduction in the FX editor (2026-11-27): squares and peak of the DI (1/2)
// and the sound (3/4) since the screen last asked (loop_source, every 100 ms). Written here, read and cleared there -
// a reading that tears now and then only moves a meter.
static volatile float s_gr_sq[2], s_gr_pk[2];
static volatile uint32_t s_gr_n;
static volatile float s_env_pk;   // the DI's peak since the auto-wah last asked (p4_audio_di_peak)
#if P4_SPECTRUM_DI
// The DI (capture 1) for the EQ editor's spectrum, written only while it shows (p4_spectrum.c): 16 bits a frame
// into internal RAM - the USB task must not touch PSRAM (REALTIME.md rule 1).
DRAM_ATTR int16_t g_p4_di_ring[P4_DI_RING];
atomic_uint g_p4_di_w;
volatile bool g_p4_di_on;
#endif
float p4_audio_di_peak(void) { float p = s_env_pk; s_env_pk = 0; return p; }

// Every 5 s: the capture's packet sizes and rate (a line when something is off, else once a minute), frames the Nano
// sent a second time (its garbled state), and the self-heal.
#define GARBLE_REPEATS 30   // repeated frames in 5 s while playing that count as a garbled stream
#define GARBLE_MILD 8       // ... and a mildly garbled one, if it lasts: three windows (15 s) in a row
                            // (run 24, 2026-11-28: 2-39 per window for 95 s, never two windows over 30 - no heal)
static unsigned s_heals;   // streams started anew because of it (since the start)
static void input_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t n = s_in_n;
        float sq1 = s_in_sq;
        s_in_n = 0;
        s_in_sq = 0;
        s_in_db = n ? 10 * log10f(sq1 / n + 1e-12f) : -120;
        uac2_host_stats_t us;
        uac2_host_stats(&us);
        // (every 5 s as a warning until 2026-11-27: 720 lines an hour that buried the real warnings - now a warning
        // only when packets of other sizes came or the rate moved by more than 0.001, else once a minute)
        static uint32_t sz_was[4], rate_was;
        static int sz_windows;
        sz_windows++;
        bool odd_sizes = us.capture_sizes[3] != sz_was[3];
        uint32_t moved = us.rate_q16 > rate_was ? us.rate_q16 - rate_was : rate_was - us.rate_q16;
        bool rate_moved = rate_was && us.rate_q16 && moved > 66;   // (66 / 65536 = 0.001 frames per microframe)
        if (odd_sizes || rate_moved || sz_windows >= 12) {
            char line[160];
            snprintf(line, sizeof(line), "input: capture packets of 5 / 6 / 7 / other frames: %u / %u / %u / %u (last %d s), rate %.6f frames per microframe",
                     (unsigned)(us.capture_sizes[0] - sz_was[0]), (unsigned)(us.capture_sizes[1] - sz_was[1]), (unsigned)(us.capture_sizes[2] - sz_was[2]),
                     (unsigned)(us.capture_sizes[3] - sz_was[3]), sz_windows * 5, us.rate_q16 / 65536.0);
            if (odd_sizes || rate_moved) ESP_LOGW(TAG, "%s", line);
            else ESP_LOGI(TAG, "%s", line);
            for (int i = 0; i < 4; i++) sz_was[i] = us.capture_sizes[i];
            rate_was = us.rate_q16;
            sz_windows = 0;
        }
        static uint32_t rep_was;
        uint32_t repeats = us.capture_repeats - rep_was;
        if (repeats) ESP_LOGW(TAG, "input: %u capture frames came a second time (garbled stream)", (unsigned)repeats);
        rep_was = us.capture_repeats;
        // Self-healing (2026-11-27, the user: after about an hour the Nano's stream went garbled again - robotic, "ganz
        // schlimm" - and only switching both off and on helped). While something is played, a clean stream repeats no
        // frame; garbled it is dozens to hundreds. (Frames with all four channels below about -70 dBFS are not counted
        // since 2026-11-27, uac2_host.c: the DI's noise floor repeated by chance, up to ~30 in 5 s of silence.)
        // Two such windows in a row: the streams are started anew (alt 0, clock, alt 1 - a fraction of a second; the
        // Nano stays connected). At most once a minute, so a Nano that does not recover is not restarted for ever.
        {
            static int bad_windows, mild_windows;
            static int64_t last_restart_us;
            bool played = n && sq1 / n >= 1e-6f;   // (above -60 dBFS on 3/4)
            bad_windows = played && repeats >= GARBLE_REPEATS ? bad_windows + 1 : 0;
            mild_windows = played && repeats >= GARBLE_MILD ? mild_windows + 1 : 0;   // (a clean stream repeats none)
            int64_t now = esp_timer_get_time();
            // (a clearly garbled window - twice the mark - is enough on its own: the first heal came after ~20 s)
            if ((bad_windows >= 2 || mild_windows >= 3 || (played && repeats >= 2 * GARBLE_REPEATS)) && (!last_restart_us || now - last_restart_us > 60 * 1000000LL)) {
                bad_windows = mild_windows = 0;
                last_restart_us = now;
                s_heals++;
                ESP_LOGW(TAG, "input: the Nano's stream is garbled (%u repeated frames in 5 s) - starting the streams anew (%u. time)",
                         (unsigned)repeats, (unsigned)s_heals);
                uac2_host_restart();
            }
        }
        static uint32_t odd_was;
        if (us.capture_odd != odd_was) ESP_LOGW(TAG, "input: %u capture packets that were not whole frames", (unsigned)(us.capture_odd - odd_was));
        odd_was = us.capture_odd;
    }
}

// Console 'K': the next capture packets as they came (all channels, raw 32-bit slots) - to see what the Nano sends.
#define RAW_FRAMES 480
static int32_t *s_raw;   // (PSRAM, taken at the first 'K': 15 KB of internal RAM it held for nothing until 2026-11-27)
static int s_raw_counts[96];
static volatile int s_raw_want, s_raw_got, s_raw_packets, s_raw_ch;

void p4_audio_restart_streams(void) { uac2_host_restart(); }



void p4_audio_raw_dump(void)
{
    if (!s_raw) s_raw = heap_caps_malloc(sizeof(int32_t) * RAW_FRAMES * 8, MALLOC_CAP_SPIRAM);
    if (!s_raw) return;
    s_raw_got = s_raw_packets = 0;
    s_raw_want = 1;
    for (int i = 0; i < 100 && s_raw_want; i++) vTaskDelay(pdMS_TO_TICKS(10));
    printf("\nRAWCAP %d frames from %d packets, %d channels; frames per packet:", s_raw_got, s_raw_packets, s_raw_ch);
    for (int i = 0; i < s_raw_packets && i < 96; i++) printf(" %d", s_raw_counts[i]);
    printf("\n");
    for (int f = 0; f < s_raw_got; f++) {
        printf("RAW%03d", f);
        for (int c = 0; c < s_raw_ch && c < 4; c++) printf(" %08lx", (unsigned long)(uint32_t)s_raw[f * 8 + c]);
        printf("\n");
    }
    fflush(stdout);
}

static IRAM_ATTR void on_capture(const int32_t *frames, int count, int channels, void *arg)
{
#ifdef NANO_UITEST
    click_capture(frames, count, channels, atomic_load(&g_p4_mix.cw));
#endif
    if (s_raw_want) {
        if (s_raw_packets < 96) s_raw_counts[s_raw_packets] = count;
        s_raw_packets++;
        s_raw_ch = channels;
        for (int i = 0; i < count && s_raw_got < RAW_FRAMES; i++, s_raw_got++)
            for (int c = 0; c < channels && c < 8; c++) s_raw[s_raw_got * 8 + c] = frames[i * channels + c];
        if (s_raw_got >= RAW_FRAMES) s_raw_want = 0;
    }
    if (channels <= OUT_RIGHT) return;
    audio_mix_capture(&g_p4_mix, frames, count, channels, OUT_LEFT, OUT_RIGHT);
    // The DI's (1/2) and the sound's (3/4) squares and peaks: summed here, stored once per packet (2026-11-27: into
    // the volatiles per sample, that was a load and a store each). Compared, not fmaxf: a library call on this chip.
    float sq_di = 0, sq_out = 0, pk_di = 0, pk_out = 0;
    for (int i = 0; i < count; i++) {
        float di = (float)(frames[i * channels] >> 8) * s_k_full;
        float out = (float)(frames[i * channels + OUT_LEFT] >> 8) * s_k_full;
        sq_di += di * di;
        sq_out += out * out;
        di = fabsf(di), out = fabsf(out);
        if (di > pk_di) pk_di = di;
        if (out > pk_out) pk_out = out;
    }
    s_in_sq += sq_out;
    s_in_n += (uint32_t)count;
    s_gr_sq[0] += sq_di;
    s_gr_sq[1] += sq_out;
    if (pk_di > s_gr_pk[0]) s_gr_pk[0] = pk_di;
    if (pk_out > s_gr_pk[1]) s_gr_pk[1] = pk_out;
    if (pk_di > s_env_pk) s_env_pk = pk_di;   // (read and set back by p4_audio_di_peak)
    s_gr_n += (uint32_t)count;
#if P4_SPECTRUM_DI
    if (g_p4_di_on) {   // (a store per frame while the EQ editor's spectrum shows; nothing else)
        uint32_t w = atomic_load_explicit(&g_p4_di_w, memory_order_relaxed);
        for (int i = 0; i < count; i++) g_p4_di_ring[(w + (uint32_t)i) & (P4_DI_RING - 1)] = (int16_t)(frames[i * channels] >> 16);
        atomic_store_explicit(&g_p4_di_w, w + (uint32_t)count, memory_order_release);
    }
#endif
    for (int i = 0; i < count; i++) {
        float l = (float)(frames[i * channels + OUT_LEFT] >> 8) * s_k_full;
        float r = (float)(frames[i * channels + OUT_RIGHT] >> 8) * s_k_full;
        float al = fabsf(l), ar = fabsf(r);   // (the peak: of the samples, before the K weighting)
        if (al > s_peak20) s_peak20 = al;
        if (ar > s_peak20) s_peak20 = ar;
        l = run(&s_high[0], run(&s_shelf[0], l));
        r = run(&s_high[1], run(&s_shelf[1], r));
        s_block_sum += l * l + r * r;
        if (++s_block_frames == 960) {
            s_sum[s_block] = s_block_sum * s_k_block;
            s_block = (s_block + 1) % (int)(sizeof(s_sum) / sizeof(s_sum[0]));
            s_acc_e += s_block_sum * s_k_block;
            if (s_peak20 > s_acc_peak) s_acc_peak = s_peak20;
            s_peak20 = 0;
            if (++s_acc_k == 5) {   // 100 ms
                unsigned nb = atomic_load(&s_loud_n);
                s_loud[nb % LOUD_RING] = (loud_block_t){ s_acc_e / 5, s_acc_peak };
                atomic_store(&s_loud_n, nb + 1);
                s_acc_e = s_acc_peak = 0;
                s_acc_k = 0;
            }
            s_block_frames = 0;
            s_block_sum = 0;
        }
    }
    int64_t now = esp_timer_get_time();
    if (now - s_shown_us < SHOW_EVERY_US) return;
    s_shown_us = now;
    double ms = 0;
    for (size_t i = 0; i < sizeof(s_sum) / sizeof(s_sum[0]); i++) ms += s_sum[i];
    ms /= sizeof(s_sum) / sizeof(s_sum[0]);
    // Only noted here: this is the USB audio task, which must never wait for the screen (show_level draws it). The
    // logarithm is the screen's: log10 / lroundf / the double-to-float step run from flash, and code in flash on this
    // core makes the USB interrupt late (firmware/REALTIME.md rule 1; found 2026-11-28). Two slots and a counter: the
    // screen reads the slot written last, the next write goes to the other one 200 ms later.
    unsigned k = atomic_load(&s_ms_seq) + 1;
    s_ms_pub[k & 1] = ms;
    atomic_store(&s_ms_seq, k);
}

// The OUT field, from the screen's own task. Below -70 LUFS nothing is being played: a dash instead of the noise floor.
static void show_level(lv_timer_t *timer)
{
    static int shown = 1;
    unsigned k = atomic_load(&s_ms_seq);
    double ms = s_ms_pub[k & 1];
    int x10 = k && ms > 1e-12 ? (int)lroundf((float)(-0.691 + 10 * log10(ms)) * 10) : -900;
    if (x10 == shown) return;
    shown = x10;
    float lufs = x10 / 10.0f;
    ui_set_output_level(lufs > -70.0f, lufs, (lufs + 60.0f) / 60.0f);   // the bar: -60 .. 0 LUFS
}

// ---- the Bluetooth bridge (classic ESP32) ----

static void bridge_send(const char *cmd)
{
    uart_write_bytes(BRIDGE_UART, cmd, strlen(cmd));
    uart_write_bytes(BRIDGE_UART, "\n", 1);
}

// One line from the bridge: "<letter> <text>*<xor>". Lines with a wrong check sum are dropped.
static void bridge_line(char *l)
{
    char *star = strrchr(l, '*');
    if (!star || strlen(star) < 3) return;
    uint8_t x = 0;
    for (char *c = l; c < star; c++) x ^= (uint8_t)*c;
    if (x != (uint8_t)strtoul(star + 1, NULL, 16)) {
        ESP_LOGW(TAG, "bridge: bad line dropped");
        return;
    }
    *star = 0;
    const char *arg = l[1] == ' ' ? l + 2 : "";
    switch (l[0]) {
    case 'C':
        s_phone.connected = arg[0] == '1';
        if (!s_phone.connected) s_phone.playing = false, s_phone.title[0] = s_phone.artist[0] = 0, s_phone.len_s = s_phone.pos_s = 0;
        else if (strlen(arg) > 2) strlcpy(s_phone.device, arg + 2, sizeof(s_phone.device));
        break;
    case 'S': s_phone.playing = !strcmp(arg, "play"); break;
    case 'R': audio_pipe_request_rate(&s_pipe, (uint32_t)atoi(arg)); return;
    case 'T': strlcpy(s_phone.title, arg, sizeof(s_phone.title)); break;
    case 'A': strlcpy(s_phone.artist, arg, sizeof(s_phone.artist)); break;
    case 'L':   // the album after the artist: "Artist · Album"
        if (arg[0] && strlen(s_phone.artist) + strlen(arg) + 6 < sizeof(s_phone.artist)) {
            strlcat(s_phone.artist, s_phone.artist[0] ? "  \xC2\xB7  " : "", sizeof(s_phone.artist));
            strlcat(s_phone.artist, arg, sizeof(s_phone.artist));
        }
        break;
    case 'D': s_phone.len_s = atoi(arg) / 1000; break;
    case 'P': s_phone.pos_s = atoi(arg) / 1000; break;
    default: return;
    }
    ui_set_phone(&s_phone);
}

static void bridge_uart_task(void *arg)
{
    char l[200];
    int n = 0;
    while (true) {
        uint8_t c;
        if (uart_read_bytes(BRIDGE_UART, &c, 1, pdMS_TO_TICKS(1000)) != 1) continue;
        if (c == '\n') {
            l[n] = 0;
            if (n) bridge_line(l);
            n = 0;
        } else if (c != '\r' && n < (int)sizeof(l) - 1) {
            l[n++] = (char)c;
        }
    }
}

// The phone's audio: I2S frames (16-bit stereo) into the ring buffer.
static void bridge_i2s_task(void *arg)
{
    i2s_chan_handle_t rx = arg;
    static int16_t block[256 * 2];
    while (true) {
        size_t got = 0;
        if (i2s_channel_read(rx, block, sizeof(block), &got, pdMS_TO_TICKS(100)) == ESP_OK && got)
            audio_pipe_write(&s_pipe, block, got / 4);
    }
}

static void bridge_start(void)
{
    // Reset the ESP32 once, so it starts with the P4 (EN low for 50 ms; open drain, the module pulls it up)
    const gpio_config_t en = { .pin_bit_mask = 1ULL << BRIDGE_EN, .mode = GPIO_MODE_OUTPUT_OD };
    gpio_config(&en);
    gpio_set_level(BRIDGE_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BRIDGE_EN, 1);

    const uart_config_t uc = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                               .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install(BRIDGE_UART, 1024, 512, 0, NULL, 0) != ESP_OK) return;
    uart_param_config(BRIDGE_UART, &uc);
    uart_set_pin(BRIDGE_UART, BRIDGE_TX, BRIDGE_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    xTaskCreatePinnedToCore(bridge_uart_task, "bridge_uart", 3072, NULL, 5, NULL, 0);

    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(BRIDGE_I2S, I2S_ROLE_SLAVE);
    if (i2s_new_channel(&chan, NULL, &rx) != ESP_OK) {
        ESP_LOGE(TAG, "bridge: no I2S channel");
        return;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = BRIDGE_BCLK, .ws = BRIDGE_WS, .dout = I2S_GPIO_UNUSED, .din = BRIDGE_DATA },
    };
    if (i2s_channel_init_std_mode(rx, &std) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "bridge: I2S not started");
        return;
    }
    xTaskCreatePinnedToCore(bridge_i2s_task, "bridge_i2s", 3072, rx, 18, NULL, 0);
    ESP_LOGI(TAG, "Bluetooth bridge: I2S in on GPIO%d/%d/%d, line on GPIO%d/%d", BRIDGE_BCLK, BRIDGE_WS, BRIDGE_DATA,
             BRIDGE_TX, BRIDGE_RX);
}

// ---- the phone ----

static void net_task(void *arg);
static void scan_task(void *arg);

static void level_set(int v);
static void level_measure(bool on);
static void on_phone(ui_phone_action_t action, int value)
{
    switch (action) {
    case UI_PHONE_VOLUME:
        s_phone.volume_db = value / 10.0f;
        audio_pipe_set_gain_db(&s_pipe, s_phone.volume_db);
        break;
    case UI_PHONE_MUTE_TUNER:
        s_phone.mute_on_tuner = value != 0;
        break;
    case UI_PHONE_TOGGLE: bridge_send("TOGGLE"); return;
    case UI_PHONE_NEXT: bridge_send("NEXT"); return;
    case UI_PHONE_PREV: bridge_send("PREV"); return;
    case UI_JAM_PLAY: p4_jam_play(value); return;
    case UI_JAM_TOGGLE: p4_jam_toggle(); return;
    case UI_JAM_PREV: p4_jam_prev(); return;
    case UI_JAM_NEXT: p4_jam_next(); return;
    case UI_JAM_VOLUME: p4_jam_set_level_db(value / 10.0f); return;
    case UI_JAM_LEVEL: p4_jam_set_track_level(value / 10.0f); return;
    case UI_LOOP_BARS: p4_looper_set_bar_lines(value != 0, 4); return;
    case UI_NET_ON: xTaskCreatePinnedToCore(net_task, "net_on", 6144, value ? (void *)1 : NULL, 3, NULL, 1); return;
    case UI_DRUM_GROOVE: p4_drums_groove(value); return;
    case UI_DRUM_LEVEL: p4_drums_level_db(value / 10.0f); return;
    case UI_DRUM_LEVEL_KEEP: p4_drums_level_db(value / 10.0f); p4_drums_level_keep(); return;
    case UI_DRUM_SYNC: p4_drums_sync(value != 0); return;
    case UI_DRUM_COUNT: p4_drums_count_in(value != 0); return;
    case UI_DRUM_KIT: p4_drums_kit(value); return;
    case UI_PART_GROOVE: p4_part_groove(value >> 8, (value & 0xFF) - 1); return;
    case UI_LOOP_QUANTIZE: p4_looper_quantize(value != 0); return;
    case UI_LOOP_ARM: p4_looper_arm(value != 0); return;
    case UI_NET_SCAN: if (!p4_net_scanning()) xTaskCreatePinnedToCore(scan_task, "net_scan", 6144, NULL, 3, NULL, 1); return;
    case UI_BRIGHTNESS: board_set_brightness(value, false); return;
    case UI_LEVEL_SET: level_set(value); return;
    case UI_LEVEL_MEASURE: level_measure(value != 0); return;
    case UI_LOOP_SAVE: p4_loops_save(); return;
    case UI_BRIGHTNESS_KEEP: board_set_brightness(value, true); return;
    default: return;
    }
    ui_set_phone(&s_phone);
}

static void health_cb(void *arg);   // (below)
// ---- what held the sound up, and what else the board did at that time (measuring, 2026-11-26) ----
// Events with their time: 'C' / 'P' / 'R' from the USB audio (uac2_host_stall), 'W' / 'E' a write to / an erase of the
// flash memory (wrapped: main/CMakeLists.txt), 'D' the screen drew for longer than 4 ms. Printed with the "sound:" line.
typedef struct { uint32_t ms; uint32_t us; uint32_t extra; char kind; } trace_t;
#define TRACE_N 256
static trace_t s_trace[TRACE_N];
static atomic_uint s_trace_n;
static IRAM_ATTR void trace(char kind, uint32_t us, uint32_t extra)   // (from the USB path: internal RAM)
{
    unsigned i = atomic_fetch_add(&s_trace_n, 1) % TRACE_N;
    s_trace[i] = (trace_t){ (uint32_t)(esp_timer_get_time() / 1000), us, extra, kind };
}
IRAM_ATTR void uac2_host_stall(char kind, uint32_t us) { trace(kind, us, 0); }
static void mix_held(uint32_t us) { trace('M', us, 0); }
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_esp_flash_write(chip, buffer, address, length);
    trace('W', (uint32_t)(esp_timer_get_time() - t0), address);
    return r;
}
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);
esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_esp_flash_erase_region(chip, start, len);
    trace('E', (uint32_t)(esp_timer_get_time() - t0), start);
    return r;
}
// The screen's driver writes what was drawn out of the cache in ONE piece (esp_lcd_panel_dpi.c: every line of the
// drawn area, up to 1.2 MB) - and esp_cache_msync does that with interrupts off, holding a lock that the USB driver's
// own cache calls on the other core wait for: the Nano's audio stood still for up to 11 ms whenever much of the
// screen was drawn anew (measured 2026-11-26: capture gaps of 11 ms exactly at such draws, "short" in the loop, the
// Nano's stream garbled after it). ESP-IDF's own chunking (CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS) keeps a mutex
// over the whole call, which the USB task would wait for as long. So here: large write-backs in pieces of 8 KB, each
// its own short call. (Smaller pieces did not help the USB interrupt, 2026-11-27: under the screen's load every
// piece took 100-390 us, 2 KB or 8 - the interrupt now stays out of the cache: components/espressif__usb.)
#define MSYNC_PIECE 0x2000
esp_err_t __real_esp_cache_msync(void *addr, size_t size, int flags);
IRAM_ATTR esp_err_t __wrap_esp_cache_msync(void *addr, size_t size, int flags)
{
    if (!(flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M) || size <= MSYNC_PIECE || xPortInIsrContext() || !xPortCanYield())
        return __real_esp_cache_msync(addr, size, flags);
    for (size_t at = 0; at < size; at += MSYNC_PIECE) {   // (pieces of whole cache lines: an aligned call stays aligned)
        esp_err_t err = __real_esp_cache_msync((uint8_t *)addr + at, size - at < MSYNC_PIECE ? size - at : MSYNC_PIECE, flags);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// Settings written to the flash (2026-11-27, the skipped USB packets): esp_flash_write writes a source in internal
// RAM in one go per page - with the cache off on both cores, so the USB interrupt waits up to ~0.9 ms for a 288-byte
// blob (banks_save, scenes, A/B ...) and the playback / capture transfers miss their next microframes (the 'K' / 'Q'
// trace, "skipped" in the sound line). A source in PSRAM goes through 32-byte pieces instead, each a short pause.
// So every blob is copied to PSRAM first - one wrap for all the store's callers (main/CMakeLists.txt).
esp_err_t __real_nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t __wrap_nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    if (length <= 32 || esp_ptr_external_ram(value)) return __real_nvs_set_blob(handle, key, value, length);
    void *copy = heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
    if (!copy) return __real_nvs_set_blob(handle, key, value, length);
    memcpy(copy, value, length);
    esp_err_t err = __real_nvs_set_blob(handle, key, copy, length);
    free(copy);
    return err;
}

static int64_t s_draw_t0;
static void on_draw(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) { s_draw_t0 = esp_timer_get_time(); return; }
    uint32_t us = (uint32_t)(esp_timer_get_time() - s_draw_t0);
    if (us > 4000) trace('D', us, 0);
}
static void trace_print(void)
{
    static unsigned shown;
    unsigned n = atomic_load(&s_trace_n);
    if (n - shown > TRACE_N) shown = n - TRACE_N;
    char line[200];
    int at = 0;
    for (; shown != n; shown++) {
        trace_t t = s_trace[shown % TRACE_N];
        if (t.kind == 'W' || t.kind == 'E') at += snprintf(line + at, sizeof(line) - at, " %u:%c%u@%X", (unsigned)t.ms, t.kind, (unsigned)t.us, (unsigned)t.extra);
        else at += snprintf(line + at, sizeof(line) - at, " %u:%c%u", (unsigned)t.ms, t.kind, (unsigned)t.us);
        if (at > 160) { ESP_LOGW(TAG, "trace:%s", line); at = 0; }
    }
    if (at) ESP_LOGW(TAG, "trace:%s", line);
}
static void health_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        trace_print();
        unsigned missing = atomic_exchange(&s_lead_missing, 0);
        if (missing) ESP_LOGW(TAG, "sound: %u frames went out as silence (the mix was more than %d ms behind)", missing, LEAD_FRAMES / 48);
        health_cb(NULL);
    }
}
static uint32_t mix_clock(void) { return esp_cpu_get_cycle_count(); }   // 400 per microsecond (400 MHz)

void p4_audio_start(void)
{
    int16_t *ring = heap_caps_malloc(RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!ring) {
        ESP_LOGE(TAG, "no memory for the phone's buffer");
        return;
    }
    audio_pipe_init(&s_pipe, ring, RING_FRAMES, 44100, 48000);
    audio_pipe_set_gain_db(&s_pipe, s_phone.volume_db);
    audio_mix_init(&g_p4_mix, &s_pipe, &g_p4_track, NULL);   // the looper joins in p4_audio_show
    g_p4_mix.clock = mix_clock;   // (measuring: the parts of a render in the "sound:" line)
    p4_jam_start();
    p4_cover_start();   // (the playing track's cover for the player's stage)
    const uac2_host_config_t cfg = {
        .vid = 0x152A, .pid = 0x88E7, .playback_ep = 0x03, .capture_ep = 0x82,
        .render = on_render, .pipe = &s_pipe, .ch_left = 0, .ch_right = 1,   // which playback channels the Nano plays: to check
        .on_capture = on_capture, .task_priority = 20, .task_core = 0,
    };
    uac2_host_hold(true);   // (until the drum kit is made and the board is calm: drums_task)
    if (uac2_host_start(&cfg) != ESP_OK) ESP_LOGE(TAG, "USB audio client not started");
    xTaskCreatePinnedToCore(mix_task, "sound_mix", 6144, NULL, 19, NULL, 1);
    xTaskCreatePinnedToCore(input_task, "sound_input", 3072, NULL, 2, NULL, 1);
    // (a task of its own, low and on core 1: from the esp_timer task - priority 22 on core 0 - writing the line held
    // the USB audio back by about 1 ms each time)
    xTaskCreatePinnedToCore(health_task, "sound_health", 3072, NULL, 2, NULL, 1);
    bridge_start();
    ui_set_phone_handler(on_phone);
}

// Every 5 s, when something went wrong in the sound since the last look: one log line with what (each a count since
// the last line). In the normal firmware, so a click someone hears can be matched with the log.
//   short     = render frames without a capture frame (the looper recorded silence there: a click in the loop)
//   dropped   = capture frames thrown away;   limited = frames the limiter turned down (the sum above 0.98)
//   skipped   = playback packets not sent in time;   mirror = short / dropped packets of the packet-by-packet pacing
//   clipped   = loop samples that were clipped (both parts);   empty in = capture packets that came empty
//   render max / capture gap max = the longest render of 1 ms of sound / the longest wait between capture transfers
static void health_cb(void *arg)
{
    (void)arg;
    static uint32_t last[9];
    audio_mix_stats_t ms;
    audio_mix_get_stats(&g_p4_mix, &ms);
    uac2_host_stats_t us;
    uac2_host_stats(&us);
    uint32_t clipped = 0;
    if (s_looper_ready) {
        loop_parts_info_t pi;
        loop_parts_info(&s_parts, &pi);
        clipped = pi.part[0].clipped + pi.part[1].clipped;
    }
    uint32_t now[9] = { ms.capture_short, ms.capture_dropped, ms.limited, us.packets_skipped, us.errors,
                        us.mirror_short, us.mirror_dropped, clipped, us.capture_empty };
    uint32_t d[9];
#define DB(x) (20.0f * log10f(fmaxf((x), 1e-5f)))
    bool any = false;
    for (int i = 0; i < 9; i++) {
        d[i] = now[i] - last[i];
        any |= d[i] != 0;
        last[i] = now[i];
    }
    static int quiet;   // windows without a fault: the timings every 30 s only (a log line holds the console 10-30 ms)
    if (!us.streaming) return;
    bool fault = d[0] || d[1] || d[3] || d[4] || d[7];   // short, dropped, skipped, errors, clipped
    if (!fault && ++quiet < 6) return;
    quiet = 0;
    // The peaks are taken only for the line (2026-11-27: taken every 5 s, a line without a fault showed the last 5 s
    // of its 30 - a render spike or capture gap in the other 25 s was lost). Now they cover all since the last line.
    uint32_t render_max, gap_max, st[AUDIO_MIX_STAGES];
    uac2_host_take_peaks(&render_max, &gap_max);
    audio_mix_take_stages(&g_p4_mix, st);
    float lv[AUDIO_SRC_COUNT], lsum, lmin;
    audio_mix_take_levels(&g_p4_mix, lv, &lsum, &lmin);
    float lin = audio_mix_take_input_peak(&g_p4_mix);
    ESP_LOGW(TAG, "sound: short %u, dropped %u, limited %u, skipped %u, errors %u, mirror %u/%u, clipped %u, empty in %u, "
             "render max %u us, capture gap max %u us; parts of one render (max us): capture %u, phone %u, track %u, looper %u, drums %u, sum %u",
             (unsigned)d[0], (unsigned)d[1], (unsigned)d[2], (unsigned)d[3], (unsigned)d[4], (unsigned)d[5], (unsigned)d[6], (unsigned)d[7],
             (unsigned)d[8], (unsigned)render_max, (unsigned)gap_max, (unsigned)(st[0] / 400), (unsigned)(st[1] / 400), (unsigned)(st[2] / 400),
             (unsigned)(st[3] / 400), (unsigned)(st[4] / 400), (unsigned)(st[5] / 400));
    ESP_LOGW(TAG, "memory: internal %u bytes free (largest %u), internal DMA largest %u, PSRAM %u KB free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGW(TAG, "sound levels (peak dBFS): in %.1f, loop %.1f, drums %.1f, track %.1f, phone %.1f, sum %.1f; limiter down to %.1f dB",
             DB(lin), DB(lv[2]), DB(lv[3]), DB(lv[1]), DB(lv[0]), DB(lsum), DB(lmin));
#undef DB
}

// After ui_init: the phone's field shows "not connected" until the ESP32 reports a phone.
static void loop_source(ui_loop_t *l, ui_jam_t *j, ui_net_t *n);

void p4_audio_show(void)
{
    ui_set_phone(&s_phone);
    ui_set_output_level(false, 0, 0);
    ui_set_brightness(board_brightness());
    if (lvgl_port_lock(0)) {
        lv_timer_create(show_level, 100, NULL);
        lvgl_port_unlock();
    }
    p4_spectrum_start();   // the EQ editor's live spectrum (a task that sleeps until the editor asks; draft 2026-11-28)

    // the looper takes what PSRAM is left after the screen, keeping 6 MB for the rest (the drums take 2.3 MB of it),
    // at most 45 s in all: two parts of 22 s
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t keep = 6u << 20, per_frame = 8 * sizeof(int16_t);   // two parts, each two stereo buffers
    uint32_t frames = free_b > keep ? (uint32_t)((free_b - keep) / per_frame) : 0;
    if (frames > LOOPER_MAX_S * 48000 / 2) frames = LOOPER_MAX_S * 48000 / 2;
    int16_t *mem = frames >= 5 * 48000 ? heap_caps_malloc((size_t)frames * per_frame, MALLOC_CAP_SPIRAM) : NULL;
    if (!mem) {
        ESP_LOGE(TAG, "looper: no memory (%u KB PSRAM free)", (unsigned)(free_b / 1024));
        return;
    }
    int16_t *pre = heap_caps_malloc(sizeof(int16_t) * 2 * LOOP_PRE, MALLOC_CAP_SPIRAM);   // (starts a little in the past)
    loop_parts_init(&s_parts, mem, frames, 48000, pre);
    looper_set_latency(&s_parts.part[0], LOOPER_LATENCY);
    looper_set_latency(&s_parts.part[1], LOOPER_LATENCY);
    nvs_handle_t nvs;   // the bar lines are kept
    if (nvs_open("p4", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nvs, "loopbars", &v) == ESP_OK) s_bar_lines = v != 0;
        nvs_close(nvs);
    }
    audio_mix_set_parts(&g_p4_mix, &s_parts);
    s_looper_ready = true;
    ESP_LOGI(TAG, "looper: two parts of %u s (%u KB PSRAM), %u KB PSRAM left", (unsigned)(frames / 48000),
             (unsigned)(frames * per_frame / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ui_set_loop_source(loop_source, p4_jam_name, p4_jam_note);
    ui_set_cover_source(p4_cover_copy);
    lvgl_port_lock(0);
    lv_display_add_event_cb(lv_display_get_default(), on_draw, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), on_draw, LV_EVENT_RENDER_READY, NULL);
    lvgl_port_unlock();

    // the drum machine: the Duo DAW's computed kit (about 2.3 MB of sounds in PSRAM), made in the background
    xTaskCreatePinnedToCore(drums_task, "drums", 6144, NULL, 2, NULL, 1);
}

// ---- drums ----

void drum_machine_yield(void) { vTaskDelay(pdMS_TO_TICKS(20)); }   // (the kit takes about 6 s: the idle task's watchdog)

static void drums_task(void *arg)
{
    drums_load();
    int64_t t0 = esp_timer_get_time();
    if (drum_machine_init(&s_drums, 48000, psram_alloc)) {
        ESP_LOGI(TAG, "drums: computed kit made in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
        s_acoustic_kit = load_acoustic_kit(&s_drums);
        drum_machine_set_kit(&s_drums, kit_to_play(s_drum_kit));
        audio_mix_set_drums(&g_p4_mix, &s_drums);
        drum_machine_set_level_db(&s_drums, s_drum_level_db + DRUM_TRIM_DB);
        drum_machine_set_groove(&s_drums, s_drum_groove);
        loop_parts_set_drums(&s_parts, &s_drums);   // (the parts start and stop them in step with the loop)
        loop_parts_set_drum_sync(&s_parts, s_drum_sync);
        loop_parts_set_count_in(&s_parts, s_count_in);
        loop_parts_set_quantize(&s_parts, s_quantize);
        loop_parts_set_arm(&s_parts, s_arm);
        parts_groove_load();
        parts_groove_apply();
        s_drums_ready = true;
        if (s_drum_kit == DRUM_KIT_LOFI) make_lofi();   // (after ready: a choice made meanwhile starts its own task)
        ui_splash_step(UI_SPLASH_DRUMS);
        ESP_LOGI(TAG, "drums: %d grooves, kit made in %d ms, %u KB PSRAM left", DRUM_GROOVE_COUNT,
                 (int)((esp_timer_get_time() - t0) / 1000), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        ESP_LOGE(TAG, "drums: no memory for the kit");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "the board is calm: the Nano's audio may start");
    ui_splash_step(UI_SPLASH_SOUND);
    uac2_host_hold(false);
    vTaskDelete(NULL);
}

bool p4_drums_ready(void) { return s_drums_ready; }
void p4_drums_start(bool on)
{
    if (!s_drums_ready) return;
    float bpm = p4_nano_tempo();
    if (bpm >= 30 && bpm <= 300) drum_machine_set_bpm(&s_drums, bpm);
    loop_parts_drums(&s_parts, on);   // (in step with the loop that plays)
    ESP_LOGI(TAG, "drums: %s at %.1f BPM", on ? "start" : "stop", bpm);
}
void p4_drums_groove(int groove)
{
    if (!s_drums_ready || groove < 0 || groove >= DRUM_GROOVE_COUNT) return;
    drum_machine_set_groove(&s_drums, groove);
    s_drum_groove = groove;
    drums_store();
    parts_groove_apply();
    ESP_LOGI(TAG, "drums: groove %d \"%s\" (%s)", groove, DRUM_GROOVES[groove].name, DRUM_GROOVES[groove].style);
}
// A looper part's own groove (-1 = the common one; part 2 = both back to the common one). The part that plays now
// changes at the next bar line, the other one when it takes over.
void p4_part_groove(int part, int groove)
{
    if (!s_drums_ready || groove >= DRUM_GROOVE_COUNT) return;
    if (part == 2) s_part_groove[0] = s_part_groove[1] = -1;
    else if (part == 0 || part == 1) s_part_groove[part] = groove < 0 ? -1 : groove;
    else return;
    parts_groove_store();
    parts_groove_apply();
    loop_parts_info_t pi;
    loop_parts_info(&s_parts, &pi);
    int now = part == 2 ? s_drum_groove : s_part_groove[pi.active] >= 0 ? s_part_groove[pi.active] : s_drum_groove;
    drum_machine_set_groove(&s_drums, now);
    ESP_LOGI(TAG, "drums: part %c groove %d, part %c plays %d", part == 1 ? 'B' : 'A', groove, 'A' + pi.active, now);
}
void p4_drums_kit(int kit)
{
    if (kit < 0 || kit >= DRUM_KITS) return;
    s_drum_kit = kit;
    if (s_drums_ready) {
        drum_machine_set_kit(&s_drums, kit_to_play(kit));
        if (kit == DRUM_KIT_LOFI && !atomic_load(&s_lofi_asked))   // (first chosen: made beside the sound, below the mix)
            xTaskCreatePinnedToCore(lofi_task, "drums_lofi", 4096, NULL, 2, NULL, 1);
    }
    drums_store();
    static const char *const NAME[DRUM_KITS] = { "computed", "acoustic", "Lo-Fi" };
    ESP_LOGI(TAG, "drums: %s kit", NAME[kit]);
}
void p4_drums_level_db(float db)
{
    s_drum_level_db = db < -40 ? -40 : db > 6 ? 6 : db;
    if (s_drums_ready) drum_machine_set_level_db(&s_drums, s_drum_level_db + DRUM_TRIM_DB);
}
void p4_drums_level_keep(void) { drums_store(); }
void p4_drums_sync(bool on)
{
    s_drum_sync = on;
    loop_parts_set_drum_sync(&s_parts, on);
    drums_store();
}
void p4_looper_quantize(bool on)
{
    s_quantize = on;
    loop_parts_set_quantize(&s_parts, on);
    drums_store();
}
void p4_looper_arm(bool on)
{
    s_arm = on;
    loop_parts_set_arm(&s_parts, on);
    drums_store();
}
void p4_drums_count_in(bool on)
{
    s_count_in = on;
    loop_parts_set_count_in(&s_parts, on);
    drums_store();
}
bool p4_drums_on(void)
{
    loop_parts_info_t pi;
    loop_parts_info(&s_parts, &pi);
    return pi.drums_on;
}
void p4_parts_choose(int part) { if (s_looper_ready) loop_parts_choose(&s_parts, part); ESP_LOGI(TAG, "looper: part %c", part ? 'B' : 'A'); }
void p4_parts_pause(void) { if (s_looper_ready) loop_parts_pause(&s_parts); ESP_LOGI(TAG, "looper: pause / go on"); }
void p4_drums_info(drum_machine_info_t *out)
{
    if (s_drums_ready) drum_machine_info(&s_drums, out);
    else memset(out, 0, sizeof(*out));
}

// ---- looper ----


bool p4_looper_ready(void) { return s_looper_ready; }

// The loop's bar in frames from the tempo as the drums keep it (1/100 BPM, drum_machine_set_bpm); 0 = no usable tempo.
// (2026-11-27: from the Nano's raw float, a tapped 116.996 made the bar 2.5 frames longer than the drums' - in a
// 4-bar loop they reached their bar line ~10 frames before the wrap, struck the downbeat, and the resync at the wrap
// struck it again. Computed this way and cut, the bar is never longer than the drums'.)
static uint32_t bar_frames(float bpm)
{
    if (!(bpm >= 30 && bpm <= 300)) return 0;
    long x100 = lrintf(bpm * 100);
    return (uint32_t)(48000.0 * 60.0 * 100.0 / x100 * s_beats);
}

void p4_looper_command(looper_cmd_t cmd)
{
    if (!s_looper_ready) return;
    uint32_t bar = bar_frames(p4_nano_tempo());
    loop_parts_set_bar(&s_parts, bar, s_bar_lines || (s_quantize && p4_drums_on()) ? bar : 0);   // (with the drums: whole bars)
    taskENTER_CRITICAL(&s_looper_lock);   // (the looper's queue takes one writer at a time)
    loop_parts_command(&s_parts, cmd);   // (to the part that plays)
    taskEXIT_CRITICAL(&s_looper_lock);
    ESP_LOGI(TAG, "looper: command %d", (int)cmd);
}

// What the screen shows of the looper and the jam tracks (asked 10 times a second from the screen's task).
// ---- the Level sheet's numbers (the screen's task): the meter, and a measurement of 4 s of played sound ----
#define LV_MEASURE 80            // 100 ms blocks above -50 LUFS that make a measurement (8 s; 4 s until 2026-11-27)
static struct { int8_t kind, profile; int16_t base_x10; } s_lvl = { 0, 0, -200 };   // NVS "p4" / "lvl"
static bool s_lvl_loaded;
static unsigned s_lv_since;
static float s_lv_e[30], s_lv_p[30];   // the last 3 s
static int s_lv_k;
static struct { bool running; int n; float e[LV_MEASURE], peak; unsigned seq; bool ok; float lufs, peak_db; } s_lvm;

static float lv_lufs(float e) { return e > 1e-12f ? -0.691f + 10 * log10f(e) : -100; }

static void level_set(int v)
{
    s_lvl.kind = v & 0xF, s_lvl.profile = (v >> 4) & 0xF, s_lvl.base_x10 = (int16_t)((v >> 8) - 1000);
    if (s_lvl.kind > 4 || s_lvl.profile > 1 || s_lvl.base_x10 < -400 || s_lvl.base_x10 > -80) s_lvl.kind = 0, s_lvl.profile = 0, s_lvl.base_x10 = -200;
    nvs_handle_t h;
    if (nvs_open("p4", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "lvl", &s_lvl, sizeof(s_lvl));
    nvs_commit(h);
    nvs_close(h);
}

static void level_measure(bool on)
{
    s_lvm.running = on;
    s_lvm.n = 0;
    s_lvm.peak = 0;
}

// Gated as BS.1770: 400 ms windows of the played blocks, the absolute gate -70, the relative -10.
static void level_finish(void)
{
    s_lvm.running = false;
    s_lvm.seq++;
    float win[LV_MEASURE];
    int nw = 0, k = 0;
    for (int i = 3; i < s_lvm.n; i++) win[nw++] = (s_lvm.e[i] + s_lvm.e[i - 1] + s_lvm.e[i - 2] + s_lvm.e[i - 3]) / 4;
    double sum = 0;
    for (int i = 0; i < nw; i++) if (lv_lufs(win[i]) > -70) sum += win[i], k++;
    s_lvm.ok = k > 0;
    if (!k) return;
    float gate = lv_lufs((float)(sum / k)) - 10;
    sum = 0, k = 0;
    for (int i = 0; i < nw; i++) if (lv_lufs(win[i]) > gate) sum += win[i], k++;
    s_lvm.lufs = lv_lufs((float)(sum / k));
    s_lvm.peak_db = 20 * log10f(fmaxf(s_lvm.peak, 1e-6f));
    ESP_LOGI(TAG, "level: measured %.1f LUFS, peaks %.1f dBFS (%d blocks)", s_lvm.lufs, s_lvm.peak_db, s_lvm.n);
}

static void level_fill(ui_loop_t *l)
{
    if (!s_lvl_loaded) {
        s_lvl_loaded = true;
        nvs_handle_t h;
        if (nvs_open("p4", NVS_READONLY, &h) == ESP_OK) {
            size_t sz = sizeof(s_lvl);
            if (nvs_get_blob(h, "lvl", &s_lvl, &sz) != ESP_OK || sz != sizeof(s_lvl) || s_lvl.kind > 4 || s_lvl.profile > 1)
                s_lvl.kind = 0, s_lvl.profile = 0, s_lvl.base_x10 = -200;
            nvs_close(h);
        }
    }
    unsigned n = atomic_load(&s_loud_n);
    if (n - s_lv_since > LOUD_RING - 2) s_lv_since = n - (LOUD_RING - 2);   // (the oldest may be written just now)
    for (; s_lv_since != n; s_lv_since++) {
        loud_block_t b = s_loud[s_lv_since % LOUD_RING];
        s_lv_e[s_lv_k % 30] = b.e;
        s_lv_p[s_lv_k % 30] = b.peak;
        s_lv_k++;
        if (s_lvm.running && lv_lufs(b.e) > -50 && s_lvm.n < LV_MEASURE) {
            s_lvm.e[s_lvm.n++] = b.e;
            if (b.peak > s_lvm.peak) s_lvm.peak = b.peak;
        }
    }
    if (s_lvm.running && s_lvm.n >= LV_MEASURE) level_finish();
    int have = s_lv_k < 30 ? s_lv_k : 30;
    float es = 0, em = 0, pk = 0;
    for (int i = 0; i < have; i++) {
        es += s_lv_e[i];
        if (s_lv_p[i] > pk) pk = s_lv_p[i];
    }
    for (int i = 0; i < 4 && i < have; i++) em += s_lv_e[(s_lv_k - 1 - i) % 30];
    l->lv_short = have ? lv_lufs(es / have) : -100;
    l->lv_mom = have ? lv_lufs(em / (have < 4 ? have : 4)) : -100;
    l->lv_peak_db = pk < 3e-4f ? -100 : 20 * log10f(pk);   // (below -70 dBFS: nothing played)
    l->lv_kind = s_lvl.kind, l->lv_profile = s_lvl.profile, l->lv_base_x10 = s_lvl.base_x10;
    l->lv_measuring = s_lvm.running ? s_lvm.n : -1;
    l->lv_result_seq = s_lvm.seq, l->lv_result_ok = s_lvm.ok, l->lv_result_lufs = s_lvm.lufs, l->lv_result_peak_db = s_lvm.peak_db;
}

static void loop_source(ui_loop_t *l, ui_jam_t *j, ui_net_t *n)
{
    level_fill(l);
    {   // the compressor meter's levels since the last look (dBFS; -100 = nothing came)
        uint32_t cnt = s_gr_n;
        for (int pr = 0; pr < 2; pr++) {
            float ms = cnt ? s_gr_sq[pr] / cnt : 0, pk = s_gr_pk[pr];
            l->gr_rms_db[pr] = ms > 1e-10f ? 10 * log10f(ms) : -100;
            l->gr_peak_in_db[pr] = pk > 1e-5f ? 20 * log10f(pk) : -100;
            s_gr_sq[pr] = 0;
            s_gr_pk[pr] = 0;
        }
        l->gr_levels = cnt > 0;
        s_gr_n = 0;
    }
    looper_info_t li;
    p4_looper_info(&li);
    l->ready = s_looper_ready;
    l->state = li.state;   // (UI_LOOP_* follow LOOPER_*)
    l->pos_s = li.pos / 48000.0f;
    l->len_s = li.len / 48000.0f;
    l->max_s = li.max_len / 48000.0f;
    l->to_bar_line = li.to_bar_line;
    l->can_undo = li.can_undo;
    l->can_redo = li.can_redo;
    l->bar_lines = s_bar_lines;
    l->passes = (int)li.passes;
    float bpm = p4_nano_tempo();
    l->bpm = bpm >= 30 && bpm <= 300 ? bpm : 0;
    uint32_t bar = bar_frames(bpm);
    if (s_looper_ready && bar) {   // (the drums' bar line follows the Nano's tempo)
        loop_parts_info_t qi;
        loop_parts_info(&s_parts, &qi);
        loop_parts_set_bar(&s_parts, bar, s_bar_lines || (s_quantize && qi.drums_on) ? bar : 0);
    }
    if (s_looper_ready) {
        loop_parts_info_t pi;
        loop_parts_info(&s_parts, &pi);
        l->part = pi.active;
        l->next_part = pi.next;
        l->paused = pi.paused;
        l->drums_on = pi.drums_on;
        l->counting = pi.counting;
        l->armed = pi.armed;
        for (int k = 0; k < 2; k++) {
            l->part_len_s[k] = pi.part[k].len / 48000.0f;
            l->part_state[k] = pi.part[k].state;
        }
    }
    l->save_state = p4_loops_state(&l->save_pct);   // (UI_SAVE_* follow P4_SAVE_*)
    l->drums_ready = s_drums_ready;
    l->drum_sync = s_drum_sync;
    l->count_in = s_count_in;
    l->quantize = s_quantize;
    l->arm = s_arm;
    l->drum_kit = s_drum_kit;
    l->acoustic_kit = s_acoustic_kit;
    l->part_groove[0] = s_part_groove[0], l->part_groove[1] = s_part_groove[1];
    if (s_drums_ready) {
        drum_machine_info_t di;
        drum_machine_info(&s_drums, &di);
        l->groove = di.groove;
        l->drum_level_db = s_drum_level_db;
    }
    if (s_drums_ready && l->bpm > 0) drum_machine_set_bpm(&s_drums, bpm);   // (the drums follow the Nano's tempo)
    if (s_bar_lines && bar && li.len) {
        l->bars = (int)lroundf(li.len / (float)bar);
        if (fabsf(l->bars * (float)bar - li.len) > 480) l->bars = 0;   // the loop was made at another tempo / free: no bar fields
    }
    track_player_info_t ti;
    int cur;
    p4_jam_info(&cur, &ti);
    j->card = p4_jam_card();
    j->unreadable = p4_jam_card_unreadable();
    j->count = p4_jam_count();
    j->current = cur;
    j->playing = ti.playing;
    j->pos_s = ti.rate ? (int)(ti.pos / ti.rate) : 0;
    j->len_s = ti.rate ? (int)(ti.total / ti.rate) : 0;
    j->volume_db = p4_jam_level_db();
    j->level_db = cur >= 0 ? p4_jam_level(cur) : 0;
    j->measuring = p4_jam_measuring();
    j->target_lufs = LOUD_TARGET_DB;
    j->cover_seq = p4_cover_seq();
    p4_net_status_t ns;
    p4_net_status(&ns);
    n->state = ns.state;   // (UI_NET_* follow P4_NET_*)
    strlcpy(n->ssid, ns.ssid, sizeof(n->ssid));
    strlcpy(n->addr, ns.addr, sizeof(n->addr));
    strlcpy(n->ap_pass, ns.ap_pass, sizeof(n->ap_pass));
    strlcpy(n->receiving, ns.receiving, sizeof(n->receiving));
    n->received = ns.received;
    n->percent = ns.percent;
    n->scanning = p4_net_scanning();
    p4_net_ap_t found[P4_NET_SCAN_MAX];
    int count = p4_net_scan_results(found, P4_NET_SCAN_MAX);
    n->scan_count = count < 16 ? count : 16;
    for (int i = 0; i < n->scan_count; i++) {
        strlcpy(n->scan[i].ssid, found[i].ssid, sizeof(n->scan[i].ssid));
        n->scan[i].rssi = found[i].rssi;
        n->scan[i].secure = found[i].secure;
    }
}

// Looking for the networks around takes about 2 s: not on the screen's task.
static void scan_task(void *arg)
{
    p4_net_scan();
    vTaskDelete(NULL);
}

// Wi-Fi on / off takes a moment (the C6 answers over SDIO): not on the screen's task.
static void net_task(void *arg)
{
    p4_net_enable(arg != NULL);
    vTaskDelete(NULL);
}

void p4_looper_info(looper_info_t *out)
{
    if (s_looper_ready) looper_get_info(&s_looper, out);
    else memset(out, 0, sizeof(*out));
}

// Console 'Y': the loop that plays, as text for the computer (its left channel, int16 as 4 hex digits, 48 a line):
// its first quarter second and its last second - to look for gaps, jumps and repeats (2026-11-19). Takes ~30 s.
void p4_looper_dump(void)
{
    if (!s_looper_ready) return;
    looper_t *lp = &s_looper;
    looper_info_t li;
    looper_get_info(lp, &li);
    uint32_t len = li.len;
    printf("\nLOOPDUMP part %c state %d len %u latency %u cushion %d\n", (atomic_load(&s_parts.i_active) & 1) ? 'B' : 'A',
           (int)li.state, (unsigned)len, (unsigned)lp->latency, MIX_CUSHION);
    if (!len) return;
    uint32_t ranges[2][2] = { { 0, len < 12000 ? len : 12000 }, { len > 48000 ? len - 48000 : 0, len } };
    for (int r = 0; r < 2; r++) {
        printf("LOOPDUMP range %u %u\n", (unsigned)ranges[r][0], (unsigned)ranges[r][1]);
        for (uint32_t i = ranges[r][0]; i < ranges[r][1];) {
            char line[48 * 4 + 16];
            int n = snprintf(line, sizeof(line), "D%u:", (unsigned)i);
            for (int k = 0; k < 48 && i < ranges[r][1]; k++, i++) n += snprintf(line + n, sizeof(line) - n, "%04x", (uint16_t)lp->base[2 * i]);
            puts(line);
        }
    }
    printf("LOOPDUMP end\n");
    fflush(stdout);
}

void p4_looper_set_bar_lines(bool on, int beats)
{
    s_bar_lines = on;
    s_beats = beats >= 1 && beats <= 16 ? beats : 4;
    nvs_handle_t nvs;
    if (nvs_open("p4", NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u8(nvs, "loopbars", on) == ESP_OK) nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "looper: bar lines %s", on ? "on" : "off");
}

void p4_looper_set_level_db(float db) { audio_mix_set_gain_db(&g_p4_mix, AUDIO_SRC_LOOPER, db); }

// ---- for saving the loops (p4_loops.c) ----
loop_parts_t *p4_loop_parts(void) { return s_looper_ready ? &s_parts : NULL; }
uint32_t p4_looper_bar_now(void) { return bar_frames(p4_nano_tempo()); }
int p4_looper_beats(void) { return s_beats; }
void p4_audio_marks(p4_sound_marks_t *out, bool restart_max)
{
    audio_mix_stats_t ms;
    audio_mix_get_stats(&g_p4_mix, &ms);
    uac2_host_stats_t us;
    uac2_host_stats(&us);
    out->late_frames = atomic_load(&s_lead_missing_all);
    out->skipped = us.packets_skipped;
    out->short_frames = ms.capture_short;
    out->mix_max_us = restart_max ? atomic_exchange(&s_mix_max_us, 0) : atomic_load(&s_mix_max_us);
}
void p4_looper_set_latency(uint32_t frames) { if (s_looper_ready) looper_set_latency(&s_looper, frames); }

// ---- console test lines (main.c ':c', ':q'; 2026-11-27, for ~/Coding/claude-werkzeuge/p4-loop-test.py) ----

void p4_audio_click_train(int db)
{
    atomic_store_explicit(&s_click_db, 0, memory_order_release);
    if (db <= 0) {
        ESP_LOGI(TAG, "test click off");
        return;
    }
    float amp = powf(10.0f, -db / 20.0f) * 8388607.0f;
    for (int k = 0; k < CLICK_TAB; k++)
        s_click_tab[k] = (int32_t)((uint32_t)(int32_t)lrintf(amp * sinf(2 * (float)M_PI * k / CLICK_TAB)) << 8);
    atomic_store_explicit(&s_click_db, db, memory_order_release);
    ESP_LOGI(TAG, "test click: 10 ms of 1 kHz every 500 ms at -%d dBFS on playback 3/4", db);
}

static const char *state_name(int st)
{
    static const char *const STATES[] = { "empty", "recording", "playing", "overdub", "stopped" };
    return STATES[st >= 0 && st <= LOOPER_STOP ? st : 0];
}

// Where sounds begin in a part's buffer (a frame above -46 dBFS after 50 ms below it) - read while the audio runs,
// good enough to look at: the first 8, how many, and the spacing of those after the first (the first may be a sound
// the loop's start cut).
static void report_onsets(const char *what, const int16_t *buf, uint32_t len)
{
    char text[100];   // (8 numbers of at most 11 characters)
    int n = 0, found = 0, peak = 0;
    uint32_t quiet = 2400, last = 0, gap_min = UINT32_MAX, gap_max = 0;
    for (uint32_t i = 0; i < len; i++) {
        int a = abs(buf[2 * i]), b = abs(buf[2 * i + 1]), v = a > b ? a : b;
        if (v > peak) peak = v;
        if (v <= 164) {
            quiet++;
            continue;
        }
        if (quiet >= 2400) {
            if (found < 8) n += snprintf(text + n, sizeof(text) - (size_t)n, " %u", (unsigned)i);
            if (found >= 2) {
                if (i - last < gap_min) gap_min = i - last;
                if (i - last > gap_max) gap_max = i - last;
            }
            last = i;
            found++;
        }
        quiet = 0;
    }
    ESP_LOGI(TAG, "REPORT %s: peak %.1f dBFS, %d sounds, beginning at frame%s; spacing after the first %u-%u frames", what,
             20 * log10f(fmaxf(peak / 32768.0f, 1e-6f)), found, found ? text : " - (none)", found > 2 ? (unsigned)gap_min : 0, (unsigned)gap_max);
}

// Console ':q' (and the screen test's "@m"): what the looper, its parts, the settings and the streams are at. (main.c
// runs it in a task of its own with 4 KB of stack, not on the console's 3 KB: long log lines with floats.)
void p4_audio_test_report(bool scan)
{
    audio_mix_stats_t ms;
    audio_mix_get_stats(&g_p4_mix, &ms);
    looper_info_t li;
    p4_looper_info(&li);
    ESP_LOGI(TAG, "REPORT mixer: %u render frames without a capture frame, %u capture frames dropped, %u frames limited",
             (unsigned)ms.capture_short, (unsigned)ms.capture_dropped, (unsigned)ms.limited);
    ESP_LOGI(TAG, "REPORT looper: %s, length %u frames (%.2f s), position %.2f s, %u passes, %u clipped, undo %d, redo %d, to the bar line %d, latency set %u frames",
             state_name(li.state), (unsigned)li.len, li.len / 48000.0f, li.pos / 48000.0f, (unsigned)li.passes,
             (unsigned)li.clipped, li.can_undo, li.can_redo, li.to_bar_line, s_looper_ready ? (unsigned)atomic_load(&s_looper.set_latency) : 0);
    uac2_host_stats_t us;
    uac2_host_stats(&us);
    ESP_LOGI(TAG, "REPORT streams: %u capture frames waiting in the mixer (cushion %d, settled %u times, last by %ld), %u packets skipped, %u errors, packet by packet %s (%u short, %u dropped)",
             (unsigned)(atomic_load(&g_p4_mix.cw) - atomic_load(&g_p4_mix.cr)), MIX_CUSHION, (unsigned)g_p4_mix.settle_count, (long)g_p4_mix.settled, (unsigned)us.packets_skipped,
             (unsigned)us.errors, us.mirroring ? "on" : "off", (unsigned)us.mirror_short, (unsigned)us.mirror_dropped);
    if (s_looper_ready) {
        loop_parts_info_t pi;
        loop_parts_info(&s_parts, &pi);
        ESP_LOGI(TAG, "REPORT parts: active %c, next %c, paused %d, drums %d, counting %d, armed %d; A %s %u frames, B %s %u frames",
                 'A' + pi.active, pi.next < 0 ? '-' : 'A' + pi.next, pi.paused, pi.drums_on, pi.counting, pi.armed,
                 state_name(pi.part[0].state), (unsigned)pi.part[0].len, state_name(pi.part[1].state), (unsigned)pi.part[1].len);
    }
    float bpm = p4_nano_tempo();
    ESP_LOGI(TAG, "REPORT settings: drum sync %d, count-in %d, quantize %d, arm %d, bar lines %d, beats %d, tempo %.3f BPM, bar %u frames, drums ready %d, test click -%d dBFS (0 = off)",
             s_drum_sync, s_count_in, s_quantize, s_arm, s_bar_lines, s_beats, bpm, (unsigned)bar_frames(bpm), s_drums_ready, atomic_load(&s_click_db));
    ESP_LOGI(TAG, "REPORT health: %u self-heals, %u repeated capture frames, capture packets of 5 / 6 / 7 / other frames %u / %u / %u / %u, %u not whole frames, rate %.6f; "
             "sound 3/4 %.1f dB in the last 5 s (played: above -60); lowest free internal %u bytes, PSRAM %u KB",
             s_heals, (unsigned)us.capture_repeats, (unsigned)us.capture_sizes[0], (unsigned)us.capture_sizes[1], (unsigned)us.capture_sizes[2],
             (unsigned)us.capture_sizes[3], (unsigned)us.capture_odd, us.rate_q16 / 65536.0, s_in_db,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
    // The onset scan reads the whole loop and layer from PSRAM (up to 2 x 4 MB): only when asked (":s"), not at every
    // poll - polled every 0.2-0.4 s it competed with the mixer and the screen for PSRAM (run 1, 2026-11-27).
    if (scan && s_looper_ready && li.len) {
        report_onsets("loop", s_looper.base, li.len);
        report_onsets("overdub", s_looper.layer, li.len);
    }
}
