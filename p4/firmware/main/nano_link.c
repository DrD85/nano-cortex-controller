// P4 board: the one link to the Nano that main.c sees (nano_link.h), over USB while the Nano's cable is in the USB-A
// socket, otherwise over Bluetooth LE (the user's choice, 2026-11-19: Bluetooth only without the cable, and the board
// must work with Bluetooth alone).
//
// - At the start USB has START_GRACE_US to find the Nano; only then is Bluetooth allowed to connect it.
// - The cable comes in while the Nano is on Bluetooth: the Bluetooth link is dropped and the link goes over USB -
//   main.c sees it go down and come up and reads everything anew, as after any reconnect.
// - The cable goes out: after USB_GONE_US (the USB link also drops for a moment when it restarts its port) Bluetooth
//   looks for the Nano again.
// Messages are passed on only from the way that carries the link; the other one's are dropped.
#include "nano_link.h"
#include "nano_link_transport.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "nano_link";

#define START_GRACE_US (4 * 1000 * 1000)
#define USB_GONE_US (3 * 1000 * 1000)

enum { WAY_NONE, WAY_USB, WAY_BLE };

static nano_message_cb s_on_message;
static nano_link_cb s_on_link;
static nano_forward_cb s_forward;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_ble_timer;
static volatile int s_way;              // the way that carries the link
static volatile bool s_usb_up, s_ble_up;

static void allow_ble_cb(void *arg)
{
    (void)arg;
    if (!s_usb_up) nano_ble_allow_nano(true);
}

static void on_usb_message(uint32_t type, const uint8_t *payload, size_t len)
{
    if (s_way == WAY_USB && s_on_message) s_on_message(type, payload, len);
}

static void on_ble_message(uint32_t type, const uint8_t *payload, size_t len)
{
    if (s_way == WAY_BLE && s_on_message) s_on_message(type, payload, len);
}

static void on_usb_forward(const uint8_t *message, size_t len)
{
    if (s_way == WAY_USB && s_forward) s_forward(message, len);
}

static void on_ble_forward(const uint8_t *message, size_t len)
{
    if (s_way == WAY_BLE && s_forward) s_forward(message, len);
}

static void on_usb_link(bool ready)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_usb_up = ready;
    if (ready) {
        esp_timer_stop(s_ble_timer);
        nano_ble_allow_nano(false);
        if (s_way == WAY_BLE) {
            ESP_LOGI(TAG, "USB cable in - the Nano goes over USB now, not Bluetooth");
            s_way = WAY_NONE;
            if (s_on_link) s_on_link(false);
        }
        s_way = WAY_USB;
        ESP_LOGI(TAG, "Nano connected over USB");
        if (s_on_link) s_on_link(true);
    } else {
        if (s_way == WAY_USB) {
            s_way = WAY_NONE;
            ESP_LOGI(TAG, "Nano gone from USB - Bluetooth looks for it in %d s if the cable stays out", USB_GONE_US / 1000000);
            if (s_on_link) s_on_link(false);
        }
        esp_timer_stop(s_ble_timer);
        esp_timer_start_once(s_ble_timer, USB_GONE_US);
    }
    xSemaphoreGive(s_lock);
}

static void on_ble_link(bool ready)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ble_up = ready;
    if (ready && s_way == WAY_NONE && !s_usb_up) {
        s_way = WAY_BLE;
        ESP_LOGI(TAG, "Nano connected over Bluetooth (no USB cable)");
        if (s_on_link) s_on_link(true);
    } else if (ready && s_way != WAY_BLE) {
        nano_ble_allow_nano(false);   // (the cable won meanwhile)
    } else if (!ready && s_way == WAY_BLE) {
        s_way = WAY_NONE;
        if (s_on_link) s_on_link(false);
    }
    xSemaphoreGive(s_lock);
}

void nano_link_start(nano_message_cb on_message, nano_link_cb on_link)
{
    s_on_message = on_message;
    s_on_link = on_link;
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);
    const esp_timer_create_args_t timer = { .callback = allow_ble_cb, .name = "link_ble" };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_ble_timer));
    nano_usb_set_forward(on_usb_forward);
    nano_ble_set_forward(on_ble_forward);
    nano_usb_start(on_usb_message, on_usb_link);
    nano_ble_start(on_ble_message, on_ble_link);
    esp_timer_start_once(s_ble_timer, START_GRACE_US);
}

bool nano_link_ready(void)
{
    return s_way == WAY_USB ? nano_usb_ready() : s_way == WAY_BLE ? nano_ble_ready() : false;
}

const char *nano_link_way(void)
{
    return s_way == WAY_USB ? "USB" : s_way == WAY_BLE ? "Bluetooth" : "";
}

void nano_link_restart(void)
{
    if (s_way == WAY_USB) nano_usb_restart();
    else if (s_way == WAY_BLE) nano_ble_restart();
}

void nano_link_pause(int seconds)
{
    if (s_way == WAY_USB) nano_usb_pause(seconds);
    else nano_ble_pause(seconds);
}

bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    if (s_way == WAY_USB) return nano_usb_send(label, type, payload, len);
    if (s_way == WAY_BLE) return nano_ble_send(label, type, payload, len);
    ESP_LOGW(TAG, "Not connected - dropped: %s", label);
    return true;   // (as both ways do while they are down)
}

bool nano_link_send_frame(const uint8_t *frame, size_t len)
{
    if (s_way == WAY_USB) return nano_usb_send_frame(frame, len);
    if (s_way == WAY_BLE) return nano_ble_send_frame(frame, len);
    return false;
}

bool nano_link_send_packets(uint8_t *packets, size_t len, uint32_t type)
{
    if (s_way == WAY_BLE) return nano_ble_send_packets(packets, len, type);
    if (s_way == WAY_USB) return nano_usb_send_packets(packets, len, type);
    ESP_LOGW(TAG, "A long message from the app (type %u): no Nano connected - dropped", (unsigned)type);
    free(packets);
    return false;
}

void nano_link_set_forward(nano_forward_cb cb)
{
    s_forward = cb;
}

size_t nano_link_nano_mfg(uint8_t *out, size_t max)
{
    return nano_ble_nano_mfg(out, max);
}

void nano_link_stats(nano_link_stats_t *out)
{
    if (s_way == WAY_BLE) nano_ble_stats(out);
    else nano_usb_stats(out);
}

void nano_link_log_status(void)
{
    ESP_LOGI(TAG, "Link: %s", s_way == WAY_USB ? "USB" : s_way == WAY_BLE ? "Bluetooth" : "none");
    nano_usb_log_status();
    nano_ble_log_status();
}
