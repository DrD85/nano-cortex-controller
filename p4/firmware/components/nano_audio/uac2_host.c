// USB audio class 2.0 host client for the Nano Cortex (see uac2_host.h). Everything (device events, control steps,
// transfer callbacks) runs in one client task, so no locks are needed.
#include "uac2_host.h"

#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "uac2_desc.h"
#include "uac2_pacer.h"

static const char *TAG = "uac2";

#define PACKETS 8          // one transfer = 8 microframes = 1 ms
#define PLAY_XFERS 8       // room for the most in flight (s_play_n: 3 normally, 3 ms ahead)
// Capture transfers waiting at the host controller: while the client task is busy (rendering the mix, which waits on
// PSRAM when the screen draws a lot) the Nano's packets still have a place to go - a capture packet without a waiting
// transfer is lost, and the looper records silence there. 6 = 6 ms; it adds no delay (each completes after its 1 ms).
// (3 lost packets in looper mode, 2026-11-19.)
#define CAP_XFERS 6
#define FB_XFERS 2
#define PREALLOC_MPS 256   // bytes per packet the transfers are made for at the start (the Nano: 224)
#define MAX_PLAY_CH 8

// The start: both streaming interfaces to alt 0 first and the clock set to 48 kHz, then alt 1 - a clean start of the
// Nano's audio. (The USB-A socket keeps its 5 V while the board restarts: the Nano is enumerated anew but its audio
// part kept running, and alt 1 at once now and then left it sending garbled data on every channel - "robotic"
// loops, both DI and sound 10 times too bright - until the Nano was switched off and on, 2026-11-19.)
typedef enum { STEP_IDLE, STEP_PLAY_ZERO, STEP_CAP_ZERO, STEP_SET_RATE, STEP_PLAY_ALT, STEP_CAP_ALT, STEP_RATE, STEP_RUN } step_t;

static uac2_host_config_t s_cfg;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_new_addr;          // set by the event callback, handled in the task loop
static bool s_gone;
static step_t s_step;
static uac2_stream_t s_play, s_cap;
static bool s_play_claimed, s_cap_claimed;
static int s_frame_bytes;           // playback: channels * subslot
static int s_cap_frame_bytes;
static uac2_pacer_t s_pacer;
static usb_transfer_t *s_ctrl;
// How the streams begin (2026-11-19): the capture first, the playback PLAY_DELAY_US later. Begun together, the
// Nano's USB audio came up garbled most of the time and stayed so for as long as the stream ran - now and then
// a 16-frame block of the wrong time on every channel (DI too), "robotic" loops. Measured with the guitar
// played: together 4 of 4 starts garbled, capture first 6 of 6 clean (macOS is clean too).
// The other variants of that test: 2 = together with 8 playback transfers, 3 = playback first.
#define PLAY_DELAY_US 50000
static int s_start_mode = 1, s_play_n = 3;
static int64_t s_play_at_us, s_cap_at_us;   // a deferred start (0 = none)

void uac2_host_set_start_mode(int mode)
{
    s_start_mode = mode >= 0 && mode <= 3 ? mode : 0;
    s_play_n = s_start_mode == 2 ? 8 : 3;
}

static usb_transfer_t *s_play_x[PLAY_XFERS], *s_cap_x[CAP_XFERS], *s_fb_x[FB_XFERS];
static int s_inflight;
static uac2_host_stats_t s_stats;

// ---- control requests (one at a time, each step starts the next) ----

static void next_step(void);

static void ctrl_done(usb_transfer_t *t)
{
    s_inflight--;
    if (s_gone) return;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED && (s_step == STEP_SET_RATE || s_step == STEP_PLAY_ZERO || s_step == STEP_CAP_ZERO)) {
        ESP_LOGW(TAG, "control request of step %d not taken (status %d) - going on", s_step, t->status);   // (only a help)
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "control request of step %d failed (status %d) - audio stays off", s_step, t->status);
        s_stats.errors++;
        s_step = STEP_IDLE;
        return;
    }
    if (s_step == STEP_RATE && t->actual_num_bytes >= USB_SETUP_PACKET_SIZE + 4) {
        const uint8_t *d = t->data_buffer + USB_SETUP_PACKET_SIZE;
        s_stats.sample_rate = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
        ESP_LOGI(TAG, "Nano clock: %lu Hz (internal DMA RAM free %u bytes, largest block %u)", (unsigned long)s_stats.sample_rate,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    next_step();
}

static bool ctrl_submit(uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint16_t length)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ctrl->data_buffer;
    setup->bmRequestType = req_type;
    setup->bRequest = req;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;
    s_ctrl->num_bytes = USB_SETUP_PACKET_SIZE + length;
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = ctrl_done;
    s_ctrl->timeout_ms = 1000;
    if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) return false;
    s_inflight++;
    return true;
}

