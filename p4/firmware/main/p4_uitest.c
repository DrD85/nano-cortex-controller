// P4 board, development only (-DNANO_UITEST=1): the screen driven and looked at from a computer, without a finger
// and without eyes on the board. One console line each, starting with '@' (main.c hands the rest of the line over):
//
//   @t X Y [MS]            a tap at X, Y (pressed for MS milliseconds, 90 if not given)
//   @d X1 Y1 X2 Y2 MS      a finger dragged from X1, Y1 to X2, Y2 in MS milliseconds (a long press: both points equal)
//   @s [N]                 a picture of what the panel shows, every Nth pixel (1, 2 or 4; 2 if not given)
//   @k                     memory and every task's stack reserve
//   @g [MS]                how long a state request waits after the one before (1600; 0 = not at all)
//   @a                     the USB audio stream's counters
//   @r                     as when the Nano has stopped answering (nano_link_restart)
//   @u [MS]                the USB-A port without power for MS milliseconds (1500): the Nano "unplugged" and back
//   @c PAIR [DB]           a 2 ms click into playback pair PAIR (1 = channels 1/2 ... 4 = 7/8) at DB dBFS (-26):
//                          on which capture channels it comes back, how loud, how many frames later (AUDIBLE)
//   @l N                   a looper command (1 rec / dub, 2 play / stop, 3 undo, 4 redo, 5 clear)
//   @y FRAMES              the looper's round-trip latency in frames
//   @v DB                  the looper's volume into the Nano
//   @m                     the mixer's and the looper's counters
//   @p                     the screen's drawing times since the last @p (each refresh: start to ready), then reset
//   @b 1 / @b 0            drums start / stop (AUDIBLE);  @b g N  groove N;  @b v DB  their level
//   @w 0|1                 Wi-Fi for jam tracks off / on (as its switch in the jam player)
//
// The taps go in where the touch chip's reading enters LVGL, so everything behind it is the real thing; the touch
// chip itself is not part of the test. The picture is the frame buffer the panel is showing, sent as lines
// "SNAP <hex>": per row runs of [count][colour high][colour low] (RGB565), between "SNAPBEGIN w h" and "SNAPEND".
// claude-werkzeuge/p4-ui.py on the Mac sends the lines and writes the picture as a PNG.
//
// Never part of a firmware for playing: a stray '@' line could press things.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nano_link.h"
#include "p4_audio.h"
#include "p4_net.h"
#include "src/display/lv_display_private.h"
#include "uac2_host.h"
#include "usb/usb_host.h"

static const char *TAG = "uitest";

static lv_indev_read_cb_t s_real_read;
static int64_t s_from_us, s_until_us;
static int s_x1, s_y1, s_x2, s_y2;

static void test_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_real_read) s_real_read(indev, data);
    int64_t now = esp_timer_get_time();
    if (now < s_from_us || now >= s_until_us) return;
    float k = (float)(now - s_from_us) / (float)(s_until_us - s_from_us);
    data->point.x = s_x1 + (int)((s_x2 - s_x1) * k);
    data->point.y = s_y1 + (int)((s_y2 - s_y1) * k);
    data->state = LV_INDEV_STATE_PRESSED;
}

static void press(int x1, int y1, int x2, int y2, int ms)
{
    if (ms < 40) ms = 40;
    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev && !s_real_read) {
        s_real_read = lv_indev_get_read_cb(indev);
        lv_indev_set_read_cb(indev, test_read);
    }
    s_x1 = x1, s_y1 = y1, s_x2 = x2, s_y2 = y2;
    s_from_us = esp_timer_get_time();
    s_until_us = s_from_us + (int64_t)ms * 1000;
    lvgl_port_unlock();
    if (!indev) ESP_LOGE(TAG, "no touch input registered - nothing pressed");
    else ESP_LOGI(TAG, "press %d, %d -> %d, %d for %d ms", x1, y1, x2, y2, ms);
}

static void snap_task(void *arg)
{
    int step = (int)(intptr_t)arg;
    lvgl_port_lock(0);
    lv_display_t *d = lv_display_get_default();
    // With two buffers LVGL has already turned to the one it draws into next: the panel shows the other one.
    lv_draw_buf_t *shown = d->buf_2 ? (d->buf_act == d->buf_1 ? d->buf_2 : d->buf_1) : d->buf_act;
    int w = lv_display_get_horizontal_resolution(d), h = lv_display_get_vertical_resolution(d);
    uint16_t *copy = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (copy && shown) {
        for (int y = 0; y < h; y++) memcpy(copy + (size_t)y * w, shown->data + (size_t)y * shown->header.stride, (size_t)w * 2);
    }
    lvgl_port_unlock();
    char *line = malloc(5 + 6 * (size_t)(w / step) + 2);
    if (!copy || !shown || !line) {
        ESP_LOGE(TAG, "no memory for the picture");
    } else {
        static const char HEX[] = "0123456789ABCDEF";
        int fd = fileno(stdout);
        fflush(stdout);
        char head[40];
        int n = snprintf(head, sizeof(head), "\nSNAPBEGIN %d %d\n", w / step, h / step);
        write(fd, head, n);
        for (int y = 0; y + step <= h; y += step) {
            const uint16_t *row = copy + (size_t)y * w;
            char *p = line;
            memcpy(p, "SNAP ", 5), p += 5;
            int x = 0;
            while (x + step <= w) {
                uint16_t colour = row[x];
                int count = 1;
                x += step;
                while (x + step <= w && row[x] == colour && count < 255) count++, x += step;
                const uint8_t b[3] = { (uint8_t)count, (uint8_t)(colour >> 8), (uint8_t)colour };
                for (int i = 0; i < 3; i++) *p++ = HEX[b[i] >> 4], *p++ = HEX[b[i] & 15];
            }
            *p++ = '\n';
            write(fd, line, p - line);   // one call per row: no other task's log line gets in between
        }
        write(fd, "SNAPEND\n", 8);
    }
    free(line);
    free(copy);
    vTaskDelete(NULL);
}

