// Pictures for the pedal view: one per FX model, in colour (RGB565A8), at the size they are shown.
// They are not part of the firmware: tools/gen_pedalboard.py writes them into one file that is flashed to a part of
// the flash memory nothing else uses (PEDALBOARD_OFFSET). The controller maps that part into memory and draws from
// there - no decoding, no RAM, no longer start. Without the file (the published firmware) there are no pictures and
// the pedal view draws every pedal itself.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

// P4 board: in the partition "pedals" (data, subtype 0x40) of partitions.csv - 0x9A0000 with 16 MB flash, 0x1000000
// with 32 MB (partitions-32mb.csv). tools/gen_pedalboard.py prints the flash command.
#define PEDALBOARD_SUBTYPE 0x40
#define PEDALBOARD_MAX (16u * 1024 * 1024)

// Looks for the picture file in the flash memory (the board only; elsewhere it does nothing).
void pedal_assets_init(void);

// Takes the pictures from a file's contents in memory, which must stay where it is (screenshots on the computer).
bool pedal_assets_load(const uint8_t *data, size_t size);

// Picture of a model (FxTypeValue type) for the pedal view, NULL if there is none.
const lv_image_dsc_t *pedal_asset(uint32_t type);
