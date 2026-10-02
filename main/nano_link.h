// Bluetooth LE link to the Nano Cortex (NimBLE central).
//
// The Nano exposes service A002 (A003 on some units) with a write characteristic C304 and a
// notify characteristic C305. Every message is framed as [length] C0 [protobuf payload] [u32 LE type];
// longer replies arrive as several notifications (byte 1: 0x40 = first packet, 0x80 = last packet).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A complete message from the Nano: protobuf payload (frame header and type removed) and its type.
// Called from the NimBLE host task; the payload is only valid during the call.
typedef void (*nano_message_cb)(uint32_t type, const uint8_t *payload, size_t len);

// Called when the link is ready (notifications on) or lost.
typedef void (*nano_link_cb)(bool ready);

void nano_link_start(nano_message_cb on_message, nano_link_cb on_link);
bool nano_link_ready(void);

// Queues a message for C304. Writes go out strictly one at a time (write with response).
bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len);
