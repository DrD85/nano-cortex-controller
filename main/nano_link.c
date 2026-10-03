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
    uint8_t *data;
} write_job_t;

static nano_message_cb s_on_message;
static nano_link_cb s_on_link;
static uint8_t s_own_addr_type;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start[2], s_svc_end[2];   // [0] = A002, [1] = A003
static int s_svc_index;
static uint16_t s_c304, s_c305, s_c305_cccd;
static volatile bool s_ready;
static bool s_security_tried;

// Scanning is shared: it runs while the Nano is not connected or another client (MIDI) asks for it.
static bool s_synced;
static volatile bool s_connecting;                 // a GAP connect procedure is running (Nano or other)
static volatile bool s_scan_other, s_scan_other_fast;
static int s_scan_mode;                            // running scan: 0 none, 1 fast, 2 slow
static nano_link_adv_cb s_adv_hook;

static QueueHandle_t s_write_queue;
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
        if (uuid_is(&chr->uuid.u, CHR_C304)) s_c304 = chr->val_handle;
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

static void deliver(const uint8_t *body, size_t n)
{
    if (n < 4) return;
    uint32_t type = body[n - 4] | (body[n - 3] << 8) | (body[n - 2] << 16) | ((uint32_t)body[n - 1] << 24);
    if (s_on_message) s_on_message(type, body, n - 4);
}

static void on_packet(const uint8_t *d, size_t n)
{
    if (n < 2) return;
    uint8_t flags = d[1];
    if (flags != 0xC0) ESP_LOGI(TAG, "PKT n=%u b0=%02X b1=%02X b2=%02X", (unsigned)n, d[0], d[1], n > 2 ? d[2] : 0);   // TEMP stream format
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

static void writer_task(void *arg)
{
    write_job_t job;
    for (;;) {
        xQueueReceive(s_write_queue, &job, portMAX_DELAY);
        if (!s_ready) {
            ESP_LOGW(TAG, "Not connected - dropped: %s", job.label);
            free(job.data);
            continue;
        }
        xSemaphoreTake(s_write_done, 0);
        log_hex(job.label, job.data, job.len);
        int rc = ble_gattc_write_flat(s_conn, s_c304, job.data, job.len, on_written, NULL);
        if (rc != 0) ESP_LOGE(TAG, "Write not started (%d): %s", rc, job.label);
        else if (xSemaphoreTake(s_write_done, pdMS_TO_TICKS(3000)) != pdTRUE) ESP_LOGW(TAG, "No write response: %s", job.label);
        else if (s_write_status != 0) ESP_LOGW(TAG, "Write failed (%d): %s", s_write_status, job.label);
        free(job.data);
    }
}

bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    if (len + 6 > 257) return false;   // the length byte covers everything after the first two bytes
    write_job_t job = { .len = (uint16_t)(len + 6) };
    job.data = malloc(job.len);
    if (!job.data) return false;
    job.data[0] = (uint8_t)(job.len - 2);
    job.data[1] = 0xC0;
    memcpy(job.data + 2, payload, len);
    for (int i = 0; i < 4; i++) job.data[2 + len + i] = (uint8_t)(type >> (8 * i));
    strlcpy(job.label, label, sizeof(job.label));
    if (xQueueSend(s_write_queue, &job, 0) != pdTRUE) {
        free(job.data);
        return false;
    }
    return true;
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
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL);
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
    start_scan();
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
    s_write_queue = xQueueCreate(16, sizeof(write_job_t));
    s_write_done = xSemaphoreCreateBinary();
    configASSERT(s_rx && s_write_queue && s_write_done);

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

    xTaskCreate(writer_task, "nano_tx", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(host_task);
}
