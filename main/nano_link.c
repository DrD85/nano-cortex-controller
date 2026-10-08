#include "nano_link.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_timer.h"
#include "app_link.h"

static const char *TAG = "nano_link";

#define SVC_A002 0xA002
#define SVC_A003 0xA003
#define CHR_C304 0xC304
#define CHR_C305 0xC305
#define DSC_CCCD 0x2902

#define MAX_MESSAGE (256 * 1024)  // the capture/IR library is the largest reply; the buffer lives in PSRAM
#define MAX_PACKET 520

// Not exported by a public header in ESP-IDF; the NimBLE examples declare it the same way.
void ble_store_config_init(void);

typedef struct {
    char label[40];
    uint16_t len;
    uint8_t *data;                      // NULL: send the newest value of s_values[value]
    uint8_t owner;                      // NANO_OWNER_BOARD / NANO_OWNER_APP
    int8_t value;                       // index in s_values, -1 = none
} write_job_t;

// Replies carry no request id. The Nano answers in order, so every request with a known reply type is noted
// with its sender; a reply goes to the sender of the oldest open request of its type. Messages the Nano sends
// on its own (preset changed on the pedal, tuner, ...) go to both.
typedef struct {
    uint32_t reply;
    uint8_t owner;
    int64_t at_us;
} pending_t;
#define PENDING_MAX 24
#define PENDING_TIMEOUT_US (5 * 1000 * 1000)
static pending_t s_pending[PENDING_MAX];
static int s_pending_count;
static SemaphoreHandle_t s_pending_lock;
static nano_forward_cb s_forward;

// Values of a control (FX parameter 99, value message 26, cab setting 94) arrive faster than the Nano takes them
// (every write waits for its response). A value still waiting in the queue is replaced by a newer one for the same
// control, so a slider never builds up a backlog; the write keeps its place among the other messages.
#define VALUE_SLOTS 12
static struct {
    bool queued;                        // its job waits in the queue
    uint32_t key;                       // message type << 16 | control
    uint8_t owner, len;
    uint8_t data[32];
    char label[40];
} s_values[VALUE_SLOTS];
static SemaphoreHandle_t s_values_lock;

// Connection interval 15 ms (NimBLE's default is 30-50 ms; with a range the Nano takes the slow end). Every write
// waits for the Nano's response (C304 takes no writes without response), so the interval sets how fast values
// follow a slider and how fast a preset changes.
static const struct ble_gap_conn_params NANO_CONN_PARAMS = {
    .scan_itvl = 0x0010,
    .scan_window = 0x0010,
    .itvl_min = 0x000C,               // units of 1.25 ms
    .itvl_max = 0x000C,
    .latency = 0,
    .supervision_timeout = 0x0100,    // units of 10 ms
    .min_ce_len = 0,
    .max_ce_len = 0,
};

static nano_message_cb s_on_message;
static nano_link_cb s_on_link;
static uint8_t s_own_addr_type;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start[2], s_svc_end[2];   // [0] = A002, [1] = A003
static int s_svc_index;
static uint16_t s_c304, s_c305, s_c305_cccd;
static bool s_c304_no_rsp;   // C304 takes writes without response: values are sent that way (no round trip)
static volatile bool s_ready;
static bool s_security_tried;

// Scanning is shared: it runs while the Nano is not connected or another client (MIDI) asks for it.
static bool s_synced;
static volatile bool s_connecting;                 // a GAP connect procedure is running (Nano or other)
static volatile bool s_scan_other, s_scan_other_fast;
static int s_scan_mode;                            // running scan: 0 none, 1 fast, 2 slow
static nano_link_adv_cb s_adv_hook;

// The controller's writes go first: the app's (mostly reads after a change) wait while the controller has something
// to send, so footswitches stay as quick as without the app.
static QueueHandle_t s_write_queue, s_app_queue;
static SemaphoreHandle_t s_write_ready;   // one count per queued job
static SemaphoreHandle_t s_write_done;
static volatile int s_write_status;

static uint8_t *s_rx;
static size_t s_rx_len;
static bool s_rx_active;

static int gap_event(struct ble_gap_event *event, void *arg);
static void discover_characteristics(void);
static void scan_update(void);

