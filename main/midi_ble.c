#include "midi_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "nano_link.h"
#include "nvs.h"

static const char *TAG = "midi_ble";

#define STORE_NAMESPACE "nano"
#define STORE_KEY "midi_dev"

// BLE MIDI service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 and its characteristic
// 7772E5DB-3868-4112-A1A9-F2669D106BF3 (byte order as NimBLE stores 128-bit UUIDs).
static const ble_uuid128_t MIDI_SERVICE = BLE_UUID128_INIT(0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7,
                                                           0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);
static const ble_uuid128_t MIDI_CHAR = BLE_UUID128_INIT(0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1,
                                                        0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);

typedef struct {
    ble_addr_t addr;
    char name[32];
} stored_device_t;

typedef struct {
    ble_addr_t addr;
    char name[32];
    int8_t rssi;
    bool midi;      // advertises the MIDI service (the name may come in a separate scan response)
} found_t;

static midi_message_cb s_on_message;
static midi_change_cb s_on_change;
static SemaphoreHandle_t s_lock;

static stored_device_t s_stored;   // addr.type 0xFF = none
static bool s_have_stored;
static found_t s_found[MIDI_MAX_DEVICES * 2];
static int s_found_count;
static bool s_searching;
static volatile bool s_allowed = true;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_connecting, s_ready, s_security_tried;
static stored_device_t s_target;   // device being connected / connected
static uint16_t s_svc_end, s_chr, s_cccd;

static int gap_event(struct ble_gap_event *event, void *arg);

// ---- stored device ----

static void store_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = sizeof(s_stored);
    s_have_stored = nvs_get_blob(nvs, STORE_KEY, &s_stored, &size) == ESP_OK && size == sizeof(s_stored);
    nvs_close(nvs);
    if (s_have_stored) ESP_LOGI(TAG, "Stored MIDI device: %s", s_stored.name);
}

static void store_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    esp_err_t err = s_have_stored ? nvs_set_blob(nvs, STORE_KEY, &s_stored, sizeof(s_stored)) : nvs_erase_key(nvs, STORE_KEY);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) ESP_LOGW(TAG, "Could not store the MIDI device");
    nvs_close(nvs);
}

// Scanning needed: while searching, or slowly while the stored device is not connected.
static void update_scan(void)
{
    bool waiting = s_have_stored && s_conn == BLE_HS_CONN_HANDLE_NONE && !s_connecting;
    nano_link_scan_request(s_allowed && (s_searching || waiting), s_searching);
}

static void changed(void)
{
    if (s_on_change) s_on_change();
}

// ---- incoming MIDI ----

static int data_bytes(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    default: return -1;   // system messages are not used
    }
}

// BLE MIDI packet: header (bit 7 set, timestamp high), then messages, each preceded by a timestamp byte
// (bit 7 set) unless it continues with running status. SysEx and system messages are skipped.
void midi_ble_parse(const uint8_t *p, size_t n, midi_message_cb on_message)
{
    if (n < 2 || !(p[0] & 0x80)) return;
    uint8_t running = 0;
    bool sysex = false;
    size_t i = 1;
    while (i < n) {
        if (p[i] & 0x80) {                    // timestamp
            if (++i >= n) break;
            if (p[i] & 0x80) {                // status
                uint8_t status = p[i++];
                if (status == 0xF0) { sysex = true; continue; }
                if (status == 0xF7) { sysex = false; continue; }
                if (status >= 0xF8) continue;  // real-time (clock etc.)
                running = status;
                sysex = false;
            }
        }
        if (sysex || !running) {               // inside a SysEx or no status yet: skip the data byte
            i++;
            continue;
        }
        int count = data_bytes(running);
        if (count < 0 || i + (size_t)count > n) {
            running = 0;
            i++;
            continue;
        }
        uint8_t d1 = p[i], d2 = count == 2 ? p[i + 1] : 0;
        i += (size_t)count;
        if ((d1 | d2) & 0x80) continue;
        if (on_message) on_message(running, d1, d2);
    }
}

