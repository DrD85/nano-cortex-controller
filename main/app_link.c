#include "app_link.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "nano_link.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "app_link";

#define NAME "Nano Cortex Controller"
#define MAX_PACKET_DATA 510   // the Nano's packets carry at most 510 bytes after the 2-byte header
// Bluetooth buffers (msys blocks) kept free while sending to the app. Without a reserve a long reply (the preset
// list, 18 KB) took all of them, and the app's next request was dropped for lack of memory ("ble_att_svr_pkt rc=6").
#define TX_RESERVE_BLOCKS 20

typedef struct {
    uint8_t *data;
    size_t len;
} tx_message_t;

static app_write_cb s_on_write;
static app_state_cb s_on_state;
static volatile bool s_enabled;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile uint16_t s_mtu = 23;
static uint16_t s_c305_handle;
static QueueHandle_t s_tx_queue;

static int gap_event(struct ble_gap_event *event, void *arg);

// ---- GATT service (as on the Nano) ----

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint8_t frame[300];
    uint16_t n = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(frame)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &n) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (n < 6 || frame[1] != 0xC0) return 0;
    uint32_t type = frame[n - 4] | (frame[n - 3] << 8) | (frame[n - 2] << 16) | ((uint32_t)frame[n - 1] << 24);
    if (type == 30) return 0;   // preset change acknowledgement: the controller answers the Nano itself
    nano_link_send_frame(frame, n);
    if (s_on_write) s_on_write(type);
    return 0;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xA002),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0xC304), .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0xC305), .access_cb = chr_access, .val_handle = &s_c305_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

void app_link_register(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(NAME);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES));
}

// ---- advertising ----

static void advertise(void)
{
    if (!s_enabled || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)NAME;
    fields.name_len = strlen(NAME);
    fields.name_is_complete = 1;
    static const ble_uuid16_t service = BLE_UUID16_INIT(0xA002);
    fields.uuids16 = &service;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising data not set: %d", rc);
        return;
    }
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x00A0;   // 100 ms
    params.itvl_max = 0x00F0;   // 150 ms
    rc = ble_gap_adv_start(nano_link_own_addr_type(), NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "Advertising not started: %d", rc);
    else ESP_LOGI(TAG, "Offered to the app as \"%s\"", NAME);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_subscribed = false;
        s_mtu = 23;
        ESP_LOGI(TAG, "App connected");
        if (!s_enabled) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        {
            // A shorter connection interval (Apple's limits: min >= 15 ms, max >= min + 15 ms, timeout 2-6 s): long
            // replies such as the preset list reach the app sooner. The app may keep its own choice.
            struct ble_gap_upd_params params = { .itvl_min = 0x000C, .itvl_max = 0x0018, .latency = 0,
                                                 .supervision_timeout = 0x01F4, .min_ce_len = 0, .max_ce_len = 0 };
            int rc = ble_gap_update_params(s_conn, &params);
            if (rc != 0) ESP_LOGW(TAG, "Connection interval request not sent (%d)", rc);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "App disconnected (reason 0x%X)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        if (s_on_state) s_on_state(false);
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_c305_handle) {
            bool was = s_subscribed;
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "App notifications %s (MTU %u)", s_subscribed ? "on" : "off", s_mtu);
            if (s_subscribed != was && s_on_state) s_on_state(s_subscribed);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_conn) s_mtu = event->mtu.value;
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (event->conn_update.status == 0 && ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "App connection interval %.2f ms", desc.conn_itvl * 1.25f);
        }
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

// ---- messages to the app ----

// Packets as the Nano sends them: [length low] [0x40 first | 0x80 last | length high] data.
static void tx_task(void *arg)
{
    tx_message_t m;
    uint8_t packet[MAX_PACKET_DATA + 2];
    for (;;) {
        xQueueReceive(s_tx_queue, &m, portMAX_DELAY);
        size_t max = s_mtu > 10 ? s_mtu - 7 : 16;
        if (max > MAX_PACKET_DATA) max = MAX_PACKET_DATA;
        for (size_t off = 0; off < m.len && s_subscribed;) {
            size_t chunk = m.len - off < max ? m.len - off : max;
            packet[0] = (uint8_t)chunk;
            packet[1] = (uint8_t)((off == 0 ? 0x40 : 0) | (off + chunk >= m.len ? 0x80 : 0) | ((chunk >> 8) & 0x3F));
            memcpy(packet + 2, m.data + off, chunk);
            // Wait until the host has sent enough of the earlier packets (at most a second).
            for (int wait = 0; os_msys_num_free() < TX_RESERVE_BLOCKS && wait < 200 && s_subscribed; wait++) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            int tries = 0, rc;
            do {   // the host may still be out of buffers for a moment
                struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, (uint16_t)(chunk + 2));
                rc = om ? ble_gatts_notify_custom(s_conn, s_c305_handle, om) : BLE_HS_ENOMEM;
                if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(5));
            } while (rc == BLE_HS_ENOMEM && ++tries < 200 && s_subscribed);
            if (rc != 0) {
                ESP_LOGW(TAG, "Packet to the app not sent (%d)", rc);
                break;
            }
            off += chunk;
        }
        free(m.data);
    }
}

void app_link_send_message(const uint8_t *message, size_t len)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE || !len) return;
    tx_message_t m = { .data = malloc(len), .len = len };
    if (!m.data) return;
    memcpy(m.data, message, len);
    if (xQueueSend(s_tx_queue, &m, 0) != pdTRUE) {
        free(m.data);
        ESP_LOGW(TAG, "Queue to the app full - message dropped");
    }
}

void app_link_send(uint32_t type, const uint8_t *payload, size_t len)
{
    uint8_t message[300];
    if (len + 4 > sizeof(message)) return;
    memcpy(message, payload, len);
    for (int i = 0; i < 4; i++) message[len + i] = (uint8_t)(type >> (8 * i));
    app_link_send_message(message, len + 4);
}

// ---- control ----

void app_link_start(app_write_cb on_write, app_state_cb on_state)
{
    s_on_write = on_write;
    s_on_state = on_state;
    s_tx_queue = xQueueCreate(24, sizeof(tx_message_t));
    configASSERT(s_tx_queue);
    xTaskCreate(tx_task, "app_tx", 3072, NULL, 5, NULL);
    nano_link_set_forward(app_link_send_message);
}

void app_link_enable(bool enabled)
{
    s_enabled = enabled;
    if (enabled) {
        advertise();
        return;
    }
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

bool app_link_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}
