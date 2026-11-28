// P4 board: the live spectrum for the EQ editor (draft 2026-11-28, drafts/eq-spectrum-2026-11-28) - see REALTIME.md.
//
// Nothing of it is on the audio path. The Nano's sound (capture 3/4) is already copied, as floats, into the mixer's
// capture ring by the USB task (audio_mix_capture, internal RAM); this only READS that ring, from a task of its own:
//   - core 1, priority 1 (below the screen's 4, the mix's 19 and the USB tasks on core 0),
//   - every 20 ms it copies the frames that came since (at most ~1000, ~10 us), a 4096-point FFT every 66 ms
//     (components/nano_audio/spectrum.c), the 120 bands smoothed and handed to the screen through two buffers,
//   - only while the screen asks (ui_set_spectrum_source: the EQ editor shows a spectrum); 0.4 s after the last
//     question it naps again: one vTaskDelay(100 ms) after another, nothing else.
// It only waits with vTaskDelay - the call the mixer's task makes too, whose parts under the kernel lock are in
// internal RAM (linker.lf) - no notifications, no queues, no mutex; the task is made once at the start (xTaskCreate
// takes the kernel lock in flash code: never while the sound runs).
// Memory: its stack, the FFT's buffers and the two hand-over buffers in PSRAM (~70 KB); in internal RAM only the
// task's control block (FreeRTOS reads its list entries on both cores, so it must be there) and a few words.
// With P4_SPECTRUM_DI (off: see the plan) the DI (capture 1) comes as a second curve: that needs a ring the USB task
// writes (p4_audio.c, 4 KB of internal RAM) and doubles the FFT work.
#include "p4_spectrum.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4_audio.h"
#include "spectrum.h"

static const char *TAG = "spectrum";

#define TICK_MS 20               // a look at the capture ring (it holds MIX_CAP_RING frames: 42 ms)
#define HOP_FRAMES 3200          // a spectrum every 66.7 ms
#define IDLE_MS 100              // asleep: how often it looks whether it is wanted
#define WANT_MS 400              // ... it runs this long after the screen last asked
#define QUIET_MS 300             // no new frames this long: the Nano's sound does not come (not over USB)
#define RELEASE_DB_S 30.0f       // the shown levels fall at most this fast (the editor: 1.4 dB per 45 ms)
#define STACK_BYTES 3072
#define KEEP_FRAMES (MIX_CAP_RING - 256)   // never read further back than this: the USB task writes there next

static spectrum_t s_out;
#if P4_SPECTRUM_DI
static spectrum_t s_di;
#endif
static ui_spectrum_t *s_pub[2];  // PSRAM: written by the task in turn, the screen copies the newer
static struct {                  // PSRAM: the levels measured and shown
    float now[SPECTRUM_BANDS], shown[SPECTRUM_BANDS];
#if P4_SPECTRUM_DI
    float di_now[SPECTRUM_BANDS], di_shown[SPECTRUM_BANDS], di_buf[256];
#endif
} *s_lv;
static atomic_int s_pub_i;
static atomic_uint s_want_until; // tick until which the screen wants it
static StaticTask_t s_tcb;       // (internal RAM)
static uint32_t s_runs, s_windows, s_cycles_max;
static uint64_t s_cycles;