// ---- connection ----

static void drop(const char *why, int rc)
{
    ESP_LOGW(TAG, "%s (%d)", why, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0) {
        s_ready = true;
        ESP_LOGI(TAG, "MIDI device ready: %s", s_target.name);
        changed();
        return 0;
    }
    if ((err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
         err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) && !s_security_tried) {
        s_security_tried = true;
        ESP_LOGI(TAG, "MIDI device asks for pairing");
        ble_gap_security_initiate(conn);
        return 0;
    }
    drop("Enabling MIDI notifications failed", err->status);
    return 0;
}

static void subscribe(void)
{
    static const uint8_t on[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(s_conn, s_cccd, on, sizeof(on), on_subscribed, NULL);
    if (rc != 0) drop("Subscribe not started", rc);
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        if (!s_cccd && dsc->handle > s_chr && ble_uuid_u16(&dsc->uuid.u) == 0x2902) s_cccd = dsc->handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_cccd) s_cccd = s_chr + 1;
        subscribe();
        return 0;
    }
    drop("MIDI descriptor discovery failed", err->status);
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (!s_chr) s_chr = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_chr) {
            drop("No MIDI characteristic", 0);
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(s_conn, s_chr, s_svc_end, on_descriptor, NULL);
        if (rc != 0) drop("Descriptor discovery not started", rc);
        return 0;
    }
    drop("MIDI characteristic discovery failed", err->status);
    return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        int rc = ble_gattc_disc_chrs_by_uuid(conn, svc->start_handle, svc->end_handle, &MIDI_CHAR.u, on_characteristic, NULL);
        s_svc_end = svc->end_handle;
        if (rc != 0) drop("Characteristic discovery not started", rc);
        return BLE_HS_EDONE;   // only the first MIDI service
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_svc_end) drop("No MIDI service", 0);
        return 0;
    }
    drop("MIDI service discovery failed", err->status);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    int rc = ble_gattc_disc_svc_by_uuid(conn, &MIDI_SERVICE.u, on_service, NULL);
    if (rc != 0) drop("Service discovery not started", rc);
    return 0;
}

static void connect_device(const stored_device_t *dev)
{
    if (!s_allowed || s_connecting || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    s_target = *dev;
    s_connecting = true;
    ESP_LOGI(TAG, "Connecting MIDI device %s", dev->name);
    if (!nano_link_connect_other(&dev->addr, gap_event, NULL)) {
        s_connecting = false;
        update_scan();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        nano_link_other_connect_done();
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "MIDI connection failed: %d", event->connect.status);
            update_scan();
            changed();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_ready = s_security_tried = false;
        s_svc_end = s_chr = s_cccd = 0;
        ESP_LOGI(TAG, "MIDI device connected - discovering");
        ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        update_scan();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "MIDI device disconnected (reason 0x%X)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        // Switched to another device in the dialog: connect it now (it was already seen in this scan).
        if (s_have_stored && ble_addr_cmp(&s_stored.addr, &s_target.addr) != 0) connect_device(&s_stored);
        update_scan();
        changed();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) subscribe();
        else drop("MIDI pairing failed", event->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != s_chr) return 0;
        uint8_t packet[256];
        uint16_t n = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (n > sizeof(packet)) n = sizeof(packet);
        os_mbuf_copydata(event->notify_rx.om, 0, n, packet);
        midi_ble_parse(packet, n, s_on_message);
        return 0;
    }
    default:
        return 0;
    }
}

// ---- scanning ----

static bool has_midi_service(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids128; i++) {
        if (ble_uuid_cmp(&f->uuids128[i].u, &MIDI_SERVICE.u) == 0) return true;
    }
    return false;
}

