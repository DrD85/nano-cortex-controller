// Nano Cortex Controller.
//
// Connects to the Nano Cortex over Bluetooth LE, reads the preset names, the current preset with its
// FX, capture and cab, and switches presets and FX. Footswitches (SX1509), the touch screen and the
// serial monitor (h = help) control it:
//   footswitch 1   preset mode <-> FX mode
//   footswitch 2   tuner on/off; held: scenes <-> presets on footswitches 3-8
//   preset mode    3-8 = the six presets of the bank; the arrow buttons on the screen change the bank
//                  (own banks: long press on a preset tile picks preset, colour and symbol; stored on the board)
//   scene mode     3-8 = the six scenes of the current preset instead (the Scenes button next to Save): a scene is
//                  which of its effects are on (long press on a tile: name, colour, effects; stored on the board)
//   FX mode        3-7 = FX slots 1-5 on/off, 8 = reverb mix Pos 1 <-> Pos 2, or reverb A <-> B;
//                  with a second effect for Pre FX 1, holding 3 swaps it A <-> B (it stays on or off)
// A long press on an FX tile opens the FX editor (model and parameters of that slot), on tile 8 the reverb
// dialog, on tiles 1-2 the footswitch learn.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef NANO_WEB   // the browser build (web/) has no serial console
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "board.h"
#include "app_link.h"
#include "footswitches.h"
#include "library.h"
#include "midi_ble.h"
#include "phone_midi.h"
#include "nano_link.h"
#include "nano_state.h"
#include "ui.h"

static const char *TAG = "nano_ctrl";

#define CONSOLE_UART UART_NUM_0  // "UART" USB-C port (CH343), used when the console is set to UART

typedef enum { EV_LINK_UP, EV_LINK_DOWN, EV_MESSAGE, EV_COMMAND, EV_PARAM, EV_TEXT, EV_MIDI } event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t type;   // EV_MESSAGE
    uint8_t *data;   // EV_MESSAGE / EV_TEXT, owned by the event
    size_t len;
    char command;    // EV_COMMAND
    int arg;         // EV_COMMAND; EV_PARAM: slot; EV_MIDI: status << 16 | data1 << 8 | data2
    int param;       // EV_PARAM
    float value;     // EV_PARAM, 0-1
} app_event_t;

#define BANKS 16
#define BANK_SLOTS 6                    // footswitches 3-8
#define DEFAULT_BANKS ((NANO_PRESETS + BANK_SLOTS - 1) / BANK_SLOTS)
#define BANK_STORE_NAMESPACE "nano"
#define BANK_STORE_KEY "banks6"

static QueueHandle_t s_events;
static nano_state_t s_state;
static esp_timer_handle_t s_refresh_timer;
static bool s_waiting_full_state;
static int s_incomplete_retries;   // replies that arrived broken (lost packet) and were asked for again
static bool s_refresh_library;     // refresh button: read the library again after the presets
static esp_timer_handle_t s_midi_gate_timer;   // MIDI waits while the Nano connects and loads its presets
// App bridge: after the app changed something the controller reads the state; after the controller changed something
// the app is told "preset changed" and reads it again (as after a change on the pedal).
static esp_timer_handle_t s_app_read_timer, s_app_notify_timer;
static bool s_app_names_changed;
static bool s_app_exp_changed;     // the app saved expression assignments: read them again too
#define MIDI_GATE_US (20 * 1000 * 1000)

static bool s_fx_mode;          // footswitches 3-8: FX (true) or bank + presets (false)
// Looper mode (footswitch 1 held): footswitches 2-8 are controllers for a looper app on a phone (phone_midi), sent
// on press (value 127) and release (0) - the app tells a tap from a hold itself. Footswitch 1 leaves the mode.
// (Full momentary control changes: what Loopy Pro's guide for footswitches asks for.)
static bool s_looper;
static volatile bool s_looper_live;   // the same, for the footswitch task
static int s_looper_sent;             // the controller of the last press, shown on tile 1 (< 0: it could not be sent)
// What the tiles of footswitches 2-8 show in looper mode (long press on a tile): the app decides what a switch
// does, so name, colour and symbol are the user's. Stored on the controller.
typedef struct {
    char name[UI_LOOPER_NAME];
    uint8_t color;   // BANK_COLORS index
    uint8_t icon;    // UI_LOOPER_SYMBOLS index
} looper_tile_t;
static const looper_tile_t LOOPER_DEFAULTS[UI_LOOPER_SWITCHES] = {
    { "Pause", 9, 3 }, { "Loop 1", 2, 1 }, { "Loop 2", 2, 1 }, { "Loop 3", 3, 1 }, { "Loop 4", 3, 1 },
    { "Loop 5", 6, 1 }, { "Loop 6", 6, 1 },
};
static looper_tile_t s_looper_tiles[UI_LOOPER_SWITCHES];
#define LOOPER_STORE_KEY "looper"
#define LOOPER_STATUS 0xBF            // control change on MIDI channel 16
#define LOOPER_CC(number) ((uint8_t)(100 + (number)))   // footswitch 2-8 = CC 102-108
static bool s_footswitches;     // SX1509 found

// Scenes: up to UI_SCENES per preset on footswitches 3-8, shown in place of the bank's presets (the Scenes button
// next to Save switches between the two). A scene is which of the five effects are on: its switch sends only what
// differs, so the sound changes without the gap of a preset change. Name, colour and effects come from the scene's
// dialog (long press on its tile) and are stored on the controller per preset.
// A scene can also carry the settings of an effect (the scene button in the FX editor stores the values shown
// there): they are sent whenever the scene's switch is pressed and the effect is on in it. An effect a scene has no
// settings for is left as it is.
typedef struct {
    uint32_t type;                    // model the values belong to, 0 = the scene leaves this effect's values alone
    uint8_t count;
    uint16_t values[NANO_MAX_PARAMS]; // 0-65535 = 0-1
} scene_fx_t;
typedef struct {
    char name[UI_SCENE_NAME];   // "" = empty
    uint8_t color;              // BANK_COLORS index
    uint8_t fx;                 // bit n = FX slot n on
    scene_fx_t set[NANO_FX_SLOTS];
} scene_t;
static scene_t *s_scenes;       // UI_SCENES of them, in PSRAM (internal RAM is short)
static int s_scenes_preset;     // preset the scenes belong to, 0 = none read yet
static bool s_scene_mode;       // footswitches 3-8 are scenes instead of the bank's presets (stored)
static int s_scene_last = -1;   // recalled last: the lit one if two scenes are alike
#define SCENE_STORE_KEY "scn%02d"
#define SCENE_VALUES_KEY "scv%02d"    // the settings, apart from the names (stored as 1.8.0 did)
#define SCENE_VALUES_MAX (UI_SCENES * NANO_FX_SLOTS * (6 + 2 * NANO_MAX_PARAMS))
#define SCENE_MODE_KEY "scenemode"
// Values of a scene on their way to the Nano: the first ones go out with the scene's switches, the rest a few at a
// time (as an FX preset is sent; the Nano takes one write per 15 ms).
#define SCENE_BURST 8
#define SCENE_SEND_CHUNK 4
#define SCENE_SEND_MS 60
static struct { uint8_t slot, param; uint16_t value; } s_scene_queue[NANO_FX_SLOTS * NANO_MAX_PARAMS];
static int s_scene_queued, s_scene_sent;
static esp_timer_handle_t s_scene_timer;
// What the effects on the Nano are set to as far as the controller knows: the values it sent or read itself. A
// scene sends only what differs from that. Forgotten whenever something else may have changed them (another
// preset or model, the app, a change on the Nano).
static struct {
    uint32_t type;                    // model in the slot the values belong to
    uint32_t known;                   // bit n = parameter n
    uint16_t values[NANO_MAX_PARAMS];
} s_values[NANO_FX_SLOTS];
static volatile bool s_values_stale;  // the app wrote to the Nano (host task): forget them before they are used

static void values_forget(void)
{
    memset(s_values, 0, sizeof(s_values));
}

static uint16_t value_u16(float value)
{
    return (uint16_t)lroundf((value < 0 ? 0 : value > 1 ? 1 : value) * 65535);
}

// Own banks: preset (1-64, 0 = empty), colour index (ui palette) and symbol (0 = none, n = PRESET_ICONS[n - 1])
// per bank and switch; stored in NVS.
typedef struct { uint8_t preset, color, icon; } bank_slot_t;
static bank_slot_t s_banks[BANKS][BANK_SLOTS];

static nano_library_t *s_library;

// Save: confirmed when a state read afterwards shows the preset without unsaved changes.
static int s_save_pending;
// Rename: the old name comes back if the Nano refuses the new one.
static int s_rename_preset;
static char s_rename_old[65];
static bool s_library_requested;
static esp_timer_handle_t s_library_timer;
// After a preset change, state replies of the previous preset can still be on their way; they are ignored
// until the new preset is confirmed (otherwise the display jumps back and forth).
static int s_expect_preset;     // 0 = none
static int64_t s_expect_us;
static int s_bank;              // 0-15
static int s_last_preset;
static volatile bool s_tuner_on;

// Reverb mix switch: the switch sets the reverb's Mix to Pos 1 or Pos 2. The two values are stored on the
// controller, per preset (long press on the mix tile: moving a slider plays that mix). Up to 1.7 they were the heel
// and toe values of the preset's expression assignment for the reverb's Amount on the Nano, which kept the
// expression pedal busy: a preset that still has that assignment and no values here gets them from it once, and the
// reverb dialog can take the reverb off the pedal.
static struct {
    int slot;          // FX slot of the reverb, -1 = none
    bool known;
    float pos[2];
    int active;        // -1 = as stored in the preset, 0 = Pos 1, 1 = Pos 2
    bool own;          // Pos 1 / Pos 2 are in the controller's store
    bool exp;          // the Nano preset has the reverb's Amount on the expression pedal
    int pending;       // expression requests without reply; only the last reply counts
    int64_t request_us;
    bool touched;          // mix changed while editing: set back to the active position when closed
} s_mix = { .slot = -1, .active = -1 };
#define MIX_STORE_KEY "mix%02d"       // Pos 1, Pos 2 (0-255)

// Expression pedal of the Nano: what it moves in the current preset, from its heel to its toe position, and what
// it switches on and off. Read with every new preset and edited in the expression dialog (the pedal button of the
// top bar). A save replaces all assignments of the preset; the result is read back and compared. Taking the reverb
// off the pedal (reverb dialog) reads the assignments again first - the app may have changed them.
enum { EXP_WRITE_NONE, EXP_WRITE_DIALOG, EXP_WRITE_FREE_REVERB };
static struct {
    bool known;
    nano_exp_range_t ranges[NANO_EXP_RANGES];
    nano_exp_switch_t switches[NANO_EXP_SWITCHES];
    bool open;             // the dialog is open: it is shown what is read
    int write;             // EXP_WRITE_*: what to write once the assignments are read again
    int verify;            // EXP_WRITE_*: what was written; the next reply is compared with `wanted`
    int preset;            // preset a write or a check belongs to
    nano_exp_range_t wanted[NANO_EXP_RANGES];
    nano_exp_switch_t wanted_switches[NANO_EXP_SWITCHES];
} s_exp;

// Expression pedal calibration (the dialog's Calibrate): the Nano forgets its calibration and reports the pedal's
// position (metering) while it is moved over its whole way; the lowest and highest position are then saved.
static struct {
    bool active;
    int value, min, max;   // min > max = nothing seen yet
    int64_t logged_us;
} s_cal;

// FX editor (long press on an FX tile). Parameter values are only read for FX that are on;
// moving a control sends at most one value per PARAM_SEND_MS, plus the last one.
#define PARAM_SEND_MS 25
static struct {
    int slot;                        // -1 = closed
    bool known;
    float values[NANO_MAX_PARAMS];
    int count;
    int request_slot;                // slot of the outstanding parameter request, -1 = none
    int pending_slot, pending_param; // value waiting for the throttle timer, pending_param -1 = none
    float pending_value;
    bool throttling;
} s_edit = { .slot = -1, .request_slot = -1, .pending_param = -1 };
static esp_timer_handle_t s_param_timer, s_read_timer;

// FX presets: up to FXP_COUNT named settings per effect model, stored on the controller and shown above the
// parameters in the FX editor. A tap loads one into the edited slot (sent like the sliders, a few values at a time);
// a long press saves the current values under a name. ORIGINAL goes back to the values the effect had when the
// editor opened (or when its model was chosen: the model's defaults).
#define FXP_COUNT 4
#define FXP_SEND_CHUNK 4
typedef struct {
    char name[16];                    // "" = empty
    uint8_t count, reserved;
    uint16_t values[NANO_MAX_PARAMS]; // 0-65535 = 0-1
} fxp_t;                              // stored in NVS as an array of FXP_COUNT
static struct {
    uint32_t type;                    // model of the list, 0 = none
    int slot;                         // slot the original values belong to
    fxp_t list[FXP_COUNT];
    bool original_known;
    int original_count;
    float original[NANO_MAX_PARAMS];
    int active;                       // loaded or saved last: -1 = none, 0 = original, 1-FXP_COUNT = presets
    int send_slot, send_count, sent;  // values being sent
    float send_values[NANO_MAX_PARAMS];
} s_fxp = { .active = -1 };
static esp_timer_handle_t s_fxp_timer;

// USB playback volume (dB) from the Nano's settings; the slider sends at most one value per PARAM_SEND_MS.
static struct {
    bool known;
    float db;
    bool pending, throttling;
    float pending_db;
} s_usb;
static esp_timer_handle_t s_usb_timer, s_exp_timer, s_ab_timer, s_jack_timer;
// The Nano's EXP/MIDI connector: TRS MIDI or an expression pedal (a global setting of the Nano, read with its
// settings and switched from the expression dialog).
static int s_jack = -1;          // NANO_JACK_*, -1 = not read yet
static int s_jack_wanted = -1;   // just set: the next settings reply should say the same

// Capture volume (VOL button), capture amp knobs (long press on the capture card) and cab settings (long press on
// the cab card): each slider sends at most one value per PARAM_SEND_MS, the last one when the timer fires. The cab
// settings are read while their dialog is open; the others come with the preset state.
enum { SRC_CAPTURE_VOLUME, SRC_CAB_OUTPUT, SRC_CAB_HIGH_PASS, SRC_CAB_LOW_PASS,
       SRC_AMP_GAIN, SRC_AMP_BASS, SRC_AMP_MID, SRC_AMP_TREBLE, SRC_COUNT };
static const int SRC_AMP_KNOB[] = { NANO_AMP_GAIN, NANO_AMP_BASS, NANO_AMP_MID, NANO_AMP_TREBLE };
#define CAB_SLIDER_MAX 1000              // cab values from the UI: 0-1000 = 0-1
static struct {
    bool throttling;
    bool pending[SRC_COUNT];
    int value[SRC_COUNT];                // capture volume and amp knobs 0-255, cab settings 0-1000
    bool cab_open;                       // cab settings dialog open
    int cab_preset, cab_slot;            // preset and cab slot the shown settings belong to
} s_src;
static esp_timer_handle_t s_src_timer;

// Second effect (A/B) of a slot: the Nano holds one model per slot, so the controller swaps the model in the slot
// between the preset's effect (A) and a second one (B) and sends the stored values of the other one. B (model and
// values) is stored per preset on the controller and edited in the FX editor while it runs. The values of the
// running effect are read before a swap (only possible while it is on), so edits are kept. The values of A are
// stored too (read whenever A is on), so a switched-off A need not be switched on for that: the swap keeps the
// effect on or off. Values may be written to an effect that is off (the desktop editor does that too).
//   reverb:   the reverb dialog (long press on tile 8) chooses B; footswitch 8 swaps A <-> B - or, saved on the
//             dialog's mix tab, switches the mix again while B stays stored for later ("parked")
//   Pre FX 1: the 2ND button in its FX editor chooses B; footswitch 3 short = on/off, held = A <-> B
typedef struct {
    uint32_t type;                    // model of B, 0 = none
    uint8_t count;                    // stored values, 0 = model defaults
    uint8_t parked;                   // reverb B: kept, but footswitch 8 is the mix switch (0 in data stored before 1.8)
    float values[NANO_MAX_PARAMS];
} fx_b_t;                             // stored in NVS as it is
_Static_assert(sizeof(fx_b_t) == 8 + 4 * NANO_MAX_PARAMS, "fx_b_t is stored as it is: its size must not change");
typedef struct {
    uint32_t type;                    // model of A the values belong to
    uint8_t count, reserved;
    uint16_t values[NANO_MAX_PARAMS]; // 0-65535 = 0-1
} fx_a_t;                             // A as last read, stored in NVS
enum { AB_REVERB, AB_PRE1, AB_COUNT };
enum { AB_IDLE, AB_TURNING_ON, AB_READING, AB_LOADING };
#define AB_SEND_CHUNK 4               // parameter values per step (the BLE write queue holds 16)
typedef struct {
    int preset;                       // preset the data belongs to
    fx_b_t b;
    uint32_t a_type;                  // preset's own effect, kept from the first read
    uint8_t a_count;                  // values of A read so far, 0 = none
    float a_values[NANO_MAX_PARAMS];
    int active;                       // 0 = A, 1 = B
    int slot, target, phase, sent;    // slot -1 = not known yet (reverb)
    bool cache_only;                  // reading the values for later, no swap
    bool restore_off;                 // A was switched on only to read it: off again before the swap
    bool open_editor;                 // open the FX editor once B is loaded
    int64_t swapped_us;               // last model change by the controller (state replies may still be older)
} ab_t;
#define AB_SETTLE_US (1500 * 1000)
static ab_t s_ab[AB_COUNT];
static const char *const AB_NAMES[AB_COUNT] = { "Reverb", "Pre FX 1" };
static const char *const AB_KEYS[AB_COUNT] = { "rvb%02d", "pf1%02d" };
static const char *const AB_A_KEYS[AB_COUNT] = { "rva%02d", "pfa%02d" };
// Switches whose short press acts on release because they have a held function (bit n = footswitch n).
static volatile uint32_t s_hold_switches;

