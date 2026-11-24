// Bluetooth audio bridge for the P4 board, on a classic ESP32 (ESP-WROOM-32 NodeMCU): the phone connects to it as
// to a Bluetooth speaker ("Nano Board"); its audio goes out over I2S to the P4 (which resamples it to 48 kHz and plays
// it into the Nano over USB), and a UART line carries the track and the playing state to the P4 and the buttons back.
//
// Wiring (PLAN.md):  I2S  BCLK GPIO26 -> P4 GPIO46,  WS GPIO25 -> P4 GPIO47,  DATA GPIO22 -> P4 GPIO48
//                    UART TX GPIO17 -> P4 GPIO50 (RX),  RX GPIO16 <- P4 GPIO49 (TX),  115200 Bd;  EN <- P4 GPIO51
//
// UART lines (text, one per line, ending in "*" and two hex digits: the XOR of the bytes before the "*"):
//   to the P4:   C 1|0 <name>        phone connected / gone (its Bluetooth name)
//                S play|pause|stop   playing state
//                R <rate>            the stream's sample rate (44100 / 48000)
//                T <title>  A <artist>  L <album>  D <ms>   track, from AVRCP
//                P <ms>              position (every second while playing)
//   from the P4: PLAY  PAUSE  TOGGLE  NEXT  PREV           (AVRCP pass-through to the phone)
//
// STATUS: builds; not yet run on hardware.
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bridge";

#define DEVICE_NAME "Nano Board"
#define I2S_BCLK GPIO_NUM_26
#define I2S_WS GPIO_NUM_25
#define I2S_DOUT GPIO_NUM_22
#define LINK_UART UART_NUM_2
#define LINK_TX GPIO_NUM_17
#define LINK_RX GPIO_NUM_16
#define LINK_BAUD 115200

static i2s_chan_handle_t s_tx;
static int s_rate = 44100;
static bool s_playing, s_connected;
static esp_bd_addr_t s_peer;
static uint8_t s_tl;   // AVRCP transaction label

// ---- UART line to the P4 ----

static void line(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 6, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 6) n = sizeof(buf) - 6;
    for (int i = 0; i < n; i++) if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '*') buf[i] = ' ';
    uint8_t x = 0;
    for (int i = 0; i < n; i++) x ^= (uint8_t)buf[i];
    n += snprintf(buf + n, 6, "*%02X\n", x);
    uart_write_bytes(LINK_UART, buf, n);
}

static uint8_t next_tl(void)
{
    s_tl = (s_tl + 1) & 0x0F;
    return s_tl;
}