// ---- helpers ----

// True for a 16-bit UUID or its 128-bit Bluetooth base form (0000xxxx-0000-1000-8000-00805f9b34fb).
static bool uuid_is(const ble_uuid_t *u, uint16_t v)
{
    if (u->type == BLE_UUID_TYPE_16) return ble_uuid_u16(u) == v;
    if (u->type == BLE_UUID_TYPE_128) {
        static const uint8_t base[12] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };
        const uint8_t *b = ((const ble_uuid128_t *)u)->value;
        return memcmp(b, base, sizeof(base)) == 0 && b[12] == (v & 0xff) && b[13] == (v >> 8) && b[14] == 0 && b[15] == 0;
    }
    return false;
}

static void log_hex(const char *prefix, const uint8_t *d, size_t n)
{
    char line[3 * 48 + 8];
    size_t shown = n < 48 ? n : 48, pos = 0;
    for (size_t i = 0; i < shown; i++) pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", d[i]);
    if (shown < n) snprintf(line + pos, sizeof(line) - pos, "...");
    ESP_LOGI(TAG, "%s (%u bytes): %s", prefix, (unsigned)n, line);
}

static void fail_link(const char *why, int rc)
{
    ESP_LOGE(TAG, "%s (%d) - disconnecting", why, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

// ---- scanning ----

// Starts, stops or changes the scan to what is needed now: fast while the Nano is missing or a device list is
// shown, slow (10 % of the radio time) while another client only waits for its stored device.
static void scan_update(void)
{
    if (!s_synced) return;
    int want = 0;
    if (!s_connecting) {
        // While the Nano link is being set up (connect, MTU, discovery) the radio belongs to it.
        bool nano_busy = s_conn != BLE_HS_CONN_HANDLE_NONE && !s_ready;
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || (s_scan_other && s_scan_other_fast && !nano_busy)) want = 1;
        else if (s_scan_other && !nano_busy) want = 2;
    }
    if (want == s_scan_mode && (want == 0) == !ble_gap_disc_active()) return;
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    s_scan_mode = 0;
    if (!want) return;
    struct ble_gap_disc_params params = { 0 };
    params.filter_duplicates = 1;
    params.passive = 0;   // active scan: the name may only be in the scan response
    if (want == 2) {
        params.itvl = 0x00A0;     // 100 ms
        params.window = 0x0010;   // 10 ms
    }
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan start failed: %d", rc);
        return;
    }
    s_scan_mode = want;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) ESP_LOGI(TAG, "Scanning for the Nano Cortex (Bluetooth on, Cortex Cloud and the editor disconnected)...");
}

static void start_scan(void)
{
    scan_update();
}

void nano_link_set_adv_hook(nano_link_adv_cb cb)
{
    s_adv_hook = cb;
}

void nano_link_scan_request(bool on, bool fast)
{
    s_scan_other = on;
    s_scan_other_fast = fast;
    scan_update();
}

bool nano_link_connect_other(const ble_addr_t *addr, ble_gap_event_fn *cb, void *arg)
{
    if (s_connecting || !s_synced) return false;
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    s_scan_mode = 0;
    s_connecting = true;
    int rc = ble_gap_connect(s_own_addr_type, addr, 10000, NULL, cb, arg);
    if (rc != 0) {
        ESP_LOGE(TAG, "Connect not started: %d", rc);
        s_connecting = false;
        scan_update();
        return false;
    }
    return true;
}

void nano_link_other_connect_done(void)
{
    s_connecting = false;
    scan_update();
}

static bool is_nano(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        uint16_t u = ble_uuid_u16(&f->uuids16[i].u);
        if (u == SVC_A002 || u == SVC_A003) return true;
    }
    if (f->name && f->name_len) {
        char name[32];
        size_t n = f->name_len < sizeof(name) - 1 ? f->name_len : sizeof(name) - 1;
        for (size_t i = 0; i < n; i++) name[i] = (char)tolower(f->name[i]);
        name[n] = 0;
        if (strstr(name, "nano cortex")) return true;   // "Mini Board Nano Cortex" (not other "nano" devices)
    }
    return false;
}