static bool set_interface(uint8_t iface, uint8_t alt)
{
    return ctrl_submit(0x01, 0x0B, alt, iface, 0);   // standard SET_INTERFACE
}

// ---- streams ----

static void play_fill(usb_transfer_t *t)
{
    uint8_t *p = t->data_buffer;
    int total = 0;
    for (int k = 0; k < PACKETS; k++) {
        int n = uac2_pacer_next(&s_pacer);
        if (s_cfg.render) s_cfg.render((int32_t *)(p + total), n, s_play.channels, s_cfg.render_arg);
        else audio_pipe_render(s_cfg.pipe, (int32_t *)(p + total), n, s_play.channels, s_cfg.ch_left, s_cfg.ch_right);
        t->isoc_packet_desc[k].num_bytes = n * s_frame_bytes;
        total += n * s_frame_bytes;
        s_stats.frames_sent += n;
    }
    t->num_bytes = total;
}

static void play_done(usb_transfer_t *t)
{
    s_inflight--;
    if (s_gone || s_step != STEP_RUN) return;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) s_stats.errors++;
    for (int k = 0; k < PACKETS; k++)
        if (t->isoc_packet_desc[k].status == USB_TRANSFER_STATUS_SKIPPED) s_stats.packets_skipped++;
    int64_t t0 = esp_timer_get_time();
    play_fill(t);
    uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
    if (took > s_stats.render_max_us) s_stats.render_max_us = took;
    if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
    else s_stats.errors++;
    s_stats.rate_q16 = s_pacer.rate_q16;
    s_stats.mirroring = s_pacer.mirroring;
    s_stats.mirror_short = s_pacer.mirror_short;
    s_stats.mirror_dropped = s_pacer.mirror_dropped;
}

static void check_repeats(const int32_t *f, int frames, int channels);

static void cap_done(usb_transfer_t *t)
{
    s_inflight--;
    if (s_gone || s_step != STEP_RUN) return;
    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (last_us && (uint32_t)(now - last_us) > s_stats.cap_gap_max_us) s_stats.cap_gap_max_us = (uint32_t)(now - last_us);
    last_us = now;
    // The received data through the cache once more before it is read (the driver does so at the dequeue too).
    // Test, 2026-11-19: a capture packet now and then held three frames of old data in its middle ("robotic").
    esp_cache_msync(t->data_buffer, t->data_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int k = 0; k < PACKETS; k++) {
            int bytes = t->isoc_packet_desc[k].actual_num_bytes;
            if (bytes == 0 || t->isoc_packet_desc[k].status != USB_TRANSFER_STATUS_COMPLETED) s_stats.capture_empty++;
            if (s_cap_frame_bytes && bytes % s_cap_frame_bytes) s_stats.capture_odd++;   // (not whole frames)
            if (s_cap_frame_bytes) {   // packets by their frames: 5, 6, 7, other
                int fr = bytes / s_cap_frame_bytes;
                s_stats.capture_sizes[fr == 5 ? 0 : fr == 6 ? 1 : fr == 7 ? 2 : 3]++;
            }
            int frames = s_cap_frame_bytes ? bytes / s_cap_frame_bytes : 0;
            uac2_pacer_capture_packet(&s_pacer, frames);
            if (frames) check_repeats((const int32_t *)(t->data_buffer + k * s_cap.mps), frames, s_cap.channels);
            if (frames && s_cfg.on_capture)
                s_cfg.on_capture((const int32_t *)(t->data_buffer + k * s_cap.mps), frames, s_cap.channels,
                                 s_cfg.capture_arg);
            s_stats.capture_packets++;
        }
    } else {
        s_stats.errors++;
    }
    if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
}

// Garbled capture (2026-11-19): now and then the Nano sends frames it already sent - three old frames in the middle
// of a packet, on every channel, for as long as the stream runs. A frame of the first four channels that comes again
// exactly within the last REPEAT_SLOTS frames is counted (even the DI's noise floor never repeats by itself).
#define REPEAT_SLOTS 512
static struct { int32_t v[4]; uint32_t at; } s_seen[REPEAT_SLOTS];
static uint32_t s_frame_no;

