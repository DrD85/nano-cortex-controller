// P4 board: the link to the Nano Cortex over USB HID instead of Bluetooth - the same nano_link.h for main.c.
//
// The Nano speaks its Bluetooth protocol over its HID interface (interface 5): a message is [len][flags][payload]
// [u32 LE type] cut into pieces of up to 62 bytes, each piece one report [report ID][n][flags][n bytes] - flags 0x40
// first, 0x00 middle, 0x80 last, 0xC0 a whole message (measured on the Mac 2026-11-13, PLAN.md). Input reports
// (ID 1) come on interrupt endpoint 0x81, output reports (ID 2) go out with SET_REPORT on the control endpoint.
// Writes go out strictly one at a time, as over Bluetooth; a newer value of a control replaces one that still waits.
//
// This file also installs the USB Host Library and runs its event task: the audio client (uac2_host) shares it.
//
// STATUS: builds; not yet run on hardware. To check first: the output report's length (65 = ID + 64 assumed; on the
// Mac 64 bytes with the ID worked too) and how a message longer than one report is written (the pieces' flags as
// in the input direction are assumed).
#include "nano_link.h"
#include "nano_link_transport.h"
#include "nano_state.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "nano_usb";

#define NANO_VID 0x152A
#define NANO_PID 0x88E7
#define HID_INTERFACE 5
#define HID_IN_EP 0x81
#define REPORT_IN_ID 1
#define REPORT_OUT_ID 2
#define REPORT_LEN 64                 // bytes after the report ID
#define PIECE_MAX (REPORT_LEN - 2)    // data bytes per report
#define MAX_MESSAGE (256 * 1024)      // the capture / IR library is the largest reply
#define QUEUE_MAX 64
#define IN_XFERS 2
// The Nano does not answer a state request that follows another one (or its reply) too closely: measured over USB on
// 2026-11-18 with NanOS 2.3.0, 1.44 s apart stayed unanswered, 1.54 s apart was answered - whether full or current
// state; other requests are answered at any time. So a state request waits here until the Nano takes one again.
#define STATE_GAP_US 1600000

typedef struct {
    uint8_t *data;                    // [len][0xC0][payload][type] as on Bluetooth
    uint32_t len;                     // (a long message from the app can be larger than 64 KB)
    int32_t key;                      // control a value frame sets, -1 = other message
    uint8_t owner;
    char label[40];
} job_t;

static nano_message_cb s_on_message;
static nano_link_cb s_on_link;
static nano_forward_cb s_forward;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_new_addr;
static bool s_gone, s_claimed, s_ready;
static usb_transfer_t *s_ctrl, *s_in[IN_XFERS];
static atomic_int s_inflight;         // transfers handed to the USB host; counted from several tasks
static SemaphoreHandle_t s_lock;
static job_t s_jobs[QUEUE_MAX];
static int s_head, s_count;
static bool s_busy;                   // s_jobs[s_head] is being written
static int s_piece;                   // which report of it
static uint8_t *s_rx;
static size_t s_rx_len;
static bool s_rx_active;
static nano_link_stats_t s_stats;
static uint32_t s_in_reports;         // input reports since the Nano appeared
static int s_in_status;               // status of the last read
static int64_t s_state_us;            // when the last state request went out or the last state reply came (lock)
static int64_t s_state_gap_us = STATE_GAP_US;
static int64_t s_ctrl_sent_us;        // when the report in flight was handed to the USB host (0 = none)
static bool s_ctrl_warned;
static volatile bool s_cycle_request;   // nano_usb_restart: the client task switches the port off and on
static int64_t s_cycled_us;

static void log_hex(const char *prefix, const uint8_t *d, size_t n)
{
    char line[3 * 32 + 8] = "";
    size_t shown = n < 32 ? n : 32;
    for (size_t i = 0; i < shown; i++) sprintf(line + 3 * i, "%02X ", d[i]);
    ESP_LOGI(TAG, "%s (%u bytes): %s%s", prefix, (unsigned)n, line, n > shown ? "..." : "");
}

