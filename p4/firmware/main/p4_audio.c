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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "nvs.h"
#include "audio_pipe.h"
#include "board.h"
#include "p4_audio.h"
#include "p4_net.h"
#include "loudness.h"
#include "uac2_host.h"
#include "ui.h"

static const char *TAG = "p4_audio";
static atomic_int s_lufs_x10 = -900;   // the Nano's output, tenths of a LUFS: written by the audio task, shown by the screen's

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
#define LOOPER_LATENCY (578 - 192 + MIX_CUSHION + MIX_OUT_DELAY)   // (578 was measured with a cushion of 192)
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
    nvs_close(nvs);
}
static void *psram_alloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
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
static double s_sum[WINDOW_FRAMES / 960];   // mean squares per 20 ms block, for a sliding 400 ms window
static int s_block, s_block_frames;
static float s_block_sum;   // (float: the P4 has no double unit - a double here cost a library call per frame)
static int64_t s_shown_us;

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

void p4_audio_test_report(void)
{
    audio_mix_stats_t ms;
    audio_mix_get_stats(&g_p4_mix, &ms);
    looper_info_t li;
    p4_looper_info(&li);
    static const char *const STATES[] = { "empty", "recording", "playing", "overdub", "stopped" };
    ESP_LOGI(TAG, "REPORT mixer: %u render frames without a capture frame, %u capture frames dropped, %u frames limited",
             (unsigned)ms.capture_short, (unsigned)ms.capture_dropped, (unsigned)ms.limited);
    ESP_LOGI(TAG, "REPORT looper: %s, length %u frames (%.2f s), position %.2f s, %u passes, %u clipped, undo %d, redo %d, to the bar line %d, latency set %u frames",
             STATES[li.state <= LOOPER_STOP ? li.state : 0], (unsigned)li.len, li.len / 48000.0f, li.pos / 48000.0f, (unsigned)li.passes,
             (unsigned)li.clipped, li.can_undo, li.can_redo, li.to_bar_line, (unsigned)atomic_load(&s_looper.set_latency));
    uac2_host_stats_t us;
    uac2_host_stats(&us);
    ESP_LOGI(TAG, "REPORT streams: %u capture frames waiting in the mixer (cushion %d, settled %u times, last by %ld), %u packets skipped, %u errors, packet by packet %s (%u short, %u dropped)",
             (unsigned)(atomic_load(&g_p4_mix.cw) - atomic_load(&g_p4_mix.cr)), MIX_CUSHION, (unsigned)g_p4_mix.settle_count, (long)g_p4_mix.settled, (unsigned)us.packets_skipped,
             (unsigned)us.errors, us.mirroring ? "on" : "off", (unsigned)us.mirror_short, (unsigned)us.mirror_dropped);
    // Where sounds begin in the loop and in the last overdub (a frame above -46 dBFS after 50 ms below it) - read
    // while the audio runs, good enough to look at; up to 8 each.
    for (int which = 0; which < 2 && s_looper_ready && li.len; which++) {
        const int16_t *buf = which ? s_looper.layer : s_looper.base;
        char text[160];
        int n = 0, found = 0;
        uint32_t quiet = 2400;
        int peak = 0;
        for (uint32_t i = 0; i < li.len; i++) {
            int v = abs(buf[2 * i]) > abs(buf[2 * i + 1]) ? abs(buf[2 * i]) : abs(buf[2 * i + 1]);
            if (v > peak) peak = v;
            if (v > 164) {
                if (quiet >= 2400 && found < 8) n += snprintf(text + n, sizeof(text) - (size_t)n, " %u", (unsigned)i), found++;
                quiet = 0;
            } else {
                quiet++;
            }
        }
        ESP_LOGI(TAG, "REPORT %s: peak %.1f dBFS, sounds begin at frame%s", which ? "overdub" : "loop",
                 20 * log10f(fmaxf(peak / 32768.0f, 1e-6f)), found ? text : " - (none)");
    }
}
#endif

static IRAM_ATTR void on_render(int32_t *frames, int count, int channels, void *arg)   // (internal RAM: see components/nano_audio/linker.lf)
{
    audio_mix_render(&g_p4_mix, frames, count, channels, 0, 1);   // playback channels 1/2: to check on the hardware
#ifdef NANO_UITEST
    click_render(frames, count, channels);
#endif
}

static float s_in_prev[2][2], s_in_sq[2], s_in_d2[2];
static uint32_t s_in_n;