// Logs every named device once, so a Nano with an unexpected name can still be identified.
static void log_seen(const struct ble_hs_adv_fields *f, const ble_addr_t *addr, int8_t rssi)
{
    static ble_addr_t seen[24];
    static int count;
    if (!f->name || !f->name_len) return;
    for (int i = 0; i < count; i++) if (ble_addr_cmp(&seen[i], addr) == 0) return;
    if (count < (int)(sizeof(seen) / sizeof(seen[0]))) seen[count++] = *addr;
    ESP_LOGI(TAG, "Seen: \"%.*s\" (RSSI %d)", f->name_len, (const char *)f->name, rssi);
}

// ---- GATT discovery and subscription ----

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0) {
        s_ready = true;
        ESP_LOGI(TAG, "Notifications on - link ready");
        if (s_on_link) s_on_link(true);
        scan_update();
        return 0;
    }
    if ((err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
         err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) && !s_security_tried) {
        s_security_tried = true;
        ESP_LOGW(TAG, "The Nano asks for pairing - starting pairing");
        ble_gap_security_initiate(conn);
        return 0;
    }
    fail_link("Enabling notifications failed", err->status);
    return 0;
}

static void subscribe(void)
{
    static const uint8_t on[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(s_conn, s_c305_cccd, on, sizeof(on), on_subscribed, NULL);
    if (rc != 0) fail_link("Subscribe write not started", rc);
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        if (!s_c305_cccd && dsc->handle > s_c305 && uuid_is(&dsc->uuid.u, DSC_CCCD)) s_c305_cccd = dsc->handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_c305_cccd) {
            s_c305_cccd = s_c305 + 1;   // the usual layout
            ESP_LOGW(TAG, "No CCCD found for C305, trying handle %u", s_c305_cccd);
        }
        subscribe();
        return 0;
    }
    fail_link("Descriptor discovery failed", err->status);
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (uuid_is(&chr->uuid.u, CHR_C304)) {
            s_c304 = chr->val_handle;
            s_c304_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
            ESP_LOGI(TAG, "C304 properties 0x%02X (write without response %s)", chr->properties, s_c304_no_rsp ? "yes" : "no");
        }
        if (uuid_is(&chr->uuid.u, CHR_C305)) s_c305 = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (s_c304 && s_c305) {
            ESP_LOGI(TAG, "Found C304 (handle %u) and C305 (handle %u) in service %s", s_c304, s_c305, s_svc_index ? "A003" : "A002");
            int rc = ble_gattc_disc_all_dscs(s_conn, s_c305, s_svc_end[s_svc_index], on_descriptor, NULL);
            if (rc != 0) fail_link("Descriptor discovery not started", rc);
        } else if (s_svc_index == 0 && s_svc_start[1]) {
            s_svc_index = 1;
            discover_characteristics();
        } else {
            fail_link("C304/C305 not found", 0);
        }
        return 0;
    }
    fail_link("Characteristic discovery failed", err->status);
    return 0;
}

static void discover_characteristics(void)
{
    int rc = ble_gattc_disc_all_chrs(s_conn, s_svc_start[s_svc_index], s_svc_end[s_svc_index], on_characteristic, NULL);
    if (rc != 0) fail_link("Characteristic discovery not started", rc);
}

static int on_service(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        for (int i = 0; i < 2; i++) {
            if (uuid_is(&svc->uuid.u, i ? SVC_A003 : SVC_A002)) {
                s_svc_start[i] = svc->start_handle;
                s_svc_end[i] = svc->end_handle;
            }
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_svc_start[0] && !s_svc_start[1]) { fail_link("Service A002/A003 not found", 0); return 0; }
        s_svc_index = s_svc_start[0] ? 0 : 1;
        discover_characteristics();
        return 0;
    }
    fail_link("Service discovery failed", err->status);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) ESP_LOGI(TAG, "MTU %u", mtu);
    else ESP_LOGW(TAG, "MTU exchange failed (%d), continuing with the default", err->status);
    int rc = ble_gattc_disc_all_svcs(s_conn, on_service, NULL);
    if (rc != 0) fail_link("Service discovery not started", rc);
    return 0;
}

// ---- incoming packets ----

