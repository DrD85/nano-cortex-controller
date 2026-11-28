// The live spectrum of the Nano's sound for the EQ editor (P4 board, draft 2026-11-28): as the desktop editor's EQ view
// shows it (its nano-spectrum helper) - a Hann-windowed FFT, its power summed into bands of 1/12 octave from 20 Hz,
// in dBFS with a full-scale sine reading 0 dB in its band, then smoothed (quick up, slow down).
//
// Never on the audio path: the samples are copied out of the mixer's capture ring by a low-priority task on core 1
// (main/p4_spectrum.c), which also runs the FFT. All buffers come from the caller's allocator (PSRAM on the board).
// This file stays in flash (linker.lf). Plain C, tested on the computer (test/test_spectrum.c).
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPECTRUM_N 4096          // FFT length: 85 ms at 48 kHz, 11.7 Hz between bins
#define SPECTRUM_RATE 48000.0f
#define SPECTRUM_BANDS 120       // 1/12 octave each, band i centred at SPECTRUM_F0 * 2^(i/12) (20 Hz .. 19.0 kHz)
#define SPECTRUM_PER_OCT 12
#define SPECTRUM_F0 20.0f
#define SPECTRUM_FLOOR_DB -120.0f

typedef struct {
    uint16_t k0, k1;             // the bins in the band: k0 <= k < k1 (k1 == k0: the band is narrower than a bin -
    float pos, width;            // ... then its centre in bins, interpolated, times its width in bins)
} spectrum_band_t;

typedef struct {
    float *hist;                 // the last SPECTRUM_N samples (a ring)
    float *work;                 // SPECTRUM_N: the windowed samples, then the FFT (SPECTRUM_N / 2 complex values)
    float *win;                  // SPECTRUM_N: the Hann window
    float *tw;                   // SPECTRUM_N / 2 complex: e^(-2 pi i k / N)
    spectrum_band_t *bands;      // SPECTRUM_BANDS
    uint32_t pos;                // next place in hist
    uint32_t fill;               // samples in hist since the last reset (a whole window is needed)
} spectrum_t;

// false: no memory. alloc is called five times (about 65 KB in all); nothing is freed again.
bool spectrum_init(spectrum_t *s, void *(*alloc)(size_t));
void spectrum_reset(spectrum_t *s);   // a gap in the sound: wait for a whole window of new samples
void spectrum_push(spectrum_t *s, const float *x, uint32_t n);
// Stereo from a ring of interleaved L/R floats (the mixer's capture ring), as mono: frames [from, to) of the ring
// (counted frames; mask = frames in the ring - 1).
void spectrum_push_lr(spectrum_t *s, const float *ring, uint32_t mask, uint32_t from, uint32_t to);
static inline bool spectrum_ready(const spectrum_t *s) { return s->fill >= SPECTRUM_N; }
// The newest window's level per band (dBFS, SPECTRUM_FLOOR_DB at the least).
void spectrum_compute(spectrum_t *s, float out_db[SPECTRUM_BANDS]);
// The shown levels follow the measured ones: up at once (65 % of the way per step), down at release_db_s.
void spectrum_smooth(float *shown, const float *now, int n, float dt_s, float release_db_s);
static inline float spectrum_band_hz(int i) { return SPECTRUM_F0 * powf(2.0f, (float)i / SPECTRUM_PER_OCT); }