// ---- receiving ----

static void deliver(const uint8_t *body, size_t n)
{
    if (n < 4) return;
    uint32_t type = body[n - 4] | (body[n - 3] << 8) | (body[n - 2] << 16) | ((uint32_t)body[n - 1] << 24);
    if (type == NANO_MSG_STATE_RESPONSE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state_us = esp_timer_get_time();
        xSemaphoreGive(s_lock);
    }
    if (s_on_message) s_on_message(type, body, n - 4);
    if (s_forward) s_forward(body, n);
}

static void on_report(const uint8_t *r, size_t len)
{
    if (len < 3 || r[0] != REPORT_IN_ID) return;
    size_t n = r[1];
    uint8_t flags = r[2];
    if (n > len - 3) n = len - 3;
    if (flags == 0xC0) {
        deliver(r + 3, n);
        return;
    }
    if (flags & 0x40) {
        s_rx_len = 0;
        s_rx_active = true;
    }
    if (!s_rx_active) return;   // a piece without its start
    if (s_rx_len + n > MAX_MESSAGE) {
        ESP_LOGE(TAG, "Message larger than %d bytes dropped", MAX_MESSAGE);
        s_rx_active = false;
        return;
    }
    memcpy(s_rx + s_rx_len, r + 3, n);
    s_rx_len += n;
    if (flags & 0x80) {
        s_rx_active = false;
        deliver(s_rx, s_rx_len);
    }
}

static void in_done(usb_transfer_t *t)
{
    s_inflight--;
    if (s_gone) return;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_in_reports++;
        on_report(t->data_buffer, t->actual_num_bytes);
    } else if ((int)t->status != s_in_status) {
        ESP_LOGW(TAG, "Read ended with status %d after %u reports", (int)t->status, (unsigned)s_in_reports);
    }
    s_in_status = (int)t->status;
    esp_err_t err = usb_host_transfer_submit(t);
    if (err == ESP_OK) s_inflight++;
    else ESP_LOGW(TAG, "Read not started again: %s", esp_err_to_name(err));
}

// ---- writing ----