static void check_repeats(const int32_t *f, int frames, int channels)
{
    if (channels < 4) return;
    for (int i = 0; i < frames; i++, f += channels) {
        s_frame_no++;
        if (!(f[0] | f[1] | f[2] | f[3])) continue;   // (silence repeats)
        uint32_t h = ((uint32_t)f[0] * 2654435761u) ^ ((uint32_t)f[1] * 40503u) ^ ((uint32_t)f[2] * 97u) ^ (uint32_t)f[3];
        h = (h ^ (h >> 15)) % REPEAT_SLOTS;
        if (s_seen[h].at && s_frame_no - s_seen[h].at < REPEAT_SLOTS && !memcmp(s_seen[h].v, f, sizeof(s_seen[h].v))) s_stats.capture_repeats++;
        memcpy(s_seen[h].v, f, sizeof(s_seen[h].v));
        s_seen[h].at = s_frame_no;
    }
}

static void fb_done(usb_transfer_t *t)
{
    s_inflight--;
    if (s_gone || s_step != STEP_RUN) return;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->isoc_packet_desc[0].actual_num_bytes >= 4) {
        const uint8_t *d = t->data_buffer;
        uac2_pacer_feedback(&s_pacer, d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24));
    }
    if (usb_host_transfer_submit(t) == ESP_OK) s_inflight++;
}

// The transfers are made once and kept for the next connection (made at the start, see PREALLOC_MPS): a new
// connection needs no memory. (Freed at every disconnect, they could not be made again after a few minutes - the
// internal DMA RAM was down to its last 31 KB block: "no memory for the audio transfers", no sound after the Nano
// was switched off and on, 2026-11-19.) A transfer too small for this stream is made anew.
static bool alloc_iso(usb_transfer_t **x, int count, int packets, int packet_bytes, uint8_t ep, usb_transfer_cb_t cb)
{
    for (int i = 0; i < count; i++) {
        if (x[i] && (x[i]->data_buffer_size < (size_t)(packets * packet_bytes) || x[i]->num_isoc_packets != packets)) {
            usb_host_transfer_free(x[i]);
            x[i] = NULL;
        }
        if (!x[i] && usb_host_transfer_alloc(packets * packet_bytes, packets, &x[i]) != ESP_OK) return false;
        x[i]->device_handle = s_dev;
        x[i]->bEndpointAddress = ep;
        x[i]->callback = cb;
        x[i]->num_bytes = packets * packet_bytes;
        for (int k = 0; k < packets; k++) x[i]->isoc_packet_desc[k].num_bytes = packet_bytes;
    }
    return true;
}