static uint32_t reply_type(uint32_t request)
{
    switch (request) {
    case 1: return 2;      // state
    case 29: return 30;    // preset change
    case 60: return 61;    // expression assignments
    case 65: return 66;    // settings
    case 67: return 68;    // update settings
    case 76: return 77;    // library
    case 78: return 79;    // load IR
    case 80: return 81;    // load capture
    case 95: return 96;    // cab settings
    case 111: return 112;  // rename
    case 137: return 138;  // FX parameters
    default: return 0;
    }
}

static void note_request(uint32_t request, uint8_t owner)
{
    uint32_t reply = reply_type(request);
    if (!reply) return;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    if (s_pending_count == PENDING_MAX) {
        memmove(s_pending, s_pending + 1, sizeof(s_pending[0]) * (PENDING_MAX - 1));
        s_pending_count--;
    }
    s_pending[s_pending_count++] = (pending_t){ .reply = reply, .owner = owner, .at_us = esp_timer_get_time() };
    xSemaphoreGive(s_pending_lock);
}

// Owner of a reply (and the open request is closed), or NANO_OWNER_BOTH for messages nobody asked for.
static int route(uint32_t type)
{
    int owner = NANO_OWNER_BOTH;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    int keep = 0;
    for (int i = 0; i < s_pending_count; i++) {   // drop requests that never got an answer
        if (now - s_pending[i].at_us < PENDING_TIMEOUT_US) s_pending[keep++] = s_pending[i];
    }
    s_pending_count = keep;
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending[i].reply != type) continue;
        owner = s_pending[i].owner;
        memmove(s_pending + i, s_pending + i + 1, sizeof(s_pending[0]) * (size_t)(s_pending_count - i - 1));
        s_pending_count--;
        break;
    }
    xSemaphoreGive(s_pending_lock);
    return owner;
}

static void deliver(const uint8_t *body, size_t n)
{
    if (n < 4) return;
    uint32_t type = body[n - 4] | (body[n - 3] << 8) | (body[n - 2] << 16) | ((uint32_t)body[n - 1] << 24);
    int owner = route(type);
    if (owner != NANO_OWNER_APP && s_on_message) s_on_message(type, body, n - 4);
    if (owner != NANO_OWNER_BOARD && s_forward) s_forward(body, n);
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

// ---- writes ----

static int on_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    s_write_status = err->status;
    xSemaphoreGive(s_write_done);
    return 0;
}

static uint32_t frame_type(const uint8_t *d, size_t n)
{
    return n < 6 ? 0 : d[n - 4] | (d[n - 3] << 8) | (d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
}

static void write_frame(const char *label, const uint8_t *data, size_t len, uint8_t owner, bool log)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "Not connected - dropped: %s", label);
        return;
    }
    xSemaphoreTake(s_write_done, 0);
    if (log) log_hex(label, data, len);
    note_request(frame_type(data, len), owner);
    int rc = ble_gattc_write_flat(s_conn, s_c304, data, len, on_written, NULL);
    if (rc != 0) ESP_LOGE(TAG, "Write not started (%d): %s", rc, label);
    else if (xSemaphoreTake(s_write_done, pdMS_TO_TICKS(3000)) != pdTRUE) ESP_LOGW(TAG, "No write response: %s", label);
    else if (s_write_status != 0) ESP_LOGW(TAG, "Write failed (%d): %s", s_write_status, label);
}

// A value without waiting for the Nano's response (if C304 allows it): the next one can follow at once.
// The host may be out of buffers for a moment; then it is tried again shortly.
static void write_value(const char *label, const uint8_t *data, size_t len)
{
    if (!s_ready) return;
    if (!s_c304_no_rsp) {
        write_frame(label, data, len, NANO_OWNER_BOARD, false);
        return;
    }
    int rc, tries = 0;
    while ((rc = ble_gattc_write_no_rsp_flat(s_conn, s_c304, data, len)) == BLE_HS_ENOMEM && ++tries < 50) vTaskDelay(pdMS_TO_TICKS(4));
    if (rc != 0) ESP_LOGW(TAG, "Value not sent (%d): %s", rc, label);
}

// The control a value frame sets (type << 16 | control), or -1 for frames that are not values.
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

