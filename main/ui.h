// Touch screen: current preset with capture and cab, previous/next buttons, a mode button, a tuner button
// and six tiles that match footswitches 3-8:
//   preset mode: the six presets of the bank (bank with the arrow buttons)
//   scene mode:  in place of them the six scenes of the current preset - which of its effects are on (the Scenes
//                button next to Save goes there and back; long press on a tile: name, colour and effects)
//   FX mode:     FX slots 1-5 on/off, reverb mix Pos 1 / Pos 2 (long press: set them); Pre FX 1 can have
//                a second effect (2ND in its FX editor), swapped by holding footswitch 3
// VOL (top left): capture volume; long press on the capture card: its gain, bass, mid and treble; long press on the
// cab card: cab output, high pass and low pass.
#pragma once

#include <stdbool.h>

#include "library.h"
#include "nano_state.h"

// Called from the LVGL task when the user taps something. Must not block.
//   'p' / 'n'      previous / next preset (swipe)
//   'k', -1 / +1   previous / next bank (arrow buttons)
//   'w', 1..8      same as footswitch 1-8 (1 = mode, 2 = tuner, 3-8 = tiles); UI_SWITCH_HOLD | n = footswitch n held
//                  (footswitch 1 held = looper mode on / off, 2 held = scenes <-> presets)
//   '^', +-1       tuner reference pitch one Hz up / down (all letters are taken)
//   '~', 0         tuner: mute the output on / off
//   'O', slot      long press on an FX tile: open the FX editor for that slot
//   'E', 0         FX editor closed
//   'M', type      FX editor: choose this model for the edited slot
//   'a'-'e'        FX editor: switch the edited slot on/off
//   'C' / 'I', n   capture slot 1-25 / cab slot 1-5 (0 = bypass)
//   'L', k<<16|i   load library item i (alphabetical) into the active slot; k: 0 capture, 1 cab
//   'B', packed    own bank: symbol << 18 (5 bits) | bank << 14 | slot << 11 | colour << 7 | preset (preset 0 = default)
//   'S', 0         save the current preset (after the user confirmed it)
//   'r', 0         refresh: read presets, names and the library again
//   'V', 0         USB volume opened: read the Nano's settings (answer with ui_set_usb_gain)
//   'U', tenths    USB playback volume in tenths of a dB (-400 = off .. 0)
//   'v', w<<16|v   capture volume (w 0: v = 0-255), cab setting (w 1-3 = output, high pass, low pass: v = 0-1000)
//                  or capture amp knob (w 4-7 = gain, bass, mid, treble: v = 0-255)
//   'y', 1 / 0     cab settings opened (read them, answer with ui_set_cab_settings) / closed
//   'z', n         FX editor: load FX preset n into the slot (0 = the original values, 1-UI_FX_PRESETS)
//   'Q', w<<8|v    reverb mix editor: Pos w+1 (0/1) moved to v (0-255), play that mix
//   'W', p1<<8|p2  reverb mix editor: save Pos 1 / Pos 2 (0-255) for this preset (on the controller)
//   '!', 0         reverb mix editor: take the reverb off the expression pedal in the Nano's preset
//   '=', 1 / 0     expression dialog opened (read the preset's assignments, answer with ui_set_expression) / closed
//   '+', mode      expression dialog: set the Nano's EXP/MIDI connector (NANO_JACK_MIDI / NANO_JACK_EXPRESSION)
//   '<', n         expression dialog, Calibrate: 1 = start (answer with ui_set_pedal while the pedal moves),
//                  2 = save what was seen, 0 = stop
//   'K', 0         reverb mix editor closed
//   'A', w<<24|t   second effect (B) of this preset: w 0 = reverb (0 = none: footswitch 8 is the mix switch),
//                  w 1 = Pre FX 1 (footswitch 3 held swaps A/B), w 2 = reverb B kept for later while footswitch 8
//                  is the mix switch; t = model, 0 = none
//   'H', 0         edit reverb B: load it and open the FX editor
//   'X', 1 / 0     Bluetooth MIDI dialog opened / closed (search for devices while open)
//   'P', n         connect MIDI device n of the list (-1 = disconnect and forget)
//   'D', n         footswitch learn: 1-8 = the next pressed footswitch becomes switch n, 0 = cancel,
//                  -1 = default order (answer with ui_learn_done)
//   '$', -1        the Scenes button: scenes on the tiles, or the bank's presets again if scenes are showing
//                  (1 / 0 = scenes / presets)
//   '&', n         FX editor, scene button: the settings of the edited effect as they are now belong to scene n (0-5)
//                  from now on; 0x100 | n = the scene leaves this effect's settings alone again
typedef void (*ui_command_cb)(char command, int arg);
#define UI_SWITCH_HOLD 0x100
#define UI_SWITCH_SENT 0x200   // looper mode: the footswitch's message went out already, only show the press
#define UI_SWITCH_LOST 0x400   // ... but it could not be sent (no phone)

