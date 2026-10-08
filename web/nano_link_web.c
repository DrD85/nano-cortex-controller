// Browser build of nano_link: the Nano is connected with Web Bluetooth (web/app.js); this file keeps the same
// framing, packet assembly and write queue as main/nano_link.c.
//
// app.js writes one frame at a time (write with response) and reports back with web_nano_written(); it passes every
// notification to web_nano_packet() and the connection state to web_nano_link().
#include "nano_link.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "nano_link";

#define MAX_MESSAGE (256 * 1024)   // the capture/IR library is the largest reply
#define QUEUE_MAX 64

typedef struct {
    uint8_t *data;
    uint16_t len;
    int32_t key;    // control a value frame sets (newer values replace a waiting one), -1 = other message
    char label[40];
} job_t;

static nano_message_cb s_on_message;
static nano_link_cb s_on_link;
static bool s_ready;
static job_t s_jobs[QUEUE_MAX];
static int s_head, s_count;
static bool s_busy;             // s_jobs[s_head] is being written
static uint8_t *s_rx;
static size_t s_rx_len;
static bool s_rx_active;

void web_platform_run(void);

EM_JS(void, js_nano_write, (const uint8_t *data, int len), {
    Module.nanoWrite(HEAPU8.slice(data, data + len));
});

static void log_hex(const char *prefix, const uint8_t *d, size_t n)
{
    char line[3 * 48 + 8] = "";
    size_t shown = n < 48 ? n : 48;
    for (size_t i = 0; i < shown; i++) sprintf(line + 3 * i, "%02X ", d[i]);
    printf("I (%lu) %s: %s (%u bytes): %s%s\n", (unsigned long)esp_log_timestamp(), TAG, prefix, (unsigned)n, line,
           n > shown ? "..." : "");
}

// ---- receiving ----

static void deliver(const uint8_t *body, size_t n)
{
    if (n < 4 || !s_on_message) return;
    uint32_t type = body[n - 4] | (body[n - 3] << 8) | (body[n - 2] << 16) | ((uint32_t)body[n - 1] << 24);
    s_on_message(type, body, n - 4);
}

static void on_packet(const uint8_t *d, size_t n)
{
    if (n < 2) return;
    uint8_t flags = d[1];
    if (flags == 0xC0) {   // complete single-packet message
        deliver(d + 2, n - 2);
        return;
    }
    if (flags & 0x40) {    // first packet of a longer message
        s_rx_len = 0;
        s_rx_active = true;
    }
    if (!s_rx_active) {
        ESP_LOGW(TAG, "Packet without start ignored (flags %02X)", flags);
        return;
    }
    if (s_rx_len + n - 2 > MAX_MESSAGE) {
        ESP_LOGE(TAG, "Message larger than %d bytes dropped", MAX_MESSAGE);
        s_rx_active = false;
        return;
    }
    memcpy(s_rx + s_rx_len, d + 2, n - 2);
    s_rx_len += n - 2;
    if (flags & 0x80) {    // last packet
        s_rx_active = false;
        deliver(s_rx, s_rx_len);
    }
}

EMSCRIPTEN_KEEPALIVE void web_nano_packet(const uint8_t *d, int n)
{
    on_packet(d, (size_t)n);
    web_platform_run();
}

// ---- writing ----