// Every 30 s while something is played: the level and the brightness (rms of the second difference / rms) of the
// Nano's capture pair 1/2 (DI) and 3/4 (what the looper records) - to see whether 3/4 carries the cab (2026-11-19).
static void input_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t n = s_in_n;
        float sq0 = s_in_sq[0], sq1 = s_in_sq[1], d0 = s_in_d2[0], d1 = s_in_d2[1];
        s_in_n = 0;
        s_in_sq[0] = s_in_sq[1] = s_in_d2[0] = s_in_d2[1] = 0;
        uac2_host_stats_t us;
        uac2_host_stats(&us);
        static uint32_t sz_was[4];
        ESP_LOGW(TAG, "input: capture packets of 5 / 6 / 7 / other frames: %u / %u / %u / %u (last 5 s), rate %.6f frames per microframe",
                 (unsigned)(us.capture_sizes[0] - sz_was[0]), (unsigned)(us.capture_sizes[1] - sz_was[1]), (unsigned)(us.capture_sizes[2] - sz_was[2]),
                 (unsigned)(us.capture_sizes[3] - sz_was[3]), us.rate_q16 / 65536.0);
        for (int i = 0; i < 4; i++) sz_was[i] = us.capture_sizes[i];
        static uint32_t rep_was;
        if (us.capture_repeats != rep_was) ESP_LOGW(TAG, "input: %u capture frames came a second time (garbled stream)", (unsigned)(us.capture_repeats - rep_was));
        rep_was = us.capture_repeats;
        static uint32_t odd_was;
        if (us.capture_odd != odd_was) ESP_LOGW(TAG, "input: %u capture packets that were not whole frames", (unsigned)(us.capture_odd - odd_was));
        odd_was = us.capture_odd;
        if (!n || sq1 / n < 1e-6f) continue;   // (below -60 dBFS: nothing played)
        ESP_LOGW(TAG, "input: DI 1/2 %.1f dB (brightness %.3f), sound 3/4 %.1f dB (brightness %.3f)",
                 10 * log10f(sq0 / n + 1e-12f), sqrtf(d0 / (sq0 + 1e-12f)), 10 * log10f(sq1 / n + 1e-12f), sqrtf(d1 / (sq1 + 1e-12f)));
    }
}

// Console 'K': the next capture packets as they came (all channels, raw 32-bit slots) - to see what the Nano sends.
#define RAW_FRAMES 480
static int32_t s_raw[RAW_FRAMES * 8];
static int s_raw_counts[96];
static volatile int s_raw_want, s_raw_got, s_raw_packets, s_raw_ch;

void p4_audio_restart_streams(void) { uac2_host_restart(); }