// Text input from the on-screen keyboard: 'N' = new name for the current preset; '1'-'4' = save the FX editor's
// values as FX preset 1-4 under this name ("" deletes it). 'L' = a looper tile from its dialog:
// "<footswitch 2-8><colour a-j><symbol a-g>name", or "<footswitch>!" for the default. 'S' = a scene of the current
// preset from its dialog: "<footswitch 3-8><colour a-j><five times 0 / 1: FX slot on>name", or "<footswitch>!" to
// empty it. 'X' = the expression dialog's assignments, saved: one entry per sweep (NANO_EXP_RANGES, in their order),
// "<0 / 1: on the pedal><heel, two hex digits><toe, two hex digits>", then one per on/off assignment
// (NANO_EXP_SWITCHES), "<0 / 1: assigned><way 0-2><0 / 1: inverted><0 / 1: latch><delay in ms, four hex digits>".
typedef void (*ui_text_cb)(char kind, const char *text);

// FX editor: a parameter was moved. value = 0-1, as the Nano expects it.
typedef void (*ui_param_cb)(int slot, int param, float value);

// Looper mode: the tiles of footswitches 2-8, each with a name, a colour (as the banks') and a symbol.
#define UI_LOOPER_SWITCHES 7
#define UI_LOOPER_NAME 16      // with the terminating 0
#define UI_LOOPER_SYMBOLS 7    // none, loop, play, pause, stop, undo, note

// Scenes of the current preset (footswitches 3-8 in scene mode).
#define UI_SCENES 6
#define UI_SCENE_NAME 16       // with the terminating 0

typedef struct {
    bool fx_mode;
    int bank;            // 0-15
    int mix_slot;        // FX slot of the reverb, -1 if there is none
    bool mix_known;      // Pos 1 / Pos 2 are set for this preset
    bool mix_exp;        // the Nano's preset has the reverb on the expression pedal (its Amount)
    bool exp_used;       // the Nano's expression pedal does something in this preset
    float mix_pos[2];    // 0-1
    int mix_active;      // -1 = as stored in the preset, 0 = Pos 1, 1 = Pos 2
    uint32_t rev_b_type; // second reverb of this preset (footswitch 8 swaps A/B), 0 = none: it is the mix switch
    uint32_t rev_b_stored; // the second reverb kept for this preset - also while footswitch 8 is the mix switch
    int rev_active;      // 0 = reverb A (the preset's), 1 = reverb B
    uint32_t pre1_b_type; // second effect of Pre FX 1 (footswitch 3 held swaps A/B), 0 = none
    int pre1_active;     // 0 = A (the preset's), 1 = B
    uint32_t pre1_a_type; // the preset's effect in Pre FX 1 (known once read), shown while B runs
    uint8_t bank_presets[6];   // own bank: preset per switch 3-8, 0 = empty
    uint8_t bank_colors[6];    // colour index (UI_BANK_COLOR_COUNT)
    uint8_t bank_icons[6];     // symbol: 0 = none, n = PRESET_ICONS[n - 1]
    bool looper;         // looper mode: the tiles are the switches of a looper app on a phone
    bool phone;          // that phone is connected (Bluetooth MIDI)
    int looper_sent;     // looper mode: controller number of the last press, < 0 if it could not be sent, 0 = none yet
    char looper_names[UI_LOOPER_SWITCHES][UI_LOOPER_NAME];   // looper mode: tiles of footswitches 2-8
    uint8_t looper_colors[UI_LOOPER_SWITCHES];                // colour index (UI_BANK_COLOR_COUNT)
    uint8_t looper_icons[UI_LOOPER_SWITCHES];                 // symbol index (UI_LOOPER_SYMBOLS)
    bool scenes;         // scene mode: the tiles of footswitches 3-8 are scenes, not the bank's presets
    int scene_active;    // scene whose effects are on right now, -1 = none
    char scene_names[UI_SCENES][UI_SCENE_NAME];               // "" = empty
    uint8_t scene_colors[UI_SCENES];                          // colour index (UI_BANK_COLOR_COUNT)
    uint8_t scene_fx[UI_SCENES];                              // bit n = FX slot n on
    uint8_t scene_set[UI_SCENES];                             // bit n = the scene carries settings for the effect in slot n
} ui_view_t;