static void report(void)
{
    static const char *const tasks[] = { "app", "console", "footswitch", "taskLVGL", "esp_timer", "nano_usb", "usb_lib", "uac2",
                                         "bridge_uart", "bridge_i2s", "jam", "jam_measure", "sys_evt", "ipc0", "ipc1", "Tmr Svc",
                                         "sdio_read", "rpc_tx", "rpc_rx", "tiT", "wifi", "httpd" };
    char text[420];
    int n = 0;
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
        TaskHandle_t task = xTaskGetHandle(tasks[i]);
        if (task) n += snprintf(text + n, sizeof(text) - (size_t)n, " %s %u", tasks[i], (unsigned)uxTaskGetStackHighWaterMark(task));
    }
    ESP_LOGI(TAG, "REPORT stack reserves (bytes):%s", text);
    ESP_LOGI(TAG, "REPORT memory: internal %u KB free (lowest %u, largest block %u), PSRAM %u KB free (lowest %u)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void audio_report(void)
{
    uac2_host_stats_t s;
    uac2_host_stats(&s);
    ESP_LOGI(TAG, "REPORT audio: %s, %s, %u Hz, %u frames sent, %u packets skipped, %u errors, %.4f frames per microframe, "
             "%u capture packets", s.connected ? "Nano on the port" : "no Nano", s.streaming ? "streaming" : "not streaming",
             (unsigned)s.sample_rate, (unsigned)s.frames_sent, (unsigned)s.packets_skipped, (unsigned)s.errors,
             s.rate_q16 / 65536.0, (unsigned)s.capture_packets);
}

extern void p4_audio_click(int pair, float db);
extern void p4_audio_test_report(bool scan);

static void click_task(void *arg)
{
    int v = (int)(intptr_t)arg;   // pair in the low byte, -dB above
    p4_audio_click(v & 0xFF, -(float)(v >> 8));
    vTaskDelete(NULL);
}

// ---- drawing times ----
// Per refresh: the whole of it (REFR_START..REFR_READY), the drawing (RENDER_START..RENDER_READY) and the flush
// (FLUSH_START..FLUSH_FINISH, the hand-over to the panel incl. waiting for its frame).
enum { P_REFR, P_RENDER, P_FLUSH, P_N };
static int64_t s_p_from[P_N], s_p_sum[P_N], s_p_max[P_N];
static uint32_t s_p_n[P_N], s_refr_slow;

static void on_refr(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    int k;
    bool start;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START: k = P_REFR, start = true; break;
    case LV_EVENT_REFR_READY: k = P_REFR, start = false; break;
    case LV_EVENT_RENDER_START: k = P_RENDER, start = true; break;
    case LV_EVENT_RENDER_READY: k = P_RENDER, start = false; break;
    case LV_EVENT_FLUSH_START: k = P_FLUSH, start = true; break;
    case LV_EVENT_FLUSH_FINISH: k = P_FLUSH, start = false; break;
    default: return;
    }
    if (start) {
        s_p_from[k] = now;
        return;
    }
    if (!s_p_from[k]) return;
    int64_t d = now - s_p_from[k];
    s_p_from[k] = 0;
    s_p_n[k]++;
    s_p_sum[k] += d;
    if (d > s_p_max[k]) s_p_max[k] = d;
    if (k == P_REFR && d > 33000) s_refr_slow++;
}

