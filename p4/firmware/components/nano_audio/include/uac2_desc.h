// Reads what the Nano's configuration descriptor says about one of its USB audio streams (UAC 2.0): the streaming
// interface and alternate setting that carry an endpoint, its packet size and interval, channels and sample format,
// the feedback endpoint (if any) and the clock source. Plain C (tested on the computer).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t ac_interface;   // AudioControl interface (requests to the clock go there)
    uint8_t clock_id;       // first clock source entity (0 = none found)
    uint8_t interface;      // AudioStreaming interface with the endpoint
    uint8_t alt;            // its alternate setting
    uint8_t ep;             // the endpoint (as asked)
    uint16_t mps;           // wMaxPacketSize (bytes per microframe, bits 0-10)
    uint8_t interval;       // bInterval (2^(n-1) microframes)
    uint8_t sync;           // bmAttributes bits 2-3: 1 async, 2 adaptive, 3 sync
    uint8_t usage;          // bits 4-5: 0 data, 1 feedback, 2 implicit feedback data
    uint8_t channels;       // AS_GENERAL bNrChannels
    uint8_t subslot;        // FORMAT_TYPE_I bSubslotSize (bytes per sample)
    uint8_t bits;           // bBitResolution
    uint8_t fb_ep;          // explicit feedback endpoint in the same alternate setting (0 = none)
    uint16_t fb_mps;
    uint8_t fb_interval;
} uac2_stream_t;

// Finds the stream of endpoint `ep` (e.g. 0x03 playback, 0x82 capture). false if it is not in the descriptor.
bool uac2_find_stream(const uint8_t *config, size_t len, uint8_t ep, uac2_stream_t *out);
