// Bluetooth LE link to the Nano Cortex (NimBLE central).
//
// The Nano exposes service A002 (A003 on some units) with a write characteristic C304 and a
// notify characteristic C305. Every message is framed as [length] C0 [protobuf payload] [u32 LE type];
// longer replies arrive as several notifications (byte 1: 0x40 = first packet, 0x80 = last packet).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_hs.h"

// A complete message from the Nano: protobuf payload (frame header and type removed) and its type.
// Called from the NimBLE host task; the payload is only valid during the call.
typedef void (*nano_message_cb)(uint32_t type, const uint8_t *payload, size_t len);

// Called when the link is ready (notifications on) or lost.
typedef void (*nano_link_cb)(bool ready);

void nano_link_start(nano_message_cb on_message, nano_link_cb on_link);
bool nano_link_ready(void);

// Queues a message for C304. Writes go out strictly one at a time (write with response).
bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len);

// ---- app bridge ----

enum { NANO_OWNER_BOARD, NANO_OWNER_APP, NANO_OWNER_BOTH };

// A complete framed message from the app ([len] C0 payload type), sent on to the Nano. Its reply goes back to the app.
bool nano_link_send_frame(const uint8_t *frame, size_t len);

// Messages for the app (replies to its requests and messages the Nano sends on its own): payload + u32 type.
// Called from the NimBLE host task.
typedef void (*nano_forward_cb)(const uint8_t *message, size_t len);
void nano_link_set_forward(nano_forward_cb cb);

uint8_t nano_link_own_addr_type(void);

// ---- shared with other Bluetooth clients (MIDI) ----

// Every advertisement seen while scanning. Called from the NimBLE host task.
typedef void (*nano_link_adv_cb)(const ble_addr_t *addr, int8_t rssi, const struct ble_hs_adv_fields *fields);
void nano_link_set_adv_hook(nano_link_adv_cb cb);

// Another client wants scanning: fast = actively searching (dialog open), otherwise a slow background scan.
void nano_link_scan_request(bool on, bool fast);

// Connects another device with its own GAP event handler (scanning pauses meanwhile). The handler must call
// nano_link_other_connect_done() on its BLE_GAP_EVENT_CONNECT. Returns false if a connection is being set up.
bool nano_link_connect_other(const ble_addr_t *addr, ble_gap_event_fn *cb, void *arg);
void nano_link_other_connect_done(void);