static uint32_t frame_type(const uint8_t *d, size_t n)
{
    return n < 6 ? 0 : d[n - 4] | (d[n - 3] << 8) | (d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
}

// The control a value frame sets (type << 16 | control), or -1 (as in nano_link.c).
static int32_t value_key(const uint8_t *frame, size_t len)
{
    uint32_t type = frame_type(frame, len);
    if (type == 99 && len >= 10 && frame[2] == 0x08 && frame[4] == 0x18 && frame[6] == 0x20)
        return (int32_t)(type << 16 | frame[5] << 8 | frame[7]);
    if (type == 26 && len >= 8 && frame[2] == 0x18) return (int32_t)(type << 16 | frame[3]);
    if (type == 94 && len >= 11) return (int32_t)(type << 16 | frame[2]);
    return -1;
}

static void ctrl_done(usb_transfer_t *t);

// Sends the next report of the job in front (called with the lock held).
static void send_piece(void)
{
    job_t *job = &s_jobs[s_head];
    const uint8_t *body = job->data + 2;   // after [len][0xC0]
    size_t total = job->len - 2, from = (size_t)s_piece * PIECE_MAX, n = total - from;
    if (n > PIECE_MAX) n = PIECE_MAX;
    uint8_t flags = total <= PIECE_MAX ? 0xC0 : s_piece == 0 ? 0x40 : from + n >= total ? 0x80 : 0x00;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ctrl->data_buffer;
    setup->bmRequestType = 0x21;                     // class request to the interface
    setup->bRequest = 0x09;                          // SET_REPORT
    setup->wValue = (2 << 8) | REPORT_OUT_ID;        // output report
    setup->wIndex = HID_INTERFACE;
    setup->wLength = 1 + REPORT_LEN;
    uint8_t *r = s_ctrl->data_buffer + USB_SETUP_PACKET_SIZE;
    memset(r, 0, 1 + REPORT_LEN);
    r[0] = REPORT_OUT_ID;
    r[1] = (uint8_t)n;
    r[2] = flags;
    memcpy(r + 3, body + from, n);
    s_ctrl->num_bytes = USB_SETUP_PACKET_SIZE + 1 + REPORT_LEN;
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = ctrl_done;
    s_ctrl->timeout_ms = 500;   // (the USB host library does not act on it: client_task watches the time)
    s_ctrl_sent_us = esp_timer_get_time();   // before the hand-over: ctrl_done may run before it returns
    s_ctrl_warned = false;
    esp_err_t err = usb_host_transfer_submit_control(s_client, s_ctrl);
    if (err == ESP_OK) s_inflight++;
    else {
        s_ctrl_sent_us = 0;
        ESP_LOGW(TAG, "Write failed (%s): %s", esp_err_to_name(err), job->label);
        s_busy = false;
    }
}

static bool is_state_request(const job_t *job)
{
    return frame_type(job->data, job->len) == NANO_MSG_STATE_REQUEST;
}

static void kick(void)   // (lock held)
{
    if (s_busy || !s_count || !s_ready) return;
    // A state request the Nano would not answer yet waits (STATE_GAP_US); what stands behind it goes first.
    bool hold = s_state_us && esp_timer_get_time() - s_state_us < s_state_gap_us;
    int pick = 0;
    while (pick < s_count && hold && is_state_request(&s_jobs[(s_head + pick) % QUEUE_MAX])) pick++;
    if (pick == s_count) return;   // only state requests wait: client_task comes back to them
    if (pick) {                    // that job to the front, the others keep their order
        job_t first = s_jobs[(s_head + pick) % QUEUE_MAX];
        for (int i = pick; i > 0; i--) s_jobs[(s_head + i) % QUEUE_MAX] = s_jobs[(s_head + i - 1) % QUEUE_MAX];
        s_jobs[s_head] = first;
    }
    job_t *job = &s_jobs[s_head];
    if (job->key < 0) log_hex(job->label, job->data, job->len);
    if (is_state_request(job)) s_state_us = esp_timer_get_time();
    s_busy = true;
    s_piece = 0;
    send_piece();
}

static void ctrl_done(usb_transfer_t *t)
{
    s_inflight--;
    int ms = (int)((esp_timer_get_time() - s_ctrl_sent_us) / 1000);
    s_ctrl_sent_us = 0;
    if (ms >= 100 || s_ctrl_warned) ESP_LOGW(TAG, "A write took %d ms (status %d)", ms, (int)t->status);
    else ESP_LOGD(TAG, "write done in %d ms (status %d)", ms, (int)t->status);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_busy && s_count) {
        job_t *job = &s_jobs[s_head];
        if (t->status != USB_TRANSFER_STATUS_COMPLETED) ESP_LOGW(TAG, "Write failed (%d): %s", t->status, job->label);
        size_t total = job->len - 2;
        s_piece++;
        if (t->status == USB_TRANSFER_STATUS_COMPLETED && (size_t)s_piece * PIECE_MAX < total && !s_gone) {
            send_piece();   // the next report of a long message
            xSemaphoreGive(s_lock);
            return;
        }
        free(job->data);
        s_head = (s_head + 1) % QUEUE_MAX;
        s_count--;
    }
    s_busy = false;
    if (!s_gone) kick();
    xSemaphoreGive(s_lock);
}