// ---- events (all state is handled in the app task) ----

static void post(app_event_t ev)
{
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        free(ev.data);
        ESP_LOGW(TAG, "Event queue full");
    }
}

static void on_message(uint32_t type, const uint8_t *payload, size_t len)
{
    if (type == NANO_MSG_TUNER_METERING && !s_tuner_on) return;
    app_event_t ev = { .kind = EV_MESSAGE, .type = type, .len = len };
    ev.data = malloc(len ? len : 1);
    if (!ev.data) {
        ESP_LOGE(TAG, "Out of memory for a message of %u bytes", (unsigned)len);
        return;
    }
    memcpy(ev.data, payload, len);
    post(ev);
}

static void on_link(bool ready)
{
    post((app_event_t){ .kind = ready ? EV_LINK_UP : EV_LINK_DOWN });
}

static void command(char c, int arg)
{
    post((app_event_t){ .kind = EV_COMMAND, .command = c, .arg = arg });
}

// A switch with a held function (s_hold_switches) acts on release when it was not held; the others at once.
// In looper mode the messages of switches 2-8 go out from here, without the way through the app task: every
// millisecond between the foot and the looper counts.
static void on_footswitch(int number, footswitch_event_t event)
{
    static bool waiting[FOOTSWITCH_COUNT + 1];   // footswitch task only
    static bool sounding[FOOTSWITCH_COUNT + 1];  // its controller is at 127
    if (sounding[number] && event == FOOTSWITCH_RELEASE) {
        sounding[number] = false;
        bool sent = phone_midi_send(LOOPER_STATUS, LOOPER_CC(number), 0);
        ESP_LOGI(TAG, "Looper switch %d released: CC %d = 0 %s", number, LOOPER_CC(number), sent ? "sent" : "NOT sent");
        return;
    }
    if (s_looper_live && number >= 2) {
        if (event == FOOTSWITCH_PRESS) {
            sounding[number] = phone_midi_send(LOOPER_STATUS, LOOPER_CC(number), 127);
            command('w', (sounding[number] ? UI_SWITCH_SENT : UI_SWITCH_SENT | UI_SWITCH_LOST) | number);   // the screen shows the press
        }
        return;
    }
    if (event == FOOTSWITCH_PRESS) {
        // (footswitch 2 closes the open tuner at once: there is nothing to hold it for then)
        waiting[number] = ((s_hold_switches >> number) & 1) && !(number == 2 && s_tuner_on);
        if (!waiting[number]) command('w', number);
    } else if (waiting[number]) {
        waiting[number] = false;
        command('w', event == FOOTSWITCH_HOLD ? UI_SWITCH_HOLD | number : number);
    }
}

static void on_footswitch_learned(int number, int swapped)
{
    command('Z', number << 8 | swapped);
}

// Bluetooth MIDI. An expression pedal sends many CC 1 values: only one event is queued, it takes the latest.
static int s_cc1 = -1;

static void on_midi(uint8_t status, uint8_t data1, uint8_t data2)
{
    if ((status & 0xF0) == 0xB0 && data1 == 1 && __atomic_exchange_n(&s_cc1, data2, __ATOMIC_SEQ_CST) >= 0) return;
    post((app_event_t){ .kind = EV_MIDI, .arg = status << 16 | data1 << 8 | data2 });
}

static void on_midi_change(void)
{
    command('N', 0);
}

// Looper app on the phone: connected / gone, and what it sends (feedback for a controller's lights).
static void on_phone_state(bool connected)
{
    command('@', connected);
}

static void on_phone_midi(uint8_t status, uint8_t data1, uint8_t data2)
{
    ESP_LOGI(TAG, "Phone MIDI %02X %u %u", status, data1, data2);
}

static void on_text(char kind, const char *text)
{
    app_event_t ev = { .kind = EV_TEXT, .command = kind, .data = (uint8_t *)strdup(text) };
    if (ev.data) post(ev);
}

static void on_param(int slot, int param, float value)
{
    post((app_event_t){ .kind = EV_PARAM, .arg = slot, .param = param, .value = value });
}

static void param_timer_cb(void *arg)
{
    command('T', 0);
}

static void jack_timer_cb(void *arg)
{
    command('?', 0);
}

static void usb_timer_cb(void *arg)
{
    command('G', 0);
}

static void fxp_timer_cb(void *arg)
{
    command('#', 0);
}

static void scene_timer_cb(void *arg)
{
    command('*', 0);
}

static void source_timer_cb(void *arg)
{
    command('u', 0);
}

static void exp_timer_cb(void *arg)
{
    command('J', 0);
}

static void midi_gate_cb(void *arg)
{
    command('q', 0);
}

static void app_read_cb(void *arg)
{
    command('j', 0);
}

static void app_notify_cb(void *arg)
{
    command('f', 0);
}

// From the NimBLE host task: the app sent a message (type) / the app connected or left.
static void on_app_write(uint32_t type)
{
    s_values_stale = true;   // it may have set an effect's values
    switch (type) {
    case 3: case 111:                               // save, rename: names may have changed
        command('i', 1);
        break;
    case 26: case 28: case 29: case 31: case 67: case 78: case 80: case 136:
        command('i', 0);
        break;
    case 62:                                        // expression assignments
        command('i', 2);
        break;
    case 94:                                        // cab setting: read it again if the cab dialog is open
        command('y', 2);
        break;
    default:                                        // reads and parameter values change nothing the controller shows
        break;
    }
}

static void on_app_state(bool connected)
{
    command('o', connected);
}

// The controller changed something on the Nano: tell the app once things are quiet (the app then reads the whole
// preset again, which keeps the link busy for a moment - after a run of footswitch presses only once).
static void app_sync_later(void)
{
    if (!app_link_connected()) return;
    esp_timer_stop(s_app_notify_timer);
    esp_timer_start_once(s_app_notify_timer, 1200 * 1000);
}

static void ab_timer_cb(void *arg)
{
    command('F', 0);
}

static void read_timer_cb(void *arg)
{
    command('R', 0);
}

static void refresh_cb(void *arg)
{
    command('s', 0);
}

// Reads the current preset again after the Nano has had time to apply a change.
static void schedule_refresh(int ms)
{
    esp_timer_stop(s_refresh_timer);
    esp_timer_start_once(s_refresh_timer, (uint64_t)ms * 1000);
}

// ---- output ----

static const char *preset_name(int preset)
{
    const char *name = s_state.preset_names[preset - 1];
    return s_state.names_loaded && name[0] ? name : "?";
}

static void print_summary(void)
{
    printf("\n==================================================\n");
    printf(" Preset %d: %s%s\n", s_state.current_preset, preset_name(s_state.current_preset), s_state.dirty ? "  (edited)" : "");
    printf(" Capture: %s\n", s_state.capture[0] ? s_state.capture : "-");
    printf(" Cab/IR:  %s\n", s_state.cab[0] ? s_state.cab : "-");
    for (int i = 0; i < NANO_FX_SLOTS; i++) {
        printf(" %c  %-9s  %-24s %s\n", 'a' + i, NANO_FX_SLOT_NAMES[i], nano_fx_name(s_state.fx_type[i]),
               !s_state.fx_known || !s_state.fx_type[i] ? "" : s_state.fx_on[i] ? "ON" : "off");
    }
    printf("==================================================\n");
}

static void print_presets(void)
{
    for (int i = 1; i <= NANO_PRESETS; i++) printf(" %2d  %s%s\n", i, preset_name(i), i == s_state.current_preset ? "   <" : "");
}

static void print_help(void)
{
    printf("\nCommands (type into the serial monitor):\n"
           "  n / p        next / previous preset\n"
           "  1-64         go to a preset (Enter or wait a moment)\n"
           "  a-e          FX on/off: a = Pre FX 1 ... e = Post FX 3\n"
           "  m / t / x    mode (footswitch 1) / tuner (2) / reverb switch (8 in FX mode)\n"
           "  o            looper mode on / off (as holding footswitch 1); then t = its switch 2\n"
           "  $            scenes / presets on footswitches 3-8 (the scenes button, or footswitch 2 held)\n"
           "  s            read the current preset again\n"
           "  r            read everything again (presets, names, library)\n"
           "  l            list all preset names\n"
           "  h            this help\n\n");
}

// ---- own banks ----

static void banks_default(void)
{
    for (int bank = 0; bank < BANKS; bank++) {
        for (int i = 0; i < BANK_SLOTS; i++) {
            int preset = bank * BANK_SLOTS + i + 1;
            s_banks[bank][i] = (bank_slot_t){ .preset = (uint8_t)(preset <= NANO_PRESETS ? preset : 0), .color = 0 };
        }
    }
}

static void banks_load(void)
{
    banks_default();
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_blob(nvs, BANK_STORE_KEY, NULL, &size) == ESP_OK) {
        static uint8_t stored[BANKS * BANK_SLOTS * sizeof(bank_slot_t)];
        if (size == sizeof(s_banks) && nvs_get_blob(nvs, BANK_STORE_KEY, stored, &size) == ESP_OK) {
            memcpy(s_banks, stored, sizeof(s_banks));
        } else if (size == BANKS * BANK_SLOTS * 2 && nvs_get_blob(nvs, BANK_STORE_KEY, stored, &size) == ESP_OK) {
            // Stored before the symbols existed: preset and colour only.
            for (int i = 0; i < BANKS * BANK_SLOTS; i++) {
                s_banks[i / BANK_SLOTS][i % BANK_SLOTS] = (bank_slot_t){ .preset = stored[2 * i], .color = stored[2 * i + 1] };
            }
        }
    }
    nvs_close(nvs);
}

static void show(void);

static void looper_tiles_load(void)
{
    memcpy(s_looper_tiles, LOOPER_DEFAULTS, sizeof(s_looper_tiles));
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    looper_tile_t stored[UI_LOOPER_SWITCHES];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, LOOPER_STORE_KEY, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for (int i = 0; i < UI_LOOPER_SWITCHES; i++) {
            stored[i].name[UI_LOOPER_NAME - 1] = 0;
            if (stored[i].color < UI_BANK_COLOR_COUNT && stored[i].icon < UI_LOOPER_SYMBOLS) s_looper_tiles[i] = stored[i];
        }
    }
    nvs_close(nvs);
}

// From the looper tile's dialog: "<footswitch 2-8><colour a-j><symbol a-g>name", or "<footswitch>!" for the default.
static void looper_tile_edit(const char *text)
{
    int i = text[0] - '2';
    if (i < 0 || i >= UI_LOOPER_SWITCHES || !text[1]) return;
    looper_tile_t tile = LOOPER_DEFAULTS[i];
    if (text[1] != '!') {
        int color = text[1] - 'a', icon = text[2] ? text[2] - 'a' : -1;
        if (color < 0 || color >= UI_BANK_COLOR_COUNT || icon < 0 || icon >= UI_LOOPER_SYMBOLS) return;
        tile.color = (uint8_t)color;
        tile.icon = (uint8_t)icon;
        const char *name = text + 3;
        while (*name == ' ') name++;
        if (*name) strlcpy(tile.name, name, sizeof(tile.name));
    }
    s_looper_tiles[i] = tile;
    ESP_LOGI(TAG, "Looper switch %d: \"%s\", colour %d, symbol %d", i + 2, tile.name, tile.color, tile.icon);
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_blob(nvs, LOOPER_STORE_KEY, s_looper_tiles, sizeof(s_looper_tiles)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
            ui_show_message("Could not store the looper tiles.");
        }
        nvs_close(nvs);
    }
    if (nano_link_ready()) show();
}

// Scenes of a preset. Stored as one entry per used scene: index, colour, effects, name with its 0.
static void scenes_load(int preset)
{
    memset(s_scenes, 0, UI_SCENES * sizeof(scene_t));
    s_scenes_preset = preset;
    s_scene_last = -1;
    s_scene_queued = s_scene_sent = 0;   // values of the previous preset's scene still on their way
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), SCENE_STORE_KEY, preset);
    uint8_t stored[UI_SCENES * (3 + UI_SCENE_NAME)];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, key, stored, &size) == ESP_OK) {
        for (size_t at = 0; at + 4 <= size;) {
            const uint8_t *entry = stored + at;
            size_t length = strnlen((const char *)entry + 3, size - at - 3);
            if (at + 3 + length >= size) break;   // no 0 at the end
            at += 3 + length + 1;
            if (entry[0] >= UI_SCENES || entry[1] >= UI_BANK_COLOR_COUNT || !length || length >= UI_SCENE_NAME) continue;
            scene_t *scene = &s_scenes[entry[0]];
            memcpy(scene->name, entry + 3, length + 1);
            scene->color = entry[1];
            scene->fx = entry[2] & ((1u << NANO_FX_SLOTS) - 1);
        }
    }
    // Settings: scene << 4 | slot, count, model (4 bytes), count values (2 bytes each)
    uint8_t *values = heap_caps_malloc(SCENE_VALUES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(key, sizeof(key), SCENE_VALUES_KEY, preset);
    size = SCENE_VALUES_MAX;
    if (values && nvs_get_blob(nvs, key, values, &size) == ESP_OK) {
        for (size_t at = 0; at + 6 <= size;) {
            const uint8_t *entry = values + at;
            int n = entry[0] >> 4, slot = entry[0] & 0xF, count = entry[1];
            if (count > NANO_MAX_PARAMS || at + 6 + 2 * (size_t)count > size) break;
            at += 6 + 2 * (size_t)count;
            uint32_t type = entry[2] | entry[3] << 8 | entry[4] << 16 | (uint32_t)entry[5] << 24;
            if (n >= UI_SCENES || slot >= NANO_FX_SLOTS || !count || !s_scenes[n].name[0] || !nano_fx_model(type)) continue;
            scene_fx_t *set = &s_scenes[n].set[slot];
            set->type = type;
            set->count = (uint8_t)count;
            for (int i = 0; i < count; i++) set->values[i] = (uint16_t)(entry[6 + 2 * i] | entry[7 + 2 * i] << 8);
        }
    }
    heap_caps_free(values);
    nvs_close(nvs);
}

static void scenes_save(void)
{
    uint8_t stored[UI_SCENES * (3 + UI_SCENE_NAME)];
    size_t size = 0;
    for (int i = 0; i < UI_SCENES; i++) {
        const scene_t *scene = &s_scenes[i];
        if (!scene->name[0]) continue;
        stored[size++] = (uint8_t)i;
        stored[size++] = scene->color;
        stored[size++] = scene->fx;
        size_t length = strlen(scene->name) + 1;
        memcpy(stored + size, scene->name, length);
        size += length;
    }
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), SCENE_STORE_KEY, s_scenes_preset);
    esp_err_t err = size ? nvs_set_blob(nvs, key, stored, size) : nvs_erase_key(nvs, key);
    bool failed = err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND;
    // The settings: written only if they differ from what is stored (spares the flash)
    uint8_t *values = heap_caps_malloc(2 * SCENE_VALUES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!values) {
        nvs_close(nvs);
        return;
    }
    uint8_t *before = values + SCENE_VALUES_MAX;
    size = 0;
    for (int i = 0; i < UI_SCENES; i++) {
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
            const scene_fx_t *set = &s_scenes[i].set[slot];
            if (!s_scenes[i].name[0] || !set->type || !set->count) continue;
            values[size++] = (uint8_t)(i << 4 | slot);
            values[size++] = set->count;
            for (int b = 0; b < 4; b++) values[size++] = (uint8_t)(set->type >> (8 * b));
            for (int v = 0; v < set->count; v++) {
                values[size++] = (uint8_t)set->values[v];
                values[size++] = (uint8_t)(set->values[v] >> 8);
            }
        }
    }
    snprintf(key, sizeof(key), SCENE_VALUES_KEY, s_scenes_preset);
    size_t stored_size = SCENE_VALUES_MAX;
    bool had = nvs_get_blob(nvs, key, before, &stored_size) == ESP_OK;
    if (!(had ? size == stored_size && !memcmp(values, before, size) : !size)) {
        err = size ? nvs_set_blob(nvs, key, values, size) : nvs_erase_key(nvs, key);
        failed |= err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND;
    }
    heap_caps_free(values);
    if (failed || nvs_commit(nvs) != ESP_OK) ui_show_message("Could not store the scene (memory full?).");
    nvs_close(nvs);
}

