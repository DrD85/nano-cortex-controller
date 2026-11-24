// How many frames go into each USB microframe of the Nano's playback (UAC 2.0, high speed, asynchronous).
//
// The Nano runs on its own clock, so the host has to send exactly as many frames as the Nano plays - on average
// 6 per microframe at 48 kHz, sometimes 5 or 7. The Nano tells the rate either with an explicit feedback endpoint
// (16.16 frames per microframe) or implicitly through its capture stream (the host sends what it receives). The
// pacer takes either and spreads the fraction over the microframes. Without any feedback it sends the nominal rate.
// Plain C (tested on the computer).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UAC2_PACER_RING 128   // capture packet sizes kept (16 ms)
#define UAC2_PACER_LAG 24     // packets the playback runs behind the capture (3 ms): room for the bursts of 8

typedef struct {
    uint32_t rate_q16;        // frames per microframe, 16.16
    uint32_t nominal_q16;
    uint32_t acc;             // fraction carried over
    int max_frames;           // wMaxPacketSize / bytes per frame
    // implicit feedback: frames counted on the capture stream
    uint64_t cap_frames;
    uint32_t cap_packets;
    bool explicit_fb;
    uint32_t rejected;        // feedback values out of range
    // implicit feedback, packet by packet (as Linux does): each playback packet carries as many frames as a capture
    // packet brought, in the same order - so exactly as many frames go out as come in, and the Nano's buffer and
    // with it the round trip stay where they started.
    uint8_t sizes[UAC2_PACER_RING];
    uint32_t sw, sr;          // written / read capture packet sizes
    bool mirroring;
    uint32_t mirror_short;    // playback packets that found no capture size waiting (sent the averaged rate)
    uint32_t mirror_dropped;  // capture sizes thrown away (far too many waiting)
} uac2_pacer_t;

// rate 48000, 8000 microframes per second (high speed), max_frames from wMaxPacketSize (224 / 32 = 7).
void uac2_pacer_init(uac2_pacer_t *p, uint32_t rate, uint32_t microframes_per_s, int max_frames);

// Explicit feedback value as it arrives (4 bytes, 16.16, frames per microframe). Values more than 1 % off the
// nominal rate are ignored and counted.
void uac2_pacer_feedback(uac2_pacer_t *p, uint32_t value_q16);

// Implicit feedback: frames of one received capture packet (bytes / bytes per frame). The rate follows the average
// over the last ~1 s.
void uac2_pacer_capture_packet(uac2_pacer_t *p, int frames);

// Frames for the next microframe.
int uac2_pacer_next(uac2_pacer_t *p);