static void on_adv(const ble_addr_t *addr, int8_t rssi, const struct ble_hs_adv_fields *f)
{
    bool midi = has_midi_service(f);
    char name[32] = "";
    if (f->name && f->name_len) {
        size_t n = f->name_len < sizeof(name) - 1 ? f->name_len : sizeof(name) - 1;
        memcpy(name, f->name, n);
        name[n] = 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    found_t *entry = NULL;
    for (int i = 0; i < s_found_count; i++) if (ble_addr_cmp(&s_found[i].addr, addr) == 0) entry = &s_found[i];
    bool news = false;
    if (!entry && (midi || name[0]) && s_found_count < (int)(sizeof(s_found) / sizeof(s_found[0]))) {
        entry = &s_found[s_found_count++];
        memset(entry, 0, sizeof(*entry));
        entry->addr = *addr;
    }
    if (entry) {
        entry->rssi = rssi;
        if (midi && !entry->midi) { entry->midi = true; news = true; }
        if (name[0] && strcmp(entry->name, name)) { strlcpy(entry->name, name, sizeof(entry->name)); news = entry->midi; }
    }
    bool stored = s_have_stored && entry && entry->midi &&
                  (ble_addr_cmp(addr, &s_stored.addr) == 0 || (entry->name[0] && !strcmp(entry->name, s_stored.name)));
    stored_device_t dev = { .addr = *addr };
    if (entry) strlcpy(dev.name, entry->name[0] ? entry->name : s_stored.name, sizeof(dev.name));
    xSemaphoreGive(s_lock);

    if (news) changed();
    if (stored) connect_device(&dev);   // the stored device is back
}

// ---- API ----

void midi_ble_start(midi_message_cb on_message, midi_change_cb on_change)
{
    s_on_message = on_message;
    s_on_change = on_change;
    s_lock = xSemaphoreCreateMutex();
    store_load();
    nano_link_set_adv_hook(on_adv);
    update_scan();
}

void midi_ble_allow(bool allowed)
{
    if (allowed == s_allowed) return;
    s_allowed = allowed;
    ESP_LOGI(TAG, "MIDI %s", allowed ? "allowed" : "paused while the Nano connects");
    update_scan();
}

void midi_ble_search(bool on)
{
    if (on) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_found_count = 0;   // a fresh list for the dialog
        xSemaphoreGive(s_lock);
    }
    s_searching = on;
    update_scan();
}

int midi_ble_devices(midi_device_t *out, int max)
{
    int count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_found_count && count < max; i++) {
        const found_t *f = &s_found[i];
        if (!f->midi) continue;
        strlcpy(out[count].name, f->name[0] ? f->name : "MIDI device", sizeof(out[count].name));
        out[count].rssi = f->rssi;
        out[count].remembered = s_have_stored && ble_addr_cmp(&f->addr, &s_stored.addr) == 0;
        count++;
    }
    xSemaphoreGive(s_lock);
    return count;
}

bool midi_ble_connect(int index)
{
    stored_device_t dev;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0, n = 0; i < s_found_count; i++) {
        if (!s_found[i].midi) continue;
        if (n++ != index) continue;
        dev.addr = s_found[i].addr;
        strlcpy(dev.name, s_found[i].name[0] ? s_found[i].name : "MIDI device", sizeof(dev.name));
        found = true;
        break;
    }
    xSemaphoreGive(s_lock);
    if (!found || s_connecting) return false;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && ble_addr_cmp(&dev.addr, &s_target.addr) == 0) return true;   // already connected
    s_stored = dev;
    s_have_stored = true;
    store_save();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);   // then the new one
    else connect_device(&dev);
    return true;
}

void midi_ble_forget(void)
{
    s_have_stored = false;
    store_save();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    update_scan();
    changed();
}

bool midi_ble_status(char *name, size_t size)
{
    if (s_ready) strlcpy(name, s_target.name, size);
    else strlcpy(name, s_have_stored ? s_stored.name : "", size);
    return s_ready;
}