static bool enqueue(const char *label, uint8_t *data, uint32_t n, uint8_t owner)
{
    int32_t key = value_key(data, n);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (key >= 0) {   // a newer value of a control that still waits replaces it, in its place
        for (int i = s_busy ? 1 : 0; i < s_count; i++) {
            job_t *job = &s_jobs[(s_head + i) % QUEUE_MAX];
            if (job->key == key) {
                free(job->data);
                job->data = data;
                job->len = n;
                xSemaphoreGive(s_lock);
                return true;
            }
        }
    }
    if (frame_type(data, n) == NANO_MSG_STATE_REQUEST) {   // the same state request already waits: one is enough
        for (int i = s_busy ? 1 : 0; i < s_count; i++) {
            job_t *job = &s_jobs[(s_head + i) % QUEUE_MAX];
            if (job->len == n && !memcmp(job->data, data, n)) {
                xSemaphoreGive(s_lock);
                free(data);
                return true;
            }
        }
    }
    if (s_count == QUEUE_MAX) {
        xSemaphoreGive(s_lock);
        free(data);
        ESP_LOGW(TAG, "Queue full - dropped: %s", label);
        return false;
    }
    job_t *job = &s_jobs[(s_head + s_count) % QUEUE_MAX];
    job->data = data;
    job->len = n;
    job->key = key;
    job->owner = owner;
    snprintf(job->label, sizeof(job->label), "%s", label);
    s_count++;
    kick();
    xSemaphoreGive(s_lock);
    return true;
}

bool nano_usb_send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "Not connected - dropped: %s", label);
        return true;
    }
    if (len + 6 > MAX_MESSAGE) return false;
    uint32_t n = (uint32_t)(len + 6);
    uint8_t *data = malloc(n);
    if (!data) return false;
    data[0] = (uint8_t)(n - 2);   // (the length byte only matters for single pieces; long ones are cut above)
    data[1] = 0xC0;
    memcpy(data + 2, payload, len);
    for (int i = 0; i < 4; i++) data[2 + len + i] = (uint8_t)(type >> (8 * i));
    return enqueue(label, data, n, NANO_OWNER_BOARD);
}

bool nano_usb_send_frame(const uint8_t *frame, size_t len)   // the app bridge (stage f): frames as they came
{
    if (!s_ready || len < 6) return false;
    uint8_t *data = malloc(len);
    if (!data) return false;
    memcpy(data, frame, len);
    return enqueue("App", data, (uint32_t)len, NANO_OWNER_APP);
}

// A long message from the app through the board: its Bluetooth packets back to back (each: 2 bytes of size, little
// endian, then the packet as the app wrote it - [n][flags][n bytes]). Over USB the message is put together again and
// cut into the Nano's 62-byte reports as any long message. The buffer is handed over and freed here.
bool nano_usb_send_packets(uint8_t *packets, size_t len, uint32_t type)
{
    size_t body = 0;
    for (size_t off = 0; off + 2 <= len;) {
        size_t n = (size_t)(packets[off] | packets[off + 1] << 8);
        off += 2;
        if (n < 2 || off + n > len) break;
        body += n - 2;
        off += n;
    }
    if (!s_ready || body < 4 || body + 2 > MAX_MESSAGE) {
        ESP_LOGW(TAG, "Long app message %lu (%u bytes) not sent%s", (unsigned long)type, (unsigned)len, s_ready ? "" : ": not connected");
        free(packets);
        return false;
    }
    uint8_t *data = heap_caps_malloc(body + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        free(packets);
        return false;
    }
    data[0] = (uint8_t)(body > 0xFF ? 0xFF : body);   // (only read for single reports)
    data[1] = 0xC0;
    size_t at = 2;
    for (size_t off = 0; off + 2 <= len;) {
        size_t n = (size_t)(packets[off] | packets[off + 1] << 8);
        off += 2;
        if (n < 2 || off + n > len) break;
        memcpy(data + at, packets + off + 2, n - 2);
        at += n - 2;
        off += n;
    }
    free(packets);
    char label[40];
    snprintf(label, sizeof(label), "App message %lu (%u bytes)", (unsigned long)type, (unsigned)body);
    return enqueue(label, data, (uint32_t)(body + 2), NANO_OWNER_APP);
}

void nano_usb_set_forward(nano_forward_cb cb)
{
    s_forward = cb;
}

// ---- device ----

static void set_ready(bool ready)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ready = ready;
    s_rx_active = false;
    if (!ready) {
        for (int i = 0; i < s_count; i++) free(s_jobs[(s_head + i) % QUEUE_MAX].data);
        s_head = s_count = 0;
        s_busy = false;
    }
    xSemaphoreGive(s_lock);
    if (ready) s_stats.ready_us = esp_timer_get_time();
    else s_stats.ready_us = 0;
    ESP_LOGI(TAG, "%s", ready ? "Nano on USB - link ready" : "Nano gone from USB");
    if (s_on_link) s_on_link(ready);
}