// The scene whose effects are on right now (the one recalled last if several are alike), -1 = none. Slots
// without an effect do not count. Not while the state of a newly chosen preset is still on its way.
static int scene_active(void)
{
    if (!s_state.fx_known || s_expect_preset) return -1;
    uint8_t filled = 0, on = 0;
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        if (!s_state.fx_type[slot]) continue;
        filled |= (uint8_t)(1u << slot);
        if (s_state.fx_on[slot]) on |= (uint8_t)(1u << slot);
    }
    int found = -1;
    for (int i = 0; i < UI_SCENES; i++) {
        if (!s_scenes[i].name[0] || (s_scenes[i].fx & filled) != on) continue;
        if (i == s_scene_last) return i;
        if (found < 0) found = i;
    }
    return found;
}

static void banks_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, BANK_STORE_KEY, s_banks, sizeof(s_banks)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Saving the banks failed");
    }
    nvs_close(nvs);
}

// Keeps the current bank if it holds the preset, otherwise jumps to the first bank that does.
static void follow_preset_with_bank(int preset)
{
    for (int i = 0; i < BANK_SLOTS; i++) if (s_banks[s_bank][i].preset == preset) return;
    for (int bank = 0; bank < BANKS; bank++) {
        for (int i = 0; i < BANK_SLOTS; i++) {
            if (s_banks[bank][i].preset == preset) {
                s_bank = bank;
                return;
            }
        }
    }
}

// ---- display ----

static int reverb_slot(void)
{
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        const nano_fx_model_t *model = nano_fx_model(s_state.fx_type[slot]);
        if (model && model->icon == NANO_ICON_REVERB) return slot;
    }
    return -1;
}

// Footswitch 8 in FX mode swaps reverb A <-> B (a second reverb is set and not parked); otherwise it is the mix switch.
static bool reverb_b_used(void)
{
    return s_ab[AB_REVERB].b.type && !s_ab[AB_REVERB].b.parked;
}

// Which footswitches wait for their release (they have a held function): 1 (held = looper mode) and 2 (held =
// scenes <-> presets) always, 3 in FX mode with a second effect. In looper mode none - there every press goes out
// at once.
static void looper_switches(void)
{
    s_hold_switches = s_looper ? 0 : 1u << 1 | 1u << 2 | (s_fx_mode && s_ab[AB_PRE1].b.type ? 1u << 3 : 0);
    s_looper_live = s_looper;
}

static void show(void)
{
    ui_view_t view = {
        .fx_mode = s_fx_mode,
        .bank = s_bank,
        .mix_slot = s_mix.slot,
        .mix_known = s_mix.known,
        .mix_pos = { s_mix.pos[0], s_mix.pos[1] },
        .mix_active = s_mix.active,
        .mix_exp = s_mix.exp,
        .rev_b_type = reverb_b_used() ? s_ab[AB_REVERB].b.type : 0,
        .rev_b_stored = s_ab[AB_REVERB].b.type,
        .rev_active = s_ab[AB_REVERB].active,
        .pre1_b_type = s_ab[AB_PRE1].b.type,
        .pre1_active = s_ab[AB_PRE1].active,
        .pre1_a_type = s_ab[AB_PRE1].a_type,
        .looper = s_looper,
        .phone = phone_midi_connected(),
        .looper_sent = s_looper_sent,
        .scenes = s_scene_mode,
    };
    for (int i = 0; i < NANO_EXP_RANGES && s_exp.known; i++) view.exp_used |= s_exp.ranges[i].on;
    for (int i = 0; i < NANO_EXP_SWITCHES && s_exp.known; i++) view.exp_used |= s_exp.switches[i].on;
    if (s_scenes_preset != s_state.current_preset) scenes_load(s_state.current_preset);
    view.scene_active = scene_active();
    for (int i = 0; i < UI_SCENES; i++) {
        strlcpy(view.scene_names[i], s_scenes[i].name, sizeof(view.scene_names[i]));
        view.scene_colors[i] = s_scenes[i].color;
        view.scene_fx[i] = s_scenes[i].fx;
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {   // settings count while their model is in the slot
            const scene_fx_t *set = &s_scenes[i].set[slot];
            if (set->type && set->type == s_state.fx_type[slot]) view.scene_set[i] |= (uint8_t)(1u << slot);
        }
    }
    for (int i = 0; i < UI_LOOPER_SWITCHES; i++) {
        strlcpy(view.looper_names[i], s_looper_tiles[i].name, sizeof(view.looper_names[i]));
        view.looper_colors[i] = s_looper_tiles[i].color;
        view.looper_icons[i] = s_looper_tiles[i].icon;
    }
    looper_switches();
    for (int i = 0; i < BANK_SLOTS; i++) {
        view.bank_presets[i] = s_banks[s_bank][i].preset;
        view.bank_colors[i] = s_banks[s_bank][i].color;
        view.bank_icons[i] = s_banks[s_bank][i].icon;
    }
    ui_show_state(&s_state, &view);
}

static void fxp_show(void);

static void editor_show(void)
{
    if (s_edit.slot < 0) return;
    ui_fx_editor_show(s_edit.slot, s_state.fx_type[s_edit.slot], s_state.fx_on[s_edit.slot],
                      s_edit.known ? s_edit.values : NULL, s_edit.count);
    fxp_show();
}

static void editor_close(void)
{
    if (s_edit.slot < 0) return;
    s_edit.slot = -1;
    s_fxp.slot = -2;   // the next editor reads its own original values
    ui_fx_editor_close();
}

// Reads again later (after a model change or after switching the FX on).
static void editor_read_later(int ms)
{
    esp_timer_stop(s_read_timer);
    esp_timer_start_once(s_read_timer, (uint64_t)ms * 1000);
}

// ---- actions ----

static int64_t s_last_change_us;   // the controller's own last change (the Nano answers it with change notices too)

static bool send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    switch (type) {
    case NANO_MSG_STATE_REQUEST: case NANO_MSG_EXP_REQUEST: case NANO_MSG_SETTINGS_REQUEST:
    case NANO_MSG_LIBRARY_REQUEST: case NANO_MSG_CAB_SETTINGS_REQUEST: case NANO_MSG_FX_PARAMS_REQUEST:
    case NANO_MSG_SET_PRESET_SLOTS_RESPONSE:
        break;   // reads change nothing
    case NANO_MSG_SET_PRESET_SLOTS: case NANO_MSG_FX_TYPE:
        values_forget();   // another preset, or a model with its own values
        // fall through
    default:
        s_last_change_us = esp_timer_get_time();
        break;
    }
    if (nano_link_send(label, type, payload, len)) return true;
    ESP_LOGW(TAG, "Could not queue: %s", label);
    return false;
}

static void request_full_state(void)
{
    uint8_t buf[8];
    s_waiting_full_state = true;
    send("Full state request", NANO_MSG_STATE_REQUEST, buf, nano_full_state_request(buf));
}

static void request_current_state(void)
{
    uint8_t buf[16];
    send("Current state request", NANO_MSG_STATE_REQUEST, buf, nano_current_state_request(buf));
}

static void ab_cache(int slot, int delay_ms);

static void select_preset(int preset)
{
    app_sync_later();
    if (preset < 1) preset = NANO_PRESETS;
    if (preset > NANO_PRESETS) preset = 1;
    uint8_t buf[64];
    char label[32];
    snprintf(label, sizeof(label), "Preset change %d", preset);
    send(label, NANO_MSG_SET_PRESET_SLOTS, buf, nano_preset_change(&s_state, preset, buf));
    s_state.current_preset = preset;
    s_expect_preset = preset;
    s_expect_us = esp_timer_get_time();
    show();
    schedule_refresh(450);
}

// Switches the effect of a slot on or off (the caller shows the state and reads it again).
static void set_fx(int slot, bool on)
{
    uint8_t buf[8];
    char label[32];
    snprintf(label, sizeof(label), "%s %s", NANO_FX_SLOT_NAMES[slot], on ? "ON" : "OFF");
    send(label, NANO_MSG_BYPASS, buf, nano_fx_bypass(slot, on, buf));
    s_state.fx_on[slot] = on;
    if (slot == s_edit.slot) {
        if (!on) s_edit.known = false;
        editor_show();
        if (on) editor_read_later(400);
    } else if (on) {
        ab_cache(slot, 400);   // values of A for a later swap, if this slot has a second effect
    }
}

static void toggle_fx(int slot)
{
    app_sync_later();
    if (!s_state.fx_known || !s_state.fx_type[slot]) {
        ESP_LOGW(TAG, "%s is empty", NANO_FX_SLOT_NAMES[slot]);
        return;
    }
    set_fx(slot, !s_state.fx_on[slot]);
    show();
    schedule_refresh(300);
}

static bool send_param(int slot, int param, float value);

// The next values of the scene that was switched to.
static void scene_send_step(int count)
{
    for (; count > 0 && s_scene_sent < s_scene_queued; count--, s_scene_sent++) {
        if (!send_param(s_scene_queue[s_scene_sent].slot, s_scene_queue[s_scene_sent].param,
                        s_scene_queue[s_scene_sent].value / 65535.0f)) break;   // write queue full: with the next step
    }
    if (s_scene_sent < s_scene_queued) esp_timer_start_once(s_scene_timer, SCENE_SEND_MS * 1000);
}

// Queues the settings a scene carries for a slot, as far as they differ from what the effect is set to.
// Returns how many there are.
static int scene_queue_values(const scene_t *scene, int slot)
{
    const scene_fx_t *set = &scene->set[slot];
    if (!set->type || set->type != s_state.fx_type[slot]) return 0;   // none, or for a model that is not in the slot
    bool tracked = s_values[slot].type == set->type;
    int queued = 0;
    for (int param = 0; param < set->count; param++) {
        if (tracked && ((s_values[slot].known >> param) & 1) && s_values[slot].values[param] == set->values[param]) continue;
        s_scene_queue[s_scene_queued].slot = (uint8_t)slot;
        s_scene_queue[s_scene_queued].param = (uint8_t)param;
        s_scene_queue[s_scene_queued++].value = set->values[param];
        queued++;
        if (slot == s_edit.slot && s_edit.known && param < s_edit.count) s_edit.values[param] = set->values[param] / 65535.0f;
    }
    return queued;
}

// Footswitch 3-8 in scene mode: the effects go on and off as the scene says and take the settings it carries -
// only what differs is sent. Effects go off at once; one that comes on gets its settings first (if they are few
// enough to go out at once), so it does not sound with the old ones for a moment.
static void scene_recall(int n)
{
    const scene_t *scene = &s_scenes[n];
    if (!scene->name[0]) {
        ui_show_message("This scene is empty - hold its tile to set it up.");
        return;
    }
    if (!s_state.fx_known || s_scenes_preset != s_state.current_preset) return;
    if (s_values_stale) {
        s_values_stale = false;
        values_forget();
    }
    esp_timer_stop(s_scene_timer);
    s_scene_queued = s_scene_sent = 0;
    int switched = 0, coming = 0;
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        bool on = (scene->fx >> slot) & 1;
        if (!s_state.fx_type[slot] || s_state.fx_on[slot] == on) continue;
        switched++;
        if (on) coming |= 1 << slot;
        else set_fx(slot, false);
    }
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        if (!((coming >> slot) & 1)) continue;
        scene_queue_values(scene, slot);
        if (s_scene_queued <= SCENE_BURST) scene_send_step(s_scene_queued - s_scene_sent);   // they fit: before the switch
        set_fx(slot, true);
    }
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {   // the effects that stay on
        if (((scene->fx >> slot) & 1) && s_state.fx_type[slot] && !((coming >> slot) & 1)) scene_queue_values(scene, slot);
    }
    int values = s_scene_queued;
    bool edited = false;   // the open FX editor shows one of the effects that got other values
    for (int i = 0; i < s_scene_queued; i++) edited |= s_scene_queue[i].slot == s_edit.slot;
    scene_send_step(SCENE_BURST - s_scene_sent);
    s_scene_last = n;
    ESP_LOGI(TAG, "Scene %d \"%s\": %d effect%s switched, %d value%s set", n + 1, scene->name, switched, switched == 1 ? "" : "s",
             values, values == 1 ? "" : "s");
    if (edited) {
        s_fxp.active = -1;   // no FX preset is what the effect is set to any more
        editor_show();
    }
    show();
    if (!switched && !values) return;
    app_sync_later();
    schedule_refresh(300);
}

// The scene button of the FX editor. arg = scene 0-5: the values of the edited effect as they are now belong to
// that scene from now on; arg | 0x100: the scene leaves this effect's values alone again.
static void scene_values(int arg)
{
    int n = arg & 0xFF, slot = s_edit.slot;
    if (n >= UI_SCENES || slot < 0 || !s_state.fx_type[slot]) return;
    if (s_scenes_preset != s_state.current_preset) scenes_load(s_state.current_preset);
    scene_t *scene = &s_scenes[n];
    if (!scene->name[0]) return;
    scene_fx_t *set = &scene->set[slot];
    char text[96];
    if (arg & 0x100) {
        if (!set->type) return;
        memset(set, 0, sizeof(*set));
        snprintf(text, sizeof(text), "Scene \"%s\" no longer sets %s.", scene->name, nano_fx_name(s_state.fx_type[slot]));
    } else {
        if (!s_state.fx_on[slot] || !s_edit.known || s_edit.count <= 0) {
            ui_show_message("Switch the effect on first - its settings can only be read while it is on.");
            return;
        }
        memset(set, 0, sizeof(*set));
        set->type = s_state.fx_type[slot];
        set->count = (uint8_t)s_edit.count;
        for (int i = 0; i < s_edit.count; i++) set->values[i] = value_u16(s_edit.values[i]);
        snprintf(text, sizeof(text), "Settings saved for scene \"%s\"%s.", scene->name,
                 (scene->fx >> slot) & 1 ? "" : " - the effect is off in it");
    }
    ESP_LOGI(TAG, "Preset %d scene %d, %s: %s", s_scenes_preset, n + 1, NANO_FX_SLOT_NAMES[slot], set->type ? "settings stored" : "settings removed");
    scenes_save();
    ui_show_message(text);
    show();
}

// From the scene's dialog: "<footswitch 3-8><colour a-j><five times 0 / 1: Pre FX 1 ... Post FX 3>name" stores the
// scene and switches to it, "<footswitch>!" empties it.
static void scene_edit(const char *text)
{
    int n = text[0] - '3';
    if (n < 0 || n >= UI_SCENES || !text[1] || !nano_link_ready()) return;
    if (s_scenes_preset != s_state.current_preset) scenes_load(s_state.current_preset);
    scene_t scene = { 0 };
    if (text[1] != '!') {
        memcpy(scene.set, s_scenes[n].set, sizeof(scene.set));   // name, colour and switches change, the settings stay
        int color = text[1] - 'a';
        if (color < 0 || color >= UI_BANK_COLOR_COUNT || strlen(text) < 2 + NANO_FX_SLOTS) return;
        scene.color = (uint8_t)color;
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
            if (text[2 + slot] == '1') scene.fx |= (uint8_t)(1u << slot);
        }
        const char *name = text + 2 + NANO_FX_SLOTS;
        while (*name == ' ') name++;
        if (*name) strlcpy(scene.name, name, sizeof(scene.name));
        else snprintf(scene.name, sizeof(scene.name), "Scene %d", n + 1);
    }
    s_scenes[n] = scene;
    scenes_save();
    ESP_LOGI(TAG, "Preset %d scene %d: %s", s_scenes_preset, n + 1, scene.name[0] ? scene.name : "emptied");
    if (scene.name[0]) scene_recall(n);
    else show();
}

// Reads the parameters of the edited slot - never for a bypassed FX (that crashed the Nano).
static void editor_read(void)
{
    int slot = s_edit.slot;
    if (slot < 0 || !s_state.fx_type[slot] || !s_state.fx_on[slot]) return;
    uint8_t buf[8];
    s_edit.request_slot = slot;
    send("FX parameters request", NANO_MSG_FX_PARAMS_REQUEST, buf, nano_fx_params_request(slot, buf));
}

// A value the effect in a slot has now (sent, or read from the Nano).
static void value_note(int slot, int param, float value)
{
    if (s_values[slot].type != s_state.fx_type[slot]) {
        s_values[slot].type = s_state.fx_type[slot];
        s_values[slot].known = 0;
    }
    s_values[slot].values[param] = value_u16(value);
    s_values[slot].known |= 1u << param;
}

static bool send_param(int slot, int param, float value)
{
    app_sync_later();
    uint8_t buf[16];
    char label[40];
    snprintf(label, sizeof(label), "%s parameter %d", NANO_FX_SLOT_NAMES[slot], param);
    if (!send(label, NANO_MSG_FX_VALUE, buf, nano_fx_param(slot, param, value, buf))) return false;
    value_note(slot, param, value);
    return true;
}

// A control moved (FX editor, reverb Pos 1 / Pos 2): send now, or remember the value until the throttle
// timer fires.
static void throttled_param(int slot, int param, float value)
{
    if (s_edit.pending_param >= 0 && (s_edit.pending_slot != slot || s_edit.pending_param != param)) {
        send_param(s_edit.pending_slot, s_edit.pending_param, s_edit.pending_value);
        s_edit.pending_param = -1;
    }
    if (s_edit.throttling) {
        s_edit.pending_slot = slot;
        s_edit.pending_param = param;
        s_edit.pending_value = value;
        return;
    }
    send_param(slot, param, value);
    s_edit.throttling = true;
    esp_timer_start_once(s_param_timer, PARAM_SEND_MS * 1000);
}

