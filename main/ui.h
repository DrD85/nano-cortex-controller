// Touch screen: current preset with capture and cab, previous/next buttons, a mode button, a tuner button
// and six tiles that match footswitches 3-8:
//   preset mode: the six presets of the bank (bank with the arrow buttons)
//   FX mode:     FX slots 1-5 on/off, reverb mix Pos 1 / Pos 2 (long press: set Pos 1 / Pos 2)
#pragma once

#include <stdbool.h>

#include "library.h"
#include "nano_state.h"

// Called from the LVGL task when the user taps something. Must not block.
//   'p' / 'n'      previous / next preset (swipe)
//   'k', -1 / +1   previous / next bank (arrow buttons)
//   'w', 1..8      same as footswitch 1-8 (1 = mode, 2 = tuner, 3-8 = tiles)
//   'O', slot      long press on an FX tile: open the FX editor for that slot
//   'E', 0         FX editor closed
//   'M', type      FX editor: choose this model for the edited slot
//   'a'-'e'        FX editor: switch the edited slot on/off
//   'C' / 'I', n   capture slot 1-25 / cab slot 1-5 (0 = bypass)
//   'L', k<<16|i   load library item i (alphabetical) into the active slot; k: 0 capture, 1 cab
//   'B', packed    own bank: symbol << 18 | bank << 14 | slot << 11 | colour << 7 | preset (preset 0 = default)
//   'S', 0         save the current preset (after the user confirmed it)
//   'V', 0         USB volume opened: read the Nano's settings (answer with ui_set_usb_gain)
//   'U', tenths    USB playback volume in tenths of a dB (-400 = off .. 0)
//   'Q', w<<8|v    reverb mix editor: Pos w+1 (0/1) moved to v (0-255), play that mix
//   'W', p1<<8|p2  reverb mix editor: save Pos 1 / Pos 2 (0-255) in the preset
//   'K', 0         reverb mix editor closed
//   'A', type      reverb B model for this preset (0 = none: footswitch 8 is the mix switch)
//   'H', 0         edit reverb B: load it and open the FX editor
//   'D', n         footswitch learn: 1-8 = the next pressed footswitch becomes switch n, 0 = cancel,
//                  -1 = default order (answer with ui_learn_done)
typedef void (*ui_command_cb)(char command, int arg);

// Text input from the on-screen keyboard: 'N' = new name for the current preset.
typedef void (*ui_text_cb)(char kind, const char *text);

// FX editor: a parameter was moved. value = 0-1, as the Nano expects it.
typedef void (*ui_param_cb)(int slot, int param, float value);

typedef struct {
    bool fx_mode;
    int bank;            // 0-15
    int mix_slot;        // FX slot of the reverb, -1 if there is none
    bool mix_known;      // Pos 1 / Pos 2 read from the preset's expression assignment
    float mix_pos[2];    // 0-1
    int mix_active;      // -1 = as stored in the preset, 0 = Pos 1, 1 = Pos 2
    uint32_t rev_b_type; // second reverb of this preset (footswitch 8 swaps A/B), 0 = none
    int rev_active;      // 0 = reverb A (the preset's), 1 = reverb B
    uint8_t bank_presets[6];   // own bank: preset per switch 3-8, 0 = empty
    uint8_t bank_colors[6];    // colour index (UI_BANK_COLOR_COUNT)
    uint8_t bank_icons[6];     // symbol: 0 = none, n = PRESET_ICONS[n - 1]
} ui_view_t;

#define UI_BANK_COLOR_COUNT 10

void ui_init(ui_command_cb on_command, ui_param_cb on_param, ui_text_cb on_text);
void ui_set_link(bool connected);
void ui_show_state(const nano_state_t *state, const ui_view_t *view);
void ui_show_tuner(bool open);
void ui_show_tuner_reading(const nano_tuner_reading_t *reading);

// FX editor. values = current parameter values (0-1, index order) or NULL while they are unknown
// (the Nano only reports them for FX that are on).
void ui_fx_editor_show(int slot, uint32_t model, bool on, const float *values, int count);
void ui_fx_editor_close(void);

// Library for the capture / cab pickers (owned by the caller, must stay valid).
void ui_set_library(const nano_library_t *library);

// USB playback volume (dB, -40 = off) read from the Nano's settings.
void ui_set_usb_gain(float db);

// Footswitch learn finished (or not possible): closes the learn window and shows the message.
void ui_learn_done(const char *message);

// Short message at the top of the screen.
void ui_show_message(const char *text);
