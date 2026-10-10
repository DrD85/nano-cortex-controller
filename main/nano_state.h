// State of the Nano Cortex as reported over Bluetooth, plus the request messages the controller sends.
// Message layouts come from the Nano Cortex Editor (index.html), where they are tested against the device.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx_models.h"

// Message types (u32 at the end of every frame)
#define NANO_MSG_STATE_REQUEST 1
#define NANO_MSG_STATE_RESPONSE 2
#define NANO_MSG_SAVE_PRESET 3
#define NANO_MSG_VALUE 26                     // one value of the current preset (capture volume)
#define NANO_MSG_SELECTOR 28                  // capture / cab slot selection
#define NANO_MSG_SET_PRESET_SLOTS 29          // preset change (both directions)
#define NANO_MSG_SET_PRESET_SLOTS_RESPONSE 30
#define NANO_MSG_BYPASS 31
#define NANO_MSG_EXP_REQUEST 60               // expression assignments of a preset
#define NANO_MSG_METERING 64                  // a level or the expression pedal's position, from the Nano
#define NANO_MSG_EXP_CAL_RESET 69             // forget the expression pedal's calibration
#define NANO_MSG_EXP_CAL_SAVE 71              // the pedal's lowest and highest position
#define NANO_MSG_EXP_SAVE 62                  // write all expression assignments of a preset
#define NANO_MSG_SETTINGS_REQUEST 65          // global settings of the Nano
#define NANO_MSG_SETTINGS_RESPONSE 66
#define NANO_MSG_UPDATE_SETTINGS 67
#define NANO_MSG_UPDATE_SETTINGS_RESPONSE 68
#define NANO_MSG_LIBRARY_REQUEST 76           // capture / IR library
#define NANO_MSG_LIBRARY_RESPONSE 77
#define NANO_MSG_CAB_LOAD 78                  // library IR into a cab slot
#define NANO_MSG_CAB_LOAD_RESPONSE 79
#define NANO_MSG_CAPTURE_LOAD 80              // library capture into a capture slot
#define NANO_MSG_CAPTURE_LOAD_RESPONSE 81
#define NANO_MSG_CAB_SETTING 94               // one cab setting of the current preset (output, high pass, low pass)
#define NANO_MSG_CAB_SETTINGS_REQUEST 95
#define NANO_MSG_CAB_SETTINGS_RESPONSE 96
#define NANO_MSG_EXP_RESPONSE 61
#define NANO_MSG_FX_VALUE 99                  // one FX parameter
#define NANO_MSG_DIRTY_CHANGED 115
#define NANO_MSG_RENAME_PRESET 111
#define NANO_MSG_RENAME_PRESET_RESPONSE 112
#define NANO_MSG_TUNER_MODE 127               // tuner on/off (both directions)
#define NANO_MSG_TUNER_METERING 128
#define NANO_MSG_FX_TYPE 136                  // FX model of a slot
#define NANO_MSG_FX_PARAMS_REQUEST 137        // read all parameters of a slot (only for active FX!)
#define NANO_MSG_FX_PARAMS_RESPONSE 138
#define NANO_MAX_PARAMS 24

#define NANO_PRESETS 64
#define NANO_FX_SLOTS 5
#define NANO_CAPTURE_SLOTS 25                 // 5 banks x 5
#define NANO_CAB_SLOTS 5

// The capture's amp knobs, in the order of their value ids (ValueMessage) and state fields (3-7).
enum { NANO_AMP_GAIN, NANO_AMP_LEVEL, NANO_AMP_BASS, NANO_AMP_MID, NANO_AMP_TREBLE, NANO_AMP_KNOBS };

typedef struct {
    int current_preset;                 // 1-64
    bool dirty;                         // unsaved changes in the current preset
    int slot_preset[4];                 // presets on footswitch slots A1, B1, A2, B2 (0-based)
    bool fx_known;
    bool fx_on[NANO_FX_SLOTS];          // Pre FX 1-2, Post FX 1-3
    uint32_t fx_type[NANO_FX_SLOTS];    // model type, 0 = empty slot
    char capture[64];
    char cab[64];
    int capture_slot;                   // 1-25, 0 = capture bypassed
    int cab_slot;                       // 1-5, 0 = cab bypassed
    int capture_volume;                 // 0-255, NANO_CAPTURE_VOLUME_0DB = 0 dB
    int amp[NANO_AMP_KNOBS];            // capture gain, level, bass, mid, treble, 0-255
    char capture_names[NANO_CAPTURE_SLOTS][48];
    char cab_names[NANO_CAB_SLOTS][48];
    float tuner_base_hz;                // reference pitch (default 440)
    bool tuner_muted;                   // output muted while the tuner is open (state field 63)
    bool names_loaded;
    char preset_names[NANO_PRESETS][65];
} nano_state_t;

