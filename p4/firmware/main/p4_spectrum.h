// P4 board: the live spectrum for the EQ editor (p4_spectrum.c; draft 2026-11-28).
#pragma once

#include <stdbool.h>

#include "ui.h"

// The DI (capture 1, before the whole chain) as a second, faint curve: 0 = off (the default - it costs the USB task
// a store per frame and 4 KB of internal RAM, see drafts/eq-spectrum-2026-11-28/PLAN.md).
#ifndef P4_SPECTRUM_DI
#define P4_SPECTRUM_DI 0
#endif

void p4_spectrum_start(void);                          // once, after the screen (p4_audio_show): task + buffers
bool p4_spectrum_take(bool want, ui_spectrum_t *out);  // ui_spectrum_cb (the screen's task)

#if P4_SPECTRUM_DI
#include <stdatomic.h>
#include <stdint.h>
#define P4_DI_RING 2048
extern int16_t g_p4_di_ring[P4_DI_RING];   // written by on_capture while g_p4_di_on (p4_audio.c)
extern atomic_uint g_p4_di_w;
extern volatile bool g_p4_di_on;
#endif