static bool enqueue(const write_job_t *job)
{
    if (xQueueSend(job->owner == NANO_OWNER_APP ? s_app_queue : s_write_queue, job, 0) != pdTRUE) return false;
    xSemaphoreGive(s_write_ready);
    return true;
}

// Queues a value frame, or replaces the value of the same control that still waits.
// 1 = queued or replaced, 0 = not a value (queue it as a message), -1 = the queue is full.
static int queue_value(const uint8_t *frame, size_t len, const char *label, uint8_t owner)
{
    int32_t key = value_key(frame, len);
    if (key < 0 || len > sizeof(s_values[0].data)) return 0;
    int index = -1, free_index = -1;
    xSemaphoreTake(s_values_lock, portMAX_DELAY);
    for (int i = 0; i < VALUE_SLOTS; i++) {
        if (s_values[i].queued && s_values[i].key == (uint32_t)key) index = i;
        else if (!s_values[i].queued && free_index < 0) free_index = i;
    }
    bool replace = index >= 0;
    if (!replace) index = free_index;
    if (index >= 0) {
        s_values[index].queued = true;
        s_values[index].key = (uint32_t)key;
        s_values[index].owner = owner;
        s_values[index].len = (uint8_t)len;
        memcpy(s_values[index].data, frame, len);
        strlcpy(s_values[index].label, label, sizeof(s_values[index].label));
    }
    xSemaphoreGive(s_values_lock);
    if (index < 0) return 0;   // all slots busy: an ordinary message
    if (replace) return 1;
    write_job_t job = { .data = NULL, .owner = owner, .value = (int8_t)index };
    if (enqueue(&job)) return 1;
    xSemaphoreTake(s_values_lock, portMAX_DELAY);
    s_values[index].queued = false;
    xSemaphoreGive(s_values_lock);
    return -1;
}

static void writer_task(void *arg)
{
    write_job_t job;
    uint8_t value[32], value_len;
    char label[40];
    for (;;) {
        xSemaphoreTake(s_write_ready, portMAX_DELAY);
        if (xQueueReceive(s_write_queue, &job, 0) != pdTRUE && xQueueReceive(s_app_queue, &job, 0) != pdTRUE) continue;
        if (job.value >= 0) {   // the newest value of this control
            xSemaphoreTake(s_values_lock, portMAX_DELAY);
            value_len = s_values[job.value].len;
            memcpy(value, s_values[job.value].data, value_len);
            strlcpy(label, s_values[job.value].label, sizeof(label));
            s_values[job.value].queued = false;
            xSemaphoreGive(s_values_lock);
            write_value(label, value, value_len);   // values are not logged (there are many of them)
            continue;
        }
        write_frame(job.label, job.data, job.len, job.owner, job.owner == NANO_OWNER_BOARD);
        free(job.data);
    }
}

bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    if (len + 6 > 257) return false;   // the length byte covers everything after the first two bytes
    write_job_t job = { .len = (uint16_t)(len + 6), .value = -1 };
    job.data = malloc(job.len);
    if (!job.data) return false;
    job.data[0] = (uint8_t)(job.len - 2);
    job.data[1] = 0xC0;
    memcpy(job.data + 2, payload, len);
    for (int i = 0; i < 4; i++) job.data[2 + len + i] = (uint8_t)(type >> (8 * i));
    strlcpy(job.label, label, sizeof(job.label));
    job.owner = NANO_OWNER_BOARD;
    int queued = queue_value(job.data, job.len, label, NANO_OWNER_BOARD);
    if (queued) {
        free(job.data);
        return queued > 0;
    }
    if (!enqueue(&job)) {
        free(job.data);
        return false;
    }
    return true;
}

bool nano_link_send_frame(const uint8_t *frame, size_t len)
{
    if (len < 6 || len > 300) return false;
    uint32_t type = frame_type(frame, len);
    char label[40];
    snprintf(label, sizeof(label), "App message %lu", (unsigned long)type);
    int queued = queue_value(frame, len, label, NANO_OWNER_APP);   // values: only the newest per control
    if (queued) return queued > 0;
    write_job_t job = { .len = (uint16_t)len, .owner = NANO_OWNER_APP, .value = -1 };
    job.data = malloc(len);
    if (!job.data) return false;
    memcpy(job.data, frame, len);
    snprintf(job.label, sizeof(job.label), "App message %lu", (unsigned long)type);
    if (!enqueue(&job)) {
        free(job.data);
        ESP_LOGW(TAG, "Write queue full - app message %lu dropped", (unsigned long)type);
        return false;
    }
    return true;
}

