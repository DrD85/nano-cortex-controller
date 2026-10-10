#include "phone_midi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"

static const char *TAG = "phone_midi";

// MIDI over Bluetooth LE (Apple / MMA): service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 with one characteristic
// 7772E5DB-3868-4112-A1A9-F2669D106BF3 (read, write without response, notify).
const ble_uuid128_t PHONE_MIDI_SERVICE_UUID =
    BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t CHR_UUID =
    BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static midi_message_cb s_on_message;
static phone_state_cb s_on_state;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_itvl;   // connection interval in 1.25 ms units
static bool s_asked_15;            // the 11.25 ms request was refused, 15 ms asked for
static uint16_t s_chr_handle;

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;   // a read answers with no data, as the specification says
    uint8_t packet[128];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, packet, sizeof(packet), &n) != 0) return 0;
    midi_ble_parse(packet, n, s_on_message);
    return 0;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &PHONE_MIDI_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &CHR_UUID.u, .access_cb = chr_access, .val_handle = &s_chr_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

void phone_midi_register(void)
{
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES));
}

void phone_midi_start(midi_message_cb on_message, phone_state_cb on_state)
{
    s_on_message = on_message;
    s_on_state = on_state;
}

uint16_t phone_midi_conn(void)
{
    return s_conn;
}

bool phone_midi_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

float phone_midi_interval_ms(void)
{
    return phone_midi_connected() ? s_itvl * 1.25f : 0;
}

// Apple: ask for 11.25 ms first, for 15 ms if that is refused; longer intervals are unsuitable for live playing.
static void request_interval(uint16_t conn, uint16_t units)
{
    struct ble_gap_upd_params params = { .itvl_min = units, .itvl_max = units, .latency = 0, .supervision_timeout = 0x00C8,
                                         .min_ce_len = 0, .max_ce_len = 0 };
    int rc = ble_gap_update_params(conn, &params);
    if (rc != 0) ESP_LOGW(TAG, "Connection interval %.2f ms not asked for (%d)", units * 1.25f, rc);
}

bool phone_midi_subscribe(uint16_t conn, uint16_t attr_handle, bool notify)
{
    if (attr_handle != s_chr_handle) return false;
    if (notify && s_conn != conn) {
        struct ble_gap_conn_desc desc;
        s_conn = conn;
        s_itvl = ble_gap_conn_find(conn, &desc) == 0 ? desc.conn_itvl : 0;
        s_asked_15 = false;
        ESP_LOGI(TAG, "Phone listens to MIDI (interval %.2f ms)", s_itvl * 1.25f);
        if (s_itvl > 9) request_interval(conn, 9);
        if (s_on_state) s_on_state(true);
    } else if (!notify && s_conn == conn) {
        phone_midi_disconnected(conn);
    }
    return true;
}

void phone_midi_disconnected(uint16_t conn)
{
    if (s_conn != conn) return;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_itvl = 0;
    ESP_LOGI(TAG, "Phone gone");
    if (s_on_state) s_on_state(false);
}

void phone_midi_conn_updated(uint16_t conn, int status)
{
    if (s_conn != conn) return;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) == 0) s_itvl = desc.conn_itvl;
    ESP_LOGI(TAG, "Phone connection interval %.2f ms%s", s_itvl * 1.25f, status ? " (request refused)" : "");
    if (s_itvl > 12 && !s_asked_15) {
        s_asked_15 = true;
        request_interval(conn, 12);
    }
    if (s_on_state) s_on_state(true);   // the interval is shown
}

bool phone_midi_send(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return false;
    // Header and timestamp (13 bits of milliseconds), then the message.
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t packet[5] = { (uint8_t)(0x80 | ((ms >> 7) & 0x3F)), (uint8_t)(0x80 | (ms & 0x7F)), status, data1, data2 };
    size_t len = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 4 : 5;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, len);
    return om && ble_gatts_notify_custom(conn, s_chr_handle, om) == 0;
}