static void editor_param(int slot, int param, float value)
{
    if (slot != s_edit.slot || param < 0 || param >= NANO_MAX_PARAMS) return;
    s_edit.values[param] = value;
    if (s_fxp.active >= 0) {   // changed by hand: no FX preset is current any more
        s_fxp.active = -1;
        fxp_show();
    }
    throttled_param(slot, param, value);
}

static void editor_throttle_done(void)
{
    s_edit.throttling = false;
    if (s_edit.pending_param < 0) return;
    send_param(s_edit.pending_slot, s_edit.pending_param, s_edit.pending_value);
    s_edit.pending_param = -1;
    s_edit.throttling = true;
    esp_timer_start_once(s_param_timer, PARAM_SEND_MS * 1000);
}

static void request_settings(void)
{
    uint8_t buf[4];
    send("Settings request", NANO_MSG_SETTINGS_REQUEST, buf, nano_settings_request(buf));
}

// Expression dialog: the connector's mode (NANO_JACK_*). Written, then the settings are read again to check.
static void jack_set(int mode)
{
    if (mode != NANO_JACK_MIDI && mode != NANO_JACK_EXPRESSION) return;
    uint8_t buf[4];
    send(mode == NANO_JACK_MIDI ? "EXP/MIDI connector: MIDI" : "EXP/MIDI connector: expression pedal", NANO_MSG_UPDATE_SETTINGS, buf,
         nano_jack_update(mode, buf));
    app_sync_later();
    s_jack_wanted = mode;
    esp_timer_stop(s_jack_timer);
    esp_timer_start_once(s_jack_timer, 500 * 1000);
}

static void send_usb_gain(float db)
{
    uint8_t buf[8];
    char label[40];
    snprintf(label, sizeof(label), "USB volume %.1f dB", db);
    send(label, NANO_MSG_UPDATE_SETTINGS, buf, nano_usb_gain(db, buf));
    s_usb.throttling = true;
    esp_timer_start_once(s_usb_timer, PARAM_SEND_MS * 1000);
}

// USB volume slider moved (tenths of a dB).
static void set_usb_gain(int tenths)
{
    float db = tenths / 10.0f;
    if (db < NANO_USB_GAIN_MIN_DB) db = NANO_USB_GAIN_MIN_DB;
    if (db > 0) db = 0;
    s_usb.db = db;
    if (s_usb.throttling) {
        s_usb.pending = true;
        s_usb.pending_db = db;
        return;
    }
    send_usb_gain(db);
}

static void usb_throttle_done(void)
{
    s_usb.throttling = false;
    if (!s_usb.pending) return;
    s_usb.pending = false;
    send_usb_gain(s_usb.pending_db);
}

static void send_source_value(int what, int value)
{
    app_sync_later();
    uint8_t buf[16];
    char label[40];
    if (what == SRC_CAPTURE_VOLUME) {
        snprintf(label, sizeof(label), "Capture volume %+.1f dB", nano_capture_volume_db(value));
        send(label, NANO_MSG_VALUE, buf, nano_capture_volume(value, buf));
        s_state.capture_volume = value;
        return;
    }
    if (what >= SRC_AMP_GAIN) {
        static const char *const knobs[] = { "gain", "bass", "mid", "treble" };
        int knob = SRC_AMP_KNOB[what - SRC_AMP_GAIN];
        snprintf(label, sizeof(label), "Capture %s %.1f", knobs[what - SRC_AMP_GAIN], value / 25.5f);
        send(label, NANO_MSG_VALUE, buf, nano_amp_knob(knob, value, buf));
        s_state.amp[knob] = value;
        return;
    }
    static const char *const names[NANO_CAB_SETTINGS] = { "output", "high pass", "low pass" };
    int which = what - SRC_CAB_OUTPUT;
    float normalized = value / (float)CAB_SLIDER_MAX;
    snprintf(label, sizeof(label), "Cab %s %.1f", names[which], nano_cab_setting_value(which, normalized));
    send(label, NANO_MSG_CAB_SETTING, buf, nano_cab_setting(which, normalized, buf));
}

// A capture volume / cab slider moved: arg = what << 16 | value.
static void source_value(int arg)
{
    int what = arg >> 16, value = arg & 0xFFFF;
    if (what < 0 || what >= SRC_COUNT) return;
    if (what >= SRC_CAB_OUTPUT && what <= SRC_CAB_LOW_PASS && !s_state.cab_slot) return;   // cab bypassed
    if (s_src.throttling) {
        s_src.pending[what] = true;
        s_src.value[what] = value;
        return;
    }
    send_source_value(what, value);
    s_src.throttling = true;
    esp_timer_start_once(s_src_timer, PARAM_SEND_MS * 1000);
}

static void source_throttle_done(void)
{
    s_src.throttling = false;
    bool sent = false;
    for (int i = 0; i < SRC_COUNT; i++) {
        if (!s_src.pending[i]) continue;
        s_src.pending[i] = false;
        send_source_value(i, s_src.value[i]);
        sent = true;
    }
    if (sent) {
        s_src.throttling = true;
        esp_timer_start_once(s_src_timer, PARAM_SEND_MS * 1000);
    }
}

// Reads the settings of the active cab for the dialog (a bypassed cab has none).
static void request_cab_settings(void)
{
    s_src.cab_preset = s_state.current_preset;
    s_src.cab_slot = s_state.cab_slot;
    if (!s_state.cab_slot) {
        ui_set_cab_settings(NULL, false, "The cab is bypassed - switch it on to change its settings.");
        return;
    }
    uint8_t buf[8];
    send("Cab settings request", NANO_MSG_CAB_SETTINGS_REQUEST, buf, nano_cab_settings_request(s_state.cab_slot, buf));
}

// Cab settings dialog: 1 = opened, 0 = closed, 2 = the app changed a cab setting.
static void cab_settings_command(int arg)
{
    if (arg == 2 && !s_src.cab_open) return;
    s_src.cab_open = arg != 0;
    if (s_src.cab_open) request_cab_settings();
}

// ---- FX presets ----

static void fxp_key(uint32_t type, char *key)
{
    snprintf(key, 16, "fxp%lu", (unsigned long)type);
}

// The presets of the model in the editor, and its original values once they are read.
static void fxp_show(void)
{
    int slot = s_edit.slot;
    uint32_t type = slot >= 0 ? s_state.fx_type[slot] : 0;
    if (type != s_fxp.type || slot != s_fxp.slot) {
        s_fxp.type = type;
        s_fxp.slot = slot;
        s_fxp.original_known = false;
        s_fxp.active = -1;
        memset(s_fxp.list, 0, sizeof(s_fxp.list));
        nvs_handle_t nvs;
        char key[16];
        fxp_key(type, key);
        if (type && nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t size = sizeof(s_fxp.list);
            if (nvs_get_blob(nvs, key, s_fxp.list, &size) != ESP_OK || size != sizeof(s_fxp.list)) memset(s_fxp.list, 0, sizeof(s_fxp.list));
            nvs_close(nvs);
        }
    }
    if (slot >= 0 && s_edit.known && !s_fxp.original_known) {
        s_fxp.original_known = true;
        s_fxp.original_count = s_edit.count;
        memcpy(s_fxp.original, s_edit.values, sizeof(s_fxp.original));
    }
    char names[FXP_COUNT][16];
    for (int i = 0; i < FXP_COUNT; i++) strlcpy(names[i], s_fxp.list[i].name, sizeof(names[i]));
    ui_fx_presets_show(names, s_fxp.active, slot >= 0 && s_edit.known && s_state.fx_on[slot]);
}

static void fxp_send_step(void)
{
    int slot = s_fxp.send_slot;
    for (int n = 0; n < FXP_SEND_CHUNK && s_fxp.sent < s_fxp.send_count; n++, s_fxp.sent++) {
        send_param(slot, s_fxp.sent, s_fxp.send_values[s_fxp.sent]);
    }
    if (s_fxp.sent < s_fxp.send_count) esp_timer_start_once(s_fxp_timer, 60 * 1000);
}

// 'z', n: load FX preset n into the edited slot (0 = the original values).
static void fxp_load(int n)
{
    int slot = s_edit.slot;
    if (slot < 0 || n < 0 || n > FXP_COUNT || !s_edit.known || !s_state.fx_on[slot]) return;
    const nano_fx_model_t *model = nano_fx_model(s_state.fx_type[slot]);
    if (!model) return;
    int count;
    float values[NANO_MAX_PARAMS];
    if (n == 0) {
        if (!s_fxp.original_known) return;
        count = s_fxp.original_count;
        memcpy(values, s_fxp.original, sizeof(values));
    } else {
        const fxp_t *p = &s_fxp.list[n - 1];
        if (!p->name[0]) return;
        count = p->count;
        for (int i = 0; i < NANO_MAX_PARAMS; i++) values[i] = p->values[i] / 65535.0f;
    }
    if (count > model->param_count) count = model->param_count;
    if (count > NANO_MAX_PARAMS) count = NANO_MAX_PARAMS;
    ESP_LOGI(TAG, "%s: FX preset %s (%d values)", NANO_FX_SLOT_NAMES[slot], n ? s_fxp.list[n - 1].name : "original", count);
    esp_timer_stop(s_fxp_timer);
    s_fxp.send_slot = slot;
    s_fxp.send_count = count;
    s_fxp.sent = 0;
    memcpy(s_fxp.send_values, values, sizeof(values));
    memcpy(s_edit.values, values, sizeof(float) * count);
    s_fxp.active = n;
    editor_show();
    fxp_send_step();
}

// Text from the keyboard for FX preset n (1-FXP_COUNT): save the current values under this name, "" = delete.
static void fxp_save(int n, const char *name)
{
    int slot = s_edit.slot;
    if (slot < 0 || n < 1 || n > FXP_COUNT || s_fxp.type != s_state.fx_type[slot]) return;
    fxp_t *p = &s_fxp.list[n - 1];
    while (*name == ' ') name++;
    if (!name[0]) {
        memset(p, 0, sizeof(*p));
        if (s_fxp.active == n) s_fxp.active = -1;
    } else {
        if (!s_edit.known) {
            ui_show_message("Switch the effect on first - its values are read only while it is on.");
            return;
        }
        memset(p, 0, sizeof(*p));
        strlcpy(p->name, name, sizeof(p->name));
        p->count = (uint8_t)s_edit.count;
        for (int i = 0; i < s_edit.count && i < NANO_MAX_PARAMS; i++) {
            float v = s_edit.values[i] < 0 ? 0 : s_edit.values[i] > 1 ? 1 : s_edit.values[i];
            p->values[i] = (uint16_t)lroundf(v * 65535);
        }
        s_fxp.active = n;
    }
    nvs_handle_t nvs;
    char key[16];
    fxp_key(s_fxp.type, key);
    bool empty = true;
    for (int i = 0; i < FXP_COUNT; i++) empty &= !s_fxp.list[i].name[0];
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        esp_err_t err = empty ? nvs_erase_key(nvs, key) : nvs_set_blob(nvs, key, s_fxp.list, sizeof(s_fxp.list));
        if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) {
            ui_show_message("Could not store the FX preset (memory full?).");
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "%s FX preset %d: %s", nano_fx_name(s_fxp.type), n, name[0] ? name : "deleted");
    ui_show_message(name[0] ? "FX preset saved." : "FX preset deleted.");
    editor_show();
}

static void editor_open(int slot)
{
    if (slot < 0 || slot >= NANO_FX_SLOTS || !s_state.fx_known) return;
    s_edit.slot = slot;
    s_edit.known = false;
    s_edit.count = 0;
    editor_show();
    editor_read();
}

static void ab_model_chosen(int slot, uint32_t type);

static void editor_choose_model(uint32_t type)
{
    app_sync_later();
    int slot = s_edit.slot;
    if (slot < 0 || !nano_fx_model(type)) return;
    ab_model_chosen(slot, type);
    uint8_t buf[16];
    char label[48];
    snprintf(label, sizeof(label), "%s model %s", NANO_FX_SLOT_NAMES[slot], nano_fx_name(type));
    send(label, NANO_MSG_FX_TYPE, buf, nano_fx_model_select(slot, type, buf));
    s_state.fx_type[slot] = type;
    s_edit.known = false;
    editor_show();
    show();
    schedule_refresh(300);
    editor_read_later(500);
}

static void mix_exp_request(const char *label)
{
    uint8_t buf[8];
    s_mix.pending++;
    s_mix.request_us = esp_timer_get_time();
    send(label, NANO_MSG_EXP_REQUEST, buf, nano_exp_request(s_state.current_preset, buf));
}

static void mix_store(void)
{
    char key[16];
    snprintf(key, sizeof(key), MIX_STORE_KEY, s_state.current_preset);
    uint8_t stored[2] = { (uint8_t)lroundf(s_mix.pos[0] * 255), (uint8_t)lroundf(s_mix.pos[1] * 255) };
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, key, stored, sizeof(stored)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
        ui_show_message("Could not store Pos 1 / Pos 2 (memory full?).");
    }
    nvs_close(nvs);
    s_mix.own = true;
}

// New preset: its Pos 1 / Pos 2 from the controller's store, and the Nano's expression assignments - for a preset
// that has its values there (set up before 1.8) and to know whether the reverb is still on the pedal.
static void mix_load(void)
{
    s_mix.known = false;
    s_mix.own = false;
    s_mix.exp = false;
    s_mix.active = -1;
    s_mix.touched = false;
    s_exp.known = false;
    s_exp.write = s_exp.verify = EXP_WRITE_NONE;
    char key[16];
    snprintf(key, sizeof(key), MIX_STORE_KEY, s_state.current_preset);
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t stored[2];
        size_t size = sizeof(stored);
        if (nvs_get_blob(nvs, key, stored, &size) == ESP_OK && size == sizeof(stored)) {
            s_mix.pos[0] = stored[0] / 255.0f;
            s_mix.pos[1] = stored[1] / 255.0f;
            s_mix.known = s_mix.own = true;
        }
        nvs_close(nvs);
    }
    mix_exp_request("Expression request");
}

static int mix_param(void)
{
    const nano_fx_model_t *model = s_mix.slot >= 0 ? nano_fx_model(s_state.fx_type[s_mix.slot]) : NULL;
    return model ? model->mix_param : -1;
}

static void toggle_mix(void)
{
    if (s_mix.slot < 0 || !s_mix.known) {
        ESP_LOGW(TAG, "Reverb mix: %s", s_mix.slot < 0 ? "no reverb in this preset"
                 : "no Pos 1 / Pos 2 set for this preset (hold the tile)");
        return;
    }
    int param = mix_param();
    if (param < 0) {
        ESP_LOGW(TAG, "Reverb mix: this reverb model has no Mix parameter");
        return;
    }
    if (!s_state.fx_on[s_mix.slot]) toggle_fx(s_mix.slot);
    int next = s_mix.active == 1 ? 0 : 1;
    uint8_t buf[16];
    char label[40];
    snprintf(label, sizeof(label), "Reverb mix Pos %d (%ld%%)", next + 1, lroundf(s_mix.pos[next] * 100));
    send(label, NANO_MSG_FX_VALUE, buf, nano_fx_param(s_mix.slot, param, s_mix.pos[next], buf));
    app_sync_later();
    s_mix.active = next;
    show();
    if (s_mix.slot == s_edit.slot && s_edit.known) {
        s_edit.values[param] = s_mix.pos[next];
        editor_show();
    }
}

// Pos 1 / Pos 2 editor: a slider moved (which << 8 | value 0-255). Plays that mix on the reverb.
static void mix_preview(int arg)
{
    int param = mix_param();
    if (param < 0) return;
    if (!s_state.fx_on[s_mix.slot]) toggle_fx(s_mix.slot);
    s_mix.touched = true;
    throttled_param(s_mix.slot, param, (arg & 0xff) / 255.0f);
}

// Pos 1 / Pos 2 editor: SAVE (Pos 1 << 8 | Pos 2, 0-255 each) - on the controller, for this preset.
static void mix_save(int arg)
{
    if (s_mix.slot < 0) return;
    s_mix.pos[0] = (uint8_t)(arg >> 8) / 255.0f;
    s_mix.pos[1] = (uint8_t)arg / 255.0f;
    s_mix.known = true;
    s_mix.touched = true;
    mix_store();
    ESP_LOGI(TAG, "Preset %d: reverb Pos 1 %ld%%, Pos 2 %ld%% stored", s_state.current_preset, lroundf(s_mix.pos[0] * 100),
             lroundf(s_mix.pos[1] * 100));
    ui_show_message("Reverb Pos 1 / Pos 2 saved.");
    show();
}

// Reverb dialog, FREE PEDAL: the Nano preset no longer moves the reverb with the expression pedal - its Amount is
// taken out of the preset's expression assignments.
static void mix_free_pedal(void)
{
    if (s_mix.slot < 0) return;
    s_exp.write = EXP_WRITE_FREE_REVERB;
    s_exp.preset = s_state.current_preset;
    mix_exp_request("Expression request");
}

