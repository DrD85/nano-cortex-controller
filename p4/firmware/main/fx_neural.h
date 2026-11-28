// A third group of sounds for the FX editor's info page: an own build can carry presets of its own for a model
// (main/private/fx_neural_private.c, written by tools/gen_neural.py - private, as the pedal pictures). The published
// firmware has none (main/fx_neural.c).
#pragma once
#include <stdint.h>

#include "fx_info.h"

// The presets of a model and how many there are; NULL and 0 if there are none.
const nano_info_sound_t *nano_fx_neural(uint32_t type, int *count);

// The focus pad's four corners from those presets (an own build, chosen by tools/gen_neural.py); NULL: the pad keeps
// the editor's corners (nano_focus, fx_focus.c).
const nano_focus_t *nano_fx_neural_focus(uint32_t type);