void nano_link_set_forward(nano_forward_cb cb)
{
    s_forward = cb;
}

uint8_t nano_link_own_addr_type(void)
{
    return s_own_addr_type;
}

bool nano_link_ready(void)
{
    return s_ready;
}

// ---- GAP ----

static void reset_link(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start[0] = s_svc_start[1] = s_svc_end[0] = s_svc_end[1] = 0;
    s_c304 = s_c305 = s_c305_cccd = 0;
    s_c304_no_rsp = false;
    s_ready = false;
    s_security_tried = false;
    s_rx_active = false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) return 0;
        log_seen(&fields, &event->disc.addr, event->disc.rssi);
        if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_connecting && is_nano(&fields)) {
            ESP_LOGI(TAG, "Nano Cortex found - connecting");
            ble_gap_disc_cancel();
            s_scan_mode = 0;
            s_connecting = true;
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, &NANO_CONN_PARAMS, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Connect not started: %d", rc);
                s_connecting = false;
                start_scan();
            }
            return 0;
        }
        if (s_adv_hook) s_adv_hook(&event->disc.addr, event->disc.rssi, &fields);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scan_mode = 0;
        start_scan();
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
            start_scan();
            return 0;
        }
        reset_link();
        s_conn = event->connect.conn_handle;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_conn, &desc) == 0) ESP_LOGI(TAG, "Connection interval %.2f ms", desc.conn_itvl * 1.25f);
        ESP_LOGI(TAG, "Connected - exchanging MTU");
        ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        start_scan();   // continues only if another client needs it
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected (reason 0x%X)", event->disconnect.reason);
        reset_link();
        s_write_status = BLE_HS_ENOTCONN;
        xSemaphoreGive(s_write_done);
        if (s_on_link) s_on_link(false);
        start_scan();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Paired / encrypted - enabling notifications again");
            subscribe();
        } else {
            fail_link("Pairing failed", event->enc_change.status);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The Nano forgot an old bond: delete ours and pair again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (event->conn_update.status == 0 && ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Connection interval now %.2f ms", desc.conn_itvl * 1.25f);
        }
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != s_c305) return 0;
        static uint8_t packet[MAX_PACKET];
        uint16_t n = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (n > sizeof(packet)) {
            ESP_LOGE(TAG, "Packet of %u bytes too large", n);
            return 0;
        }
        os_mbuf_copydata(event->notify_rx.om, 0, n, packet);
        on_packet(packet, n);
        return 0;
    }
    default:
        return 0;
    }
}

// ---- host ----

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    ESP_LOGI(TAG, "Bluetooth ready");
    start_scan();
}

void nano_link_log_status(void)
{
    ESP_LOGI(TAG, "Status: Bluetooth %s, scan %d (%s), Nano %s%s", s_synced ? "ready" : "NOT READY", s_scan_mode,
             ble_gap_disc_active() ? "running" : "off", s_conn == BLE_HS_CONN_HANDLE_NONE ? "not connected" : "connected",
             s_connecting ? ", connecting" : "");
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void nano_link_start(nano_message_cb on_message, nano_link_cb on_link)
{
    s_on_message = on_message;
    s_on_link = on_link;
    s_rx = heap_caps_malloc(MAX_MESSAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_write_queue = xQueueCreate(32, sizeof(write_job_t));
    s_app_queue = xQueueCreate(32, sizeof(write_job_t));
    s_write_ready = xSemaphoreCreateCounting(64, 0);
    s_write_done = xSemaphoreCreateBinary();
    s_pending_lock = xSemaphoreCreateMutex();
    s_values_lock = xSemaphoreCreateMutex();
    configASSERT(s_rx && s_write_queue && s_app_queue && s_write_ready && s_write_done && s_pending_lock && s_values_lock);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    app_link_register();   // GATT server for the app bridge (before the host starts)

    xTaskCreate(writer_task, "nano_tx", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(host_task);
}
