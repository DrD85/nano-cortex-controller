// The Nano's USB audio on the P4's high-speed host port: plays the phone audio (audio_pipe) into the Nano's playback
// endpoint and listens to its capture endpoint (pacing by implicit feedback, and the Nano's output for a meter).
//
// Only standard USB audio class requests are used (SET_INTERFACE, GET_CUR of the clock's sample rate) - what every
// computer sends to the Nano; nothing of the Nano's own protocol.
//
// The USB Host Library (usb_host_install and the task that handles its library events) belongs to the caller,
// because the HID client that controls the Nano shares it.
//
// STATUS: builds; not yet run on hardware (the P4 board is not here yet).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_pipe.h"

// Every received capture packet: frames of `channels` 32-bit slots (24-bit samples in the upper bits).
// Called from the USB client task - keep it short (e.g. a level meter).
typedef void (*uac2_capture_cb)(const int32_t *frames, int count, int channels, void *arg);

// Every playback packet: fill `count` frames of `channels` 32-bit slots (e.g. audio_mix_render). Called from the USB
// client task, ahead of time (the transfer is queued).
typedef void (*uac2_render_cb)(int32_t *frames, int count, int channels, void *arg);

typedef struct {
    uint16_t vid, pid;          // 0x152A / 0x88E7 = Nano Cortex
    uint8_t playback_ep;        // 0x03
    uint8_t capture_ep;         // 0x82; 0 = do not open the capture stream
    uac2_render_cb render;      // what is played (the mixer); NULL = the phone audio from `pipe` alone
    void *render_arg;
    audio_pipe_t *pipe;         // phone audio (used when render is NULL)
    int ch_left, ch_right;      // playback channels for the phone (0-based); which ones the Nano plays: to be checked
    uac2_capture_cb on_capture; // optional
    void *capture_arg;
    int task_priority;          // client task (handles the transfer callbacks): high, e.g. 20
    int task_core;
} uac2_host_config_t;

typedef struct {
    bool connected;             // the Nano is on the port
    bool streaming;             // playback transfers are running
    uint32_t sample_rate;       // read from the clock (0 = not read)
    uint32_t frames_sent;
    uint32_t packets_skipped;   // packets the controller could not send in time
    uint32_t errors;            // failed transfers
    uint32_t rate_q16;          // frames per microframe in use (16.16)
    uint32_t capture_packets;
    bool mirroring;             // playback packets follow the capture packets one by one (uac2_pacer.h)
    uint32_t mirror_short, mirror_dropped;
    uint32_t capture_empty;     // capture packets that came empty or with an error (the Nano's sound lost there)
    uint32_t capture_odd;       // capture packets that were not whole frames (the channels would slip)
    uint32_t capture_repeats;   // capture frames that came a second time (the Nano's garbled state)
    uint32_t capture_sizes[4];  // capture packets of 5, 6, 7 and other frames
    uint32_t render_max_us;     // longest render of one playback transfer (1 ms of sound) since the last take
    uint32_t cap_gap_max_us;    // longest time between two finished capture transfers (each 1 ms) since the last take
} uac2_host_stats_t;

esp_err_t uac2_host_start(const uac2_host_config_t *cfg);
// While held, the Nano's audio is not started (the board's own start is busy: a stream begun then was found to put the
// Nano's USB audio out of step for good - garbled data on every channel until it was switched off, 2026-11-19).
void uac2_host_hold(bool hold);
// The streams stopped and started anew (alt 0, clock, alt 1) - the device stays open.
void uac2_host_restart(void);
// Test: how the streams begin (0-3, see uac2_host.c) - before uac2_host_start.
void uac2_host_set_start_mode(int mode);
void uac2_host_stats(uac2_host_stats_t *out);
// The two peaks of the stats since the last call, then set back to 0.
void uac2_host_take_peaks(uint32_t *render_max_us, uint32_t *cap_gap_max_us);