static void open_device(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || dd->idVendor != NANO_VID || dd->idProduct != NANO_PID) {
        usb_host_device_close(s_client, dev);
        return;
    }
    if (usb_host_interface_claim(s_client, dev, HID_INTERFACE, 0) != ESP_OK) {
        ESP_LOGE(TAG, "HID interface %d not claimed", HID_INTERFACE);
        usb_host_device_close(s_client, dev);
        return;
    }
    s_dev = dev;
    s_claimed = true;
    s_in_reports = 0;
    s_in_status = USB_TRANSFER_STATUS_COMPLETED;
    s_state_us = 0;
    for (int i = 0; i < IN_XFERS; i++) {
        if (!s_in[i] && usb_host_transfer_alloc(128, 0, &s_in[i]) != ESP_OK) continue;
        s_in[i]->device_handle = dev;
        s_in[i]->bEndpointAddress = HID_IN_EP;
        s_in[i]->callback = in_done;
        s_in[i]->num_bytes = 128;   // a 65-byte report ends with a short packet
        if (usb_host_transfer_submit(s_in[i]) == ESP_OK) s_inflight++;
    }
    set_ready(true);
}

static void close_device(void)
{
    if (s_claimed) usb_host_interface_release(s_client, s_dev, HID_INTERFACE);
    s_claimed = false;
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    s_gone = false;
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV && !s_dev) s_new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && s_dev && msg->dev_gone.dev_hdl == s_dev) {
        s_gone = true;
        s_stats.drops++;
        s_stats.last_drop_us = esp_timer_get_time();
        set_ready(false);
    }
}

// The way out when the Nano no longer takes a write (the USB host library waits for ever) or no longer answers: the
// USB-A port is switched off and on. The Nano then comes back as after plugging it in - link and current preset
// within 0.6 s (measured 2026-11-18). Not twice within ten seconds. (client task only)
// The port may only come on again once this client and the audio client have let go of the Nano: switched on
// earlier (tried: 300 ms inside this task, without handling its events) the Nano was never found again. So the
// task goes on handling events while the port is off (port_cycle_step).
#define PORT_OFF_MIN_US 700000         // after this client has closed the Nano: time for the audio client to do so
#define PORT_OFF_MAX_US 4000000
#define PORT_CYCLE_APART_US (10 * 1000000LL)
// No Nano on the port for this long: the port is switched off and on, again and again. With no Nano there it does
// nothing; a port that got stuck (seen twice on the first day: the Nano was only found again after a restart of the
// board) finds the Nano again.
#define PORT_RETRY_US (20 * 1000000LL)
static int64_t s_port_off_us, s_closed_us;   // port off since / this client closed the Nano at (0 = not)
static int64_t s_absent_us;                  // no Nano since (0 = it is there)

static bool port_cycle(const char *why)
{
    int64_t now = esp_timer_get_time();
    if (s_port_off_us || (s_cycled_us && now - s_cycled_us < PORT_CYCLE_APART_US)) return false;
    s_cycled_us = now;
    ESP_LOGW(TAG, "%s - USB port off and on", why);
    s_closed_us = 0;
    s_port_off_us = now;
    usb_host_lib_set_root_port_power(false);
    return true;
}

static void port_cycle_step(void)
{
    if (!s_port_off_us) return;
    int64_t now = esp_timer_get_time();
    if (!s_dev && !s_closed_us) s_closed_us = now;
    bool let_go = s_closed_us && now - s_closed_us > PORT_OFF_MIN_US;
    if (!let_go && now - s_port_off_us < PORT_OFF_MAX_US) return;
    if (!let_go) ESP_LOGW(TAG, "The Nano was not let go of in time - port on anyway");
    s_port_off_us = 0;
    esp_err_t err = usb_host_lib_set_root_port_power(true);
    ESP_LOGW(TAG, "USB port on again after %d ms (%s)", (int)((now - s_cycled_us) / 1000), esp_err_to_name(err));
}

