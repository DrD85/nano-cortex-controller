// Pictures for the pedal view: one per FX model, in colour (RGB565A8), at the size they are shown.
// They are not part of the firmware: tools/gen_pedalboard.py writes them into one file that is flashed to a part of
// the flash memory nothing else uses (PEDALBOARD_OFFSET). The controller maps that part into memory and draws from
// there - no decoding, no RAM, no longer start. Without the file (the published firmware) there are no pictures and
// the pedal view draws every pedal itself.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#define PEDALBOARD_OFFSET 0x700000u            // after the firmware (6 MB from 0x10000), before the store's copy at 0xF00000
#define PEDALBOARD_MAX (8u * 1024 * 1024)

// Looks for the picture file in the flash memory (the board only; elsewhere it does nothing).
void pedal_assets_init(void);

// Takes the pictures from a file's contents in memory, which must stay where it is (screenshots on the computer).
bool pedal_assets_load(const uint8_t *data, size_t size);

// Picture of a model (FxTypeValue type) for the pedal view, NULL if there is none.
const lv_image_dsc_t *pedal_asset(uint32_t type);