// Expression dialog, Calibrate. arg 1: start - the Nano forgets its calibration and its positions are collected;
// 2: save the lowest and highest position seen; 0: stop collecting (the calibration stays forgotten).
static void exp_calibrate(int arg)
{
    if (arg == 1) {
        uint8_t none[1] = { 0 };   // the message has no content
        send("Reset the pedal calibration", NANO_MSG_EXP_CAL_RESET, none, 0);
        s_cal.active = true;
        s_cal.min = INT32_MAX;
        s_cal.max = INT32_MIN;
        ui_set_pedal(s_cal.value, 0, -1);
    } else if (arg == 2) {
        if (!s_cal.active || s_cal.max <= s_cal.min) return;
        s_cal.active = false;
        uint8_t buf[16];
        char label[48];
        snprintf(label, sizeof(label), "Pedal calibration %d - %d", s_cal.min, s_cal.max);
        send(label, NANO_MSG_EXP_CAL_SAVE, buf, nano_exp_calibration_save(s_cal.min, s_cal.max, buf));
        ui_show_message("Pedal calibration saved.");
    } else {
        s_cal.active = false;
    }
}

// Expression dialog opened (1): read the assignments for it; closed (0).
static void exp_dialog(int open)
{
    s_exp.open = open != 0;
    if (!open) return;
    mix_exp_request("Expression request");
    request_settings();   // how the connector is set, for the switch at the top of the dialog
}

// Writes the wanted assignments into the preset; the result is read back and compared.
static void exp_write(int what)
{
    if (s_exp.preset != s_state.current_preset) return;
    if (what == EXP_WRITE_FREE_REVERB) {   // the assignments as they are now have just arrived
        if (!s_mix.exp) {
            ui_show_message("The expression pedal does not move the reverb in this preset.");
            return;
        }
        memcpy(s_exp.wanted, s_exp.ranges, sizeof(s_exp.wanted));
        memcpy(s_exp.wanted_switches, s_exp.switches, sizeof(s_exp.wanted_switches));
        s_exp.wanted[s_mix.slot].on = false;
    }
    uint8_t buf[256];
    size_t n = nano_exp_save(s_state.current_preset, s_exp.wanted, s_exp.wanted_switches, buf, 251);
    if (!n) {
        ui_show_message("Not saved: too many expression assignments for one message.");
        return;
    }
    char label[64];
    snprintf(label, sizeof(label), "Save expression assignments (preset %d)", s_state.current_preset);
    send(label, NANO_MSG_EXP_SAVE, buf, n);
    app_sync_later();
    s_exp.verify = what;
    esp_timer_start_once(s_exp_timer, 400 * 1000);   // then read back and compare, as the editor does
}

// Expression dialog, SAVE: one entry per sweep, "<0 / 1: on the pedal><heel, two hex digits><toe, two hex digits>",
// then one per on/off assignment, "<0 / 1: assigned><way 0-2><0 / 1: inverted><0 / 1: latch><delay in ms, four hex digits>".
static void exp_dialog_save(const char *text)
{
    if (!nano_link_ready() || strlen(text) != 5 * NANO_EXP_RANGES + 8 * NANO_EXP_SWITCHES) return;
    nano_exp_range_t ranges[NANO_EXP_RANGES];
    nano_exp_switch_t switches[NANO_EXP_SWITCHES];
    for (int i = 0; i < NANO_EXP_RANGES; i++) {
        unsigned heel = 0, toe = 0;
        if (sscanf(text + 5 * i + 1, "%2x%2x", &heel, &toe) != 2) return;
        ranges[i] = (nano_exp_range_t){ .on = text[5 * i] == '1', .heel = (uint8_t)heel, .toe = (uint8_t)toe };
    }
    for (int i = 0; i < NANO_EXP_SWITCHES; i++) {
        const char *entry = text + 5 * NANO_EXP_RANGES + 8 * i;
        unsigned delay = 0;
        int mode = entry[1] - '0';
        if (mode < 0 || mode >= NANO_EXP_MODES || sscanf(entry + 4, "%4x", &delay) != 1) return;
        switches[i] = (nano_exp_switch_t){ .on = entry[0] == '1', .mode = (uint8_t)mode, .inverted = entry[2] == '1', .latch = entry[3] == '1',
                                           .delay_ms = (uint16_t)(delay > NANO_EXP_DELAY_MAX ? NANO_EXP_DELAY_MAX : delay) };
    }
    memcpy(s_exp.wanted, ranges, sizeof(ranges));
    memcpy(s_exp.wanted_switches, switches, sizeof(switches));
    s_exp.preset = s_state.current_preset;
    exp_write(EXP_WRITE_DIALOG);
}

static void mix_verify_request(void)
{
    if (s_exp.verify && s_exp.preset == s_state.current_preset) mix_exp_request("Expression check");
}

// Pos 1 / Pos 2 editor closed: back to the mix of the active position (Pos 1 if none was chosen yet).
static void mix_editor_closed(void)
{
    int param = mix_param();
    if (!s_mix.touched || param < 0 || !s_mix.known) return;
    s_mix.touched = false;
    s_mix.active = s_mix.active == 1 ? 1 : 0;
    throttled_param(s_mix.slot, param, s_mix.pos[s_mix.active]);
    show();
}

// ---- second effect (A/B) ----

static void ab_step_later(int ms)
{
    esp_timer_stop(s_ab_timer);
    esp_timer_start_once(s_ab_timer, (uint64_t)ms * 1000);
}

// The A/B that is switching or reading right now (one at a time: the timer and the parameter reply are its).
static ab_t *ab_running(void)
{
    for (int i = 0; i < AB_COUNT; i++) if (s_ab[i].phase != AB_IDLE) return &s_ab[i];
    return NULL;
}

static void ab_store(ab_t *ab)
{
    char key[16];
    nvs_handle_t nvs;
    if (!ab->preset || nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    snprintf(key, sizeof(key), AB_KEYS[ab - s_ab], ab->preset);
    esp_err_t err = ab->b.type ? nvs_set_blob(nvs, key, &ab->b, sizeof(ab->b)) : nvs_erase_key(nvs, key);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the second effect of %s", AB_NAMES[ab - s_ab]);
    }
    nvs_close(nvs);
}

// Keeps the values of A for this preset (read while A was on), so a later swap needs no reading.
static void ab_store_a(ab_t *ab)
{
    char key[16];
    nvs_handle_t nvs;
    if (!ab->preset || nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    snprintf(key, sizeof(key), AB_A_KEYS[ab - s_ab], ab->preset);
    fx_a_t a = { .type = ab->a_type, .count = ab->a_count };
    for (int i = 0; i < ab->a_count && i < NANO_MAX_PARAMS; i++) {
        float v = ab->a_values[i] < 0 ? 0 : ab->a_values[i] > 1 ? 1 : ab->a_values[i];
        a.values[i] = (uint16_t)lroundf(v * 65535);
    }
    esp_err_t err = ab->a_count && ab->b.type ? nvs_set_blob(nvs, key, &a, sizeof(a)) : nvs_erase_key(nvs, key);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the A values of %s", AB_NAMES[ab - s_ab]);
    }
    nvs_close(nvs);
}

// New preset: its second effects, and the stored values of A if they belong to the model now in the slot.
static void ab_load(int preset)
{
    esp_timer_stop(s_ab_timer);
    nvs_handle_t nvs;
    bool open = nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK;
    for (int i = 0; i < AB_COUNT; i++) {
        ab_t *ab = &s_ab[i];
        memset(ab, 0, sizeof(*ab));
        ab->preset = preset;
        ab->slot = i == AB_PRE1 ? 0 : -1;
        char key[16];
        snprintf(key, sizeof(key), AB_KEYS[i], preset);
        size_t size = sizeof(ab->b);
        if (!open || nvs_get_blob(nvs, key, &ab->b, &size) != ESP_OK || size != sizeof(ab->b) || !nano_fx_model(ab->b.type)) {
            memset(&ab->b, 0, sizeof(ab->b));
        }
        if (ab->b.type) ESP_LOGI(TAG, "%s B of preset %d: %s", AB_NAMES[i], preset, nano_fx_name(ab->b.type));
        fx_a_t a;
        size = sizeof(a);
        int slot = i == AB_PRE1 ? 0 : s_mix.slot;
        snprintf(key, sizeof(key), AB_A_KEYS[i], preset);
        if (open && ab->b.type && slot >= 0 && nvs_get_blob(nvs, key, &a, &size) == ESP_OK && size == sizeof(a) &&
            a.type == s_state.fx_type[slot] && a.count <= NANO_MAX_PARAMS) {
            ab->a_type = a.type;
            ab->a_count = a.count;
            for (int n = 0; n < a.count; n++) ab->a_values[n] = a.values[n] / 65535.0f;
        }
    }
    if (open) nvs_close(nvs);
}

// The slot of an A/B now (the reverb's slot comes from the preset), -1 = none.
static int ab_slot(const ab_t *ab)
{
    return ab - s_ab == AB_REVERB ? s_mix.slot : 0;
}

// The FX editor shows the effect of an A/B slot: keep its values (called when the editor closes or the preset
// changes). The model of B only changes by an explicit choice.
static void ab_capture_editor(void)
{
    for (int i = 0; i < AB_COUNT; i++) {
        ab_t *ab = &s_ab[i];
        if (!ab->b.type || ab->slot < 0 || s_edit.slot != ab->slot || !s_edit.known) continue;
        if (ab->active == 1) {
            ab->b.count = (uint8_t)s_edit.count;
            memcpy(ab->b.values, s_edit.values, sizeof(ab->b.values));
            ab_store(ab);
        } else {
            if (!ab->a_type) ab->a_type = s_state.fx_type[ab->slot];
            ab->a_count = (uint8_t)s_edit.count;
            memcpy(ab->a_values, s_edit.values, sizeof(ab->a_values));
            ab_store_a(ab);
        }
    }
}

// A model was chosen in the FX editor: it replaces the running effect (A or B) of an A/B slot.
static void ab_model_chosen(int slot, uint32_t type)
{
    const nano_fx_model_t *model = nano_fx_model(type);
    for (int i = 0; i < AB_COUNT; i++) {
        ab_t *ab = &s_ab[i];
        if (!ab->b.type || slot != ab->slot || !model) continue;
        if (i == AB_REVERB && model->icon != NANO_ICON_REVERB) continue;
        if (ab->active == 1) {
            ab->b.type = type;
            ab->b.count = 0;
            ab_store(ab);
        } else if (ab->a_type) {
            ab->a_type = type;
            ab->a_count = 0;
            ab_store_a(ab);
        }
    }
}

// Puts the target effect into the slot: the model now, its values in the following steps.
static void ab_load_target(ab_t *ab)
{
    uint32_t type = ab->target ? ab->b.type : ab->a_type;
    uint8_t buf[16];
    char label[64];
    snprintf(label, sizeof(label), "%s %c: %s", AB_NAMES[ab - s_ab], ab->target ? 'B' : 'A', nano_fx_name(type));
    send(label, NANO_MSG_FX_TYPE, buf, nano_fx_model_select(ab->slot, type, buf));
    s_state.fx_type[ab->slot] = type;
    ab->active = ab->target;
    ab->swapped_us = esp_timer_get_time();
    ab->phase = AB_LOADING;
    ab->sent = 0;
    ab_step_later(400);   // the Nano needs a moment for the model change
    show();
}

static void ab_read(ab_t *ab)
{
    if (ab->cache_only && s_edit.request_slot >= 0) {   // the editor's reply comes first: try another time
        ab->phase = AB_IDLE;
        return;
    }
    uint8_t buf[8];
    char label[48];
    snprintf(label, sizeof(label), "%s values request", AB_NAMES[ab - s_ab]);
    ab->phase = AB_READING;
    send(label, NANO_MSG_FX_PARAMS_REQUEST, buf, nano_fx_params_request(ab->slot, buf));
    ab_step_later(1500);   // timeout
}

// Reads the values of A while it is on (preset loaded, or switched on), so a later swap need not switch it on.
static void ab_cache(int slot, int delay_ms)
{
    if (ab_running() || s_edit.request_slot >= 0 || slot < 0 || !s_state.fx_on[slot] || !s_state.fx_type[slot]) return;
    for (int i = 0; i < AB_COUNT; i++) {
        ab_t *ab = &s_ab[i];
        if (!ab->b.type || ab->active != 0 || ab_slot(ab) != slot) continue;
        ab->slot = slot;
        ab->cache_only = true;
        ab->phase = AB_TURNING_ON;
        ab_step_later(delay_ms);
        return;
    }
}

// Footswitch (8 for the reverb, 3 held for Pre FX 1): swap A <-> B; the effect stays on or off.
static void ab_swap(int which)
{
    ab_t *ab = &s_ab[which];
    if (!ab->b.type) return;
    ab_t *running = ab_running();
    if (running && !running->cache_only) {
        ui_show_message("Still switching - try again in a moment.");
        return;
    }
    if (running) {   // a read for later: the swap reads anyway
        esp_timer_stop(s_ab_timer);
        running->phase = AB_IDLE;
        running->cache_only = false;
    }
    int slot = ab_slot(ab);
    if (slot < 0 || !s_state.fx_type[slot]) {
        ui_show_message(which == AB_REVERB ? "No reverb in this preset." : "Pre FX 1 is empty in this preset.");
        return;
    }
    ab->slot = slot;
    ab->target = ab->active ? 0 : 1;
    ab->cache_only = false;
    ab_capture_editor();
    if (!s_state.fx_on[slot]) {
        if (ab->active == 1 || ab->a_count) {   // values known: swap while the effect stays off
            ab_load_target(ab);
            return;
        }
        // A was never on since B was set up: switch it on once to read its values (they are kept), then off again.
        ab->restore_off = true;
        ab->phase = AB_TURNING_ON;
        toggle_fx(slot);
        ab_step_later(400);
        return;
    }
    ab_read(ab);
}

// FX parameter values arrived: true if they were for an A/B (kept, then the swap goes on).
static bool ab_values_read(const uint8_t *payload, size_t len)
{
    ab_t *ab = ab_running();
    if (!ab || ab->phase != AB_READING) return false;
    esp_timer_stop(s_ab_timer);
    float values[NANO_MAX_PARAMS];
    int count = nano_fx_params_values(payload, len, values, NANO_MAX_PARAMS);
    if (count <= 0) {
        ab->phase = AB_IDLE;
        if (!ab->cache_only) ui_show_message("Not switched: the Nano sent no values.");
        ab->cache_only = false;
        if (ab->restore_off) {
            ab->restore_off = false;
            if (s_state.fx_on[ab->slot]) toggle_fx(ab->slot);
        }
        return true;
    }
    if (ab->active == 0) {
        if (!ab->a_type) ab->a_type = s_state.fx_type[ab->slot];   // the preset's effect, kept from now on
        bool same = ab->a_count == count;   // stored only when changed (spares the flash)
        for (int i = 0; same && i < count; i++) same = lroundf(ab->a_values[i] * 65535) == lroundf(values[i] * 65535);
        ab->a_count = (uint8_t)count;
        memcpy(ab->a_values, values, sizeof(values));
        if (!same) ab_store_a(ab);
    } else {   // values only: a state reply can still name the previous model
        ab->b.count = (uint8_t)count;
        memcpy(ab->b.values, values, sizeof(values));
        ab_store(ab);
    }
    if (ab->cache_only) {
        ab->cache_only = false;
        ab->phase = AB_IDLE;
        ESP_LOGI(TAG, "%s A: %d values kept", AB_NAMES[ab - s_ab], count);
        return true;
    }
    if (ab->restore_off) {   // switched on only for reading
        ab->restore_off = false;
        if (s_state.fx_on[ab->slot]) toggle_fx(ab->slot);
    }
    ab_load_target(ab);
    return true;
}

static void ab_step(void)
{
    ab_t *ab = ab_running();
    if (!ab) return;
    switch (ab->phase) {
    case AB_TURNING_ON:
        ab_read(ab);
        break;
    case AB_READING:
        ab->phase = AB_IDLE;
        if (!ab->cache_only) ui_show_message("Not switched: the Nano did not send the effect's values.");
        ab->cache_only = false;
        if (ab->restore_off) {
            ab->restore_off = false;
            if (s_state.fx_on[ab->slot]) toggle_fx(ab->slot);
        }
        break;
    case AB_LOADING: {
        int count = ab->target ? ab->b.count : ab->a_count;
        const float *values = ab->target ? ab->b.values : ab->a_values;
        for (int n = 0; n < AB_SEND_CHUNK && ab->sent < count; n++, ab->sent++) send_param(ab->slot, ab->sent, values[ab->sent]);
        if (ab->sent < count) {
            ab_step_later(60);
            break;
        }
        ab->phase = AB_IDLE;
        app_sync_later();
        ESP_LOGI(TAG, "%s %c active (%d values)", AB_NAMES[ab - s_ab], ab->active ? 'B' : 'A', count);
        schedule_refresh(300);
        if (ab->open_editor) {
            ab->open_editor = false;
            editor_open(ab->slot);
        } else if (s_edit.slot == ab->slot) {
            s_edit.known = false;
            editor_show();
            editor_read_later(300);
        }
        break;
    }
    default:
        break;
    }
}