void p4_audio_raw_dump(void)
{
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
    // (measuring: how bright the looper's input is - its level and its second difference, per pair 1/2 and 3/4)
    for (int i = 0; i < count; i++) {
        for (int pr = 0; pr < 2; pr++) {
            float v = (float)(frames[i * channels + (pr ? OUT_LEFT : 0)] >> 8) / 8388608.0f;
            float d2 = v - 2 * s_in_prev[pr][0] + s_in_prev[pr][1];
            s_in_prev[pr][1] = s_in_prev[pr][0];
            s_in_prev[pr][0] = v;
            s_in_sq[pr] += v * v;
            s_in_d2[pr] += d2 * d2;
        }
        s_in_n++;
    }
    for (int i = 0; i < count; i++) {
        float l = (float)(frames[i * channels + OUT_LEFT] >> 8) / 8388608.0f;
        float r = (float)(frames[i * channels + OUT_RIGHT] >> 8) / 8388608.0f;
        l = run(&s_high[0], run(&s_shelf[0], l));
        r = run(&s_high[1], run(&s_shelf[1], r));
        s_block_sum += l * l + r * r;
        if (++s_block_frames == 960) {
            s_sum[s_block] = s_block_sum / 960.0f;
            s_block = (s_block + 1) % (int)(sizeof(s_sum) / sizeof(s_sum[0]));
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
    // Only noted here: this is the USB audio task, which must never wait for the screen (show_level draws it).
    atomic_store(&s_lufs_x10, ms > 1e-12 ? (int)lroundf((float)(-0.691 + 10 * log10(ms)) * 10) : -900);
}

// The OUT field, from the screen's own task. Below -70 LUFS nothing is being played: a dash instead of the noise floor.
static void show_level(lv_timer_t *timer)
{
    static int shown = 1;
    int x10 = atomic_load(&s_lufs_x10);
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
    case UI_LOOP_QUANTIZE: p4_looper_quantize(value != 0); return;
    case UI_LOOP_ARM: p4_looper_arm(value != 0); return;
    case UI_NET_SCAN: if (!p4_net_scanning()) xTaskCreatePinnedToCore(scan_task, "net_scan", 6144, NULL, 3, NULL, 1); return;
    case UI_BRIGHTNESS: board_set_brightness(value, false); return;
    case UI_BRIGHTNESS_KEEP: board_set_brightness(value, true); return;
    default: return;
    }
    ui_set_phone(&s_phone);
}

static void health_cb(void *arg);   // (below)
static void health_task(void *arg) { for (;;) { vTaskDelay(pdMS_TO_TICKS(5000)); health_cb(NULL); } }
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
    const uac2_host_config_t cfg = {
        .vid = 0x152A, .pid = 0x88E7, .playback_ep = 0x03, .capture_ep = 0x82,
        .render = on_render, .pipe = &s_pipe, .ch_left = 0, .ch_right = 1,   // which playback channels the Nano plays: to check
        .on_capture = on_capture, .task_priority = 20, .task_core = 0,
    };
    uac2_host_hold(true);   // (until the drum kit is made and the board is calm: drums_task)
    if (uac2_host_start(&cfg) != ESP_OK) ESP_LOGE(TAG, "USB audio client not started");
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
    uint32_t render_max, gap_max, st[AUDIO_MIX_STAGES];
    uac2_host_take_peaks(&render_max, &gap_max);
    audio_mix_take_stages(&g_p4_mix, st);
    float lv[AUDIO_SRC_COUNT], lsum, lmin;
    audio_mix_take_levels(&g_p4_mix, lv, &lsum, &lmin);
    float lin = audio_mix_take_input_peak(&g_p4_mix);
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
        audio_mix_set_drums(&g_p4_mix, &s_drums);
        drum_machine_set_level_db(&s_drums, s_drum_level_db + DRUM_TRIM_DB);
        drum_machine_set_groove(&s_drums, s_drum_groove);
        loop_parts_set_drums(&s_parts, &s_drums);   // (the parts start and stop them in step with the loop)
        loop_parts_set_drum_sync(&s_parts, s_drum_sync);
        loop_parts_set_count_in(&s_parts, s_count_in);
        loop_parts_set_quantize(&s_parts, s_quantize);
        loop_parts_set_arm(&s_parts, s_arm);
        s_drums_ready = true;
        ESP_LOGI(TAG, "drums: %d grooves, kit made in %d ms, %u KB PSRAM left", DRUM_GROOVE_COUNT,
                 (int)((esp_timer_get_time() - t0) / 1000), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        ESP_LOGE(TAG, "drums: no memory for the kit");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "the board is calm: the Nano's audio may start");
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
    ESP_LOGI(TAG, "drums: groove %d \"%s\" (%s)", groove, DRUM_GROOVES[groove].name, DRUM_GROOVES[groove].style);
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

void p4_looper_command(looper_cmd_t cmd)
{
    if (!s_looper_ready) return;
    float bpm = p4_nano_tempo();
    uint32_t bar = bpm >= 30 && bpm <= 300 ? (uint32_t)(48000.0f * 60.0f / bpm * s_beats) : 0;
    loop_parts_set_bar(&s_parts, bar, s_bar_lines || (s_quantize && p4_drums_on()) ? bar : 0);   // (with the drums: whole bars)
    taskENTER_CRITICAL(&s_looper_lock);   // (the looper's queue takes one writer at a time)
    loop_parts_command(&s_parts, cmd);   // (to the part that plays)
    taskEXIT_CRITICAL(&s_looper_lock);
    ESP_LOGI(TAG, "looper: command %d", (int)cmd);
}

// What the screen shows of the looper and the jam tracks (asked 10 times a second from the screen's task).
static void loop_source(ui_loop_t *l, ui_jam_t *j, ui_net_t *n)
{
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
    if (s_looper_ready && l->bpm > 0) {   // (the drums' bar line follows the Nano's tempo)
        uint32_t bar = (uint32_t)(48000.0f * 60.0f / bpm * s_beats);
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
    l->drums_ready = s_drums_ready;
    l->drum_sync = s_drum_sync;
    l->count_in = s_count_in;
    l->quantize = s_quantize;
    l->arm = s_arm;
    if (s_drums_ready) {
        drum_machine_info_t di;
        drum_machine_info(&s_drums, &di);
        l->groove = di.groove;
        l->drum_level_db = s_drum_level_db;
    }
    if (s_drums_ready && l->bpm > 0) drum_machine_set_bpm(&s_drums, bpm);   // (the drums follow the Nano's tempo)
    if (s_bar_lines && l->bpm > 0 && li.len) {
        float bar = 48000.0f * 60.0f / l->bpm * s_beats;
        l->bars = (int)lroundf(li.len / bar);
        if (fabsf(l->bars * bar - li.len) > 480) l->bars = 0;   // the loop was made at another tempo / free: no bar fields
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
void p4_looper_set_latency(uint32_t frames) { if (s_looper_ready) looper_set_latency(&s_looper, frames); }