static void *psram(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void publish(const float *out_db, const float *di_db, bool live)
{
    static uint32_t seq;
    int i = atomic_load_explicit(&s_pub_i, memory_order_relaxed) ^ 1;
    ui_spectrum_t *p = s_pub[i];
    memcpy(p->out_db, out_db, sizeof(p->out_db));
    p->has_di = di_db != NULL;
    if (di_db) memcpy(p->di_db, di_db, sizeof(p->di_db));
    p->live = live;
    p->seq = ++seq;
    atomic_store_explicit(&s_pub_i, i, memory_order_release);
}

static void spectrum_task(void *arg)
{
    (void)arg;
    float *now = s_lv->now, *shown = s_lv->shown;
#if P4_SPECTRUM_DI
    float *di_now = s_lv->di_now, *di_shown = s_lv->di_shown, *di_buf = s_lv->di_buf;
    uint32_t di_read = 0;
#endif
    bool running = false, live = false;
    uint32_t read = 0, since = 0, quiet_ms = 0;
    for (;;) {
        uint32_t t = (uint32_t)xTaskGetTickCount();
        bool wanted = (int32_t)(atomic_load_explicit(&s_want_until, memory_order_acquire) - t) > 0;
        if (!wanted) {
            if (running) {
                running = false;
#if P4_SPECTRUM_DI
                g_p4_di_on = false;
#endif
                ESP_LOGI(TAG, "stopped: %u windows, %.2f ms each (longest %.2f ms) at %d MHz", (unsigned)s_windows,
                         s_windows ? (double)s_cycles / s_windows / (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000.0) : 0.0,
                         (double)s_cycles_max / (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000.0), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
            }
            vTaskDelay(pdMS_TO_TICKS(IDLE_MS));
            continue;
        }
        if (!running) {   // (again) from the start: a whole new window before the first spectrum
            running = true;
            s_runs++;
            s_windows = 0;
            s_cycles = s_cycles_max = 0;
            spectrum_reset(&s_out);
            read = atomic_load_explicit(&g_p4_mix.cw, memory_order_acquire);
            since = quiet_ms = 0;
            live = false;
            for (int i = 0; i < SPECTRUM_BANDS; i++) shown[i] = SPECTRUM_FLOOR_DB;
#if P4_SPECTRUM_DI
            spectrum_reset(&s_di);
            for (int i = 0; i < SPECTRUM_BANDS; i++) di_shown[i] = SPECTRUM_FLOOR_DB;
            di_read = atomic_load_explicit(&g_p4_di_w, memory_order_acquire);
            g_p4_di_on = true;
#endif
            publish(shown, NULL, false);
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        // the frames that came since the last look - only read; the USB task writes on at cw
        uint32_t w = atomic_load_explicit(&g_p4_mix.cw, memory_order_acquire);
        if (w - read > KEEP_FRAMES) {   // held up for longer than the ring: a gap, start the window again
            read = w - KEEP_FRAMES / 2;
            spectrum_reset(&s_out);
        }
        if (w != read) {
            spectrum_push_lr(&s_out, g_p4_mix.cap, MIX_CAP_RING - 1, read, w);
            uint32_t after = atomic_load_explicit(&g_p4_mix.cw, memory_order_acquire);
            if (after - read >= MIX_CAP_RING) spectrum_reset(&s_out);   // overwritten while it was copied (held up)
            since += w - read;
            read = w;
            quiet_ms = 0;
            live = true;
        } else if ((quiet_ms += TICK_MS) >= QUIET_MS && live) {
            live = false;   // nothing comes: the Nano is not on USB (or its streams stand)
            for (int i = 0; i < SPECTRUM_BANDS; i++) shown[i] = SPECTRUM_FLOOR_DB;
            publish(shown, NULL, false);
        }
#if P4_SPECTRUM_DI
        uint32_t dw = atomic_load_explicit(&g_p4_di_w, memory_order_acquire);
        if (dw - di_read > P4_DI_RING - 256) { di_read = dw - (P4_DI_RING - 256) / 2; spectrum_reset(&s_di); }
        while (di_read != dw) {
            uint32_t n = dw - di_read > 256 ? 256 : dw - di_read;
            for (uint32_t k = 0; k < n; k++) di_buf[k] = g_p4_di_ring[(di_read + k) & (P4_DI_RING - 1)] * (1.0f / 32768.0f);
            spectrum_push(&s_di, di_buf, n);
            di_read += n;
        }
#endif
        if (since < HOP_FRAMES || !spectrum_ready(&s_out)) continue;
        float dt = since / SPECTRUM_RATE;
        since = 0;
        uint32_t c0 = esp_cpu_get_cycle_count();
        spectrum_compute(&s_out, now);
        spectrum_smooth(shown, now, SPECTRUM_BANDS, dt, RELEASE_DB_S);
        const float *di = NULL;
#if P4_SPECTRUM_DI
        if (spectrum_ready(&s_di)) {
            spectrum_compute(&s_di, di_now);
            spectrum_smooth(di_shown, di_now, SPECTRUM_BANDS, dt, RELEASE_DB_S);
            di = di_shown;
        }
#endif
        uint32_t took = esp_cpu_get_cycle_count() - c0;   // (with what the screen took in between: an upper bound)
        s_cycles += took;
        if (took > s_cycles_max) s_cycles_max = took;
        s_windows++;
        publish(shown, di, live);
    }
}

void p4_spectrum_start(void)
{
    s_pub[0] = psram(sizeof(ui_spectrum_t));
    s_pub[1] = psram(sizeof(ui_spectrum_t));
    StackType_t *stack = psram(STACK_BYTES);
    s_lv = psram(sizeof(*s_lv));
    bool ok = s_pub[0] && s_pub[1] && stack && s_lv && spectrum_init(&s_out, psram);
#if P4_SPECTRUM_DI
    ok = ok && spectrum_init(&s_di, psram);
#endif
    if (!ok) {
        ESP_LOGE(TAG, "no memory - the EQ editor shows no spectrum");
        return;
    }
    memset(s_pub[0], 0, sizeof(ui_spectrum_t));
    memset(s_pub[1], 0, sizeof(ui_spectrum_t));
    if (!xTaskCreateStaticPinnedToCore(spectrum_task, "spectrum", STACK_BYTES, NULL, 1, stack, &s_tcb, 1)) {
        ESP_LOGE(TAG, "no task");
        return;
    }
    ui_set_spectrum_source(p4_spectrum_take);
}

bool p4_spectrum_take(bool want, ui_spectrum_t *out)
{
    uint32_t t = (uint32_t)xTaskGetTickCount();
    atomic_store_explicit(&s_want_until, want ? t + pdMS_TO_TICKS(WANT_MS) : t, memory_order_release);
    if (!out || !s_pub[0]) return false;
    // (the screen's task is above the spectrum's on the same core: the buffer cannot change while it is copied)
    memcpy(out, s_pub[atomic_load_explicit(&s_pub_i, memory_order_acquire)], sizeof(*out));
    return out->seq != 0;
}