// True if the payload is a complete protobuf message (every field parses up to the end).
bool nano_payload_complete(const uint8_t *payload, size_t len);

// Applies a StateResponse payload. Returns true if it contained the preset name list.
bool nano_state_apply(nano_state_t *st, const uint8_t *payload, size_t len);

// Applies a preset change reported by the Nano (SetPresetSlots from the device).
void nano_state_apply_preset_change(nano_state_t *st, const uint8_t *payload, size_t len);

// Reads one varint field (0 if missing) from a small message such as a status reply.
uint64_t nano_field_varint(const uint8_t *payload, size_t len, uint32_t field);

// Request payloads; each returns the payload length.
size_t nano_full_state_request(uint8_t *out);           // all presets, captures and cabs
size_t nano_current_state_request(uint8_t *out);        // current preset only
size_t nano_preset_change(const nano_state_t *st, int preset, uint8_t *out);
size_t nano_fx_bypass(int slot, bool on, uint8_t *out); // slot 0-4
size_t nano_preset_change_ack(uint8_t *out);            // answer to a preset change made on the device
// SetPresetSlots as the Nano sends it after a change on the pedal (current preset and footswitch slots) - for the app.
size_t nano_preset_changed_notice(const nano_state_t *st, uint8_t *out);
size_t nano_tuner_mode(bool on, float base_hz, bool muted, uint8_t *out);
size_t nano_fx_param(int slot, int param, float normalized, uint8_t *out);
size_t nano_exp_request(int preset, uint8_t *out);
size_t nano_capture_select(int slot, uint8_t *out);     // 1-25, 0 = bypass
// SavePreset / RenamePreset; return 0 if the name does not fit into one message.
size_t nano_save_preset(int preset, const char *name, uint8_t *out, size_t max);
size_t nano_rename_preset(int preset, const char *name, uint8_t *out, size_t max);
size_t nano_cab_select(int slot, uint8_t *out);         // 1-5, 0 = bypass
size_t nano_fx_model_select(int slot, uint32_t type, uint8_t *out);
// Reading the parameters of a bypassed FX crashed the Nano: only request them for FX that are on.
size_t nano_fx_params_request(int slot, uint8_t *out);

// USB playback volume ("USB Playback Volume" in the app's settings): dB, -40 = off, 0 = full.
#define NANO_USB_GAIN_MIN_DB (-40.0f)
size_t nano_settings_request(uint8_t *out);
size_t nano_usb_gain(float db, uint8_t *out);
// USB playback volume from a settings reply. False if the field is missing (0 dB, or an older firmware).
bool nano_settings_usb_gain(const uint8_t *payload, size_t len, float *db);

// What the Nano's EXP/MIDI connector takes ("EXP/MIDI Input Behavior" in the Cortex Cloud app's settings): TRS MIDI
// or an expression pedal. A global setting of the Nano.
enum { NANO_JACK_MIDI, NANO_JACK_EXPRESSION };
int nano_settings_jack(const uint8_t *payload, size_t len);   // NANO_JACK_*, -1 for a value that is not known
size_t nano_jack_update(int mode, uint8_t *out);

// Capture volume of the current preset: 0-255 on the Nano, -24 dB .. 0 dB below 128, 0 .. +12 dB above.
#define NANO_CAPTURE_VOLUME_0DB 128
float nano_capture_volume_db(int raw);
int nano_capture_volume_raw(float db);
size_t nano_capture_volume(int raw, uint8_t *out);
// One amp knob of the capture (NANO_AMP_GAIN ...), 0-255.
size_t nano_amp_knob(int knob, int value, uint8_t *out);

