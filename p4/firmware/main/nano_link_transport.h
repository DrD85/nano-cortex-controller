// P4 board: the two ways to the Nano behind nano_link.h. nano_link.c chooses between them and is the only one that
// calls these (main.c and the Bluetooth clients use nano_link.h).
//
// - USB (nano_link_usb.c): the Nano's HID interface on the USB-A socket. Preferred: no pairing window, no radio.
// - Bluetooth LE (nano_link_ble.c, the small board's nano_link.c): only while no cable is in. The same file also owns
//   NimBLE for the other Bluetooth clients (MIDI controller, phone, app bridge), which run either way.
#pragma once

#include "nano_link.h"

void nano_usb_start(nano_message_cb on_message, nano_link_cb on_link);
bool nano_usb_ready(void);
void nano_usb_restart(void);
void nano_usb_pause(int seconds);
bool nano_usb_send(const char *label, uint32_t type, const uint8_t *payload, size_t len);
bool nano_usb_send_frame(const uint8_t *frame, size_t len);
bool nano_usb_send_packets(uint8_t *packets, size_t len, uint32_t type);
void nano_usb_set_forward(nano_forward_cb cb);
void nano_usb_stats(nano_link_stats_t *out);
void nano_usb_log_status(void);

void nano_ble_start(nano_message_cb on_message, nano_link_cb on_link);
bool nano_ble_ready(void);
void nano_ble_restart(void);
void nano_ble_pause(int seconds);
bool nano_ble_send(const char *label, uint32_t type, const uint8_t *payload, size_t len);
bool nano_ble_send_frame(const uint8_t *frame, size_t len);
bool nano_ble_send_packets(uint8_t *packets, size_t len, uint32_t type);
void nano_ble_set_forward(nano_forward_cb cb);
size_t nano_ble_nano_mfg(uint8_t *out, size_t max);
void nano_ble_stats(nano_link_stats_t *out);
void nano_ble_log_status(void);
// Whether the Nano may be connected over Bluetooth (false while its cable is in): false drops a connection to it and
// stops looking for it; the scan for the other clients goes on.
void nano_ble_allow_nano(bool allowed);
// Bluetooth is up (the C6 answered and NimBLE is in sync).
bool nano_ble_available(void);