static void client_task(void *arg)
{
    // A Nano that was on the port before this client registered gets no NEW_DEV event: open it from the list.
    uint8_t addrs[8];
    int found = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &found) == ESP_OK)
        for (int i = 0; i < found && !s_dev; i++) open_device(addrs[i]);
    while (true) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        if (s_new_addr) {
            uint8_t a = s_new_addr;
            s_new_addr = 0;
            open_device(a);
        }
        if (s_gone && s_inflight == 0) close_device();
        port_cycle_step();
        if (s_dev) s_absent_us = 0;
        else if (!s_absent_us) s_absent_us = esp_timer_get_time();
        else if (esp_timer_get_time() - s_absent_us > PORT_RETRY_US && port_cycle("No Nano for 20 s")) s_absent_us = 0;
        if (s_count && !s_busy && s_ready) {   // a state request that had to wait (STATE_GAP_US)
            xSemaphoreTake(s_lock, portMAX_DELAY);
            kick();
            xSemaphoreGive(s_lock);
        }
        // The USB host library has no time limit for a write: say so when the Nano does not take one.
        int64_t sent = s_ctrl_sent_us;
        if (sent && !s_ctrl_warned && esp_timer_get_time() - sent > 1000000) {
            s_ctrl_warned = true;
            ESP_LOGW(TAG, "A write has been waiting for the Nano for 1 s (%u reports read so far, %d transfers out)",
                     (unsigned)s_in_reports, (int)atomic_load(&s_inflight));
        }
        if (sent && esp_timer_get_time() - sent > 3000000) port_cycle("The Nano has not taken a write for 3 s");
        if (s_cycle_request) {
            s_cycle_request = false;
            if (s_dev && !s_gone && !port_cycle("The Nano does not answer")) {   // (just done: read everything again)
                set_ready(false);
                set_ready(true);
            }
        }
    }
}

// The USB Host Library's own events (shared with the audio client).
static void lib_task(void *arg)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

void nano_usb_start(nano_message_cb on_message, nano_link_cb on_link)
{
    s_on_message = on_message;
    s_on_link = on_link;
    s_lock = xSemaphoreCreateMutex();
    s_rx = heap_caps_malloc(MAX_MESSAGE, MALLOC_CAP_SPIRAM);
    const usb_host_config_t host = { .intr_flags = 0 };   // default: the high-speed port (USB-A)
    ESP_ERROR_CHECK(usb_host_install(&host));
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 21, NULL, 0);
    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &s_client));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + 1 + REPORT_LEN, 0, &s_ctrl));
    xTaskCreatePinnedToCore(client_task, "nano_usb", 4096, NULL, 19, NULL, 0);
    ESP_LOGI(TAG, "USB host ready - waiting for the Nano on the USB-A port");
}

#ifdef NANO_UITEST
void nano_link_usb_test_gap(int ms)   // tests: another wait between state requests (0 = none)
{
    s_state_gap_us = (int64_t)ms * 1000;
    ESP_LOGW(TAG, "TEST: state requests now wait %d ms", ms);
}
#endif

bool nano_usb_ready(void)
{
    return s_ready;
}

// The Nano stopped answering: the link is lost and found again, so main.c reads everything anew (a cable has no
// pairing to restart).
void nano_usb_restart(void)
{
    if (!s_dev) return;
    s_cycle_request = true;   // (the client task does it: the port off and on, the Nano is found anew)
}

void nano_usb_pause(int seconds)
{
    ESP_LOGW(TAG, "Pause is a Bluetooth test - not on USB (%d s)", seconds);
}

void nano_usb_stats(nano_link_stats_t *out)
{
    *out = s_stats;
}

void nano_usb_log_status(void)
{
    ESP_LOGI(TAG, "USB: %s, %d writes waiting, %d drops", s_ready ? "Nano connected" : "no Nano", s_count, s_stats.drops);
}