static uint32_t frame_type(const uint8_t *d, size_t n)
{
    return n < 6 ? 0 : d[n - 4] | (d[n - 3] << 8) | (d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
}

// The control a value frame sets (type << 16 | control), or -1 for frames that are not values (as in nano_link.c).
static int32_t value_key(const uint8_t *frame, size_t len)
{
    uint32_t type = frame_type(frame, len);
    if (type == 99 && len >= 10 && frame[2] == 0x08 && frame[4] == 0x18 && frame[6] == 0x20) {   // 08 01 18 slot 20 param
        return (int32_t)(type << 16 | frame[5] << 8 | frame[7]);
    }
    if (type == 26 && len >= 8 && frame[2] == 0x18) return (int32_t)(type << 16 | frame[3]);       // 18 id 20 value
    if (type == 94 && len >= 11) return (int32_t)(type << 16 | frame[2]);                           // field tag, float
    return -1;
}

static void clear_queue(void)
{
    for (int i = 0; i < s_count; i++) free(s_jobs[(s_head + i) % QUEUE_MAX].data);
    s_head = s_count = 0;
    s_busy = false;
}

static void kick(void)
{
    if (s_busy || !s_count || !s_ready) return;
    job_t *job = &s_jobs[s_head];
    if (job->key < 0) log_hex(job->label, job->data, job->len);   // values are not logged (there are many)
    s_busy = true;
    js_nano_write(job->data, job->len);
}

EMSCRIPTEN_KEEPALIVE void web_nano_written(int ok)
{
    if (s_busy && s_count) {
        job_t *job = &s_jobs[s_head];
        if (!ok) ESP_LOGW(TAG, "Write failed: %s", job->label);
        free(job->data);
        s_head = (s_head + 1) % QUEUE_MAX;
        s_count--;
    }
    s_busy = false;
    kick();
    web_platform_run();
}

bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    if (len + 6 > 257) return false;   // the length byte covers everything after the first two bytes
    if (!s_ready) {
        ESP_LOGW(TAG, "Not connected - dropped: %s", label);
        return true;
    }
    uint16_t n = (uint16_t)(len + 6);
    uint8_t *data = malloc(n);
    if (!data) return false;
    data[0] = (uint8_t)(n - 2);
    data[1] = 0xC0;
    memcpy(data + 2, payload, len);
    for (int i = 0; i < 4; i++) data[2 + len + i] = (uint8_t)(type >> (8 * i));
    int32_t key = value_key(data, n);
    if (key >= 0) {   // a newer value of a control that still waits replaces it (in its place in the queue)
        for (int i = s_busy ? 1 : 0; i < s_count; i++) {
            job_t *job = &s_jobs[(s_head + i) % QUEUE_MAX];
            if (job->key == key) {
                free(job->data);
                job->data = data;
                job->len = n;
                return true;
            }
        }
    }
    if (s_count == QUEUE_MAX) {
        free(data);
        return false;
    }
    job_t *job = &s_jobs[(s_head + s_count) % QUEUE_MAX];
    job->data = data;
    job->len = n;
    job->key = key;
    snprintf(job->label, sizeof(job->label), "%s", label);
    s_count++;
    kick();
    return true;
}

// ---- connection ----

EMSCRIPTEN_KEEPALIVE void web_nano_link(int ready)
{
    s_ready = ready != 0;
    s_rx_active = false;
    clear_queue();
    ESP_LOGI(TAG, "%s", s_ready ? "Notifications on - link ready" : "Disconnected");
    if (s_on_link) s_on_link(s_ready);
    web_platform_run();
}

void nano_link_start(nano_message_cb on_message, nano_link_cb on_link)
{
    s_on_message = on_message;
    s_on_link = on_link;
    s_rx = malloc(MAX_MESSAGE);
}

bool nano_link_ready(void)
{
    return s_ready;
}

void nano_link_log_status(void)
{
    ESP_LOGI(TAG, "Web Bluetooth: %s, %d writes waiting", s_ready ? "connected" : "not connected", s_count);
}

// The app bridge and other Bluetooth clients do not exist in the browser.
bool nano_link_send_frame(const uint8_t *frame, size_t len) { (void)frame; (void)len; return false; }
void nano_link_set_forward(nano_forward_cb cb) { (void)cb; }
uint8_t nano_link_own_addr_type(void) { return 0; }
void nano_link_set_adv_hook(nano_link_adv_cb cb) { (void)cb; }
void nano_link_scan_request(bool on, bool fast) { (void)on; (void)fast; }
bool nano_link_connect_other(const ble_addr_t *addr, ble_gap_event_fn *cb, void *arg) { (void)addr; (void)cb; (void)arg; return false; }
void nano_link_other_connect_done(void) {}