// From the reverb dialog or the 2ND list of Pre FX 1: arg = which << 24 | model of B (0 = none).
// arg = w << 24 | model of B (0 = none). w: 0 = reverb, 1 = Pre FX 1, 2 = reverb with footswitch 8 as the mix
// switch - B stays stored with its values ("parked") until the dialog is saved on its 2nd reverb tab again.
static void ab_set_b(int arg)
{
    int which = arg >> 24;
    uint32_t type = (uint32_t)arg & 0xFFFFFF;
    bool parked = which == 2 && type;
    if (which == 2) which = AB_REVERB;
    if (which < 0 || which >= AB_COUNT || (type && !nano_fx_model(type))) return;
    ab_t *ab = &s_ab[which];
    ab_t *running = ab_running();
    if (running && !running->cache_only) {
        ui_show_message("Still switching - try again in a moment.");
        return;
    }
    bool changed = type != ab->b.type;
    if (!changed && parked == (ab->b.parked != 0)) return;
    if (changed) {
        ab->b.type = type;
        ab->b.count = 0;
    }
    ab->b.parked = parked;
    ab_store(ab);
    if (!type) ab_store_a(ab);   // no B: the kept A values are not needed any more
    ESP_LOGI(TAG, "%s B: %s%s", AB_NAMES[which], type ? nano_fx_name(type) : "none", parked ? " (kept; footswitch 8 is the mix switch)" : "");
    if (ab->active == 1) {   // B is running: show the new choice, or go back to A
        if (parked && !changed) {
            ab_swap(which);   // back to A as the footswitch does it: B's values are read and kept first
        } else if (changed || parked) {
            ab->target = type && !parked ? 1 : 0;
            if (ab->target || ab->a_type) ab_load_target(ab);
            else ab->active = 0;
        }
    }
    show();
    if (s_edit.slot == ab->slot) editor_show();
}

// From the reverb dialog: edit reverb B in the FX editor (B is loaded first).
static void ab_edit_b(int which)
{
    ab_t *ab = &s_ab[which];
    if (!ab->b.type) return;
    if (ab->active == 1) editor_open(ab->slot);
    else {
        ab->open_editor = true;
        ab_swap(which);
    }
}

static void set_tuner(bool on)
{
    uint8_t buf[16];
    send(on ? "Tuner on" : "Tuner off", NANO_MSG_TUNER_MODE, buf,
         nano_tuner_mode(on, s_state.tuner_base_hz, s_state.tuner_muted, buf));
    s_tuner_on = on;
    ui_show_tuner(on);
    if (on) ui_tuner_settings(s_state.tuner_base_hz, s_state.tuner_muted);
}

// Reference pitch or mute changed on the tuner screen: the Nano takes both with the tuner mode (as the editor does).
static void tuner_settings_changed(void)
{
    uint8_t buf[16];
    if (s_tuner_on) {
        char what[40];
        snprintf(what, sizeof(what), "Tuner %.0f Hz%s", (double)s_state.tuner_base_hz, s_state.tuner_muted ? ", muted" : "");
        send(what, NANO_MSG_TUNER_MODE, buf, nano_tuner_mode(true, s_state.tuner_base_hz, s_state.tuner_muted, buf));
    }
    ui_tuner_settings(s_state.tuner_base_hz, s_state.tuner_muted);
}

static void select_capture(int slot)
{
    app_sync_later();
    if (slot < 0 || slot > NANO_CAPTURE_SLOTS) return;
    uint8_t buf[8];
    char label[32];
    snprintf(label, sizeof(label), slot ? "Capture slot %d" : "Capture bypass", slot);
    send(label, NANO_MSG_SELECTOR, buf, nano_capture_select(slot, buf));
    s_state.capture_slot = slot;
    if (slot) strlcpy(s_state.capture, s_state.capture_names[slot - 1], sizeof(s_state.capture));
    show();
    schedule_refresh(300);
}

static void select_cab(int slot)
{
    app_sync_later();
    if (slot < 0 || slot > NANO_CAB_SLOTS) return;
    uint8_t buf[8];
    char label[32];
    snprintf(label, sizeof(label), slot ? "Cab slot %d" : "Cab bypass", slot);
    send(label, NANO_MSG_SELECTOR, buf, nano_cab_select(slot, buf));
    s_state.cab_slot = slot;
    if (slot) strlcpy(s_state.cab, s_state.cab_names[slot - 1], sizeof(s_state.cab));
    show();
    schedule_refresh(300);
}

// Long press on a preset tile: arg = bank << 14 | slot << 11 | colour << 7 | preset (0 = back to default).
static void set_bank_slot(int arg)
{
    int icon = (arg >> 18) & 0x1F, bank = (arg >> 14) & 0xF, slot = (arg >> 11) & 0x7, color = (arg >> 7) & 0xF, preset = arg & 0x7F;
    if (bank >= BANKS || slot >= BANK_SLOTS || preset > NANO_PRESETS) return;
    int default_preset = bank * BANK_SLOTS + slot + 1;
    s_banks[bank][slot] = preset ? (bank_slot_t){ .preset = (uint8_t)preset, .color = (uint8_t)color, .icon = (uint8_t)icon }
                                 : (bank_slot_t){ .preset = (uint8_t)(default_preset <= NANO_PRESETS ? default_preset : 0) };
    banks_save();
    ESP_LOGI(TAG, "Bank %d switch %d: preset %d, colour %d, symbol %d", bank + 1, slot + 3, s_banks[bank][slot].preset,
             s_banks[bank][slot].color, s_banks[bank][slot].icon);
    show();
}

static void step_bank(int step)
{
    s_bank = (s_bank + BANKS + (step < 0 ? -1 : 1)) % BANKS;
    show();
}

static void save_preset(void)
{
    app_sync_later();
    int preset = s_state.current_preset;
    const char *name = s_state.preset_names[preset - 1];
    uint8_t buf[96];
    size_t n = nano_save_preset(preset, name[0] ? name : "Preset", buf, sizeof(buf));
    if (!n) return;
    char label[32];
    snprintf(label, sizeof(label), "Save preset %d", preset);
    send(label, NANO_MSG_SAVE_PRESET, buf, n);
    s_save_pending = preset;
    schedule_refresh(400);
}

static void rename_preset(const char *name)
{
    app_sync_later();
    int preset = s_state.current_preset;
    size_t len = strlen(name);
    if (len < 4 || len > 32) {
        ui_show_message("Preset names need 4 to 32 characters.");
        return;
    }
    for (int i = 0; i < NANO_PRESETS; i++) {
        if (i != preset - 1 && strcasecmp(s_state.preset_names[i], name) == 0) {
            ui_show_message("This preset name already exists.");
            return;
        }
    }
    uint8_t buf[96];
    size_t n = nano_rename_preset(preset, name, buf, sizeof(buf));
    if (!n) return;
    char label[48];
    snprintf(label, sizeof(label), "Rename preset %d", preset);
    send(label, NANO_MSG_RENAME_PRESET, buf, n);
    s_rename_preset = preset;
    strlcpy(s_rename_old, s_state.preset_names[preset - 1], sizeof(s_rename_old));
    strlcpy(s_state.preset_names[preset - 1], name, sizeof(s_state.preset_names[0]));
    show();
}

static void request_library(void)
{
    uint8_t buf[16];
    send("Library request", NANO_MSG_LIBRARY_REQUEST, buf, nano_library_request(buf));
}

static void library_timer_cb(void *arg)
{
    command('Y', 0);
}

// Loads library item `position` (alphabetical) into the active capture or cab slot. arg = kind << 16 | position.
static void load_library_item(int arg)
{
    app_sync_later();
    bool cab = (arg >> 16) & 1;
    int position = arg & 0xFFFF;
    const lib_list_t *list = s_library ? (cab ? &s_library->cabs : &s_library->captures) : NULL;
    if (!list || position >= list->count) return;
    const lib_item_t *item = &list->items[position];
    int slot = cab ? s_state.cab_slot : s_state.capture_slot;
    if (!slot) {
        ui_show_message(cab ? "Choose a cab slot first (the library loads into the active slot)."
                            : "Choose a capture slot first (the library loads into the active slot).");
        return;
    }
    uint8_t buf[200];
    size_t n = cab ? nano_cab_load(slot, item, buf, sizeof(buf)) : nano_capture_load(slot, item, buf, sizeof(buf));
    if (!n) {
        ui_show_message("This name is too long to load.");
        return;
    }
    char label[48];
    snprintf(label, sizeof(label), "Load %s into slot %d", cab ? "cab" : "capture", slot);
    send(label, cab ? NANO_MSG_CAB_LOAD : NANO_MSG_CAPTURE_LOAD, buf, n);
    // Names of the slots: shown right away, confirmed by the reply.
    strlcpy(cab ? s_state.cab_names[slot - 1] : s_state.capture_names[slot - 1], item->name,
            sizeof(s_state.capture_names[0]));
    strlcpy(cab ? s_state.cab : s_state.capture, item->name, sizeof(s_state.capture));
    show();
}

static void looper_set(bool on)
{
    if (on == s_looper) return;
    s_looper = on;
    s_looper_sent = 0;
    ESP_LOGI(TAG, "Looper mode %s%s", on ? "on" : "off", on && !phone_midi_connected() ? " (no phone connected)" : "");
    looper_switches();
    if (nano_link_ready()) show();
    if (on && !phone_midi_connected()) ui_show_message("Looper mode - no phone yet: in Loopy Pro open the menu > Bluetooth Devices.");
}