static void start_streams(void)
{
    uint32_t rate = s_stats.sample_rate ? s_stats.sample_rate : 48000;
    if (rate != 48000) {
        ESP_LOGE(TAG, "Nano runs at %lu Hz, expected 48000 - audio stays off", (unsigned long)rate);
        s_step = STEP_IDLE;
        return;
    }
    uac2_pacer_init(&s_pacer, rate, 8000, s_play.mps / s_frame_bytes);
    if (s_cfg.capture_ep && s_cap_claimed) {
        if (!alloc_iso(s_cap_x, CAP_XFERS, PACKETS, s_cap.mps, s_cap.ep, cap_done)) goto nomem;
        if (s_start_mode == 3) s_cap_at_us = esp_timer_get_time() + 50000;
        else for (int i = 0; i < CAP_XFERS; i++)
            if (usb_host_transfer_submit(s_cap_x[i]) == ESP_OK) s_inflight++;
    }
    if (s_play.fb_ep) {
        if (!alloc_iso(s_fb_x, FB_XFERS, 1, s_play.fb_mps, s_play.fb_ep, fb_done)) goto nomem;
        for (int i = 0; i < FB_XFERS; i++)
            if (usb_host_transfer_submit(s_fb_x[i]) == ESP_OK) s_inflight++;
    }
    if (!alloc_iso(s_play_x, s_play_n, PACKETS, s_play.mps, s_play.ep, play_done)) goto nomem;
    s_step = STEP_RUN;
    if (s_start_mode == 1) s_play_at_us = esp_timer_get_time() + PLAY_DELAY_US;
    else for (int i = 0; i < s_play_n; i++) {
        play_fill(s_play_x[i]);
        if (usb_host_transfer_submit(s_play_x[i]) == ESP_OK) s_inflight++;
        else s_stats.errors++;
    }
    s_stats.streaming = true;
    ESP_LOGI(TAG, "playback running: %d ch, %d-byte slots, %d bytes/microframe max, feedback %s", s_play.channels,
             s_play.subslot, s_play.mps, s_play.fb_ep ? "explicit" : s_cap_claimed ? "from capture" : "none");
    return;
nomem:
    ESP_LOGE(TAG, "no memory for the audio transfers (internal DMA RAM free %u bytes, largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    s_step = STEP_IDLE;
}

static void next_step(void)
{
    switch (s_step) {
    case STEP_PLAY_ZERO:
        if (s_cfg.capture_ep && s_cap_claimed) {
            s_step = STEP_CAP_ZERO;
            if (set_interface(s_cap.interface, 0)) return;
        }
        /* fall through */
    case STEP_CAP_ZERO:
        if (s_play.clock_id) {   // SET_CUR sampling frequency: 48000
            s_step = STEP_SET_RATE;
            uint8_t *d = s_ctrl->data_buffer + USB_SETUP_PACKET_SIZE;
            d[0] = 0x80, d[1] = 0xBB, d[2] = 0x00, d[3] = 0x00;
            if (ctrl_submit(0x21, 0x01, 0x0100, (uint16_t)(s_play.clock_id << 8 | s_play.ac_interface), 4)) return;
        }
        /* fall through */
    case STEP_SET_RATE:
        s_step = STEP_PLAY_ALT;
        if (set_interface(s_play.interface, s_play.alt)) return;
        s_step = STEP_IDLE;
        return;
    case STEP_PLAY_ALT:
        if (s_cfg.capture_ep && s_cap_claimed) {
            s_step = STEP_CAP_ALT;
            if (set_interface(s_cap.interface, s_cap.alt)) return;
        }
        /* fall through */
    case STEP_CAP_ALT:
        if (s_play.clock_id) {
            s_step = STEP_RATE;   // GET_CUR sampling frequency of the clock source (read only)
            if (ctrl_submit(0xA1, 0x01, 0x0100, (uint16_t)(s_play.clock_id << 8 | s_play.ac_interface), 4)) return;
        }
        /* fall through */
    case STEP_RATE:
        start_streams();
        return;
    default:
        return;
    }
}

// ---- device ----

static void close_device(void)
{
    // (the transfers stay: the next connection uses them again - alloc_iso)
    if (s_play_claimed) usb_host_interface_release(s_client, s_dev, s_play.interface);
    if (s_cap_claimed) usb_host_interface_release(s_client, s_dev, s_cap.interface);
    s_play_claimed = s_cap_claimed = false;
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    s_step = STEP_IDLE;
    s_gone = false;
    s_stats.connected = s_stats.streaming = false;
    ESP_LOGI(TAG, "Nano audio closed");
}

static void open_device(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || dd->idVendor != s_cfg.vid || dd->idProduct != s_cfg.pid) {
        usb_host_device_close(s_client, dev);   // not the Nano
        return;
    }
    s_dev = dev;
    s_stats.connected = true;
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) == ESP_OK)
        ESP_LOGI(TAG, "Nano on the port, %s speed", info.speed == USB_SPEED_HIGH ? "HIGH" : "FULL/LOW (no audio)");
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return;
    const uint8_t *raw = (const uint8_t *)cfg;
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, raw, cfg->wTotalLength, ESP_LOG_INFO);   // the full descriptor, for the notes

    if (!uac2_find_stream(raw, cfg->wTotalLength, s_cfg.playback_ep, &s_play) || s_play.subslot != 4 ||
        s_play.channels < 2 || s_play.channels > MAX_PLAY_CH || s_play.mps == 0) {
        ESP_LOGE(TAG, "no usable playback stream on EP 0x%02X - audio stays off", s_cfg.playback_ep);
        return;
    }
    s_frame_bytes = s_play.channels * s_play.subslot;
    ESP_LOGI(TAG, "playback: interface %d alt %d, EP 0x%02X, %d bytes, interval %d, sync %d, %d ch, %d bits",
             s_play.interface, s_play.alt, s_play.ep, s_play.mps, s_play.interval, s_play.sync, s_play.channels,
             s_play.bits);
    if (usb_host_interface_claim(s_client, dev, s_play.interface, s_play.alt) != ESP_OK) return;
    s_play_claimed = true;
    if (s_cfg.capture_ep && uac2_find_stream(raw, cfg->wTotalLength, s_cfg.capture_ep, &s_cap) && s_cap.subslot) {
        s_cap_frame_bytes = s_cap.channels * s_cap.subslot;
        s_cap_claimed = usb_host_interface_claim(s_client, dev, s_cap.interface, s_cap.alt) == ESP_OK;
    }
    s_step = STEP_PLAY_ZERO;
    ESP_LOGI(TAG, "audio: interfaces to alt 0, clock to 48 kHz, then alt 1");
    if (!set_interface(s_play.interface, 0)) s_step = STEP_IDLE;
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV && !s_dev) s_new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && s_dev && msg->dev_gone.dev_hdl == s_dev) s_gone = true;
}