// Cab settings of the current preset (Level, High Pass and Low Pass in the editor), 0-1 on the Nano.
enum { NANO_CAB_OUTPUT, NANO_CAB_HIGH_PASS, NANO_CAB_LOW_PASS, NANO_CAB_SETTINGS };
float nano_cab_setting_value(int which, float normalized);   // output in dB (-96 .. +12), filters in Hz
float nano_cab_setting_normalized(int which, float value);
size_t nano_cab_settings_request(int slot, uint8_t *out);    // slot 1-5
size_t nano_cab_setting(int which, float normalized, uint8_t *out);
// The three settings (0-1) of a cab settings reply. False if the reply does not carry them.
bool nano_cab_settings_values(const uint8_t *payload, size_t len, float normalized[NANO_CAB_SETTINGS]);

// Parameter values (0-1, in parameter index order) of an FxParameters reply. Returns the count.
int nano_fx_params_values(const uint8_t *payload, size_t len, float *values, int max);

// Expression pedal: the values it sweeps in a preset, from its heel to its toe position - the Amount of the five FX
// slots (the first five, so the index is the slot) and the capture's knobs, level and the input gate.
enum { NANO_EXP_PRE1, NANO_EXP_PRE2, NANO_EXP_POST1, NANO_EXP_POST2, NANO_EXP_POST3,
       NANO_EXP_GAIN, NANO_EXP_BASS, NANO_EXP_MID, NANO_EXP_TREBLE, NANO_EXP_LEVEL, NANO_EXP_GATE, NANO_EXP_RANGES };
typedef struct {
    bool on;             // assigned to the pedal
    uint8_t heel, toe;   // 0-255 = 0-100 %
} nano_exp_range_t;

// ... and what it switches on and off: the capture, the cab, the five effects and the input gate, each in one of
// three ways (the Cortex Cloud app's names): Heel-Toe and Stop with a delay, Switch for a footswitch on the
// connector - with latch emulation for one that only makes contact while it is held.
enum { NANO_EXP_SW_CAPTURE, NANO_EXP_SW_CAB, NANO_EXP_SW_PRE1, NANO_EXP_SW_PRE2, NANO_EXP_SW_POST1, NANO_EXP_SW_POST2,
       NANO_EXP_SW_POST3, NANO_EXP_SW_GATE, NANO_EXP_SWITCHES };
enum { NANO_EXP_HEEL_TOE, NANO_EXP_SWITCH, NANO_EXP_STOP, NANO_EXP_MODES };
#define NANO_EXP_DELAY_MAX 2000   // ms
#define NANO_EXP_DELAY_DEFAULT 600
typedef struct {
    bool on;             // assigned
    uint8_t mode;        // NANO_EXP_HEEL_TOE ...
    bool inverted;       // Heel-Toe and Switch
    bool latch;          // Switch: latch emulation
    uint16_t delay_ms;   // Heel-Toe and Stop
} nano_exp_switch_t;

// The assignments of an expression reply.
void nano_exp_assignments(const uint8_t *payload, size_t len, nano_exp_range_t ranges[NANO_EXP_RANGES],
                          nano_exp_switch_t switches[NANO_EXP_SWITCHES]);

// SaveExpAssignments: all assignments of the preset (the save replaces them). Returns 0 if they do not fit.
size_t nano_exp_save(int preset, const nano_exp_range_t ranges[NANO_EXP_RANGES], const nano_exp_switch_t switches[NANO_EXP_SWITCHES],
                     uint8_t *out, size_t max);

// Metering from the Nano: what (NANO_METER_*) and its value.
enum { NANO_METER_CAPTURE_IN, NANO_METER_CAPTURE_OUT, NANO_METER_PEDAL };
void nano_metering(const uint8_t *payload, size_t len, int *what, int *value);

// Expression pedal calibration: the lowest and highest position seen while the pedal was moved over its whole way.
size_t nano_exp_calibration_save(int min, int max, uint8_t *out);

typedef struct {
    char note[8];
    float cents;
    bool valid;
    bool centered;
} nano_tuner_reading_t;
void nano_tuner_reading(const uint8_t *payload, size_t len, nano_tuner_reading_t *out);

const nano_fx_model_t *nano_fx_model(uint32_t type);    // NULL for an empty slot or an unknown model
const char *nano_fx_name(uint32_t type);                // model name, "Empty" or "Unknown model"
extern const char *const NANO_FX_SLOT_NAMES[NANO_FX_SLOTS];