// The Scenes button: to the scenes of the preset, or back to the bank's presets if they are showing. Either way
// out of FX and looper mode - the button leads to what it names. Footswitch 1 then goes between FX and that.
static void scene_mode(int arg)
{
    bool showing = s_scene_mode && !s_fx_mode && !s_looper;
    bool on = arg < 0 ? !showing : arg != 0;
    if (on != s_scene_mode) {
        s_scene_mode = on;
        nvs_handle_t nvs;
        if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            uint8_t stored = on;
            if (nvs_set_blob(nvs, SCENE_MODE_KEY, &stored, 1) == ESP_OK) nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    ESP_LOGI(TAG, "%s mode", on ? "Scene" : "Preset");
    s_fx_mode = false;
    if (!on) follow_preset_with_bank(s_state.current_preset);
    if (s_looper) looper_set(false);
    else show();
}

// Looper mode and footswitch 1 ('w'): held = into the mode, any press of it while in the mode = out. In the mode
// switches 2-8 from the touch screen or over MIDI send a short press; from the footswitches it went out already
// (UI_SWITCH_SENT, see on_footswitch). True if the press was handled here.
static bool looper_switch(int arg)
{
    int number = arg & 0xFF;
    if (number < 1 || number > FOOTSWITCH_COUNT) return false;
    if (number == 1 && (s_looper || (arg & UI_SWITCH_HOLD))) {
        if (!s_looper && !nano_link_ready()) return false;
        ui_flash_tile(0);
        looper_set(!s_looper);
        return true;
    }
    if (!s_looper) return false;
    ui_flash_tile(number - 1);
    bool sent = !(arg & UI_SWITCH_LOST);
    if (!(arg & UI_SWITCH_SENT)) {
        sent = phone_midi_send(LOOPER_STATUS, LOOPER_CC(number), 127);
        phone_midi_send(LOOPER_STATUS, LOOPER_CC(number), 0);
    }
    ESP_LOGI(TAG, "Looper switch %d: CC %d %s", number, LOOPER_CC(number), sent ? "sent" : "NOT sent");
    s_looper_sent = sent ? LOOPER_CC(number) : -LOOPER_CC(number);
    if (nano_link_ready()) show();   // tile 1 says what went out
    if (!phone_midi_connected()) ui_show_message("No phone connected - in Loopy Pro open the menu > Bluetooth Devices.");
    return true;
}

static void handle_switch(int number)
{
    if (looper_switch(number)) return;
    ui_flash_tile(number - 1);
    if (number == 1) {
        s_fx_mode = !s_fx_mode;
        if (!s_fx_mode) follow_preset_with_bank(s_state.current_preset);
        ESP_LOGI(TAG, "%s mode", s_fx_mode ? "FX" : s_scene_mode ? "Scene" : "Preset");
        show();
    } else if (number == 2) {
        set_tuner(!s_tuner_on);
    } else if (s_fx_mode) {
        if (number <= 7) toggle_fx(number - 3);
        else if (reverb_b_used()) ab_swap(AB_REVERB);
        else toggle_mix();
    } else if (number >= 3 && number <= 8 && s_scene_mode) {
        scene_recall(number - 3);
    } else if (number >= 3 && number <= 8) {
        int preset = s_banks[s_bank][number - 3].preset;
        if (preset) select_preset(preset);
        else ui_show_message("This switch is empty - hold its tile to choose a preset.");
    }
}

// Footswitch learn: 'D' from the screen (1-8 learn, 0 cancel, -1 default order), 'Z' = result from the
// footswitch task (learned switch << 8 | switch that got its old pin; 0 = nothing pressed in time).
static void footswitch_learn(char c, int arg)
{
    char text[96];
    if (c == 'D' && !s_footswitches) {
        ui_learn_done("No footswitches found (SX1509 at 0x71/0x70).");
    } else if (c == 'D' && arg < 0) {
        footswitches_reset();
        ui_learn_done("Footswitches back to the default order.");
    } else if (c == 'D') {
        footswitches_learn(arg);
    } else if (!(arg >> 8)) {
        ui_learn_done("No footswitch pressed - nothing changed.");
    } else {
        int number = arg >> 8, swapped = arg & 0xff;
        if (swapped) snprintf(text, sizeof(text), "Footswitch %d learned (swapped with footswitch %d).", number, swapped);
        else snprintf(text, sizeof(text), "Footswitch %d learned.", number);
        ui_learn_done(text);
    }
}

// ---- Bluetooth MIDI ----

// MIDI dialog and device state: 'X' dialog open (1) / closed (0), 'P' connect device n (-1 = forget),
// 'N' devices or connection changed.
static void midi_command(char c, int arg)
{
    static bool was_connected;
    if (c == 'X') midi_ble_search(arg != 0);
    else if (c == 'P' && arg < 0) midi_ble_forget();
    else if (c == 'P' && !midi_ble_connect(arg)) ui_show_message("MIDI: busy connecting - try again in a moment.");

    ui_midi_t m = { 0 };
    midi_device_t devices[MIDI_MAX_DEVICES];
    m.connected = midi_ble_status(m.name, sizeof(m.name));
    m.count = midi_ble_devices(devices, MIDI_MAX_DEVICES);
    for (int i = 0; i < m.count; i++) {
        strlcpy(m.devices[i].name, devices[i].name, sizeof(m.devices[i].name));
        m.devices[i].rssi = devices[i].rssi;
        m.devices[i].remembered = devices[i].remembered;
    }
    ui_set_midi(&m);
    if (m.connected != was_connected) {
        char text[64];
        snprintf(text, sizeof(text), "MIDI: %s %s", m.name, m.connected ? "connected" : "disconnected");
        ui_show_message(text);
        was_connected = m.connected;
    }
}

// CC 1 (expression pedal): reverb mix between Pos 1 (heel, 0) and Pos 2 (toe, 127).
static void midi_expression(int value)
{
    int param = mix_param();
    if (s_mix.slot < 0 || !s_mix.known || param < 0) return;
    throttled_param(s_mix.slot, param, s_mix.pos[0] + (s_mix.pos[1] - s_mix.pos[0]) * value / 127.0f);
    int side = value >= 64 ? 1 : 0;
    if (side != s_mix.active) {
        s_mix.active = side;
        show();
    }
}

// As the Nano's own MIDI over USB: PC 0-63 = presets, CC 37-41 = FX 1-5 (127 on, 0 off), CC 1 = expression.
// In addition CC 50-57 (value 64-127) press footswitches 1-8, CC 59 is the looper mode and CC 60 the scene mode
// (64-127 on, below off). All channels.
static void handle_midi(int arg)
{
    uint8_t status = (uint8_t)(arg >> 16), d1 = (uint8_t)(arg >> 8), d2 = (uint8_t)arg;
    uint8_t type = status & 0xF0;
    if (type == 0xB0 && d1 == 1) {
        int value = __atomic_exchange_n(&s_cc1, -1, __ATOMIC_SEQ_CST);
        if (value >= 0 && nano_link_ready()) midi_expression(value);
        return;
    }
    ESP_LOGI(TAG, "MIDI %02X %u %u", status, d1, d2);
    if (type == 0xB0 && d1 == 59) {   // looper mode on (64-127) / off
        if (d2 >= 64 && !s_looper && !nano_link_ready()) return;
        looper_set(d2 >= 64);
        return;
    }
    if (!nano_link_ready()) return;
    if (type == 0xB0 && d1 == 60) {   // footswitches 3-8: scenes (64-127) / the bank's presets
        scene_mode(d2 >= 64);
        return;
    }
    if (type == 0xC0) {
        if (d1 < NANO_PRESETS) select_preset(d1 + 1);
    } else if (type == 0xB0 && d1 >= 37 && d1 <= 41) {
        int slot = d1 - 37;
        if (s_state.fx_known && s_state.fx_type[slot] && s_state.fx_on[slot] != (d2 >= 64)) toggle_fx(slot);
    } else if (type == 0xB0 && d1 >= 50 && d1 <= 57 && d2 >= 64) {
        handle_switch(d1 - 49);
    } else if (type == 0xB0 && d1 == 58 && d2 >= 64) {   // as footswitch 3 held: Pre FX 1 A <-> B
        ab_swap(AB_PRE1);
    }
}

static void handle_command(char c, int arg)
{
    if (c == 'h') { print_help(); return; }
    if (c == 'l') { print_presets(); return; }
    if (c == 'D' || c == 'Z') { footswitch_learn(c, arg); return; }   // works without the Nano
    if (c == 'X' || c == 'P' || c == 'N') { midi_command(c, arg); return; }
    if (c == '@') {                                    // looper app on the phone connected / gone / other interval
        static int was = -1;
        int now = arg ? (int)(phone_midi_interval_ms() * 100) : 0;
        if (now == was) return;
        was = now;
        ESP_LOGI(TAG, "Phone %s", arg ? "connected (Bluetooth MIDI)" : "disconnected");
        char text[64];
        snprintf(text, sizeof(text), "Phone connected - MIDI every %.4g ms.", (double)phone_midi_interval_ms());
        ui_show_message(arg ? text : "Phone disconnected.");
        if (nano_link_ready()) show();
        return;
    }
    if (c == 'w' && looper_switch(arg)) return;        // looper mode keeps working without the Nano
    if (c == 'q') {                                    // MIDI gate timer: the Nano did not come
        nano_link_log_status();
#ifndef NANO_WEB   // in the browser the start screen keeps asking to click CONNECT
        ui_splash_status("No Nano Cortex yet - switch it on (tap to continue)", NULL);
#endif
        midi_ble_allow(true);
        app_link_allow_phone(true);
        return;
    }
    if (c == 'o') {                                    // app connected / left through the controller
        ESP_LOGI(TAG, "App %s", arg ? "connected through the controller" : "disconnected");
        ui_set_app(arg != 0);
        ui_show_message(arg ? "App connected through the controller." : "App disconnected.");
        return;
    }
    if (c == 'r') {                                    // refresh button / console: read everything again
        if (!nano_link_ready()) {
            ui_show_message("Not connected - still searching for the Nano.");
            return;
        }
        ui_show_message("Reading the Nano again...");
        s_incomplete_retries = 0;
        s_refresh_library = true;
        request_full_state();
        return;
    }
    if (!nano_link_ready()) {
        ESP_LOGW(TAG, "Not connected yet");
        return;
    }
    switch (c) {
    case 'n': select_preset(s_state.current_preset + 1); break;
    case 'p': select_preset(s_state.current_preset - 1); break;
    case 'g':
        if (arg >= 1 && arg <= NANO_PRESETS) select_preset(arg);
        else ESP_LOGW(TAG, "Presets are 1-64");
        break;
    case 's': request_current_state(); break;
    case 'a': case 'b': case 'c': case 'd': case 'e': toggle_fx(c - 'a'); break;
    case 'm': handle_switch(1); break;
    case 't': handle_switch(2); break;
    case 'x':   // as footswitch 8 in FX mode
        if (reverb_b_used()) ab_swap(AB_REVERB);
        else toggle_mix();
        break;
    case 'w': {
        int number = arg & 0xFF;
        if (number < 1 || number > FOOTSWITCH_COUNT) break;
        if ((arg & UI_SWITCH_HOLD) && number == 3 && s_ab[AB_PRE1].b.type) {   // held: Pre FX 1 A <-> B
            ui_flash_tile(2);
            ab_swap(AB_PRE1);
        } else if ((arg & UI_SWITCH_HOLD) && number == 2) {   // held: scenes <-> presets, as the scenes button
            if (!s_tuner_on) {
                ui_flash_tile(1);
                scene_mode(-1);
            }
        }
        else if (!(arg & (UI_SWITCH_HOLD | UI_SWITCH_SENT))) handle_switch(number);
        break;
    }
    case '^': {   // tuner reference pitch +/- arg Hz (400-480)
        float hz = s_state.tuner_base_hz + (float)arg;
        s_state.tuner_base_hz = hz < 400 ? 400 : hz > 480 ? 480 : hz;
        tuner_settings_changed();
        break;
    }
    case '~':   // tuner: mute the output on / off
        s_state.tuner_muted = !s_state.tuner_muted;
        tuner_settings_changed();
        break;
    case 'O': editor_open(arg); break;
    case 'E':
        ab_capture_editor();
        s_edit.slot = -1;
        s_fxp.slot = -2;   // the next editor reads its own original values
        break;
    case 'A': ab_set_b(arg); break;
    case 'H': ab_edit_b(arg); break;
    case 'F': ab_step(); break;
    case 'M': editor_choose_model((uint32_t)arg); break;
    case 'R': editor_read(); break;
    case 'C': select_capture(arg); break;
    case 'B': set_bank_slot(arg); break;
    case 'S': save_preset(); break;
    case 'k': step_bank(arg); break;
    case 'L': load_library_item(arg); break;
    case 'Y': if (!s_library) request_library(); break;
    case 'I': select_cab(arg); break;
    case 'T': editor_throttle_done(); break;
    case 'V': request_settings(); break;
    case 'U': set_usb_gain(arg); break;
    case 'G': usb_throttle_done(); break;
    case 'v': source_value(arg); break;
    case 'z': fxp_load(arg); break;
    case '#': fxp_send_step(); break;
    case 'u': source_throttle_done(); break;
    case 'y': cab_settings_command(arg); break;
    case 'i':   // the app changed something: read it shortly after (once for several changes)
        if (arg & 1) s_app_names_changed = true;
        if (arg & 2) s_app_exp_changed = true;
        esp_timer_stop(s_app_read_timer);
        esp_timer_start_once(s_app_read_timer, 600 * 1000);
        break;
    case 'j':
        if (s_app_names_changed) request_full_state();
        else request_current_state();
        if (s_app_exp_changed) mix_exp_request("Expression request");   // is the reverb (still) on the pedal?
        s_app_names_changed = s_app_exp_changed = false;
        break;
    case 'f':   // tell the app: preset "changed" -> it reads the current state again
        if (app_link_connected()) {
            uint8_t buf[64];
            app_link_send(NANO_MSG_SET_PRESET_SLOTS, buf, nano_preset_changed_notice(&s_state, buf));
        }
        break;
    case 'Q': mix_preview(arg); break;
    case 'W': mix_save(arg); break;
    case '!': mix_free_pedal(); break;
    case '=': exp_dialog(arg); break;
    case '<': exp_calibrate(arg); break;
    case '+': jack_set(arg); break;
    case '?': request_settings(); break;
    case 'J': mix_verify_request(); break;
    case 'K': mix_editor_closed(); break;
    case '$': scene_mode(arg); break;
    case '&': scene_values(arg); break;
    case '*': scene_send_step(SCENE_SEND_CHUNK); break;
    default: break;
    }
}

// The Nano reports a change made elsewhere (its own knobs, the editor connected to it directly, USB MIDI from an
// MC6): read the preset again shortly after (once for a burst), and the values of the open FX editor.
static void external_change(uint32_t type)
{
    if (esp_timer_get_time() - s_last_change_us < 700 * 1000) return;   // the echo of our own change: nothing new
    values_forget();
    esp_timer_stop(s_app_read_timer);
    esp_timer_start_once(s_app_read_timer, 400 * 1000);
    if (type == NANO_MSG_FX_VALUE && s_edit.slot >= 0 && s_edit.request_slot < 0) editor_read_later(500);
    if (type == NANO_MSG_CAB_SETTING) cab_settings_command(2);
}

static void handle_message(uint32_t type, const uint8_t *payload, size_t len)
{
    switch (type) {
    case NANO_MSG_STATE_RESPONSE: {
        if (!nano_payload_complete(payload, len) && s_incomplete_retries < 3) {
            // A lost packet leaves a broken message (garbled names): ask again.
            s_incomplete_retries++;
            ESP_LOGW(TAG, "%s state reply incomplete (%u bytes) - asking again", s_waiting_full_state ? "Full" : "Current", (unsigned)len);
            if (s_waiting_full_state) request_full_state();
            else request_current_state();
            break;
        }
        s_incomplete_retries = 0;
        if (!s_waiting_full_state && s_expect_preset) {
            int reply_preset = (int)nano_field_varint(payload, len, 13) + 1;
            if (reply_preset != s_expect_preset && esp_timer_get_time() - s_expect_us < 2000000) {
                ESP_LOGI(TAG, "State of preset %d ignored (waiting for preset %d)", reply_preset, s_expect_preset);
                schedule_refresh(150);   // ask again for the new one
                break;
            }
            s_expect_preset = 0;
        }
        bool names = nano_state_apply(&s_state, payload, len);
        // Right after an A/B swap a reply sent before the model change still names the previous model: the slot
        // holds the loaded one. Later a different model was chosen elsewhere (editor, app): it becomes A or B.
        for (int i = 0; i < AB_COUNT; i++) {
            ab_t *ab = &s_ab[i];
            if (ab->preset != s_state.current_preset || ab->slot < 0 || ab->phase != AB_IDLE) continue;
            uint32_t expected = ab->active ? ab->b.type : ab->a_type, now = s_state.fx_type[ab->slot];
            if (!expected || now == expected) continue;
            if (esp_timer_get_time() - ab->swapped_us < AB_SETTLE_US) {
                s_state.fx_type[ab->slot] = expected;
            } else if (now) {
                ESP_LOGI(TAG, "%s %c changed elsewhere: %s", AB_NAMES[i], ab->active ? 'B' : 'A', nano_fx_name(now));
                if (ab->active) {
                    ab->b.type = now;
                    ab->b.count = 0;
                    ab_store(ab);
                } else {
                    ab->a_type = now;
                    ab->a_count = 0;
                    ab_store_a(ab);
                }
            }
        }
        if (s_waiting_full_state) {
            // The full state carries all names; the current preset is read separately, as the editor does.
            s_waiting_full_state = false;
            int count = 0;
            for (int i = 0; i < NANO_PRESETS; i++) count += s_state.preset_names[i][0] != 0;
            printf("\n%d preset names loaded (%u bytes)%s\n", count, (unsigned)len, names ? "" : " - no name list in this reply");
            if (names) print_presets();
            print_help();
            request_current_state();
            // Presets are in: MIDI may scan and connect now, and the app may connect through the controller.
            esp_timer_stop(s_midi_gate_timer);
            midi_ble_allow(true);
            app_link_allow_phone(true);
            app_link_enable(true);
            ui_splash_done();
            if (s_refresh_library) {
                s_refresh_library = false;
                esp_timer_start_once(s_library_timer, 1500 * 1000);
            }
        } else {
            print_summary();
            if (s_save_pending && s_save_pending == s_state.current_preset) {
                ui_show_message(s_state.dirty ? "Save not confirmed - the preset still has unsaved changes."
                                              : "Preset saved.");
                s_save_pending = 0;
            }
            s_mix.slot = reverb_slot();
            if (s_state.current_preset != s_last_preset) {
                // New preset: follow it with the bank and read its reverb Pos 1 / Pos 2.
                s_last_preset = s_state.current_preset;
                values_forget();
                follow_preset_with_bank(s_state.current_preset);
                if (!s_library_requested) {
                    // The library is a large reply: ask once, after the first preset has been read.
                    s_library_requested = true;
                    esp_timer_start_once(s_library_timer, 1500 * 1000);
                    request_settings();   // the Nano's global settings, once (shown in the log)
                }
                ab_capture_editor();   // edits of a second effect in the previous preset
                ab_load(s_state.current_preset);
                editor_close();
                mix_load();
                ab_cache(0, 700);      // values of Pre FX 1 A for a later swap, if it has a B and is on
            }
            show();
            // Other preset or cab while the cab dialog is open: read its settings.
            if (s_src.cab_open && (s_src.cab_preset != s_state.current_preset || s_src.cab_slot != s_state.cab_slot)) {
                request_cab_settings();
            }
            if (s_edit.slot >= 0) {
                editor_show();
                if (s_state.fx_on[s_edit.slot] && !s_edit.known && s_edit.request_slot < 0) editor_read_later(100);
            }
        }
        break;
    }
    case NANO_MSG_EXP_RESPONSE: {
        // Replies come in order; skip older ones while a newer request is out (unless it is overdue).
        bool newer_pending = s_mix.pending > 1 && esp_timer_get_time() - s_mix.request_us < 3000000;
        s_mix.pending = newer_pending ? s_mix.pending - 1 : 0;
        if (newer_pending) break;
        s_mix.slot = reverb_slot();
        nano_exp_assignments(payload, len, s_exp.ranges, s_exp.switches);
        s_exp.known = true;
        // The reverb's Amount on the pedal (FX slots are the first sweeps, in slot order)
        s_mix.exp = s_mix.slot >= 0 && s_exp.ranges[s_mix.slot].on;
        if (s_mix.exp && !s_mix.own) {
            // Set up before 1.8: its Pos 1 / Pos 2 live in that assignment. From now on they are the controller's.
            s_mix.pos[0] = s_exp.ranges[s_mix.slot].heel / 255.0f;
            s_mix.pos[1] = s_exp.ranges[s_mix.slot].toe / 255.0f;
            s_mix.known = true;
            mix_store();
            ESP_LOGI(TAG, "Preset %d: reverb Pos 1 %ld%%, Pos 2 %ld%% taken over from the expression assignment",
                     s_state.current_preset, lroundf(s_mix.pos[0] * 100), lroundf(s_mix.pos[1] * 100));
        }
        if (s_exp.write) {
            int what = s_exp.write;
            s_exp.write = EXP_WRITE_NONE;
            exp_write(what);
        } else if (s_exp.verify) {
            int what = s_exp.verify;
            s_exp.verify = EXP_WRITE_NONE;
            bool same = true;
            for (int i = 0; i < NANO_EXP_RANGES && same; i++) {
                const nano_exp_range_t *is = &s_exp.ranges[i], *want = &s_exp.wanted[i];
                same = is->on == want->on && (!is->on || (is->heel == want->heel && is->toe == want->toe));
            }
            for (int i = 0; i < NANO_EXP_SWITCHES && same; i++) {
                const nano_exp_switch_t *is = &s_exp.switches[i], *want = &s_exp.wanted_switches[i];
                same = is->on == want->on;
                if (!same || !is->on) continue;
                same = is->mode == want->mode && (is->mode == NANO_EXP_STOP || is->inverted == want->inverted)
                       && (is->mode != NANO_EXP_SWITCH || is->latch == want->latch)
                       && (is->mode == NANO_EXP_SWITCH || is->delay_ms == want->delay_ms);
            }
            ESP_LOGI(TAG, "Expression assignments check: %s", same ? "matches" : "DIFFERENT");
            ui_show_message(!same ? "Not confirmed: the preset reports other expression assignments."
                            : what == EXP_WRITE_FREE_REVERB ? "The expression pedal no longer moves the reverb in this preset."
                            : "Expression pedal saved.");
        }
        if (s_exp.open && !s_exp.write && !s_exp.verify) ui_set_expression(s_exp.ranges, s_exp.switches);
        show();
        break;
    }
    case NANO_MSG_TUNER_MODE: {   // tuner switched on the Nano itself
        bool on = nano_field_varint(payload, len, 4) != 0;
        ESP_LOGI(TAG, "Tuner %s on the Nano", on ? "opened" : "closed");
        s_tuner_on = on;
        if (on) s_state.tuner_muted = nano_field_varint(payload, len, 7) != 0;
        ui_show_tuner(on);
        if (on) ui_tuner_settings(s_state.tuner_base_hz, s_state.tuner_muted);
        break;
    }
    case NANO_MSG_LIBRARY_RESPONSE: {
        if (!nano_payload_complete(payload, len) && s_incomplete_retries < 3) {
            s_incomplete_retries++;
            ESP_LOGW(TAG, "Library reply incomplete (%u bytes) - asking again", (unsigned)len);
            request_library();
            break;
        }
        s_incomplete_retries = 0;
        nano_library_t *lib = nano_library_parse(payload, len);
        if (!lib) {
            ESP_LOGW(TAG, "Library reply (%u bytes) could not be read", (unsigned)len);
            break;
        }
        nano_library_t *old = s_library;   // a refresh replaces it
        s_library = lib;
        ESP_LOGI(TAG, "Library: %d captures, %d cabs (%u bytes)", lib->captures.count, lib->cabs.count, (unsigned)len);
        ui_set_library(lib);
        if (old) {
            free(old->captures.items);
            free(old->cabs.items);
            free(old);
        }
        break;
    }
    case NANO_MSG_RENAME_PRESET_RESPONSE: {
        bool ok = nano_field_varint(payload, len, 4) == 1;
        ESP_LOGI(TAG, "Rename %s", ok ? "confirmed" : "FAILED");
        if (!ok && s_rename_preset) {
            strlcpy(s_state.preset_names[s_rename_preset - 1], s_rename_old, sizeof(s_state.preset_names[0]));
            ui_show_message("The Nano did not accept this name.");
            show();
        }
        s_rename_preset = 0;
        break;
    }
    case NANO_MSG_CAPTURE_LOAD_RESPONSE:
    case NANO_MSG_CAB_LOAD_RESPONSE: {
        bool ok = nano_field_varint(payload, len, 3) == 1;
        ESP_LOGI(TAG, "Library %s %s", type == NANO_MSG_CAB_LOAD_RESPONSE ? "cab" : "capture", ok ? "loaded" : "load FAILED");
        if (!ok) ui_show_message("The Nano did not load this library item.");
        schedule_refresh(300);
        break;
    }
    case NANO_MSG_FX_PARAMS_RESPONSE: {
        if (ab_values_read(payload, len)) break;
        int slot = s_edit.request_slot;
        s_edit.request_slot = -1;
        if (slot < 0 || slot != s_edit.slot) break;   // editor closed or another slot meanwhile
        s_edit.count = nano_fx_params_values(payload, len, s_edit.values, NANO_MAX_PARAMS);
        s_edit.known = s_edit.count > 0;
        for (int i = 0; i < s_edit.count; i++) value_note(slot, i, s_edit.values[i]);
        ESP_LOGI(TAG, "%s: %d parameter values read", NANO_FX_SLOT_NAMES[slot], s_edit.count);
        editor_show();
        break;
    }
    case NANO_MSG_TUNER_METERING: {
        static char last_note[sizeof(((nano_tuner_reading_t *)0)->note)];
        nano_tuner_reading_t reading;
        nano_tuner_reading(payload, len, &reading);
        if (strcmp(reading.note, last_note)) {
            ESP_LOGI(TAG, "Tuner note \"%s\" (%+.1f ct)", reading.note, reading.cents);
            strlcpy(last_note, reading.note, sizeof(last_note));
        }
        ui_show_tuner_reading(&reading);
        break;
    }
    case NANO_MSG_SET_PRESET_SLOTS: {   // preset changed on the Nano itself
        nano_state_apply_preset_change(&s_state, payload, len);
        s_expect_preset = 0;
        ESP_LOGI(TAG, "Preset changed on the Nano: %d", s_state.current_preset);
        uint8_t buf[4];
        send("Preset change ack", NANO_MSG_SET_PRESET_SLOTS_RESPONSE, buf, nano_preset_change_ack(buf));
        schedule_refresh(150);
        break;
    }
    case NANO_MSG_SET_PRESET_SLOTS_RESPONSE:
        ESP_LOGI(TAG, "Preset change %s", nano_field_varint(payload, len, 4) == 1 ? "confirmed" : "FAILED");
        if (s_expect_preset) schedule_refresh(40);   // the new preset is ready: read it now (not after a fixed wait)
        break;
    case NANO_MSG_SETTINGS_RESPONSE: {
        // The whole reply in the log: the Nano's global settings (only the USB volume is used so far).
        ESP_LOGI(TAG, "Settings reply (%u bytes):", (unsigned)len);
        ESP_LOG_BUFFER_HEX(TAG, payload, len < 160 ? len : 160);
        // 0 dB is the protobuf default and is then left out of the reply.
        s_usb.db = 0;
        bool present = nano_settings_usb_gain(payload, len, &s_usb.db);
        s_usb.known = true;
        ESP_LOGI(TAG, "USB playback volume %.1f dB%s", s_usb.db, present ? "" : " (not in the reply)");
        ui_set_usb_gain(s_usb.db);
        s_jack = nano_settings_jack(payload, len);
        ESP_LOGI(TAG, "EXP/MIDI connector: %s", s_jack == NANO_JACK_MIDI ? "MIDI" : s_jack == NANO_JACK_EXPRESSION ? "expression pedal" : "?");
        ui_set_jack(s_jack);
        if (s_jack_wanted >= 0) {
            bool same = s_jack == s_jack_wanted;
            s_jack_wanted = -1;
            ui_show_message(!same ? "Not confirmed: the Nano reports the other mode for its EXP/MIDI connector."
                            : s_jack == NANO_JACK_MIDI ? "The Nano's EXP/MIDI connector is set to MIDI."
                            : "The Nano's EXP/MIDI connector is set to expression pedal.");
        }
        break;
    }
    case NANO_MSG_CAB_SETTINGS_RESPONSE: {
        float values[NANO_CAB_SETTINGS];
        if (!s_src.cab_open) break;
        char info[96];
        snprintf(info, sizeof(info), "%s  -  slot %d", s_state.cab[0] ? s_state.cab : "Cab", s_state.cab_slot);
        if (nano_cab_settings_values(payload, len, values)) {
            ESP_LOGI(TAG, "Cab settings: output %.1f dB, high pass %.0f Hz, low pass %.0f Hz",
                     nano_cab_setting_value(NANO_CAB_OUTPUT, values[0]), nano_cab_setting_value(NANO_CAB_HIGH_PASS, values[1]),
                     nano_cab_setting_value(NANO_CAB_LOW_PASS, values[2]));
            ui_set_cab_settings(values, true, info);
        } else {
            ESP_LOGW(TAG, "Cab settings reply without the values (%u bytes)", (unsigned)len);
            ESP_LOG_BUFFER_HEX(TAG, payload, len < 64 ? len : 64);
            ui_set_cab_settings(NULL, true, "Values not readable - moving a slider still sets it.");
        }
        break;
    }
    case NANO_MSG_METERING: {
        // The expression pedal's position: sent after a settings read and while the pedal moves.
        int what, value;
        nano_metering(payload, len, &what, &value);
        int64_t now = esp_timer_get_time();
        if (now - s_cal.logged_us > 500 * 1000) {   // (a moving pedal sends many)
            s_cal.logged_us = now;
            ESP_LOGI(TAG, "Metering: type %d, value %d (%u bytes)", what, value, (unsigned)len);
            ESP_LOG_BUFFER_HEX(TAG, payload, len < 16 ? len : 16);
        }
        if (what != NANO_METER_PEDAL) break;
        s_cal.value = value;
        if (!s_cal.active) break;
        if (value < s_cal.min) s_cal.min = value;
        if (value > s_cal.max) s_cal.max = value;
        ui_set_pedal(value, s_cal.min, s_cal.max);
        break;
    }
    case NANO_MSG_UPDATE_SETTINGS_RESPONSE:
        ESP_LOGI(TAG, "Settings update answered (%u bytes)", (unsigned)len);
        ESP_LOG_BUFFER_HEX(TAG, payload, len < 32 ? len : 32);
        break;
    case NANO_MSG_DIRTY_CHANGED:
        s_state.dirty = nano_field_varint(payload, len, 3) != 0;
        ESP_LOGI(TAG, "Preset %s", s_state.dirty ? "edited (unsaved changes)" : "without unsaved changes");
        show();
        if (s_state.dirty) external_change(type);
        break;
    case NANO_MSG_VALUE: {   // a knob on the Nano, or the editor connected to the Nano: shown at once
        int id = (int)nano_field_varint(payload, len, 3), value = (int)nano_field_varint(payload, len, 4);
        if (id >= 0 && id < NANO_AMP_KNOBS) s_state.amp[id] = value;
        else if (id == 10) s_state.capture_volume = value;
        ESP_LOGI(TAG, "Value %d = %d (changed on the Nano or by another app)", id, value);
        show();
        external_change(type);
        break;
    }
    case NANO_MSG_SELECTOR:
    case NANO_MSG_BYPASS:
    case NANO_MSG_CAB_SETTING:
    case NANO_MSG_FX_VALUE:
    case NANO_MSG_FX_TYPE:
        ESP_LOGI(TAG, "Message type %lu (%u bytes) - changed elsewhere, reading again", (unsigned long)type, (unsigned)len);
        external_change(type);
        break;
    default:
        ESP_LOGI(TAG, "Message type %lu (%u bytes)", (unsigned long)type, (unsigned)len);
        break;
    }
}

// ---- tasks ----

static void app_event(app_event_t ev)
{
    switch (ev.kind) {
    case EV_LINK_UP:
        ESP_LOGI(TAG, "Connected to the Nano Cortex - reading presets");
        ui_splash_status("Connected - loading presets ...", NULL);
        ui_set_link(true);
        s_incomplete_retries = 0;
        request_full_state();
        break;
    case EV_LINK_DOWN:
        ESP_LOGW(TAG, "Connection lost - searching again");
        s_waiting_full_state = false;
        midi_ble_allow(false);   // until the Nano is back (or the gate timer opens it)
        app_link_allow_phone(false);   // no new phone meanwhile; a connected one stays
        app_link_enable(false);  // the app can only use the Nano through the controller while it is connected
        esp_timer_stop(s_midi_gate_timer);
        esp_timer_start_once(s_midi_gate_timer, MIDI_GATE_US);
        s_tuner_on = false;
        s_last_preset = 0;
        s_mix.pending = 0;
        s_usb.known = false;
        memset(&s_src, 0, sizeof(s_src));   // also a throttle whose timer command came while disconnected
        s_edit.slot = -1;
        s_edit.request_slot = -1;
        s_scene_queued = s_scene_sent = 0;
        values_forget();
        ui_set_link(false);
        break;
    case EV_MESSAGE:
        handle_message(ev.type, ev.data, ev.len);
        free(ev.data);
        break;
    case EV_COMMAND:
        handle_command(ev.command, ev.arg);
        break;
    case EV_PARAM:
        editor_param(ev.arg, ev.param, ev.value);
        break;
    case EV_TEXT:
        if (ev.command == 'N' && nano_link_ready()) rename_preset((const char *)ev.data);
        else if (ev.command >= '1' && ev.command < '1' + FXP_COUNT) fxp_save(ev.command - '0', (const char *)ev.data);
        else if (ev.command == 'L') looper_tile_edit((const char *)ev.data);
        else if (ev.command == 'S') scene_edit((const char *)ev.data);
        else if (ev.command == 'X') exp_dialog_save((const char *)ev.data);
        free(ev.data);
        break;
    case EV_MIDI:
        handle_midi(ev.arg);
        break;
    }
}

#ifdef NANO_WEB
// Browser build: the page's main loop handles the queued events (there are no tasks).
void web_app_pump(void)
{
    app_event_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) app_event(ev);
}
#else
static void app_task(void *arg)
{
    app_event_t ev;
    for (;;) {
        xQueueReceive(s_events, &ev, portMAX_DELAY);
        app_event(ev);
    }
}