static void press(uint8_t key)
{
    esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

static void uart_task(void *arg)
{
    char cmd[32];
    int n = 0;
    while (true) {
        uint8_t c;
        if (uart_read_bytes(LINK_UART, &c, 1, pdMS_TO_TICKS(1000)) != 1) continue;
        if (c != '\n' && c != '\r') {
            if (n < (int)sizeof(cmd) - 1) cmd[n++] = (char)c;
            continue;
        }
        cmd[n] = 0;
        n = 0;
        if (!cmd[0] || !s_connected) continue;
        if (!strcmp(cmd, "PLAY")) press(ESP_AVRC_PT_CMD_PLAY);
        else if (!strcmp(cmd, "PAUSE")) press(ESP_AVRC_PT_CMD_PAUSE);
        else if (!strcmp(cmd, "TOGGLE")) press(s_playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
        else if (!strcmp(cmd, "NEXT")) press(ESP_AVRC_PT_CMD_FORWARD);
        else if (!strcmp(cmd, "PREV")) press(ESP_AVRC_PT_CMD_BACKWARD);
        else ESP_LOGW(TAG, "unknown command from the P4: %s", cmd);
    }
}

// ---- audio: A2DP -> I2S ----

static void i2s_start(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;   // silence when nothing comes (the P4 then fades out)
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

static void i2s_rate(int rate)
{
    if (rate == s_rate) return;
    s_rate = rate;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(s_tx);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
}

static void on_audio(const uint8_t *data, uint32_t len)
{
    size_t written;
    i2s_channel_write(s_tx, data, len, &written, pdMS_TO_TICKS(20));   // (never block the Bluetooth task for long)
}

static void on_a2dp(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            memcpy(s_peer, p->conn_stat.remote_bda, sizeof(s_peer));
            nvs_handle_t h;   // the last phone: connect to it again at the next start
            if (nvs_open("bridge", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_blob(h, "peer", s_peer, sizeof(s_peer));
                nvs_commit(h);
                nvs_close(h);
            }
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            line("C 1 %s", "Phone");
            esp_bt_gap_read_remote_name(s_peer);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_connected = s_playing = false;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            line("C 0");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        s_playing = p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        line("S %s", s_playing ? "play" : "pause");
        break;
    case ESP_A2D_AUDIO_CFG_EVT: {
        int rate = 16000;
        uint8_t sf = p->audio_cfg.mcc.cie.sbc_info.samp_freq;
        if (sf & ESP_A2D_SBC_CIE_SF_32K) rate = 32000;
        else if (sf & ESP_A2D_SBC_CIE_SF_44K) rate = 44100;
        else if (sf & ESP_A2D_SBC_CIE_SF_48K) rate = 48000;
        i2s_rate(rate);
        line("R %d", rate);
        break;
    }
    default:
        break;
    }
}

// ---- AVRCP: what plays, and the buttons ----

static void ask_metadata(void)
{
    esp_avrc_ct_send_metadata_cmd(next_tl(), ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM |
                                             ESP_AVRC_MD_ATTR_PLAYING_TIME);
}

static void on_avrc(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (p->conn_stat.connected) {
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_POS_CHANGED, 1);
            ask_metadata();
        }
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        char text[160];
        int n = p->meta_rsp.attr_length < (int)sizeof(text) - 1 ? p->meta_rsp.attr_length : (int)sizeof(text) - 1;
        memcpy(text, p->meta_rsp.attr_text, n);
        text[n] = 0;
        switch (p->meta_rsp.attr_id) {
        case ESP_AVRC_MD_ATTR_TITLE: line("T %s", text); break;
        case ESP_AVRC_MD_ATTR_ARTIST: line("A %s", text); break;
        case ESP_AVRC_MD_ATTR_ALBUM: line("L %s", text); break;
        case ESP_AVRC_MD_ATTR_PLAYING_TIME: line("D %s", text); break;
        default: break;
        }
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        switch (p->change_ntf.event_id) {   // (a notification is answered once: register it again)
        case ESP_AVRC_RN_TRACK_CHANGE:
            ask_metadata();
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
            break;
        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            s_playing = p->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING;
            line("S %s", s_playing ? "play" : p->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_STOPPED ? "stop" : "pause");
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            break;
        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            line("P %lu", (unsigned long)p->change_ntf.event_parameter.play_pos);
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_POS_CHANGED, 1);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

// ---- GAP: name of the phone ----

static void on_gap(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p)
{
    if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT && p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS && s_connected)
        line("C 1 %s", (const char *)p->read_rmt_name.rmt_name);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    const uart_config_t uc = { .baud_rate = LINK_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                               .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 512, 512, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX, LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    i2s_start();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));   // classic Bluetooth only
    esp_bt_controller_config_t bt = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_set_device_name(DEVICE_NAME);
    esp_bt_gap_register_callback(on_gap);
    esp_bt_sp_param_t param = ESP_BT_SP_IOCAP_MODE;   // pairing without a PIN ("just works")
    esp_bt_io_cap_t cap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param, &cap, sizeof(cap));

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(on_avrc);
    esp_a2d_register_callback(on_a2dp);
    esp_a2d_sink_register_data_callback(on_audio);
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    nvs_handle_t h;   // the last phone: try it again
    size_t size = sizeof(s_peer);
    if (nvs_open("bridge", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "peer", s_peer, &size) == ESP_OK) esp_a2d_sink_connect(s_peer);
        nvs_close(h);
    }
    xTaskCreate(uart_task, "uart", 3072, NULL, 5, NULL);
    line("C 0");
    ESP_LOGI(TAG, "Bluetooth audio bridge ready: \"%s\"", DEVICE_NAME);
}