#define UI_BANK_COLOR_COUNT 10

void ui_init(ui_command_cb on_command, ui_param_cb on_param, ui_text_cb on_text);
void ui_set_link(bool connected);
void ui_show_state(const nano_state_t *state, const ui_view_t *view);
void ui_show_tuner(bool open);
void ui_show_tuner_reading(const nano_tuner_reading_t *reading);
// Reference pitch (Hz) and output mute shown on the tuner screen.
void ui_tuner_settings(float base_hz, bool muted);

// FX editor. values = current parameter values (0-1, index order) or NULL while they are unknown
// (the Nano only reports them for FX that are on).
void ui_fx_editor_show(int slot, uint32_t model, bool on, const float *values, int count);
void ui_fx_editor_close(void);

// Library for the capture / cab pickers (owned by the caller, must stay valid).
void ui_set_library(const nano_library_t *library);

// USB playback volume (dB, -40 = off) read from the Nano's settings.
void ui_set_usb_gain(float db);

// FX presets of the model in the FX editor (a row above its parameters): names of the places ("" = empty), the one
// loaded or saved last (-1 = none, 0 = ORIGINAL, 1-UI_FX_PRESETS) and whether they can be used (the effect is on).
#define UI_FX_PRESETS 4
void ui_fx_presets_show(const char names[][16], int active, bool usable);

// Cab settings dialog: values = output, high pass, low pass (0-1, NANO_CAB_SETTINGS) or NULL while unknown;
// enabled = the sliders can be used; info = line under the title (cab name or why the values are missing).
void ui_set_cab_settings(const float *values, bool enabled, const char *info);

// Expression dialog: what the Nano's expression pedal does in the current preset. ranges = what it sweeps - the
// Amount of FX slot 1-5, then the capture's gain, bass, mid, treble and level and the input gate (NANO_EXP_RANGES),
// each from its heel to its toe value (0-255 = 0-100 %); switches = what it switches on and off (NANO_EXP_SWITCHES).
#define UI_EXP_TARGETS NANO_EXP_RANGES
typedef nano_exp_range_t ui_exp_range_t;
void ui_set_expression(const nano_exp_range_t *ranges, const nano_exp_switch_t *switches);
// Calibrate: the pedal's position as the Nano reports it and the lowest and highest seen so far (min > max: none yet).
void ui_set_pedal(int value, int min, int max);
// What the Nano's EXP/MIDI connector is set to (NANO_JACK_*, -1 = not known), shown at the top of that dialog.
void ui_set_jack(int mode);

// The Nano Cortex Editor is connected through the controller (shown in the status line).
void ui_set_app(bool connected);

// Footswitch learn finished (or not possible): closes the learn window and shows the message.
void ui_learn_done(const char *message);

// Bluetooth MIDI: connection and the devices found while the MIDI dialog is open.
typedef struct {
    bool connected;
    char name[32];       // connected or stored device, "" = none
    int count;
    struct {
        char name[32];
        int8_t rssi;
        bool remembered;
    } devices[8];
} ui_midi_t;
void ui_set_midi(const ui_midi_t *midi);

// Start screen (from ui_init until ui_splash_done or a tap): status line and version; NULL keeps the old text.
void ui_splash_status(const char *status, const char *version);
void ui_splash_done(void);

// Short message at the top of the screen.
void ui_show_message(const char *text);
// Footswitch tile 0-7 lights up for a moment (the press arrived).
void ui_flash_tile(int tile);

#ifdef NANO_BENCH
// Development (-DNANO_BENCH=1): times the FX editor's lists on the board and prints the result (console '%').
void ui_bench(void);
#endif