// Characters typed into the serial monitor: the letters of the help are commands. Digits select a preset: the number is
// shown while typing and taken on Enter or after a short pause, so Enter is not required.
// The console runs on the board's native "USB" port (USB Serial/JTAG) or, if configured, on the "UART" port.
#define DIGIT_PAUSE_MS 1200
#define CONSOLE_COMMANDS "npabcdemtxsrlh"

static int console_read(uint8_t *c, TickType_t wait)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(c, 1, wait);
#else
    return uart_read_bytes(CONSOLE_UART, c, 1, wait);
#endif
}

static void console_task(void *arg)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();
#else
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONSOLE_UART);
#endif
    char digits[3];
    int count = 0;
    for (;;) {
        uint8_t c;
        if (console_read(&c, count ? pdMS_TO_TICKS(DIGIT_PAUSE_MS) : portMAX_DELAY) != 1) {
            c = '\r';   // pause after digits: take the number
        }
        if (isdigit(c)) {
            if (count < 2) digits[count++] = (char)c;
            digits[count] = 0;
            printf("\rPreset: %s ", digits);
            fflush(stdout);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (count) {
                printf("\n");
                command('g', atoi(digits));
                count = 0;
            }
            continue;
        }
        count = 0;
        if (c == 'o' || c == 'O') {
            command('w', UI_SWITCH_HOLD | 1);
            continue;
        }
        if (c == '$') {
            command('$', -1);
            continue;
        }
#ifdef NANO_BENCH
        if (c == '%') {
            ui_bench();
            continue;
        }
#endif
        c = (uint8_t)tolower(c);
        if (c && strchr(CONSOLE_COMMANDS, c)) command((char)c, 0);   // other letters are used by the screen
    }
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xQueueCreate(32, sizeof(app_event_t));
    s_state.tuner_base_hz = 440;
    const esp_timer_create_args_t timer = { .callback = refresh_cb, .name = "refresh" };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_refresh_timer));
    const esp_timer_create_args_t param_timer = { .callback = param_timer_cb, .name = "param" };
    ESP_ERROR_CHECK(esp_timer_create(&param_timer, &s_param_timer));
    const esp_timer_create_args_t read_timer = { .callback = read_timer_cb, .name = "read" };
    ESP_ERROR_CHECK(esp_timer_create(&read_timer, &s_read_timer));
    const esp_timer_create_args_t jack_timer = { .callback = jack_timer_cb, .name = "jack" };
    ESP_ERROR_CHECK(esp_timer_create(&jack_timer, &s_jack_timer));
    const esp_timer_create_args_t usb_timer = { .callback = usb_timer_cb, .name = "usb" };
    ESP_ERROR_CHECK(esp_timer_create(&usb_timer, &s_usb_timer));
    const esp_timer_create_args_t fxp_timer = { .callback = fxp_timer_cb, .name = "fxp" };
    ESP_ERROR_CHECK(esp_timer_create(&fxp_timer, &s_fxp_timer));
    const esp_timer_create_args_t source_timer = { .callback = source_timer_cb, .name = "source" };
    ESP_ERROR_CHECK(esp_timer_create(&source_timer, &s_src_timer));
    const esp_timer_create_args_t exp_timer = { .callback = exp_timer_cb, .name = "exp" };
    ESP_ERROR_CHECK(esp_timer_create(&exp_timer, &s_exp_timer));
    const esp_timer_create_args_t ab_timer = { .callback = ab_timer_cb, .name = "ab" };
    ESP_ERROR_CHECK(esp_timer_create(&ab_timer, &s_ab_timer));
    const esp_timer_create_args_t gate_timer = { .callback = midi_gate_cb, .name = "midi_gate" };
    ESP_ERROR_CHECK(esp_timer_create(&gate_timer, &s_midi_gate_timer));
    const esp_timer_create_args_t app_read_timer = { .callback = app_read_cb, .name = "app_read" };
    ESP_ERROR_CHECK(esp_timer_create(&app_read_timer, &s_app_read_timer));
    const esp_timer_create_args_t app_notify_timer = { .callback = app_notify_cb, .name = "app_notify" };
    ESP_ERROR_CHECK(esp_timer_create(&app_notify_timer, &s_app_notify_timer));
    const esp_timer_create_args_t library_timer = { .callback = library_timer_cb, .name = "library" };
    ESP_ERROR_CHECK(esp_timer_create(&library_timer, &s_library_timer));
    banks_load();
    looper_tiles_load();
    s_scenes = heap_caps_calloc(UI_SCENES, sizeof(scene_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    configASSERT(s_scenes);
    const esp_timer_create_args_t scene_timer = { .callback = scene_timer_cb, .name = "scene" };
    ESP_ERROR_CHECK(esp_timer_create(&scene_timer, &s_scene_timer));
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t stored = 0;
        size_t size = 1;
        s_scene_mode = nvs_get_blob(nvs, SCENE_MODE_KEY, &stored, &size) == ESP_OK && size == 1 && stored;
        nvs_close(nvs);
    }

    board_display_init();

    static const char *const reasons[] = { "unknown", "power-on", "external pin", "software", "panic (crash)",
                                           "interrupt watchdog", "task watchdog", "other watchdog", "deep sleep",
                                           "brownout (supply voltage too low)", "SDIO", "USB", "JTAG", "eFuse",
                                           "power glitch", "CPU lockup" };
    esp_reset_reason_t reason = esp_reset_reason();
    printf("\nNano Cortex Controller %s - started after: %s\n", esp_app_get_description()->version,
           reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[reason] : "?");
    // Bluetooth first: its host task and buffers need internal RAM, which the screen's many small objects would
    // otherwise take (they move to PSRAM when internal RAM runs short). Its events wait in the queue until the
    // app task runs, after the screen is built.
    app_link_start(on_app_write, on_app_state);
    nano_link_start(on_message, on_link);
    midi_ble_start(on_midi, on_midi_change);
    midi_ble_allow(false);   // first the Nano and its presets
    phone_midi_start(on_phone_midi, on_phone_state);
    esp_timer_start_once(s_midi_gate_timer, MIDI_GATE_US);

    ui_init(command, on_param, on_text);
    char version[40];
    snprintf(version, sizeof(version), "v%s", esp_app_get_description()->version);
#ifdef NANO_WEB
    ui_splash_status("Click CONNECT and choose your Nano Cortex", version);
#else
    ui_splash_status("Searching for the Nano Cortex ...", version);
#endif
    s_footswitches = footswitches_start(board_i2c_bus(), on_footswitch, on_footswitch_learned);
#ifndef NANO_WEB
    xTaskCreate(app_task, "app", 6144, NULL, 4, NULL);
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
#endif
    ESP_LOGI(TAG, "Free internal RAM %u KB (largest block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#ifndef NANO_WEB
    nvs_stats_t stats;   // banks, FX presets, second effects and scenes share this store
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        ESP_LOGI(TAG, "Settings store: %u of %u entries used", (unsigned)stats.used_entries, (unsigned)stats.total_entries);
    }
#endif
}