static void perf_report(void)
{
    static bool hooked;
    static const lv_event_code_t CODES[] = { LV_EVENT_REFR_START, LV_EVENT_REFR_READY, LV_EVENT_RENDER_START, LV_EVENT_RENDER_READY,
                                             LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH };
    lvgl_port_lock(0);
    if (!hooked) {
        lv_display_t *d = lv_display_get_default();
        for (size_t i = 0; i < sizeof(CODES) / sizeof(CODES[0]); i++) lv_display_add_event_cb(d, on_refr, CODES[i], NULL);
        hooked = true;
    }
    char text[200];
    int n = 0;
    static const char *const NAMES[P_N] = { "refresh", "drawing", "flush" };
    for (int k = 0; k < P_N; k++) {
        n += snprintf(text + n, sizeof(text) - (size_t)n, "%s %u x %.1f ms (longest %.1f), ", NAMES[k], (unsigned)s_p_n[k],
                      s_p_n[k] ? s_p_sum[k] / 1000.0 / s_p_n[k] : 0.0, s_p_max[k] / 1000.0);
        s_p_n[k] = 0, s_p_sum[k] = s_p_max[k] = 0;
    }
    uint32_t slow = s_refr_slow;
    s_refr_slow = 0;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "REPORT drawing: %s%u refreshes longer than 33 ms", text, (unsigned)slow);
}

static void net_task(void *arg)
{
    p4_net_enable(arg != NULL);
    p4_net_status_t st;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        p4_net_status(&st);
        if (st.state != P4_NET_STARTING && st.state != P4_NET_JOINING) break;
    }
    p4_net_status(&st);
    ESP_LOGI(TAG, "REPORT wifi: state %d, network \"%s\", address %s", (int)st.state, st.ssid, st.addr);
    vTaskDelete(NULL);
}

static void port_task(void *arg)
{
    int ms = (int)(intptr_t)arg;
    ESP_LOGW(TAG, "USB-A port off for %d ms", ms);
    esp_err_t off = usb_host_lib_set_root_port_power(false);
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_err_t on = usb_host_lib_set_root_port_power(true);
    ESP_LOGW(TAG, "USB-A port on again (off: %s, on: %s)", esp_err_to_name(off), esp_err_to_name(on));
    vTaskDelete(NULL);
}

void p4_uitest_line(const char *line)
{
    int a = 0, b = 0, c = 0, d = 0, ms = 0;
    switch (line[0]) {
    case 't':
        ms = 90;
        if (sscanf(line + 1, "%d %d %d", &a, &b, &ms) >= 2) press(a, b, a, b, ms);
        break;
    case 'd':
        if (sscanf(line + 1, "%d %d %d %d %d", &a, &b, &c, &d, &ms) == 5) press(a, b, c, d, ms);
        break;
    case 's':
        a = 2;
        sscanf(line + 1, "%d", &a);
        if (a != 1 && a != 4) a = 2;
        xTaskCreate(snap_task, "snap", 4096, (void *)(intptr_t)a, 3, NULL);
        break;
    case 'k':
        report();
        break;
    case 'g': {
        extern void nano_link_usb_test_gap(int ms);
        a = 1600;
        sscanf(line + 1, "%d", &a);
        nano_link_usb_test_gap(a);
        break;
    }
    case 'a':
        audio_report();
        break;
    case 'r':   // as when the Nano has stopped answering
        nano_link_restart();
        break;
    case 'c':
        b = -26;
        if (sscanf(line + 1, "%d %d", &a, &b) >= 1 && a >= 1 && a <= 4 && b <= -6 && b >= -80)
            xTaskCreate(click_task, "click", 4096, (void *)(intptr_t)((a - 1) | (-b) << 8), 3, NULL);
        else ESP_LOGW(TAG, "@c PAIR(1-4) [DB(-80..-6)]");
        break;
    case 'l':
        if (sscanf(line + 1, "%d", &a) == 1 && a >= LOOPER_CMD_REC_DUB && a <= LOOPER_CMD_CLEAR) p4_looper_command((looper_cmd_t)a);
        break;
    case 'y':
        if (sscanf(line + 1, "%d", &a) == 1 && a >= 0 && a < 48000) {
            p4_looper_set_latency((uint32_t)a);
            ESP_LOGI(TAG, "looper latency %d frames", a);
        }
        break;
    case 'v':
        if (sscanf(line + 1, "%d", &a) == 1 && a >= -90 && a <= 12) {
            p4_looper_set_level_db((float)a);
            ESP_LOGI(TAG, "looper volume %d dB", a);
        }
        break;
    case 'm':
        p4_audio_test_report(true);
        break;
    case 'p':
        perf_report();
        break;
    case 'b':
        if (line[1] == ' ' && line[2] == 'g' && sscanf(line + 3, "%d", &a) == 1) p4_drums_groove(a);
        else if (line[1] == ' ' && line[2] == 'v' && sscanf(line + 3, "%d", &a) == 1) p4_drums_level_db((float)a);
        else if (sscanf(line + 1, "%d", &a) == 1) p4_drums_start(a != 0);
        break;
    case 'w':
        if (sscanf(line + 1, "%d", &a) == 1) xTaskCreatePinnedToCore(net_task, "net_test", 6144, (void *)(intptr_t)(a != 0), 3, NULL, 1);
        break;

    case 'u':
        a = 1500;
        sscanf(line + 1, "%d", &a);
        xTaskCreate(port_task, "port", 3072, (void *)(intptr_t)a, 3, NULL);
        break;
    default:
        ESP_LOGW(TAG, "unknown test line: %s", line);
    }
}
