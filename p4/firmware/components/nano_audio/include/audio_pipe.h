// Phone audio on its way into the Nano: a ring buffer between the I2S input (the ESP32 with the phone's Bluetooth
// audio, usually 44.1 kHz, 16-bit stereo) and the Nano's USB playback (48 kHz, 8 channels, 24 bits in 4-byte slots),
// with a resampler that follows both clocks.
//
// The two clocks are independent (the ESP32's I2S clock and the Nano's own clock), so the resampler's ratio is
// trimmed by a slow controller on the buffer's fill level: at most +-2000 ppm (about 3.5 cents), normally a few
// dozen ppm - not audible.
//
// Live rule: the USB side never waits and never clicks. Without phone audio it renders silence; when the buffer runs
// dry it fades the last sample out, waits until the buffer is filled again and fades in. Volume changes are ramped.
//
// One writer (I2S task) and one reader (USB callback) may run at the same time on different cores. Plain C, no
// ESP-IDF headers, so it is tested on the computer (test/).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t written;          // input frames taken in
    uint32_t dropped;          // input frames dropped because the buffer was full
    uint32_t underruns;        // times the buffer ran dry while playing
    uint32_t fill;             // input frames in the buffer now
    int32_t ppm;               // current correction of the ratio
    bool playing;              // false = prefilling or silent
} audio_pipe_stats_t;

typedef struct {
    int16_t *buf;              // stereo frames (L, R)
    uint32_t cap;              // frames, power of two
    _Atomic uint32_t w, r;     // free-running frame counters

    uint32_t in_rate, out_rate;
    uint32_t target;           // fill level the controller aims for (frames)
    double step;               // in_rate / out_rate
    float frac;                // position between win[1] and win[2] (float: a double is a library call per frame on
                               // the P4; ~1e-7 of the rate, which the controller takes up, 2026-11-27)
    float win[4][2];           // last four input frames for the cubic interpolation
    double fill_avg;           // smoothed fill level
    double integ;              // integral part of the controller
    double corr;               // ratio correction (relative)

    int state;                 // PIPE_IDLE, PIPE_PREFILL, PIPE_RUN, PIPE_FADEOUT
    float fade, fade_step;     // 0..1 envelope for start / stop
    float gain, gain_target, gain_step;

    audio_pipe_stats_t st;
    _Atomic uint32_t rate_request;   // a new input rate asked for by the writer's side (0 = none)
} audio_pipe_t;

// buffer: frames * 2 int16 (frames must be a power of two, e.g. 4096 = 93 ms at 44.1 kHz).
void audio_pipe_init(audio_pipe_t *p, int16_t *buffer, uint32_t frames, uint32_t in_rate, uint32_t out_rate);

// New input rate (the phone stream changed, 44100 or 48000): empties the buffer and starts again with a fade-in.
// Call only from the reader's side or while the reader is stopped.
void audio_pipe_set_input_rate(audio_pipe_t *p, uint32_t in_rate);

// The same from the writer's side (any task): the reader takes the new rate at its next render.
void audio_pipe_request_rate(audio_pipe_t *p, uint32_t in_rate);

// Writer: interleaved stereo frames. Returns the number taken (the rest is dropped and counted).
uint32_t audio_pipe_write(audio_pipe_t *p, const int16_t *stereo, uint32_t frames);

// Reader: renders frames at out_rate into interleaved 32-bit slots of `channels` channels (24-bit samples in the
// upper bits, as UAC 2.0 with 4-byte subslots and 24-bit resolution expects). The phone's left / right go to the
// channels ch_left / ch_right (0-based), every other channel is zero.
void audio_pipe_render(audio_pipe_t *p, int32_t *out, uint32_t frames, int channels, int ch_left, int ch_right);

// The same as stereo float frames (L, R; full scale = 1.0, volume already applied) - for the mixer (audio_mix.h).
// A render stops early only to wait for the buffer: the rest of `lr` is silence.
void audio_pipe_render_stereo(audio_pipe_t *p, float *lr, uint32_t frames);

// A float sample (full scale 1.0, clipped) as a 32-bit slot with 24 bits in the upper bits.
static inline int32_t audio_to_slot(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    int32_t s = (int32_t)(v * 8388607.0f + (v >= 0 ? 0.5f : -0.5f));   // 24 bits, rounded
    return (int32_t)((uint32_t)s << 8);                                  // in the upper 24 bits of the 4-byte slot
}

// Volume in dB (<= -90 = off), ramped over about 20 ms.
void audio_pipe_set_gain_db(audio_pipe_t *p, float db);

void audio_pipe_stats(audio_pipe_t *p, audio_pipe_stats_t *out);