static volatile bool s_hold;   // uac2_host_hold: no stream starts yet
static volatile bool s_restart_req;   // uac2_host_restart: the streams anew
static bool s_restarting;

void uac2_host_restart(void)
{
    s_restart_req = true;
}

void uac2_host_hold(bool hold)
{
    s_hold = hold;
}

static void client_task(void *arg)
{
    bool listed = false;
    while (true) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(s_play_at_us || s_cap_at_us ? 5 : 100));
        if (s_hold) continue;   // (a NEW_DEV meanwhile waits in s_new_addr)
        if (s_play_at_us && esp_timer_get_time() >= s_play_at_us && s_step == STEP_RUN && !s_gone) {   // (the playback after the capture)
            s_play_at_us = 0;
            for (int i = 0; i < s_play_n; i++) {
                play_fill(s_play_x[i]);
                if (usb_host_transfer_submit(s_play_x[i]) == ESP_OK) s_inflight++;
            }
        }
        if (s_cap_at_us && esp_timer_get_time() >= s_cap_at_us && s_step == STEP_RUN && !s_gone) {   // (start variant 3)
            s_cap_at_us = 0;
            for (int i = 0; i < CAP_XFERS; i++)
                if (usb_host_transfer_submit(s_cap_x[i]) == ESP_OK) s_inflight++;
        }
        if (s_restart_req && s_dev && !s_gone && s_step == STEP_RUN) {   // stop resubmitting; start again once all came back
            s_restart_req = false;
            s_restarting = true;
            s_step = STEP_IDLE;
            s_stats.streaming = false;
            ESP_LOGW(TAG, "audio: streams stopped to start them anew");
        }
        if (s_restarting && s_inflight == 0 && s_dev && !s_gone) {
            s_restarting = false;
            s_step = STEP_PLAY_ZERO;
            ESP_LOGW(TAG, "audio: starting the streams anew");
            if (!set_interface(s_play.interface, 0)) s_step = STEP_IDLE;
        }
        if (!listed) {   // a Nano that was on the port before this client registered gets no NEW_DEV event: from the list
            listed = true;
            uint8_t addrs[8];
            int found = 0;
            if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &found) == ESP_OK)
                for (int i = 0; i < found && !s_dev; i++) open_device(addrs[i]);
            if (s_dev) s_new_addr = 0;
        }
        if (s_new_addr) {
            uint8_t a = s_new_addr;
            s_new_addr = 0;
            open_device(a);
        }
        if (s_gone && s_inflight == 0) close_device();   // every transfer came back
    }
}

esp_err_t uac2_host_start(const uac2_host_config_t *cfg)
{
    s_cfg = *cfg;
    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event, .callback_arg = NULL },
    };
    esp_err_t err = usb_host_client_register(&cc, &s_client);
    if (err != ESP_OK) return err;
    if (usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + 64, 0, &s_ctrl) != ESP_OK) return ESP_ERR_NO_MEM;
    // the streams' transfers now, while the internal DMA RAM has room (the Nano's packets: 224 bytes)
    for (int i = 0; i < PLAY_XFERS; i++) if (usb_host_transfer_alloc(PACKETS * PREALLOC_MPS, PACKETS, &s_play_x[i]) != ESP_OK) return ESP_ERR_NO_MEM;
    for (int i = 0; i < CAP_XFERS; i++) if (usb_host_transfer_alloc(PACKETS * PREALLOC_MPS, PACKETS, &s_cap_x[i]) != ESP_OK) return ESP_ERR_NO_MEM;
    // (8 KB: with 4 KB the mixer's rendering left 1.1 KB on the P4, measured 2026-11-18)
    if (xTaskCreatePinnedToCore(client_task, "uac2", 8192, NULL, cfg->task_priority, NULL, cfg->task_core) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void uac2_host_stats(uac2_host_stats_t *out)
{
    *out = s_stats;
}

void uac2_host_take_peaks(uint32_t *render_max_us, uint32_t *cap_gap_max_us)
{
    *render_max_us = s_stats.render_max_us;
    *cap_gap_max_us = s_stats.cap_gap_max_us;
    s_stats.render_max_us = s_stats.cap_gap_max_us = 0;   // (a race with the client task loses at most one peak)
}
