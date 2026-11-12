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
//                  with a second effect for Pre FX 1, holding 3 swaps it A <-> B (it stays on or off);
//                  a wah set to auto-wah moves in tempo while it is on (set to Hold: only while its switch is held)
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
#ifndef NANO_WEB
#include "esp_attr.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#ifndef NANO_WEB
#include "esp_flash.h"
#include "esp_partition.h"
#endif

#include "board.h"
#include "app_link.h"
#include "footswitches.h"
#include "library.h"
#include "midi_ble.h"
#include "phone_midi.h"
#include "nano_link.h"
#include "nano_state.h"
#include "ui.h"
#include "pedal_assets.h"

static const char *TAG = "nano_ctrl";

#define CONSOLE_UART UART_NUM_0  // "UART" USB-C port (CH343), used when the console is set to UART

typedef enum { EV_LINK_UP, EV_LINK_DOWN, EV_MESSAGE, EV_COMMAND, EV_PARAM, EV_TEXT, EV_MIDI, EV_APP_MESSAGE } event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t type;   // EV_MESSAGE, EV_APP_MESSAGE
    uint8_t *data;   // EV_MESSAGE / EV_APP_MESSAGE / EV_TEXT, owned by the event
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
// A state request whose reply never comes is asked again; after STATE_RETRIES the link is set up afresh. Without
// this a lost reply right after connecting left the start screen at "loading presets" for good.
#define STATE_REPLY_US (4 * 1000 * 1000)
#define STATE_REPLY_FULL_US (8 * 1000 * 1000)   // all preset names: a long reply
#define STATE_RETRIES 2
static esp_timer_handle_t s_state_timer;
static int s_state_retries;
// Back after a lost link: the preset names are still known, so only the current preset is read (a fifth of a second
// instead of more than two) and the names are read again later, in a quiet moment.
static bool s_reconnected, s_names_due;
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
// The Nano Cortex Editor, connected through the controller, hears of every press too ({ 1: footswitch }; a message
// type of our own, far from the Nano's): it treats it as that controller, for example for its jam track player.
#define APP_MSG_SWITCH 0x4E430001u
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
    uint8_t capture;            // capture slot 1-25 the scene loads, 0 = it leaves the capture as it is
    scene_fx_t set[NANO_FX_SLOTS];
} scene_t;
static scene_t *s_scenes;       // UI_SCENES of them, in PSRAM (internal RAM is short)
static int s_scenes_preset;     // preset the scenes belong to, 0 = none read yet
static bool s_scene_mode;       // footswitches 3-8 are scenes instead of the bank's presets (stored)
static int s_scene_last = -1;   // recalled last: the lit one if two scenes are alike
#define SCENE_STORE_KEY "scn%02d"
#define SCENE_VALUES_KEY "scv%02d"    // the settings, apart from the names (stored as 1.8.0 did)
#define SCENE_CAPTURE_KEY "scc%02d"   // the capture of each scene (UI_SCENES bytes, 0 = as it is; since 1.12.0)
#define SCENE_VALUES_MAX (UI_SCENES * NANO_FX_SLOTS * (6 + 2 * NANO_MAX_PARAMS))
#define SCENE_MODE_KEY "scenemode"
#define LAYOUT_KEY "layout"   // { count, order[8] }
#define PEDALS_KEY "pedals"   // 1 = the full-screen view shows the FX slots as pedals

// Tile layout: footswitch / CC 50+p-1 / Chocolate switch p presses the switch s_layout[p - 1] (1-8) - for p up to
// s_layout_count (6 or 8); the switches themselves keep their numbers everywhere else. Set in the layout dialog.
static volatile uint8_t s_layout[FOOTSWITCH_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static volatile int s_layout_count = FOOTSWITCH_COUNT;
// What footswitch 1 changes between (UI_SWITCH1_*), chosen in the tile layout dialog.
#define SWITCH1_KEY "switch1"
static int s_switch1 = UI_SWITCH1_FX_PRESETS;

// The switch at position p (1-8), 0 = no field there.
static int layout_switch(int p)
{
    return p >= 1 && p <= s_layout_count ? s_layout[p - 1] : 0;
}
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

// What an effect was last switched to (from here or by the app) while the Nano's report has not followed it, -1 =
// nothing waiting. The Nano reports an effect whose on/off is on the expression pedal (heel-toe) as the pedal has it,
// not as it was switched over Bluetooth - the effect sounds anyway. Without this the next press sent "on" again.
static int8_t s_fx_switched[NANO_FX_SLOTS] = { -1, -1, -1, -1, -1 };
static bool s_fx_reported[NANO_FX_SLOTS];   // the on/off states of the last reply
static uint32_t s_fx_reports_seen;
static int s_fx_preset;

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
static int s_preset_tries;       // requests sent for the preset change under way
static int s_preset_refused;     // preset the Nano refused to load, 0 = none (shown until a change succeeds)
static bool s_expect_ack;       // the Nano has not confirmed the change yet: a state reply is from before it
static int64_t s_expect_us;
static int s_bank;              // 0-15

// Set list: songs in the order of the gig (presets; one may come more than once), stored on the controller. While it
// is on, footswitches 3-8 in preset mode are six songs of it (a page) instead of the bank's presets; the arrow buttons
// turn the pages, a swipe goes to the previous / next song. Choosing the last song of a page turns to the next page,
// so the footswitches alone get through the whole set.
// A song can start with a scene of its preset: choosing it loads the preset, then the scene.
#define SETLIST_KEY "setlist"   // { on, count, presets[count], scenes[count] } (1.12.0 before the scenes: no scenes)
static struct {
    bool on;
    int count;
    uint8_t presets[UI_SETLIST_MAX];
    uint8_t scenes[UI_SETLIST_MAX];   // start scene 1-6, 0 = none
    int page;                   // the first song shown is page * BANK_SLOTS
    int song;                   // the song chosen last (0-based), -1 = none
    int start_preset, start_scene;    // a start scene waiting for its preset to be loaded (0 = none)
    char names[BANK_SLOTS][UI_SCENE_NAME];   // names of the start scenes of the songs on the page
    int names_page;             // the page they were read for, -1 = read again
} s_setlist = { .song = -1, .names_page = -1 };
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
    bool touched;          // mix changed while editing: set back to the active position when closed
} s_mix = { .slot = -1, .active = -1 };
#define MIX_STORE_KEY "mix%02d"       // before 1.15: Pos 1, Pos 2 (0-255), an entry per preset
#define MIX_ALL_KEY "mixes"           // all presets in one entry (an entry per preset took three places in the store each)
static struct {
    uint8_t pos[NANO_PRESETS][2];     // Pos 1, Pos 2 (0-255)
    uint8_t set[(NANO_PRESETS + 7) / 8];   // one bit per preset: it has its own positions
} s_mixes;

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
    int64_t pending_us;    // when that write or check was started (one that gets no reply is given up)
    nano_exp_range_t wanted[NANO_EXP_RANGES];
    nano_exp_switch_t wanted_switches[NANO_EXP_SWITCHES];
    bool shown;            // the open dialog has got the assignments
    int retries;           // ... it asked again so often
} s_exp;

// Expression requests on their way. The reply (61) does not say which preset it is for, so every request is
// remembered with its preset and what it is for; a reply belongs to the oldest one and is dropped if that was for
// another preset than the current one. A request without a reply is forgotten after EXP_REPLY_US - a lost reply
// must not shift the pairing for good (it once left the expression dialog waiting for ever).
enum { EXP_ASK_READ, EXP_ASK_WRITE, EXP_ASK_VERIFY };
#define EXP_ASKED 6
#define EXP_REPLY_US (3 * 1000 * 1000)
static struct { int preset, purpose; int64_t at_us; } s_exp_asked[EXP_ASKED];
static int s_exp_asked_count;
static esp_timer_handle_t s_exp_retry_timer;

// Expression pedal calibration (the dialog's Calibrate): the Nano forgets its calibration and reports the pedal's
// position (metering) while it is moved over its whole way; the lowest and highest position are then saved - on the
// Nano, and here: Cancel puts them back, so the pedal is not left without a calibration.
static struct {
    bool active;
    bool reset;            // the Nano's calibration was reset and not saved again yet
    int64_t logged_us;
} s_cal;
#define CAL_STORE_KEY "expcal"        // the range saved last: lowest, highest (int32 each)
// The pedal's position arrives many times a second while the pedal moves. It is taken where it arrives (host task),
// with the lowest and highest while calibrating; the app task hears of it once per round, not once per message.
static volatile int s_pedal_value, s_pedal_min, s_pedal_max;   // min > max = nothing seen yet
static volatile bool s_pedal_collect;
static volatile int64_t s_pedal_collect_from_us;   // positions before that are still the old calibration's
static int s_pedal_event;                          // an event is on its way (atomic)

// FX editor (long press on an FX tile). Parameter values are only read for FX that are on;
// moving a control sends at most one value per PARAM_SEND_MS, plus the last one.
#define PARAM_SEND_MS 25
static struct {
    int slot;                        // -1 = closed
    bool known;
    float values[NANO_MAX_PARAMS];
    int count;
    int read_tries;                  // reads of its values that got no reply in a row
    int pending_slot, pending_param; // value waiting for the throttle timer, pending_param -1 = none
    float pending_value;
    bool throttling;
} s_edit = { .slot = -1, .pending_param = -1 };

// Reading an effect's parameter values. The Nano's reply (138) does not say which slot it is for; replies come in
// the order of the requests. So only one read is open at a time - who wants to read meanwhile tries again a moment
// later - and a reply counts only for the open read: in time, with the model still in the slot and with as many
// values as that model has. No read goes out while the Nano loads a preset or a model (its reply would come late).
// Before this, two reads open at once, or a late reply taken for the next read, put one effect's values into
// another: a reverb's Mix into the Input of the compressor on Pre FX 1, kept there as the values of its "A".
enum { READ_NONE, READ_EDITOR, READ_AB, READ_RP };
#define PARAM_READ_US (2000 * 1000)   // a reply later than this is not waited for
#define PARAM_BUSY_US (900 * 1000)    // no read this long after a preset or model change
static struct {
    int purpose, slot;                // the open read (READ_NONE = none)
    uint32_t type;                    // model in the slot when it was asked
    int64_t at_us;
    uint32_t odd_type;                // a reply with another number of values than the model's table says:
    int odd_count;                    // taken when it comes twice (then the table is off, not the reply)
} s_read;
static int64_t s_read_busy_until_us;
static esp_timer_handle_t s_param_timer, s_read_timer;

// FX presets: up to FXP_COUNT named settings per effect model, stored on the controller and shown above the
// parameters in the FX editor. A tap loads one into the edited slot (sent like the sliders, a few values at a time);
// a long press saves the current values under a name. ORIGINAL goes back to the values the effect had when the
// editor opened (or when its model was chosen: the model's defaults).
#define FXP_COUNT 4
#define FXP_SEND_CHUNK 8
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

// Reverb switch, third way (next to the mix positions and the second reverb): two named settings of the preset's
// reverb - "Room" and "Atmos", every parameter - that the switch changes between. Each is what the reverb was set to
// when "Set from the reverb" was tapped in the reverb dialog; stored on the controller, per preset. They are their
// own thing: the second effect (A / B) of the reverb's slot is not touched and stays on the reverb's own tile.
// The preset's reverb and the second one of the reverb's tile (B) each have their own two: RP_PAIRS places per
// preset, each for one reverb model.
#define RP_PAIRS 2
static const char *const RP_STORE_KEYS[RP_PAIRS] = { "rvp%02d", "rvq%02d" };
typedef struct {
    uint32_t type;                       // the reverb model the settings are for: they count while it is in the slot
    uint8_t used;                        // 1 = the reverb switch changes between them
    uint8_t count[2];                    // values of setting 1 / 2, 0 = not set yet
    uint8_t reserved;
    char name[2][16];
    uint16_t values[2][NANO_MAX_PARAMS]; // 0-65535 = 0-1
} rp_t;                                  // stored in NVS: the head as it is, then the values that are set
#define RP_HEAD offsetof(rp_t, values)
_Static_assert(RP_HEAD == 40 && sizeof(rp_t) == 40 + 4 * NANO_MAX_PARAMS, "rp_t's head is stored as it is: it must not change");
static rp_t s_rp[RP_PAIRS];
static int s_rp_preset;                  // the preset they were read for, 0 = none yet
#define RP_NEITHER 2
#define RP_MATCH 0.004f                  // values count as the same within this (0-1)
static uint8_t s_rp_active[RP_PAIRS];    // 0 / 1: the setting that is on, RP_NEITHER: the reverb is set to something else.
                                         // After loading a preset 0 until its values were read (rp_match).
static esp_timer_handle_t s_rp_timer;    // reads the reverb's values after a preset change
static int s_rp_check_tries;
static int s_rp_capture = -1;            // the setting that waits for the reverb's values, -1 = none

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
//   reverb:   B is the reverb's like any slot's: its own tile swaps it. Footswitch 8 swaps A <-> B too only when the
//             reverb dialog (long press on tile 8) was saved on its Reverb A / B tab; otherwise B is "parked" for
//             footswitch 8, which then is the mix switch (or the two settings) - the reverb's tile keeps its A / B
//   Pre FX 1: the 2ND button in its FX editor chooses B; footswitch 3 short = on/off, held = A <-> B
typedef struct {
    uint32_t type;                    // model of B, 0 = none
    uint8_t count;                    // stored values, 0 = model defaults
    uint8_t parked;                   // reverb B: footswitch 8 does not swap it (mix / two settings); its tile does
                                      // (0 in data stored before 1.8: footswitch 8 swaps)
    uint8_t tap;                      // what the slot's switch does (UI_AB_*): 0 = a tap switches on / off and holding
    uint8_t reserved;                 // swaps (also data stored before 1.12), 1 = the other way round, 2 = a press swaps
    float values[NANO_MAX_PARAMS];
} fx_b_t;                             // in NVS: its first FX_B_HEAD bytes, then the values that are set, 16 bits each
#define FX_B_HEAD 8
_Static_assert(sizeof(fx_b_t) == FX_B_HEAD + 4 * NANO_MAX_PARAMS && offsetof(fx_b_t, values) == FX_B_HEAD,
               "fx_b_t's head is stored as it is, and firmware before 1.15 stored all of it: it must not change");
typedef struct {
    uint32_t type;                    // model of A the values belong to
    uint8_t count, reserved;
    uint16_t values[NANO_MAX_PARAMS]; // 0-65535 = 0-1
} fx_a_t;                             // A as last read; in NVS: its first FX_A_HEAD bytes, then the values that are set
#define FX_A_HEAD 6
_Static_assert(sizeof(fx_a_t) == 56 && offsetof(fx_a_t, values) == FX_A_HEAD,
               "fx_a_t is stored from its start, and firmware before 1.15 stored all of it: it must not change");

// A second effect in the store takes what it needs: the head and the values that are set, as 16-bit numbers.
// Firmware before 1.15 stored the whole struct (B with floats); both are read, and store_compact() rewrites the old
// entries once. The functions return the stored size / whether the entry could be read.
static size_t fx_b_pack(const fx_b_t *b, uint8_t buf[FX_B_HEAD + 2 * NANO_MAX_PARAMS])
{
    size_t count = b->count > NANO_MAX_PARAMS ? NANO_MAX_PARAMS : b->count;
    memcpy(buf, b, FX_B_HEAD);
    buf[offsetof(fx_b_t, count)] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        uint16_t v = value_u16(b->values[i]);
        memcpy(buf + FX_B_HEAD + 2 * i, &v, 2);
    }
    return FX_B_HEAD + 2 * count;
}

static bool fx_b_unpack(const uint8_t *buf, size_t size, fx_b_t *b)
{
    memset(b, 0, sizeof(*b));
    if (size == sizeof(*b)) {   // as before 1.15
        memcpy(b, buf, sizeof(*b));
    } else if (size >= FX_B_HEAD && size == FX_B_HEAD + 2 * (size_t)buf[offsetof(fx_b_t, count)]) {
        memcpy(b, buf, FX_B_HEAD);
        for (size_t i = 0; i < b->count && i < NANO_MAX_PARAMS; i++) {
            uint16_t v;
            memcpy(&v, buf + FX_B_HEAD + 2 * i, 2);
            b->values[i] = v / 65535.0f;
        }
    }
    if (!b->type || b->count > NANO_MAX_PARAMS) memset(b, 0, sizeof(*b));
    return b->type != 0;
}

static size_t fx_a_pack(const fx_a_t *a, uint8_t buf[sizeof(fx_a_t)])
{
    size_t count = a->count > NANO_MAX_PARAMS ? NANO_MAX_PARAMS : a->count;
    memcpy(buf, a, FX_A_HEAD + 2 * count);
    return FX_A_HEAD + 2 * count;
}

static bool fx_a_unpack(const uint8_t *buf, size_t size, fx_a_t *a)
{
    memset(a, 0, sizeof(*a));
    if (size == sizeof(*a) || (size >= FX_A_HEAD && size == FX_A_HEAD + 2 * (size_t)buf[offsetof(fx_a_t, count)])) memcpy(a, buf, size);
    if (!a->type || a->count > NANO_MAX_PARAMS) memset(a, 0, sizeof(*a));
    return a->type != 0;
}
#define AB_COUNT NANO_FX_SLOTS         // one per FX slot (before 1.12: the reverb and Pre FX 1 only)
enum { AB_IDLE, AB_TURNING_ON, AB_READING, AB_LOADING };
#define AB_SEND_CHUNK 8               // parameter values per step (the BLE write queue holds 32)
typedef struct {
    int preset;                       // preset the data belongs to
    fx_b_t b;
    uint32_t a_type;                  // preset's own effect, kept from the first read
    uint8_t a_count;                  // values of A read so far, 0 = none
    float a_values[NANO_MAX_PARAMS];
    int active;                       // 0 = A, 1 = B
    int slot, target, phase, sent;    // slot = its index in s_ab
    bool reverb;                      // the preset's reverb is in this slot: stored under the second reverb's keys
    bool cache_only;                  // reading the values for later, no swap
    int read_tries;                   // the read could not go out yet (another read open, the Nano busy)
    bool restore_off;                 // A was switched on only to read it: off again before the swap
    bool open_editor;                 // open the FX editor once B is loaded
    int64_t swapped_us;               // last model change by the controller (state replies may still be older)
} ab_t;
#define AB_SETTLE_US (1500 * 1000)
static ab_t s_ab[AB_COUNT];

// NVS key of an A/B's B (or of the kept values of its A). The preset's reverb slot and Pre FX 1 keep the keys they
// had when they were the only two; the other slots have their own.
static void ab_key(const ab_t *ab, bool a, char key[16])
{
    if (ab->reverb) snprintf(key, 16, a ? "rva%02d" : "rvb%02d", ab->preset);
    else if (ab->slot == 0) snprintf(key, 16, a ? "pfa%02d" : "pf1%02d", ab->preset);
    else snprintf(key, 16, a ? "aa%d%02d" : "ab%d%02d", ab->slot, ab->preset);
}

static const char *ab_name(const ab_t *ab)
{
    return NANO_FX_SLOT_NAMES[ab->slot];
}

// The slot has a second effect that its switch can swap in. (For the reverb: whether footswitch 8 swaps it too is
// reverb_b_used().)
static bool ab_used(int slot)
{
    if (slot < 0 || slot >= AB_COUNT) return false;
    const ab_t *ab = &s_ab[slot];
    return ab->preset && ab->preset == s_state.current_preset && ab->b.type;
}
// Switches whose short press acts on release because they have a held function (bit n = footswitch n).
static volatile uint32_t s_hold_switches;

// Auto-wah: the controller moves the position of a wah (its only parameter) between a heel and a toe value, in
// tempo - the Nano's own (its state reports it) or one set here - while the effect is on. Set up per preset in the
// FX editor of the wah and stored on the controller. In mode "Hold" the footswitch of the slot switches the wah on
// only while it is held (it acts on press and release, so the wah starts at once). The values go out like those of
// a moved slider: the link keeps only the newest per control and sends them without waiting for an answer.
typedef struct {
    uint32_t type;      // wah model the settings belong to, 0 = none set
    uint8_t mode;       // UI_WAH_OFF / UI_WAH_ON / UI_WAH_HELD
    uint8_t rate;       // index into WAH_BEATS
    uint8_t shape;      // 0 triangle, 1 sine, 2 up (heel to toe, then back at once)
    uint8_t heel, toe;  // 0-100 %
    uint8_t own_tempo;  // 0 = the Nano's tempo
    uint16_t bpm;       // the own tempo
} wah_t;                // stored in NVS as an array of NANO_FX_SLOTS
_Static_assert(sizeof(wah_t) == 12, "wah_t is stored as it is: its size must not change");
static const wah_t WAH_DEFAULT = { .rate = 3, .toe = 100, .bpm = 120 };
static const float WAH_BEATS[UI_WAH_RATES] = { 8, 4, 2, 1, 0.5f, 0.25f };   // beats per round: 2 bars ... 1/16
static wah_t s_wah[NANO_FX_SLOTS];
static int s_wah_preset;              // preset the settings belong to, 0 = none read yet
#define WAH_STORE_KEY "awh%02d"
#define WAH_STEP_MS 25                // one value per step (as a moved slider, PARAM_SEND_MS)
#define WAH_PARAM 0                   // the position is the only parameter of the four wah models
static struct {
    int slot;                         // FX slot that is being moved, -1 = none
    int64_t start_us;                 // the round starts at the heel
    unsigned steps;                   // (the FX editor's bar follows every fourth)
} s_sweep = { .slot = -1 };
static esp_timer_handle_t s_wah_timer;
// Footswitches that switch their effect on while they are held (auto-wah "Hold"; bit n = footswitch n).
static volatile uint32_t s_momentary_switches;

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
    if (type == NANO_MSG_METERING) {
        int what, value;
        nano_metering(payload, len, &what, &value);
        if (what != NANO_METER_PEDAL) return;   // (levels are not used)
        s_pedal_value = value;
        if (s_pedal_collect && esp_timer_get_time() >= s_pedal_collect_from_us) {
            if (value < s_pedal_min) s_pedal_min = value;
            if (value > s_pedal_max) s_pedal_max = value;
        }
        if (__atomic_exchange_n(&s_pedal_event, 1, __ATOMIC_SEQ_CST)) return;
        app_event_t ev = { .kind = EV_COMMAND, .command = ':' };
        if (xQueueSend(s_events, &ev, 0) != pdTRUE) __atomic_store_n(&s_pedal_event, 0, __ATOMIC_SEQ_CST);
        return;
    }
    app_event_t ev = { .kind = EV_MESSAGE, .type = type, .len = len };
    ev.data = malloc(len ? len : 1);
    if (!ev.data) {
        ESP_LOGE(TAG, "Out of memory for a message of %u bytes", (unsigned)len);
        return;
    }
    memcpy(ev.data, payload, len);
    post(ev);
}

// (From the Bluetooth host task. A lost event would leave the screen and the state on the wrong side of the link,
// so this one waits for room in the queue.)
static void on_link(bool ready)
{
    app_event_t ev = { .kind = ready ? EV_LINK_UP : EV_LINK_DOWN };
    if (xQueueSend(s_events, &ev, pdMS_TO_TICKS(3000)) != pdTRUE) ESP_LOGE(TAG, "Event queue full - link event lost");
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
    number = layout_switch(number);   // the field at this footswitch's position
    if (!number) return;
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
    // A switch that acts while it is held: down and up go out as they happen (its release also if the mode changed
    // meanwhile - what went on has to go off).
    static bool held[FOOTSWITCH_COUNT + 1];
    if (held[number] || (event == FOOTSWITCH_PRESS && ((s_momentary_switches >> number) & 1))) {
        if (event == FOOTSWITCH_PRESS) {
            held[number] = true;
            command('w', UI_SWITCH_DOWN | number);
        } else if (event == FOOTSWITCH_RELEASE) {
            held[number] = false;
            command('w', UI_SWITCH_UP | number);
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

static void exp_retry_timer_cb(void *arg)
{
    command(';', 0);
}

static void usb_timer_cb(void *arg)
{
    command('G', 0);
}

static void fxp_timer_cb(void *arg)
{
    command('#', 0);
}

static void rp_timer_cb(void *arg)
{
    command('-', 48);
}

static void scene_timer_cb(void *arg)
{
    command('*', 0);
}

static void wah_timer_cb(void *arg)
{
    command('/', 0);
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

// The Nano can be found for a few seconds only after it lost its connection or was switched on. Later it has to be
// put into pairing mode by hand: EXIT + CAPTURE on the Nano. The controller says so when the Nano has not come
// after PAIR_HINT_US.
#define PAIR_HINT_US (7 * 1000 * 1000)
static esp_timer_handle_t s_pair_hint_timer;

static void pair_hint_cb(void *arg)
{
    command(',', 0);
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
static void on_app_write(uint32_t type, const uint8_t *payload, size_t len)
{
    if (type >= APP_LINK_CONTROLLER_TYPE) {   // for the controller itself (settings backup): handled in the app task
        app_event_t ev = { .kind = EV_APP_MESSAGE, .type = type, .len = len };
        ev.data = malloc(len ? len : 1);
        if (!ev.data) return;
        memcpy(ev.data, payload, len);
        post(ev);
        return;
    }
    s_values_stale = true;   // it may have set an effect's values
    if (type == NANO_MSG_BYPASS) {   // BypassControl { 3: slot 4-8, 4: 0 = on }: the editor switched an effect
        int slot = (int)nano_field_varint(payload, len, 3) - 4;
        if (slot >= 0 && slot < NANO_FX_SLOTS) command('%', slot << 1 | (nano_field_varint(payload, len, 4) == 0));
    }
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

static void state_timer_cb(void *arg)
{
    command('{', 0);
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
           "  A-E          FX mode: hold its footswitch (second effect, settings swap)\n"
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

// A layout is valid if it shows 6 or 8 fields and lists every switch once.
static bool layout_valid(int count, const uint8_t *order)
{
    if (count != 6 && count != FOOTSWITCH_COUNT) return false;
    unsigned seen = 0;
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) {
        if (order[p] < 1 || order[p] > FOOTSWITCH_COUNT) return false;
        seen |= 1u << order[p];
    }
    return seen == 0x1FE;
}

static bool s_pedal_view;

static void layout_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t stored[1 + FOOTSWITCH_COUNT];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, LAYOUT_KEY, stored, &size) == ESP_OK && size == sizeof(stored) && layout_valid(stored[0], stored + 1)) {
        for (int p = 0; p < FOOTSWITCH_COUNT; p++) s_layout[p] = stored[1 + p];
        s_layout_count = stored[0];
    }
    uint8_t switch1;
    size = 1;
    if (nvs_get_blob(nvs, SWITCH1_KEY, &switch1, &size) == ESP_OK && size == 1 && switch1 < UI_SWITCH1_COUNT) s_switch1 = switch1;
    uint8_t pedals = 0;
    size = 1;
    if (nvs_get_blob(nvs, PEDALS_KEY, &pedals, &size) == ESP_OK && size == 1) s_pedal_view = pedals == 1;
    nvs_close(nvs);
}

// The "PEDALS" field of the full-screen bar: FX slots as pedals, or as tiles. (Also sent when the full-screen view
// comes up, to have the state shown again: then nothing is stored.)
static void pedal_view_set(bool on)
{
    if (on != s_pedal_view) {
        s_pedal_view = on;
        nvs_handle_t nvs;
        if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            uint8_t stored = on;
            if (nvs_set_blob(nvs, PEDALS_KEY, &stored, 1) == ESP_OK) nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Pedal view %s", on ? "on" : "off");
    }
    ui_set_pedal_view(s_pedal_view);
    if (nano_link_ready()) show();
}

// From the layout dialog: count << 24 | (switch at position p - 1) << 3p.
static void layout_set(int packed)
{
    uint8_t stored[1 + FOOTSWITCH_COUNT] = { (uint8_t)((packed >> 24) & 0xF) };
    int switch1 = (packed >> 28) & 3;
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) stored[1 + p] = (uint8_t)(((packed >> (3 * p)) & 7) + 1);
    if (!layout_valid(stored[0], stored + 1)) return;
    s_layout_count = stored[0];
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) s_layout[p] = stored[1 + p];
    s_switch1 = switch1;
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        uint8_t s1 = (uint8_t)switch1;
        if (nvs_set_blob(nvs, LAYOUT_KEY, stored, sizeof(stored)) == ESP_OK && nvs_set_blob(nvs, SWITCH1_KEY, &s1, 1) == ESP_OK) nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Tile layout: %d fields, switches %d %d %d %d %d %d %d %d", stored[0], stored[1], stored[2], stored[3],
             stored[4], stored[5], stored[6], stored[7], stored[8]);
    ui_set_layout(stored[0], stored + 1, switch1);
    if (nano_link_ready()) show();
    ui_show_message(stored[0] == 6 ? "Tile layout saved: 6 fields." : "Tile layout saved: 8 fields.");
}

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

// From the looper tile's dialog: "<footswitch 2-8><colour a-j><symbol a-j>name", or "<footswitch>!" for the default.
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
    uint8_t captures[UI_SCENES];
    snprintf(key, sizeof(key), SCENE_CAPTURE_KEY, preset);
    size = sizeof(captures);
    if (nvs_get_blob(nvs, key, captures, &size) == ESP_OK && size == sizeof(captures)) {
        for (int i = 0; i < UI_SCENES; i++) {
            if (s_scenes[i].name[0] && captures[i] <= NANO_CAPTURE_SLOTS) s_scenes[i].capture = captures[i];
        }
    }
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
    uint8_t captures[UI_SCENES], any = 0;
    for (int i = 0; i < UI_SCENES; i++) any |= captures[i] = s_scenes[i].name[0] ? s_scenes[i].capture : 0;
    snprintf(key, sizeof(key), SCENE_CAPTURE_KEY, s_scenes_preset);
    err = any ? nvs_set_blob(nvs, key, captures, sizeof(captures)) : nvs_erase_key(nvs, key);
    failed |= err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND;
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
static bool setlist_used(void)
{
    return s_setlist.on && s_setlist.count > 0;
}

// The song that is on: the one chosen last if its preset is the current one, otherwise the first with that preset.
static int setlist_song_now(void)
{
    if (s_setlist.song >= 0 && s_setlist.song < s_setlist.count && s_setlist.presets[s_setlist.song] == s_state.current_preset) {
        return s_setlist.song;
    }
    for (int i = 0; i < s_setlist.count; i++) if (s_setlist.presets[i] == s_state.current_preset) return i;
    return -1;
}

// The page that shows a song: the next one after the last song of a page (if there is a next song).
static int setlist_page_of(int song)
{
    if (song % BANK_SLOTS == BANK_SLOTS - 1 && song + 1 < s_setlist.count) return song / BANK_SLOTS + 1;
    return song / BANK_SLOTS;
}

static int setlist_pages(void)
{
    return (s_setlist.count + BANK_SLOTS - 1) / BANK_SLOTS;
}

static void select_preset(int preset);

static void setlist_choose(int song)
{
    if (song < 0 || song >= s_setlist.count) return;
    s_setlist.song = song;
    s_setlist.page = setlist_page_of(song);
    s_setlist.start_preset = s_setlist.scenes[song] ? s_setlist.presets[song] : 0;
    s_setlist.start_scene = s_setlist.scenes[song];
    ESP_LOGI(TAG, "Set list: song %d of %d, preset %d%s", song + 1, s_setlist.count, s_setlist.presets[song],
             s_setlist.scenes[song] ? ", then its scene" : "");
    select_preset(s_setlist.presets[song]);
}

static void scene_recall(int n);

// A new preset is read: the start scene of the song that loaded it.
static void setlist_start_scene(void)
{
    if (!s_setlist.start_scene || s_setlist.start_preset != s_state.current_preset || !s_state.fx_known) return;
    int n = s_setlist.start_scene - 1;
    s_setlist.start_scene = s_setlist.start_preset = 0;
    if (s_scenes_preset != s_state.current_preset) scenes_load(s_state.current_preset);
    if (s_scenes[n].name[0]) scene_recall(n);
    else ESP_LOGW(TAG, "Set list: scene %d of preset %d is empty", n + 1, s_state.current_preset);
}

// Names of the scenes of a preset as stored ("" = empty) - also for the set list dialog (from the LVGL task).
static void scene_names_read(int preset, char names[UI_SCENES][UI_SCENE_NAME])
{
    memset(names, 0, UI_SCENES * UI_SCENE_NAME);
    nvs_handle_t nvs;
    if (preset < 1 || preset > NANO_PRESETS || nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), SCENE_STORE_KEY, preset);
    uint8_t stored[UI_SCENES * (3 + UI_SCENE_NAME)];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, key, stored, &size) == ESP_OK) {
        for (size_t at = 0; at + 4 <= size;) {   // as scenes_load reads them
            const uint8_t *entry = stored + at;
            size_t length = strnlen((const char *)entry + 3, size - at - 3);
            if (at + 3 + length >= size) break;
            at += 3 + length + 1;
            if (entry[0] < UI_SCENES && length && length < UI_SCENE_NAME) memcpy(names[entry[0]], entry + 3, length + 1);
        }
    }
    nvs_close(nvs);
}

// Swipe or MIDI: the previous / next song (from the song that is on, or the first).
static void setlist_step(int step)
{
    int now = setlist_song_now();
    int song = now < 0 ? 0 : now + (step < 0 ? -1 : 1);
    if (song < 0 || song >= s_setlist.count) {
        ui_show_message(song < 0 ? "This is the first song of the set list." : "This is the last song of the set list.");
        return;
    }
    setlist_choose(song);
}

static void setlist_store(void)
{
    uint8_t stored[2 + 2 * UI_SETLIST_MAX] = { s_setlist.on, (uint8_t)s_setlist.count };
    memcpy(stored + 2, s_setlist.presets, (size_t)s_setlist.count);
    memcpy(stored + 2 + s_setlist.count, s_setlist.scenes, (size_t)s_setlist.count);
    s_setlist.names_page = -1;
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(nvs, SETLIST_KEY, stored, 2 + 2 * (size_t)s_setlist.count);
    if (err != ESP_OK || nvs_commit(nvs) != ESP_OK) ui_show_message("Could not store the set list (memory full?).");
    nvs_close(nvs);
}

static void setlist_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t stored[2 + 2 * UI_SETLIST_MAX];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, SETLIST_KEY, stored, &size) == ESP_OK && size >= 2 && stored[1] <= UI_SETLIST_MAX
        && (size == 2 + (size_t)stored[1] || size == 2 + 2 * (size_t)stored[1])) {
        bool scenes = size == 2 + 2 * (size_t)stored[1];
        s_setlist.on = stored[0] != 0;
        s_setlist.count = 0;
        for (int i = 0; i < stored[1]; i++) {
            if (stored[2 + i] < 1 || stored[2 + i] > NANO_PRESETS) continue;
            uint8_t scene = scenes ? stored[2 + stored[1] + i] : 0;
            s_setlist.scenes[s_setlist.count] = scene <= UI_SCENES ? scene : 0;
            s_setlist.presets[s_setlist.count++] = stored[2 + i];
        }
    }
    nvs_close(nvs);
}

static void show(void);

static void setlist_set_on(bool on)
{
    if (on == s_setlist.on) return;
    s_setlist.on = on;
    setlist_store();
    int now = setlist_song_now();
    s_setlist.page = now >= 0 ? setlist_page_of(now) : 0;
    ESP_LOGI(TAG, "Set list %s", on ? "on" : "off");
    show();
}

// Text from the set list dialog: "<0 / 1: on>" and per song "<preset, two hex digits><start scene 0-6>".
static void setlist_edit(const char *text)
{
    if (!text[0]) return;
    int count = 0;
    for (const char *c = text + 1; c[0] && c[1] && c[2] && count < UI_SETLIST_MAX; c += 3) {
        char hex[3] = { c[0], c[1], 0 };
        int preset = (int)strtol(hex, NULL, 16), scene = c[2] - '0';
        if (preset < 1 || preset > NANO_PRESETS) continue;
        s_setlist.scenes[count] = (uint8_t)(scene >= 0 && scene <= UI_SCENES ? scene : 0);
        s_setlist.presets[count++] = (uint8_t)preset;
    }
    s_setlist.on = text[0] == '1';
    s_setlist.count = count;
    if (s_setlist.song >= count) s_setlist.song = -1;
    int now = setlist_song_now();
    s_setlist.page = now >= 0 ? setlist_page_of(now) : 0;
    setlist_store();
    ESP_LOGI(TAG, "Set list: %d songs, %s", count, s_setlist.on ? "on" : "off");
    ui_show_message(!count ? "Set list emptied." : s_setlist.on ? "Set list saved - footswitches 3-8 are its songs." : "Set list saved (off).");
    show();
}

static void follow_preset_with_bank(int preset)
{
    if (setlist_used()) {   // the page with the song that is on
        int now = setlist_song_now();
        if (now >= 0 && now / BANK_SLOTS != s_setlist.page && setlist_page_of(now) != s_setlist.page) s_setlist.page = setlist_page_of(now);
        return;
    }
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

// Footswitch 8 in FX mode swaps reverb A <-> B when the reverb dialog was saved on its Reverb A / B tab (B not
// parked); otherwise it is the mix switch. The reverb's own tile swaps its A / B either way.
static bool ab_used(int slot);

static bool reverb_b_used(void)
{
    return ab_used(s_mix.slot) && !s_ab[s_mix.slot].b.parked;
}

// Which footswitches wait for their release (they have a held function): 1 (held = looper mode) and 2 (held =
// scenes <-> presets) always, 3 in FX mode with a second effect. In looper mode none - there every press goes out
// at once.
static int wah_mode(int slot);
static bool ab_used(int slot);
static bool rp_used(void);
static bool rp_set(int n);
static int rp_pair(void);
static void rp_match(int slot, const float *values, int count);
static void rp_check_later(int ms);

static void looper_switches(void)
{
    uint32_t hold = s_looper ? 0 : 1u << 1 | 1u << 2;
    // FX mode: a slot with a second effect has a held function (A <-> B, or on / off if a tap swaps), so its switch
    // acts when the foot lifts. The switch of a wah set to "Hold" acts while it is held - unless its slot has a
    // second effect: that keeps the held function, and the wah moves while it is on.
    uint32_t momentary = 0;
    for (int slot = 0; slot < NANO_FX_SLOTS && s_fx_mode && !s_looper; slot++) {
        if (ab_used(slot)) {
            if (s_ab[slot].b.tap != UI_AB_PRESS) hold |= 1u << (slot + 3);   // (a press that only swaps acts at once)
        } else if (wah_mode(slot) == UI_WAH_HELD) momentary |= 1u << (slot + 3);
    }
    s_hold_switches = hold;
    s_momentary_switches = momentary;
    s_looper_live = s_looper;
}

static void wah_load(int preset);
static void wah_update(void);

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
        .looper = s_looper,
        .phone = phone_midi_connected(),
        .looper_sent = s_looper_sent,
        .scenes = s_scene_mode,
    };
    const ab_t *reverb = s_mix.slot >= 0 && s_ab[s_mix.slot].preset == s_state.current_preset ? &s_ab[s_mix.slot] : NULL;
    view.rev_b_type = reverb && reverb_b_used() ? reverb->b.type : 0;
    view.rev_b_stored = reverb ? reverb->b.type : 0;
    view.rev_active = reverb ? reverb->active : 0;
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        if (!ab_used(slot)) continue;
        view.ab_b_type[slot] = s_ab[slot].b.type;
        view.ab_a_type[slot] = s_ab[slot].a_type;
        if (s_ab[slot].active) view.ab_active |= (uint8_t)(1u << slot);
        view.ab_mode[slot] = s_ab[slot].b.tap <= UI_AB_PRESS ? s_ab[slot].b.tap : UI_AB_HOLD;
    }
    view.rp_used = rp_used();
    view.not_loaded = s_preset_refused && s_preset_refused == s_state.current_preset;
    int rp = rp_pair();
    view.rp_active = rp >= 0 ? s_rp_active[rp] : 0;
    for (int n = 0; n < 2; n++) {
        if (rp_set(n)) view.rp_set |= (uint8_t)(1u << n);
        if (rp >= 0) strlcpy(view.rp_names[n], s_rp[rp].name[n], sizeof(view.rp_names[n]));
    }
    for (int slot = 0; slot < NANO_FX_SLOTS && s_exp.known; slot++) {   // effects the Nano's pedal moves or switches
        if (s_exp.ranges[NANO_EXP_PRE1 + slot].on) view.exp_amount |= (uint8_t)(1u << slot);
        if (s_exp.switches[NANO_EXP_SW_PRE1 + slot].on) view.exp_switch |= (uint8_t)(1u << slot);
    }
    for (int i = 0; i < NANO_EXP_RANGES && s_exp.known; i++) view.exp_used |= s_exp.ranges[i].on;
    for (int i = 0; i < NANO_EXP_SWITCHES && s_exp.known; i++) view.exp_used |= s_exp.switches[i].on;
    if (s_scenes_preset != s_state.current_preset) scenes_load(s_state.current_preset);
    if (s_wah_preset != s_state.current_preset) wah_load(s_state.current_preset);
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        view.wah_mode[slot] = (uint8_t)wah_mode(slot);
        view.wah_rate[slot] = s_wah[slot].rate;
    }
    wah_update();
    view.scene_active = scene_active();
    for (int i = 0; i < UI_SCENES; i++) {
        strlcpy(view.scene_names[i], s_scenes[i].name, sizeof(view.scene_names[i]));
        view.scene_colors[i] = s_scenes[i].color;
        view.scene_fx[i] = s_scenes[i].fx;
        view.scene_capture[i] = s_scenes[i].capture;
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
    view.setlist_on = s_setlist.on;
    view.setlist_count = (uint8_t)s_setlist.count;
    memcpy(view.setlist_presets, s_setlist.presets, sizeof(view.setlist_presets));
    memcpy(view.setlist_scenes, s_setlist.scenes, sizeof(view.setlist_scenes));
    if (setlist_used()) {   // the songs of the page, in the colour and with the symbol of their preset's bank switch
        if (s_setlist.page >= setlist_pages()) s_setlist.page = 0;
        view.setlist_first = (uint8_t)(s_setlist.page * BANK_SLOTS);
        view.setlist_song = (int8_t)setlist_song_now();
        if (s_setlist.names_page != s_setlist.page) {   // read when the page or the scenes change, not with every show
            s_setlist.names_page = s_setlist.page;
            for (int i = 0; i < BANK_SLOTS; i++) {
                int song = view.setlist_first + i;
                s_setlist.names[i][0] = 0;
                if (song >= s_setlist.count || !s_setlist.scenes[song]) continue;
                char names[UI_SCENES][UI_SCENE_NAME];
                scene_names_read(s_setlist.presets[song], names);
                strlcpy(s_setlist.names[i], names[s_setlist.scenes[song] - 1], sizeof(s_setlist.names[i]));
            }
        }
        memcpy(view.setlist_scene_names, s_setlist.names, sizeof(view.setlist_scene_names));
        for (int i = 0; i < BANK_SLOTS; i++) {
            int song = view.setlist_first + i;
            int preset = song < s_setlist.count ? s_setlist.presets[song] : 0;
            view.bank_presets[i] = (uint8_t)preset;
            view.bank_colors[i] = view.bank_icons[i] = 0;
            for (int bank = 0; bank < BANKS && preset; bank++) {
                int slot = 0;
                while (slot < BANK_SLOTS && s_banks[bank][slot].preset != preset) slot++;
                if (slot == BANK_SLOTS) continue;
                view.bank_colors[i] = s_banks[bank][slot].color;
                view.bank_icons[i] = s_banks[bank][slot].icon;
                break;
            }
        }
    }
    ui_show_state(&s_state, &view);
}

static void fxp_show(void);

static void wah_show(int slot);

static void editor_show(void)
{
    if (s_edit.slot < 0) return;
    wah_show(s_edit.slot);   // first: the editor builds the auto-wah rows of a wah with these values
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
        s_read_busy_until_us = esp_timer_get_time() + PARAM_BUSY_US;
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
    esp_timer_stop(s_state_timer);
    esp_timer_start_once(s_state_timer, STATE_REPLY_FULL_US);
}

static void request_current_state(void)
{
    uint8_t buf[16];
    send("Current state request", NANO_MSG_STATE_REQUEST, buf, nano_current_state_request(buf));
    if (!s_waiting_full_state) {   // (the full state's own, longer wait goes on)
        esp_timer_stop(s_state_timer);
        esp_timer_start_once(s_state_timer, STATE_REPLY_US);
    }
}

// No state reply in time: ask again, then connect afresh.
static void state_overdue(void)
{
    if (!nano_link_ready()) return;
    if (++s_state_retries > STATE_RETRIES) {
        ESP_LOGE(TAG, "The Nano does not answer - connecting again");
        s_state_retries = 0;
        nano_link_restart();
        return;
    }
    ESP_LOGW(TAG, "No %s state reply - asking again (%d)", s_waiting_full_state ? "full" : "current", s_state_retries);
    if (s_waiting_full_state) request_full_state();
    else request_current_state();
}

static bool param_read_open(void)
{
    return s_read.purpose != READ_NONE && esp_timer_get_time() - s_read.at_us < PARAM_READ_US;
}

// Asks for the values of the effect in a slot. False: not now (another read is open, the Nano is busy, or the
// effect is off - the Nano only reports values of effects that are on) - try again a moment later.
static bool param_read(int purpose, int slot)
{
    if (param_read_open() || esp_timer_get_time() < s_read_busy_until_us) return false;
    if (slot < 0 || slot >= NANO_FX_SLOTS || !s_state.fx_type[slot] || !s_state.fx_on[slot]) return false;
    uint8_t buf[8];
    char label[48];
    snprintf(label, sizeof(label), "%s values request", NANO_FX_SLOT_NAMES[slot]);
    if (!send(label, NANO_MSG_FX_PARAMS_REQUEST, buf, nano_fx_params_request(slot, buf))) return false;
    s_read.purpose = purpose;
    s_read.slot = slot;
    s_read.type = s_state.fx_type[slot];
    s_read.at_us = esp_timer_get_time();
    return true;
}

// The reply: whose read it answers (READ_NONE = nobody's), the slot and the values. count 0 = it does not fit the
// read (the model changed meanwhile, or another number of values than the model has).
static int param_read_reply(const uint8_t *payload, size_t len, int *slot, float *values, int *count)
{
    int purpose = param_read_open() ? s_read.purpose : READ_NONE;
    s_read.purpose = READ_NONE;
    *slot = s_read.slot;
    *count = nano_fx_params_values(payload, len, values, NANO_MAX_PARAMS);
    if (purpose == READ_NONE) {
        ESP_LOGW(TAG, "FX values nobody waits for (%d values) - ignored", *count);
        return READ_NONE;
    }
    const nano_fx_model_t *model = nano_fx_model(s_state.fx_type[*slot]);
    int expected = !model ? 0 : model->param_count < NANO_MAX_PARAMS ? model->param_count : NANO_MAX_PARAMS;
    if (s_state.fx_type[*slot] != s_read.type) {
        ESP_LOGW(TAG, "%s: values of the model that was there before - ignored", NANO_FX_SLOT_NAMES[*slot]);
        *count = 0;
    } else if (*count != expected) {
        bool again = s_read.odd_type == s_read.type && s_read.odd_count == *count;
        ESP_LOGW(TAG, "%s: %d values, %s has %d - %s", NANO_FX_SLOT_NAMES[*slot], *count, nano_fx_name(s_read.type), expected,
                 again ? "taken (the same as last time)" : "ignored");
        s_read.odd_type = s_read.type;
        s_read.odd_count = *count;
        if (!again) *count = 0;
    }
    return purpose;
}

static void ab_cache(int slot, int delay_ms);
static void ab_leave_preset(void);

// The Nano can refuse a preset change (its reply says so): it then reports the new number and name, but capture and
// effects stay those of the preset before. Seen in a 30-minute run: from one moment on every change was refused.
// The change is asked for once more; if that is refused too the player is told - the screen would otherwise show a
// preset that is not the one that sounds.

static void preset_request(int preset)
{
    uint8_t buf[64];
    char label[32];
    size_t n = nano_preset_change(&s_state, preset, buf);
    snprintf(label, sizeof(label), "Preset change %d", preset);
    send(label, NANO_MSG_SET_PRESET_SLOTS, buf, n);
    ESP_LOGI(TAG, "Preset change %d: slot field %d, the Nano's slots %d %d %d %d", preset, buf[n - 1], s_state.slot_preset[0] + 1,
             s_state.slot_preset[1] + 1, s_state.slot_preset[2] + 1, s_state.slot_preset[3] + 1);
}

static void select_preset(int preset)
{
    app_sync_later();
    if (preset < 1) preset = NANO_PRESETS;
    if (preset > NANO_PRESETS) preset = 1;
    ab_leave_preset();   // also for the preset that is on: the Nano reads it from its memory again
    s_preset_tries = 1;
    preset_request(preset);
    s_state.current_preset = preset;
    s_expect_preset = preset;
    s_expect_ack = true;
    s_expect_us = esp_timer_get_time();
    show();
    schedule_refresh(1200);   // (the fallback: the Nano's confirmation reads the preset at once, see there)
}

// Switches the effect of a slot on or off (the caller shows the state and reads it again).
static void set_fx(int slot, bool on)
{
    uint8_t buf[8];
    char label[32];
    snprintf(label, sizeof(label), "%s %s", NANO_FX_SLOT_NAMES[slot], on ? "ON" : "OFF");
    size_t n = nano_fx_bypass(slot, on, buf);
    send(label, NANO_MSG_BYPASS, buf, n);
    app_link_send(NANO_MSG_BYPASS, buf, n);   // the editor shows it at once (and does not go by the Nano's report)
    s_state.fx_on[slot] = on;
    s_fx_switched[slot] = on;
    if (slot == s_edit.slot) {
        if (!on) s_edit.known = false;
        editor_show();
        if (on) editor_read_later(400);
    } else if (on) {
        ab_cache(slot, 400);   // values of A for a later swap, if this slot has a second effect
    }
}

// After a state reply: the on/off states as the Nano reports them - except for an effect whose report did not move
// since it was switched: then the switch counts (an effect on the expression pedal, or a reply that left the Nano
// before the switch).
static void fx_follow_switches(void)
{
    if (s_state.fx_reports == s_fx_reports_seen) return;   // no on/off states in this reply
    s_fx_reports_seen = s_state.fx_reports;
    bool other_preset = s_state.current_preset != s_fx_preset;
    s_fx_preset = s_state.current_preset;
    for (int i = 0; i < NANO_FX_SLOTS; i++) {
        bool reported = s_state.fx_on[i];
        if (other_preset || reported != s_fx_reported[i] || reported == s_fx_switched[i]) {
            s_fx_switched[i] = -1;   // the report moved or agrees: it counts
        } else if (s_fx_switched[i] >= 0) {
            ESP_LOGI(TAG, "%s reported %s, switched %s - stays %s (expression pedal?)", NANO_FX_SLOT_NAMES[i],
                     reported ? "ON" : "off", s_fx_switched[i] ? "ON" : "off", s_fx_switched[i] ? "ON" : "off");
            s_state.fx_on[i] = s_fx_switched[i];
        }
        s_fx_reported[i] = reported;
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
    if (slot == s_mix.slot && s_state.fx_on[slot] && rp_used()) rp_check_later(500);   // on again: which setting is it?
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

static void select_capture(int slot);

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
    // The scene's capture first: loading it takes the Nano a moment, the effects follow
    if (scene->capture && scene->capture != s_state.capture_slot) {
        ESP_LOGI(TAG, "Scene %d: capture slot %d", n + 1, scene->capture);
        select_capture(scene->capture);
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
        if (color < 0 || color >= UI_BANK_COLOR_COUNT || strlen(text) < 3 + NANO_FX_SLOTS) return;
        scene.color = (uint8_t)color;
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
            if (text[2 + slot] == '1') scene.fx |= (uint8_t)(1u << slot);
        }
        int capture = text[2 + NANO_FX_SLOTS] - 'a';
        scene.capture = capture >= 0 && capture <= NANO_CAPTURE_SLOTS ? (uint8_t)capture : 0;
        const char *name = text + 3 + NANO_FX_SLOTS;
        while (*name == ' ') name++;
        if (*name) strlcpy(scene.name, name, sizeof(scene.name));
        else snprintf(scene.name, sizeof(scene.name), "Scene %d", n + 1);
    }
    s_scenes[n] = scene;
    scenes_save();
    s_setlist.names_page = -1;   // the set list shows scene names
    ESP_LOGI(TAG, "Preset %d scene %d: %s", s_scenes_preset, n + 1, scene.name[0] ? scene.name : "emptied");
    if (scene.name[0]) scene_recall(n);
    else show();
}

// Reads the parameters of the edited slot - never for a bypassed FX (that crashed the Nano).
static void editor_read(void)
{
    int slot = s_edit.slot;
    if (slot < 0 || !s_state.fx_type[slot] || !s_state.fx_on[slot]) return;
    if (param_read_open() && s_read.purpose == READ_EDITOR && s_read.slot == slot) return;   // asked: its reply comes
    if (!param_read(READ_EDITOR, slot)) {   // another read is open or the Nano is busy: a moment later
        editor_read_later(250);
        return;
    }
    // No reply in time: asked again, three times (the timer is stopped when the reply comes).
    if (++s_edit.read_tries <= 3) editor_read_later(PARAM_READ_US / 1000 + 100);
    else ESP_LOGW(TAG, "%s: the Nano does not send the values", NANO_FX_SLOT_NAMES[slot]);
}

static void value_note(int slot, int param, float value);

static bool editor_reading(void)
{
    return param_read_open() && s_read.purpose == READ_EDITOR;
}

// The values of the edited effect arrived (count 0: a reply that did not fit - read again).
static void editor_values(int slot, const float *values, int count)
{
    if (slot != s_edit.slot) return;   // the editor closed or shows another slot meanwhile
    if (count <= 0) {
        editor_read_later(300);
        return;
    }
    esp_timer_stop(s_read_timer);
    s_edit.read_tries = 0;
    memcpy(s_edit.values, values, sizeof(float) * (size_t)count);
    s_edit.count = count;
    s_edit.known = true;
    for (int i = 0; i < count; i++) value_note(slot, i, values[i]);
    ESP_LOGI(TAG, "%s: %d parameter values read", NANO_FX_SLOT_NAMES[slot], count);
    editor_show();
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
    // The log names every value that goes to an effect (a moving control: twice a second) - what the controller
    // set is then never a guess.
    static int logged_slot = -1, logged_param = -1;
    static int64_t logged_us;
    int64_t now = esp_timer_get_time();
    if (slot != logged_slot || param != logged_param || now - logged_us > 500 * 1000) {
        const nano_fx_model_t *model = nano_fx_model(s_state.fx_type[slot]);
        ESP_LOGI(TAG, "-> %s %s: %s = %.3f", NANO_FX_SLOT_NAMES[slot], model ? model->name : "?",
                 model && param < model->param_count ? model->params[param].name : "?", (double)value);
        logged_slot = slot;
        logged_param = param;
        logged_us = now;
    }
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
    if (s_edit.known) rp_match(slot, s_edit.values, s_edit.count);   // the reverb set by hand: is it still a setting?
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

// ---- auto-wah ----

static bool is_wah(uint32_t type)
{
    const nano_fx_model_t *model = nano_fx_model(type);
    return model && model->icon == NANO_ICON_WAH;
}

// The auto-wah mode of the wah in this slot: its settings count while their model is the one in the slot.
static int wah_mode(int slot)
{
    const wah_t *w = &s_wah[slot];
    return s_state.fx_known && w->type && w->type == s_state.fx_type[slot] && is_wah(w->type) ? w->mode : UI_WAH_OFF;
}

static void wah_load(int preset)
{
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) s_wah[slot] = WAH_DEFAULT;
    s_wah_preset = preset;
    char key[16];
    snprintf(key, sizeof(key), WAH_STORE_KEY, preset);
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    wah_t stored[NANO_FX_SLOTS];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, key, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
            const wah_t *w = &stored[slot];
            if (w->mode > UI_WAH_HELD || w->rate >= UI_WAH_RATES || w->shape >= UI_WAH_SHAPES || w->heel > 100 || w->toe > 100
                || w->bpm < UI_WAH_BPM_MIN || w->bpm > UI_WAH_BPM_MAX) continue;   // not ours: the defaults stay
            s_wah[slot] = *w;
        }
    }
    nvs_close(nvs);
}

static void wah_save(void)
{
    char key[16];
    snprintf(key, sizeof(key), WAH_STORE_KEY, s_wah_preset);
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, key, s_wah, sizeof(s_wah)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
        ui_show_message("Could not store the auto-wah (memory full?).");
    }
    nvs_close(nvs);
}

// The FX editor's auto-wah rows: the settings of the wah in this slot (the defaults until it has its own).
static void wah_show(int slot)
{
    const wah_t *w = s_wah[slot].type && s_wah[slot].type == s_state.fx_type[slot] ? &s_wah[slot] : &WAH_DEFAULT;
    ui_autowah_t shown = { .mode = w->mode, .rate = w->rate, .shape = w->shape, .heel = w->heel, .toe = w->toe,
                           .own_tempo = w->own_tempo != 0, .bpm = w->bpm, .nano_bpm = s_state.tempo };
    ui_set_autowah(&shown);
}

static float wah_bpm(const wah_t *w)
{
    return w->own_tempo || s_state.tempo < UI_WAH_BPM_MIN ? (float)w->bpm : s_state.tempo;
}

// The next position: where the round is now, between heel and toe.
static void wah_step(void)
{
    int slot = s_sweep.slot;
    if (slot < 0) return;
    esp_timer_start_once(s_wah_timer, WAH_STEP_MS * 1000);
    if (s_tuner_on || s_expect_preset) return;   // nothing to hear, or the Nano is loading another preset
    const wah_t *w = &s_wah[slot];
    double period_us = (double)WAH_BEATS[w->rate] * 60e6 / (double)wah_bpm(w);
    double phase = fmod((double)(esp_timer_get_time() - s_sweep.start_us), period_us) / period_us;   // 0-1
    float x = w->shape == 2 ? (float)phase
            : w->shape == 1 ? 0.5f - 0.5f * cosf(2 * (float)M_PI * (float)phase)
            : (float)(phase < 0.5 ? 2 * phase : 2 - 2 * phase);
    float value = ((float)w->heel + ((float)w->toe - (float)w->heel) * x) / 100;
    send_param(slot, WAH_PARAM, value);
    if (slot == s_edit.slot && s_edit.known && ++s_sweep.steps % 4 == 0) {   // the wah's bar in the FX editor moves along
        s_edit.values[WAH_PARAM] = value;
        editor_show();
    }
}

// Starts or stops the movement: it runs while a wah with auto-wah is on (the first such slot).
static void wah_update(void)
{
    int slot = -1;
    for (int i = 0; i < NANO_FX_SLOTS && slot < 0 && nano_link_ready(); i++) {
        if (wah_mode(i) != UI_WAH_OFF && s_state.fx_on[i]) slot = i;
    }
    if (slot == s_sweep.slot) return;
    int before = s_sweep.slot;
    esp_timer_stop(s_wah_timer);
    s_sweep.slot = slot;
    if (slot >= 0) {
        const wah_t *w = &s_wah[slot];
        ESP_LOGI(TAG, "Auto-wah on %s: a round per %g beats at %.0f BPM (%s), %d-%d %%", NANO_FX_SLOT_NAMES[slot], (double)WAH_BEATS[w->rate],
                 (double)wah_bpm(w), w->own_tempo || s_state.tempo < UI_WAH_BPM_MIN ? "own" : "the Nano's", w->heel, w->toe);
        s_sweep.start_us = esp_timer_get_time();
        wah_step();
    } else {
        ESP_LOGI(TAG, "Auto-wah stopped");
        // Switched off in the editor while the wah stays on: it stays where it stopped, and the editor reads that.
        if (before == s_edit.slot && s_state.fx_on[before] && nano_link_ready()) editor_read_later(300);
    }
}

// A row of the FX editor's auto-wah settings changed (UI_WAH_*).
static void wah_edit(int row, int value)
{
    int slot = s_edit.slot;
    if (slot < 0 || !is_wah(s_state.fx_type[slot]) || s_wah_preset != s_state.current_preset) return;
    wah_t *w = &s_wah[slot];
    if (w->type != s_state.fx_type[slot]) {   // the first setting for this wah: from the defaults
        *w = WAH_DEFAULT;
        w->type = s_state.fx_type[slot];
    }
    switch (row) {
    case UI_WAH_MODE:
        w->mode = (uint8_t)(value < 0 ? 0 : value > UI_WAH_HELD ? UI_WAH_HELD : value);
        if (w->mode == UI_WAH_HELD && ab_used(slot)) {
            ui_show_message("Pre FX 1 has a second effect: holding its switch swaps A and B, so this wah moves while it is on.");
        } else if (w->mode == UI_WAH_HELD) {
            ui_show_message("Hold: in FX mode the wah is on and moves while you hold its footswitch.");
        }
        break;
    case UI_WAH_RATE: w->rate = (uint8_t)(value < 0 ? 0 : value >= UI_WAH_RATES ? UI_WAH_RATES - 1 : value); break;
    case UI_WAH_SHAPE: w->shape = (uint8_t)(value < 0 ? 0 : value >= UI_WAH_SHAPES ? UI_WAH_SHAPES - 1 : value); break;
    case UI_WAH_HEEL: w->heel = (uint8_t)(value < 0 ? 0 : value > 100 ? 100 : value); break;
    case UI_WAH_TOE: w->toe = (uint8_t)(value < 0 ? 0 : value > 100 ? 100 : value); break;
    case UI_WAH_TEMPO:
        w->own_tempo = value != 0;
        if (w->own_tempo && s_state.tempo >= UI_WAH_BPM_MIN && s_state.tempo <= UI_WAH_BPM_MAX) w->bpm = (uint16_t)lroundf(s_state.tempo);   // start from the Nano's
        break;
    case UI_WAH_BPM:   // moving the tempo makes it the own one
        w->own_tempo = 1;
        w->bpm = (uint16_t)(value < UI_WAH_BPM_MIN ? UI_WAH_BPM_MIN : value > UI_WAH_BPM_MAX ? UI_WAH_BPM_MAX : value);
        break;
    default: return;
    }
    wah_save();
    if (row == UI_WAH_RATE || row == UI_WAH_SHAPE) s_sweep.start_us = esp_timer_get_time();   // the new round starts at the heel
    show();          // the tile's note, the switches, and the movement starts or stops
    editor_show();   // the rows (tempo and BPM belong together)
}

// A footswitch that acts while it is held (auto-wah "Hold") went down or up: its wah on or off.
static void wah_hold(int number, bool down)
{
    int slot = number - 3;
    if (slot < 0 || slot >= NANO_FX_SLOTS || !s_state.fx_known || !s_state.fx_type[slot]) return;
    if (down) ui_flash_tile(number - 1);
    app_sync_later();
    set_fx(slot, down);
    show();
    schedule_refresh(300);
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

// The FX presets stored for a model (empty places if there are none).
static void fxp_read(uint32_t type, fxp_t list[FXP_COUNT])
{
    size_t size = sizeof(fxp_t) * FXP_COUNT;
    memset(list, 0, size);
    nvs_handle_t nvs;
    char key[16];
    fxp_key(type, key);
    if (!type || nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    if (nvs_get_blob(nvs, key, list, &size) != ESP_OK || size != sizeof(fxp_t) * FXP_COUNT) memset(list, 0, sizeof(fxp_t) * FXP_COUNT);
    nvs_close(nvs);
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
        fxp_read(type, s_fxp.list);
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

// A value run (an FX preset or a reverb setting going out in steps) is under way.
static bool fxp_sending(void)
{
    return s_fxp.sent < s_fxp.send_count;
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
    if (fxp_sending() && s_fxp.send_slot != slot) {   // (a reverb setting is still going out to another slot)
        ui_show_message("Still switching - try again in a moment.");
        return;
    }
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

static void editor_fxp_changed(uint32_t type);

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
    editor_fxp_changed(s_fxp.type);
    ui_show_message(name[0] ? "FX preset saved." : "FX preset deleted.");
    editor_show();
}

static void editor_open(int slot)
{
    if (slot < 0 || slot >= NANO_FX_SLOTS || !s_state.fx_known) return;
    s_edit.slot = slot;
    s_edit.known = false;
    s_edit.count = 0;
    s_edit.read_tries = 0;
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
    s_edit.read_tries = 0;
    editor_show();
    show();
    schedule_refresh(300);
    editor_read_later(PARAM_BUSY_US / 1000 + 50);   // once the Nano has loaded the model
}

// Forgets expression requests that got no reply in time, and a write or check that waited for one of them.
static void exp_expire(void)
{
    int64_t now = esp_timer_get_time();
    int gone = 0;
    while (gone < s_exp_asked_count && now - s_exp_asked[gone].at_us > EXP_REPLY_US) gone++;
    if (gone) {
        ESP_LOGW(TAG, "%d expression request%s without a reply", gone, gone == 1 ? "" : "s");
        s_exp_asked_count -= gone;
        memmove(s_exp_asked, s_exp_asked + gone, sizeof(s_exp_asked[0]) * (size_t)s_exp_asked_count);
    }
    if ((s_exp.write || s_exp.verify) && now - s_exp.pending_us > EXP_REPLY_US) {
        if (s_exp.verify) ui_show_message("Not confirmed: the Nano did not report the expression assignments.");
        s_exp.write = s_exp.verify = EXP_WRITE_NONE;
    }
}

static void mix_exp_request(const char *label, int purpose)
{
    uint8_t buf[8];
    exp_expire();
    if (!send(label, NANO_MSG_EXP_REQUEST, buf, nano_exp_request(s_state.current_preset, buf))) return;   // not on its way
    if (s_exp_asked_count == EXP_ASKED) {
        s_exp_asked_count--;
        memmove(s_exp_asked, s_exp_asked + 1, sizeof(s_exp_asked[0]) * (size_t)s_exp_asked_count);
    }
    s_exp_asked[s_exp_asked_count].preset = s_state.current_preset;
    s_exp_asked[s_exp_asked_count].purpose = purpose;
    s_exp_asked[s_exp_asked_count++].at_us = esp_timer_get_time();
}

// The mix positions of all presets from the store. Entries of firmware before 1.15 (one per preset) are taken over
// and - if `convert` - removed, also after a backup of such a firmware was written back. Returns how many such
// entries there are (store_compact() asks first, keeps a copy of the store, then converts).
static int mixes_load(bool convert)
{
    memset(&s_mixes, 0, sizeof(s_mixes));
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return 0;
    size_t size = sizeof(s_mixes);
    if (nvs_get_blob(nvs, MIX_ALL_KEY, &s_mixes, &size) != ESP_OK || size != sizeof(s_mixes)) memset(&s_mixes, 0, sizeof(s_mixes));
    int old = 0;
    char key[16];
    for (int preset = 1; preset <= NANO_PRESETS; preset++) {
        uint8_t stored[2];
        size = sizeof(stored);
        snprintf(key, sizeof(key), MIX_STORE_KEY, preset);
        if (nvs_get_blob(nvs, key, stored, &size) != ESP_OK || size != sizeof(stored)) continue;
        memcpy(s_mixes.pos[preset - 1], stored, 2);
        s_mixes.set[(preset - 1) / 8] |= (uint8_t)(1u << ((preset - 1) % 8));
        old++;
    }
    // The new entry first, and only when it is safely there the old ones go.
    if (old && convert && nvs_set_blob(nvs, MIX_ALL_KEY, &s_mixes, sizeof(s_mixes)) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
        for (int preset = 1; preset <= NANO_PRESETS; preset++) {
            snprintf(key, sizeof(key), MIX_STORE_KEY, preset);
            nvs_erase_key(nvs, key);
        }
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return old;
}

static void mix_store(void)
{
    int i = s_state.current_preset - 1;
    if (i < 0 || i >= NANO_PRESETS) return;
    s_mixes.pos[i][0] = (uint8_t)lroundf(s_mix.pos[0] * 255);
    s_mixes.pos[i][1] = (uint8_t)lroundf(s_mix.pos[1] * 255);
    s_mixes.set[i / 8] |= (uint8_t)(1u << (i % 8));
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    char old[16];   // an entry of before 1.15 for this preset (if the store could not be rewritten at the start) must
    snprintf(old, sizeof(old), MIX_STORE_KEY, s_state.current_preset);   // not win over the new values at the next start
    nvs_erase_key(nvs, old);
    if (nvs_set_blob(nvs, MIX_ALL_KEY, &s_mixes, sizeof(s_mixes)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
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
    int i = s_state.current_preset - 1;
    if (i >= 0 && i < NANO_PRESETS && (s_mixes.set[i / 8] >> (i % 8) & 1)) {
        s_mix.pos[0] = s_mixes.pos[i][0] / 255.0f;
        s_mix.pos[1] = s_mixes.pos[i][1] / 255.0f;
        s_mix.known = s_mix.own = true;
    }
    mix_exp_request("Expression request", EXP_ASK_READ);
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

// The reverb's tile swapped A / B while footswitch 8 is the mix switch: the new reverb brought its own mix along -
// the switch's position (Pos 1 / Pos 2) goes out again, so the tile and the sound agree. The reverb's type and its
// amount are two switches.
static void mix_after_swap(int slot)
{
    if (slot != s_mix.slot || rp_used() || reverb_b_used() || !s_mix.known || s_mix.active < 0) return;
    int param = mix_param();
    if (param < 0) return;
    uint8_t buf[16];
    char label[40];
    snprintf(label, sizeof(label), "Reverb mix Pos %d (%ld%%)", s_mix.active + 1, lroundf(s_mix.pos[s_mix.active] * 100));
    send(label, NANO_MSG_FX_VALUE, buf, nano_fx_param(s_mix.slot, param, s_mix.pos[s_mix.active], buf));
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
    s_exp.pending_us = esp_timer_get_time();
    mix_exp_request("Expression request", EXP_ASK_WRITE);
}

// The range of the pedal that was saved last (Calibrate), false if there is none.
static bool cal_stored(int32_t range[2], bool write)
{
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, write ? NVS_READWRITE : NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = 2 * sizeof(int32_t);
    bool ok = write ? nvs_set_blob(nvs, CAL_STORE_KEY, range, size) == ESP_OK && nvs_commit(nvs) == ESP_OK
                    : nvs_get_blob(nvs, CAL_STORE_KEY, range, &size) == ESP_OK && size == 2 * sizeof(int32_t) && range[1] > range[0];
    nvs_close(nvs);
    return ok;
}

static void cal_send(int min, int max)
{
    uint8_t buf[16];
    char label[48];
    snprintf(label, sizeof(label), "Pedal calibration %d - %d", min, max);
    send(label, NANO_MSG_EXP_CAL_SAVE, buf, nano_exp_calibration_save(min, max, buf));
    s_cal.reset = false;
}

// Expression dialog, Calibrate. arg 1: start - the Nano forgets its calibration and its positions are collected;
// 2: save the lowest and highest position seen; 0: stop - the range saved last is put back (if there is one), so
// the pedal is not left without a calibration.
static void exp_calibrate(int arg)
{
    if (arg == 1) {
        uint8_t none[1] = { 0 };   // the message has no content
        send("Reset the pedal calibration", NANO_MSG_EXP_CAL_RESET, none, 0);
        s_cal.reset = true;
        s_pedal_min = INT32_MAX;
        s_pedal_max = INT32_MIN;
        s_pedal_collect_from_us = esp_timer_get_time() + 400 * 1000;   // (what is on its way is the old calibration's)
        s_pedal_collect = s_cal.active = true;
        ui_set_pedal(s_pedal_value, 0, -1);
    } else if (arg == 2) {
        int min = s_pedal_min, max = s_pedal_max;
        if (!s_cal.active || max <= min) return;
        s_pedal_collect = s_cal.active = false;
        cal_send(min, max);
        int32_t range[2] = { min, max };
        cal_stored(range, true);
        ESP_LOGI(TAG, "Pedal calibrated: %d - %d", min, max);
        ui_show_message("Pedal calibration saved.");
    } else {
        s_pedal_collect = s_cal.active = false;
        if (!s_cal.reset) return;
        int32_t range[2];
        if (cal_stored(range, false)) {
            cal_send(range[0], range[1]);
            ui_show_message("Calibration as it was before.");
        } else {
            ui_show_message("The pedal is not calibrated now - calibrate it again.");
        }
    }
}

// The pedal moved (or the Nano reported where it is): shown while calibrating.
static void pedal_moved(void)
{
    int value = s_pedal_value;
    int64_t now = esp_timer_get_time();
    if (now - s_cal.logged_us > 500 * 1000) {   // (a moving pedal reports many positions)
        s_cal.logged_us = now;
        ESP_LOGI(TAG, "Pedal position %d", value);
    }
    if (s_cal.active) ui_set_pedal(value, s_pedal_min, s_pedal_max);
}

// The open expression dialog still waits for the assignments: ask again, a few times.
static void exp_dialog_retry(void)
{
    if (!s_exp.open || s_exp.shown) return;
    exp_expire();
    if (s_exp.write || s_exp.verify) {   // a save is being checked: its reply fills the dialog
        esp_timer_start_once(s_exp_retry_timer, 1000 * 1000);
        return;
    }
    if (s_exp.retries++ >= 3) {
        ui_show_message("The Nano does not report the expression assignments - close the dialog and try again.");
        return;
    }
    ESP_LOGW(TAG, "No expression assignments yet - asking again");
    mix_exp_request("Expression request (again)", EXP_ASK_READ);
    esp_timer_start_once(s_exp_retry_timer, 1500 * 1000);
}

// Expression dialog opened (1): read the assignments for it; closed (0).
static void exp_dialog(int open)
{
    s_exp.open = open != 0;
    esp_timer_stop(s_exp_retry_timer);
    if (!open) return;
    s_exp.shown = false;
    s_exp.retries = 0;
    mix_exp_request("Expression request", EXP_ASK_READ);
    request_settings();   // how the connector is set, for the switch at the top of the dialog
    esp_timer_start_once(s_exp_retry_timer, 1500 * 1000);
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
    s_exp.pending_us = esp_timer_get_time();
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
    if (s_exp.verify && s_exp.preset == s_state.current_preset) mix_exp_request("Expression check", EXP_ASK_VERIFY);
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
    ab_key(ab, false, key);
    uint8_t buf[FX_B_HEAD + 2 * NANO_MAX_PARAMS];
    esp_err_t err = ab->b.type ? nvs_set_blob(nvs, key, buf, fx_b_pack(&ab->b, buf)) : nvs_erase_key(nvs, key);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the second effect of %s", ab_name(ab));
    }
    nvs_close(nvs);
}

// Keeps the values of A for this preset (read while A was on), so a later swap needs no reading.
static void ab_store_a(ab_t *ab)
{
    char key[16];
    nvs_handle_t nvs;
    if (!ab->preset || nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    ab_key(ab, true, key);
    fx_a_t a = { .type = ab->a_type, .count = ab->a_count };
    for (int i = 0; i < ab->a_count && i < NANO_MAX_PARAMS; i++) {
        float v = ab->a_values[i] < 0 ? 0 : ab->a_values[i] > 1 ? 1 : ab->a_values[i];
        a.values[i] = (uint16_t)lroundf(v * 65535);
    }
    uint8_t buf[sizeof(fx_a_t)];
    esp_err_t err = ab->a_count && ab->b.type ? nvs_set_blob(nvs, key, buf, fx_a_pack(&a, buf)) : nvs_erase_key(nvs, key);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the A values of %s", ab_name(ab));
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
        ab->slot = i;
        ab->reverb = i > 0 && i == s_mix.slot;   // (the state reply set s_mix.slot just before: the preset's reverb)
        char key[16];
        ab_key(ab, false, key);
        uint8_t buf[sizeof(fx_b_t)];
        size_t size = sizeof(buf);
        if (!open || nvs_get_blob(nvs, key, buf, &size) != ESP_OK || !fx_b_unpack(buf, size, &ab->b) || !nano_fx_model(ab->b.type)) {
            memset(&ab->b, 0, sizeof(ab->b));
        }
        if (ab->b.type) ESP_LOGI(TAG, "%s B of preset %d: %s%s", ab_name(ab), preset, nano_fx_name(ab->b.type),
                                 ab->b.tap == UI_AB_TAP ? " (a tap swaps)" : ab->b.tap == UI_AB_PRESS ? " (a press swaps)" : "");
        fx_a_t a;
        size = sizeof(buf);
        int slot = i;
        ab_key(ab, true, key);
        if (open && ab->b.type && nvs_get_blob(nvs, key, buf, &size) == ESP_OK && fx_a_unpack(buf, size, &a) &&
            a.type == s_state.fx_type[slot]) {
            ab->a_type = a.type;
            ab->a_count = a.count;
            for (int n = 0; n < a.count; n++) ab->a_values[n] = a.values[n] / 65535.0f;
        }
    }
    if (open) nvs_close(nvs);
}

static int ab_slot(const ab_t *ab);
static void ab_capture_editor(void);

// A preset is about to be loaded from the Nano's memory - another one, or the one that is on once more. Edits of
// a second effect are kept; a swap under way stops; and nothing of "B is running" outlives it: the preset's own
// effect (A) is in the slot again. The next state reply reads the second effects afresh (ab_load), also for the
// same preset number. Without this, "B" stayed marked as running over a reload, and the state reply naming A's
// model was taken for a new choice of B - which overwrote the stored second effect.
static void ab_leave_preset(void)
{
    ab_capture_editor();
    esp_timer_stop(s_ab_timer);
    esp_timer_stop(s_fxp_timer);   // a value run (FX preset, reverb setting) belongs to the preset that was on
    esp_timer_stop(s_rp_timer);
    s_fxp.sent = s_fxp.send_count = 0;
    for (int i = 0; i < AB_COUNT; i++) {
        ab_t *ab = &s_ab[i];
        ab->phase = AB_IDLE;
        ab->cache_only = ab->restore_off = ab->open_editor = false;
        ab->preset = 0;   // nothing is stored or compared with the Nano's state until ab_load()
    }
    s_last_preset = 0;
}

static int ab_slot(const ab_t *ab)
{
    return ab->slot;
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
    // A and B of the same model (two settings of one reverb): the model stays where it is and only the values go
    // out - at once, and the sound does not break off. Otherwise the model first; the Nano needs a moment for it.
    bool same_model = type == s_state.fx_type[ab->slot];
    if (!same_model) {
        uint8_t buf[16];
        char label[64];
        snprintf(label, sizeof(label), "%s %c: %s", ab_name(ab), ab->target ? 'B' : 'A', nano_fx_name(type));
        send(label, NANO_MSG_FX_TYPE, buf, nano_fx_model_select(ab->slot, type, buf));
        s_state.fx_type[ab->slot] = type;
        ab->swapped_us = esp_timer_get_time();
    }
    ab->active = ab->target;
    ab->phase = AB_LOADING;
    ab->sent = 0;
    ab_step_later(same_model ? 1 : 400);
    show();
}

// The read of a swap (or of the values for later) came to nothing: nothing is switched.
static void ab_read_failed(ab_t *ab, const char *message)
{
    ab->phase = AB_IDLE;
    if (!ab->cache_only) ui_show_message(message);
    ab->cache_only = false;
    if (ab->restore_off) {
        ab->restore_off = false;
        if (s_state.fx_on[ab->slot]) toggle_fx(ab->slot);
    }
}

static void ab_read(ab_t *ab)
{
    if (!param_read(READ_AB, ab->slot)) {   // another read is open or the Nano is busy
        if (ab->cache_only || ++ab->read_tries > 12) {   // (a read for later: another time)
            ab_read_failed(ab, "Not switched: the Nano is busy - try again.");
        } else {
            ab->phase = AB_TURNING_ON;
            ab_step_later(150);
        }
        return;
    }
    ab->phase = AB_READING;
    ab_step_later(PARAM_READ_US / 1000 + 100);   // timeout
}

// Reads the values of A while it is on (preset loaded, or switched on), so a later swap need not switch it on.
static void ab_cache(int slot, int delay_ms)
{
    if (ab_running() || slot < 0 || !s_state.fx_on[slot] || !s_state.fx_type[slot]) return;
    for (int i = 0; i < AB_COUNT; i++) {
        ab_t *ab = &s_ab[i];
        if (!ab->b.type || ab->active != 0 || ab_slot(ab) != slot) continue;
        ab->slot = slot;
        ab->cache_only = true;
        ab->read_tries = 0;
        ab->phase = AB_TURNING_ON;
        ab_step_later(delay_ms);
        return;
    }
}

// The switch of an FX slot (held, or tapped if set so; footswitch 8 for the reverb): swap A <-> B. The effect stays
// on or off.
static void ab_swap(int which)
{
    if (which < 0 || which >= AB_COUNT) return;
    ab_t *ab = &s_ab[which];
    if (!ab->b.type || ab->preset != s_state.current_preset) return;
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
    int slot = ab->slot;
    if (!s_state.fx_type[slot]) {
        ui_show_message("This slot is empty in this preset.");
        return;
    }
    if (fxp_sending() && s_fxp.send_slot == slot) {   // its values are still going out: a read now would see half of them
        ui_show_message("Still switching - try again in a moment.");
        return;
    }
    ab->target = ab->active ? 0 : 1;
    ab->cache_only = false;
    ab->read_tries = 0;
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

// The values an A/B asked for arrived (kept, then the swap goes on).
static void ab_values_read(int slot, const float *values, int count)
{
    ab_t *ab = ab_running();
    if (!ab || ab->phase != AB_READING || ab->slot != slot) return;   // (a read that was given up meanwhile)
    esp_timer_stop(s_ab_timer);
    if (count <= 0) {
        ab_read_failed(ab, "Not switched: the Nano sent no values.");
        return;
    }
    if (ab->active == 0) {
        if (!ab->a_type) ab->a_type = s_state.fx_type[ab->slot];   // the preset's effect, kept from now on
        bool same = ab->a_count == count;   // stored only when changed (spares the flash)
        for (int i = 0; same && i < count; i++) same = lroundf(ab->a_values[i] * 65535) == lroundf(values[i] * 65535);
        ab->a_count = (uint8_t)count;
        memcpy(ab->a_values, values, sizeof(float) * (size_t)count);
        if (!same) ab_store_a(ab);
    } else {   // values only: a state reply can still name the previous model
        bool same = ab->b.count == count;   // (as for A: stored only when changed)
        for (int i = 0; same && i < count; i++) same = value_u16(ab->b.values[i]) == value_u16(values[i]);
        ab->b.count = (uint8_t)count;
        memcpy(ab->b.values, values, sizeof(float) * (size_t)count);
        if (!same) ab_store(ab);
    }
    if (ab->cache_only) {
        ab->cache_only = false;
        ab->phase = AB_IDLE;
        ESP_LOGI(TAG, "%s A: %d values kept", ab_name(ab), count);
        return;
    }
    if (ab->restore_off) {   // switched on only for reading
        ab->restore_off = false;
        if (s_state.fx_on[ab->slot]) toggle_fx(ab->slot);
    }
    ab_load_target(ab);
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
        ab_read_failed(ab, "Not switched: the Nano did not send the effect's values.");
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
        ESP_LOGI(TAG, "%s %c active (%d values)", ab_name(ab), ab->active ? 'B' : 'A', count);
        mix_after_swap(ab->slot);
        rp_match(ab->slot, values, count);
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

// From the reverb dialog, the 2ND list of Pre FX 1 or an FX editor: arg = w << 24 | model of B (0 = none).
// w: 0 = reverb, footswitch 8 swaps A / B; 2 = reverb, footswitch 8 is the mix switch or the two settings - B stays
// the reverb tile's ("parked" for footswitch 8); 1 = Pre FX 1; 8 + n = FX slot n (for the reverb's slot: footswitch 8
// keeps what it does, and a new B is parked - the tile swaps it, the reverb switch stays as it was).
static void ab_set_b(int arg)
{
    int w = arg >> 24;
    uint32_t type = (uint32_t)arg & 0xFFFFFF;
    int which = w == 0 || w == 2 ? s_mix.slot : w == 1 ? 0 : w - 8;
    if (which < 0 || which >= AB_COUNT || (type && !nano_fx_model(type))) return;
    ab_t *ab = &s_ab[which];
    if (ab->preset != s_state.current_preset) return;
    bool parked = type && (w == 2 || (w >= 8 && which == s_mix.slot && (ab->b.parked || !ab->b.type)));
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
    ESP_LOGI(TAG, "%s B: %s%s", ab_name(ab), type ? nano_fx_name(type) : "none", parked ? " (footswitch 8: mix / settings, the tile swaps)" : "");
    if (ab->active == 1 && changed) {   // B is running: show the new choice, or go back to A (parking leaves it as it is)
        ab->target = type ? 1 : 0;
        if (ab->target || ab->a_type) ab_load_target(ab);
        else ab->active = 0;
    }
    show();
    if (s_edit.slot == ab->slot) editor_show();
}

// The FX editor's choice of what the slot's switch does: a tap swaps A / B and holding switches on / off, or the
// other way round.
static void ab_set_tap(int slot, int mode)
{
    static const char *const SAID[] = { "Tap: on / off  -  hold: A / B.", "Tap: A / B  -  hold: on / off.", "Press: A / B at once (no on / off)." };
    if (slot < 0 || slot >= AB_COUNT || !s_ab[slot].b.type || s_ab[slot].preset != s_state.current_preset) return;
    if (mode < UI_AB_HOLD || mode > UI_AB_PRESS) return;
    s_ab[slot].b.tap = (uint8_t)mode;
    ab_store(&s_ab[slot]);
    ESP_LOGI(TAG, "%s: %s", ab_name(&s_ab[slot]), SAID[mode]);
    ui_show_message(SAID[mode]);
    show();
    if (s_edit.slot == slot) editor_show();
}

// From the reverb dialog: edit reverb B in the FX editor (B is loaded first).
static void ab_edit_b(int which)
{
    which = s_mix.slot;   // (the dialog is the reverb's)
    if (which < 0) return;
    ab_t *ab = &s_ab[which];
    if (!ab->b.type) return;
    if (ab->active == 1) editor_open(ab->slot);
    else {
        ab->open_editor = true;
        ab_swap(which);
    }
}

// ---- reverb switch: two settings of the reverb (see s_rp) ----

// The place with the settings of the reverb that is in the slot, -1 = it has none.
static int rp_pair(void)
{
    if (s_rp_preset != s_state.current_preset || s_mix.slot < 0) return -1;
    uint32_t type = s_state.fx_type[s_mix.slot];
    for (int p = 0; type && p < RP_PAIRS; p++) {
        if (s_rp[p].type == type) return p;
    }
    return -1;
}

static bool rp_set(int n)
{
    int p = rp_pair();
    return p >= 0 && s_rp[p].count[n] > 0;
}

// The reverb switch changes between the two settings: chosen in the reverb dialog, and both are set for the reverb
// that is in the slot.
static bool rp_used(void)
{
    int p = rp_pair();
    return p >= 0 && s_rp[p].used && s_rp[p].count[0] > 0 && s_rp[p].count[1] > 0;
}

// In the store: the head, then the values that are set. (The first 1.14 build stored both rows in full.)
static size_t rp_pack(const rp_t *rp, uint8_t buf[sizeof(rp_t)])
{
    size_t c0 = rp->count[0], c1 = rp->count[1];
    memcpy(buf, rp, RP_HEAD);
    memcpy(buf + RP_HEAD, rp->values[0], 2 * c0);
    memcpy(buf + RP_HEAD + 2 * c0, rp->values[1], 2 * c1);
    return RP_HEAD + 2 * (c0 + c1);
}

static bool rp_unpack(const uint8_t *buf, size_t size, rp_t *rp)
{
    memset(rp, 0, sizeof(*rp));
    if (size < RP_HEAD) return false;
    memcpy(rp, buf, RP_HEAD);
    size_t c0 = rp->count[0], c1 = rp->count[1];
    if (!rp->type || c0 > NANO_MAX_PARAMS || c1 > NANO_MAX_PARAMS) {
        memset(rp, 0, sizeof(*rp));
    } else if (size == sizeof(rp_t)) {
        memcpy(rp, buf, sizeof(rp_t));
    } else if (size == RP_HEAD + 2 * (c0 + c1)) {
        memcpy(rp->values[0], buf + RP_HEAD, 2 * c0);
        memcpy(rp->values[1], buf + RP_HEAD + 2 * c0, 2 * c1);
    } else {
        memset(rp, 0, sizeof(*rp));
    }
    rp->name[0][sizeof(rp->name[0]) - 1] = rp->name[1][sizeof(rp->name[1]) - 1] = 0;
    return rp->type != 0;
}

static void rp_load(int preset)
{
    memset(s_rp, 0, sizeof(s_rp));
    memset(s_rp_active, 0, sizeof(s_rp_active));
    s_rp_preset = preset;
    s_rp_capture = -1;
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    for (int p = 0; p < RP_PAIRS; p++) {
        rp_t *rp = &s_rp[p];
        uint8_t buf[sizeof(rp_t)];
        char key[16];
        snprintf(key, sizeof(key), RP_STORE_KEYS[p], preset);
        size_t size = sizeof(buf);
        if (nvs_get_blob(nvs, key, buf, &size) != ESP_OK) continue;
        if (rp_unpack(buf, size, rp)) {
            ESP_LOGI(TAG, "Reverb settings of preset %d for %s: \"%s\" / \"%s\"%s", preset, nano_fx_name(rp->type), rp->name[0], rp->name[1],
                     rp->used ? " (on the reverb switch)" : "");
        }
    }
    nvs_close(nvs);
}

static void rp_store(int p)
{
    const rp_t *rp = &s_rp[p];
    nvs_handle_t nvs;
    char key[16];
    uint8_t buf[sizeof(rp_t)];
    snprintf(key, sizeof(key), RP_STORE_KEYS[p], s_rp_preset);
    if (!s_rp_preset || nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    esp_err_t err = !rp->count[0] && !rp->count[1] ? nvs_erase_key(nvs, key) : nvs_set_blob(nvs, key, buf, rp_pack(rp, buf));
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) ui_show_message("Could not store the reverb settings (memory full?).");
    nvs_close(nvs);
}

// The place for the settings of the reverb in the slot. A new one takes an empty place, or that of a reverb this
// preset has no more (neither its own nor the second one of the reverb's tile).
static int rp_pair_make(void)
{
    int slot = s_mix.slot;
    if (slot < 0 || !s_state.fx_type[slot]) return -1;
    if (s_rp_preset != s_state.current_preset) rp_load(s_state.current_preset);
    int p = rp_pair();
    if (p >= 0) return p;
    const ab_t *ab = &s_ab[slot];
    bool ab_known = ab->preset == s_state.current_preset;
    for (int i = 0; i < RP_PAIRS && p < 0; i++) {
        if (!s_rp[i].type) p = i;
    }
    for (int i = 0; i < RP_PAIRS && p < 0; i++) {
        if (!ab_known || (s_rp[i].type != ab->a_type && s_rp[i].type != ab->b.type)) p = i;
    }
    if (p < 0) p = 0;
    bool used = false;   // what the switch does is the preset's: the new place takes it over
    for (int i = 0; i < RP_PAIRS; i++) used |= i != p && s_rp[i].type && s_rp[i].used;
    memset(&s_rp[p], 0, sizeof(s_rp[p]));
    s_rp[p].type = s_state.fx_type[slot];
    s_rp[p].used = used;
    s_rp_active[p] = 0;
    return p;
}

// Values of the reverb in the slot, just read or sent: which of its two settings is that? The tile and the switch
// go by it, so a preset stored with setting 2, or a reverb set by hand, is shown as what it is.
static void rp_match(int slot, const float *values, int count)
{
    int p = rp_pair();
    if (slot != s_mix.slot || p < 0 || count <= 0) return;
    const rp_t *rp = &s_rp[p];
    uint8_t found = RP_NEITHER;
    for (int n = 1; n >= 0; n--) {   // (both the same: setting 1)
        bool same = rp->count[n] == count;
        for (int i = 0; same && i < count; i++) same = fabsf(values[i] - rp->values[n][i] / 65535.0f) < RP_MATCH;
        if (same) found = (uint8_t)n;
    }
    if (found == s_rp_active[p]) return;
    ESP_LOGI(TAG, "Reverb: %s", found == RP_NEITHER ? "set to neither of its two settings" : rp->name[found]);
    s_rp_active[p] = found;
    show();
}

// After a preset change, or when the reverb comes on: read its values, to know which setting is on.
static void rp_check_later(int ms)
{
    s_rp_check_tries = 0;
    esp_timer_stop(s_rp_timer);
    esp_timer_start_once(s_rp_timer, (uint64_t)ms * 1000);
}

static void rp_check(void)
{
    int slot = s_mix.slot;
    if (!rp_used() || !s_state.fx_on[slot]) return;
    if (param_read(READ_RP, slot)) return;   // (its reply goes to rp_match, as every reply for the reverb does)
    if (++s_rp_check_tries <= 10) {          // another read is open, or the Nano is busy with the new preset
        esp_timer_stop(s_rp_timer);
        esp_timer_start_once(s_rp_timer, 300 * 1000);
    }
}

// The reverb switch: the other setting - all its values go out, the reverb comes on if it was off.
static void rp_toggle(void)
{
    int slot = s_mix.slot;
    int p = rp_pair();
    if (!rp_used()) return;
    const rp_t *rp = &s_rp[p];
    const ab_t *running = ab_running();
    if ((running && !running->cache_only) || (fxp_sending() && s_fxp.send_slot != slot)) {
        ui_show_message("Still switching - try again in a moment.");
        return;
    }
    if (!s_state.fx_on[slot]) toggle_fx(slot);
    int next = s_rp_active[p] == 0 ? 1 : 0;   // (from neither: setting 1)
    esp_timer_stop(s_rp_timer);   // (what is on is known from here)
    esp_timer_stop(s_fxp_timer);
    s_fxp.send_slot = slot;
    s_fxp.send_count = rp->count[next];
    s_fxp.sent = 0;
    for (int i = 0; i < rp->count[next]; i++) s_fxp.send_values[i] = rp->values[next][i] / 65535.0f;
    s_rp_active[p] = (uint8_t)next;
    ESP_LOGI(TAG, "Reverb setting %d \"%s\" (%s, %d values)", next + 1, rp->name[next], nano_fx_name(rp->type), rp->count[next]);
    fxp_send_step();
    app_sync_later();
    show();
    if (slot == s_edit.slot) {   // the open FX editor shows the reverb: its values are others now
        s_edit.known = false;
        s_fxp.active = -1;
        editor_show();
        editor_read_later(400);
    }
    schedule_refresh(600);
}

// Reverb dialog, "Set from the reverb": setting n (0 / 1) becomes what the reverb is set to right now.
static void rp_capture(int n)
{
    int slot = s_mix.slot;
    if (n < 0 || n > 1 || slot < 0) return;
    if (!s_state.fx_on[slot]) {
        ui_show_message("Switch the reverb on first - its values are read only while it is on.");
        return;
    }
    esp_timer_stop(s_rp_timer);
    if (!param_read(READ_RP, slot)) {
        ui_show_message("The Nano is busy - tap again in a moment.");
        return;
    }
    s_rp_capture = n;
}

// The values the capture asked for arrived.
static void rp_values(int slot, const float *values, int count)
{
    int n = s_rp_capture;
    s_rp_capture = -1;
    if (n < 0 || slot != s_mix.slot) return;
    if (count <= 0) {
        ui_show_message("Not set: the Nano sent no values - tap again.");
        return;
    }
    int p = rp_pair_make();
    if (p < 0) return;
    rp_t *rp = &s_rp[p];
    rp->count[n] = (uint8_t)count;
    for (int i = 0; i < count; i++) rp->values[n][i] = value_u16(values[i]);
    if (!rp->name[n][0]) strlcpy(rp->name[n], n ? "Setting 2" : "Setting 1", sizeof(rp->name[n]));
    rp_store(p);
    s_rp_active[p] = (uint8_t)n;
    ESP_LOGI(TAG, "Reverb setting %d \"%s\": %d values of %s kept", n + 1, rp->name[n], count, nano_fx_name(rp->type));
    char text[64];
    snprintf(text, sizeof(text), "\"%s\" set from the reverb.", rp->name[n]);
    ui_show_message(text);
    show();
}

// Text from the keyboard: "<1 / 2><name>" - the name of a setting.
static void rp_name(const char *text)
{
    int n = text[0] - '1';
    if (n < 0 || n > 1) return;
    int p = rp_pair_make();
    if (p < 0) return;
    rp_t *rp = &s_rp[p];
    const char *name = text + 1;
    while (*name == ' ') name++;
    if (*name) strlcpy(rp->name[n], name, sizeof(rp->name[n]));
    else strlcpy(rp->name[n], n ? "Setting 2" : "Setting 1", sizeof(rp->name[n]));
    if (rp->count[0] || rp->count[1]) rp_store(p);   // (a name alone is kept once a setting exists)
    show();
}

// Reverb dialog saved: the switch changes between the two settings (or not: the tab in front was another one).
// What the switch does is the preset's, so it goes for both of its reverbs.
static void rp_use(bool use)
{
    if (s_rp_preset != s_state.current_preset) rp_load(s_state.current_preset);
    bool changed = false;
    for (int p = 0; p < RP_PAIRS; p++) {
        if (!s_rp[p].type || (s_rp[p].used != 0) == use) continue;
        s_rp[p].used = use;
        rp_store(p);
        changed = true;
    }
    if (!changed) return;
    ESP_LOGI(TAG, "Reverb switch: %s", use ? "the two settings" : "not the two settings");
    show();
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
    if (setlist_used()) {   // the arrows turn the set list's pages
        int pages = setlist_pages();
        s_setlist.page = (s_setlist.page + pages + (step < 0 ? -1 : 1)) % pages;
        show();
        return;
    }
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
    static int64_t asked_us = -100 * 1000 * 1000;
    int64_t now = esp_timer_get_time();
    if (now - asked_us < 8 * 1000 * 1000) return;   // asked a moment ago: its (long) reply is on the way
    asked_us = now;
    uint8_t buf[16];
    send("Library request", NANO_MSG_LIBRARY_REQUEST, buf, nano_library_request(buf));
}

static void library_timer_cb(void *arg)
{
    command('(', 0);
}

// The library is a long reply (220 captures: three seconds in which the Nano answers nothing else). It is read by
// itself only in a quiet moment - not while footswitches, the screen or MIDI are in use - or when its list is opened.
#define LIBRARY_QUIET_US (8 * 1000 * 1000)
static int64_t s_last_action_us;

static void library_when_quiet(void)
{
    if (!nano_link_ready() || (s_library && !s_names_due)) return;
    if (esp_timer_get_time() - s_last_action_us < LIBRARY_QUIET_US) {
        esp_timer_stop(s_library_timer);
        esp_timer_start_once(s_library_timer, 3000 * 1000);
        return;
    }
    if (s_names_due) {   // after a reconnect: the preset names once more (they may have changed meanwhile)
        s_names_due = false;
        s_incomplete_retries = 0;
        request_full_state();
        if (!s_library) esp_timer_start_once(s_library_timer, 6000 * 1000);
        return;
    }
    request_library();
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
    if (on && !phone_midi_connected() && !app_link_connected()) {
        ui_show_message("Looper mode - no phone yet: in Loopy Pro open the menu > Bluetooth Devices.");
    } else if (on && !phone_midi_connected()) {
        ui_show_message("Looper mode - the footswitches go to the Nano Cortex Editor.");
    }
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
    bool app = app_link_connected();   // the editor takes the press as well
    if (app) {
        const uint8_t payload[2] = { 0x08, (uint8_t)number };
        app_link_send(APP_MSG_SWITCH, payload, sizeof(payload));
    }
    ESP_LOGI(TAG, "Looper switch %d: CC %d %s%s", number, LOOPER_CC(number), sent ? "sent" : "NOT sent", app ? ", told the editor" : "");
    s_looper_sent = sent || app ? LOOPER_CC(number) : -LOOPER_CC(number);
    if (nano_link_ready()) show();   // tile 1 says what went out
    if (!phone_midi_connected() && !app) ui_show_message("No phone connected - in Loopy Pro open the menu > Bluetooth Devices.");
    return true;
}

// Footswitch 1: the next mode of the ones it changes between (s_switch1). Holding footswitch 2 still changes between
// scenes and presets.
static void mode_switch(void)
{
    bool fx = s_fx_mode, scenes = !fx && s_scene_mode;
    bool to_fx;
    switch (s_switch1) {
    case UI_SWITCH1_FX_SCENES:      // FX <-> scenes (from presets: scenes)
        to_fx = scenes;
        if (!to_fx) {
            scene_mode(1);
            return;
        }
        break;
    case UI_SWITCH1_PRESETS_SCENES: // presets <-> scenes (from FX: presets)
        scene_mode(!fx && !scenes);
        return;
    case UI_SWITCH1_ALL:            // presets -> scenes -> FX -> presets
        if (fx || !scenes) {
            scene_mode(!fx);
            return;
        }
        to_fx = true;
        break;
    default:                        // FX <-> presets or scenes (whichever was chosen last)
        to_fx = !fx;
        break;
    }
    s_fx_mode = to_fx;
    if (!s_fx_mode) follow_preset_with_bank(s_state.current_preset);
    ESP_LOGI(TAG, "%s mode", s_fx_mode ? "FX" : s_scene_mode ? "Scene" : "Preset");
    show();
}

static void handle_switch(int number)
{
    if (looper_switch(number)) return;
    ui_flash_tile(number - 1);
    if (number == 1) {
        mode_switch();
    } else if (number == 2) {
        set_tuner(!s_tuner_on);
    } else if (s_fx_mode) {
        if (number <= 7 && ab_used(number - 3) && (s_ab[number - 3].b.tap == UI_AB_TAP || s_ab[number - 3].b.tap == UI_AB_PRESS)) {
            ab_swap(number - 3);   // a tap (or the press itself) swaps A / B
        }
        else if (number <= 7) toggle_fx(number - 3);
        else if (rp_used()) rp_toggle();
        else if (reverb_b_used()) ab_swap(s_mix.slot);
        else toggle_mix();
    } else if (number >= 3 && number <= 8 && s_scene_mode) {
        scene_recall(number - 3);
    } else if (number >= 3 && number <= 8 && setlist_used()) {
        int song = s_setlist.page * BANK_SLOTS + number - 3;
        if (song < s_setlist.count) setlist_choose(song);
        else ui_show_message("No song here - tap PRESET / SET LIST at the top to edit the set list.");
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

// M-VAVE Chocolate (4 switches, no display). Out of the box it sends PC 0-3 on A-D and moves in groups of four with
// A+B (down) / C+D (up). Here a group is footswitches 1-4, the next one 5-8 (PC n = footswitch n % 8 + 1). In its CC
// mode A-D send CC 20-23 (127 press, 0 release), A+B CC 24 and C+D CC 25: those choose footswitches 1-4 / 5-8.
// True when the message was the Chocolate's.
static bool chocolate_midi(uint8_t type, uint8_t d1, uint8_t d2)
{
    static int group;   // CC mode
    int number;
    if (type == 0xC0) {
        number = d1 % 8 + 1;
    } else if (type == 0xB0 && (d1 == 24 || d1 == 25)) {
        if (d2 >= 64) group = d1 - 24;
        return true;
    } else if (type == 0xB0 && d1 >= 20 && d1 <= 23) {
        if (d2 < 64) return true;
        number = group * 4 + d1 - 19;
    } else {
        return false;
    }
    number = layout_switch(number);   // field 1-8 of the layout
    if (number && !looper_switch(number) && nano_link_ready()) handle_switch(number);
    return true;
}

// As the Nano's own MIDI over USB: PC 0-63 = presets, CC 37-41 = FX 1-5 (127 on, 0 off), CC 1 = expression.
// In addition CC 50-57 (value 64-127) press footswitches 1-8, CC 59 is the looper mode and CC 60 the scene mode
// (64-127 on, below off). All channels. The Chocolate has its own mapping (chocolate_midi).
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
    s_last_action_us = esp_timer_get_time();
    if (midi_ble_chocolate() && chocolate_midi(type, d1, d2)) return;
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
        int number = layout_switch(d1 - 49);   // field 1-8 of the layout
        if (number) handle_switch(number);
    } else if (type == 0xB0 && d1 == 58 && d2 >= 64) {   // Pre FX 1 A <-> B
        if (ab_used(0)) ab_swap(0);
    } else if (type == 0xB0 && d1 >= 61 && d1 <= 65) {   // FX slot 1-5: B (64-127) or A (0-63)
        int slot = d1 - 61;
        if (ab_used(slot) && (s_ab[slot].active != 0) != (d2 >= 64)) ab_swap(slot);
    } else if (type == 0xB0 && (d1 == 66 || d1 == 67) && d2 >= 64 && setlist_used()) {   // next / previous song
        setlist_step(d1 == 66 ? 1 : -1);
    } else if (type == 0xB0 && d1 == 68) {   // set list on (64-127) / off
        setlist_set_on(d2 >= 64);
    }
}

static void system_page(void);

static void handle_command(char c, int arg)
{
    if (c == ':') {                                    // the pedal's position (see on_message)
        __atomic_store_n(&s_pedal_event, 0, __ATOMIC_SEQ_CST);
        pedal_moved();
        return;
    }
#ifdef NANO_WDTEST
    if (c == '}') {
        ESP_LOGW(TAG, "Test: hanging the %s on purpose", arg ? "screen" : "app task");
        if (arg) ui_test_hang();
        else for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
#endif
    if (c == ')') { system_page(); return; }           // (also without the Nano: that is when it is looked at)
    if (c == 'h') { print_help(); return; }
    if (c == 'l') { print_presets(); return; }
    if (c == 'D' || c == 'Z') { footswitch_learn(c, arg); return; }   // works without the Nano
    if (c == ']') { layout_set(arg); return; }        // tile layout, also without the Nano
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
    if (c == ',') {                                    // the Nano has not come for some seconds
#ifndef NANO_WEB
        if (!nano_link_ready()) {
            ESP_LOGW(TAG, "Nano not found yet - it may need pairing (EXIT + CAPTURE on the Nano)");
            ui_splash_status("Not found yet - press EXIT + CAPTURE on the Nano", NULL);
            if (s_state.names_loaded) ui_show_message("Nano not found - press EXIT + CAPTURE on it to pair again.");
        }
#endif
        return;
    }
    if (c == 'q') {                                    // MIDI gate timer: the Nano did not come
        nano_link_log_status();
#ifndef NANO_WEB   // in the browser the start screen keeps asking to click CONNECT
        ui_splash_status("No Nano yet - switch it on or press EXIT + CAPTURE on it (tap to continue)", NULL);
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
    if (strchr("wnpgkabcdemtxCI$OMz", c)) s_last_action_us = esp_timer_get_time();   // somebody is playing or setting up
    switch (c) {
    case 'n':
        if (setlist_used()) setlist_step(1);
        else select_preset(s_state.current_preset + 1);
        break;
    case 'p':
        if (setlist_used()) setlist_step(-1);
        else select_preset(s_state.current_preset - 1);
        break;
    case 'g':
        if (arg >= 1 && arg <= NANO_PRESETS) select_preset(arg);
        else ESP_LOGW(TAG, "Presets are 1-64");
        break;
    case 's': request_current_state(); break;
    case 'a': case 'b': case 'c': case 'd': case 'e': toggle_fx(c - 'a'); break;
    case 'm': handle_switch(1); break;
    case 't': handle_switch(2); break;
    case 'x':   // as footswitch 8 in FX mode
        if (rp_used()) rp_toggle();
        else if (reverb_b_used()) ab_swap(s_mix.slot);
        else toggle_mix();
        break;
    case '-':   // reverb dialog: 1 / 2 = set that setting from the reverb, 16 / 17 = the switch uses the settings: no / yes
        if (arg == 1 || arg == 2) rp_capture(arg - 1);
        else if (arg == 16 || arg == 17) rp_use(arg == 17);
        else if (arg == 48) rp_check();
#ifdef NANO_RPTEST
        else if (arg == 32 && rp_pair() >= 0) {   // forget both settings of the reverb in the slot
            int p = rp_pair();
            memset(&s_rp[p], 0, sizeof(s_rp[p]));
            rp_store(p);
            ESP_LOGI(TAG, "Reverb settings of preset %d removed (place %d)", s_rp_preset, p + 1);
            show();
        }
#endif
        break;
    case 'w': {
        int number = arg & 0xFF;
        if (number < 1 || number > FOOTSWITCH_COUNT) break;
        if (arg & (UI_SWITCH_DOWN | UI_SWITCH_UP)) {   // a wah set to "Hold": on while its switch is down
            if (!s_looper && s_fx_mode) wah_hold(number, (arg & UI_SWITCH_DOWN) != 0);
            else if (arg & UI_SWITCH_DOWN) handle_switch(number);   // the mode changed meanwhile: an ordinary press
        } else if ((arg & UI_SWITCH_HOLD) && s_fx_mode && number >= 3 && number <= 7 && ab_used(number - 3)) {
            ui_flash_tile(number - 1);   // held: A <-> B - or on / off, if a tap swaps
            if (s_ab[number - 3].b.tap == UI_AB_TAP) toggle_fx(number - 3);
            else if (s_ab[number - 3].b.tap != UI_AB_PRESS) ab_swap(number - 3);
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
    case '{': state_overdue(); break;

    case '(': library_when_quiet(); break;
    case '_': pedal_view_set(arg != 0); break;
    case '[': ab_swap(arg); break;   // FX editor, A | B: the other one
    case '>': ab_set_tap(arg >> 8, arg & 0xFF); break;
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
        if (s_app_exp_changed) mix_exp_request("Expression request", EXP_ASK_READ);   // is the reverb (still) on the pedal?
        s_app_names_changed = s_app_exp_changed = false;
        break;
    case 'f':   // tell the app: preset "changed" -> it reads the current state again
        if (app_link_connected()) {
            uint8_t buf[64];
            app_link_send(NANO_MSG_SET_PRESET_SLOTS, buf, nano_preset_changed_notice(&s_state, buf));
        }
        break;
    case '%':   // the app switched an effect: shown at once, and the Nano's report may not follow it (s_fx_switched)
        s_state.fx_on[arg >> 1] = arg & 1;
        s_fx_switched[arg >> 1] = arg & 1;
        show();
        break;
    case 'Q': mix_preview(arg); break;
    case 'W': mix_save(arg); break;
    case '!': mix_free_pedal(); break;
    case '=': exp_dialog(arg); break;
    case '<': exp_calibrate(arg); break;
    case ';': exp_dialog_retry(); break;
    case '+': jack_set(arg); break;
    case '?': request_settings(); break;
    case 'J': mix_verify_request(); break;
    case 'K': mix_editor_closed(); break;
    case '$': scene_mode(arg); break;
    case '&': scene_values(arg); break;
    case '*': scene_send_step(SCENE_SEND_CHUNK); break;
    case '|': wah_edit(arg >> 16, arg & 0xFFFF); break;
    case '/': wah_step(); break;
    default: break;
    }
}

// The Nano reports a change made elsewhere (its own knobs, the editor connected to it directly, USB MIDI from an
// MC6): read the preset again shortly after (once for a burst), and the values of the open FX editor.
static void store_warn(void);

// The presets are in: MIDI may scan and connect now, and the app may connect through the controller.
static void link_usable(void)
{
    esp_timer_stop(s_midi_gate_timer);
    midi_ble_allow(true);
    app_link_allow_phone(true);
    app_link_enable(true);
    ui_splash_done();
}

static void external_change(uint32_t type)
{
    if (esp_timer_get_time() - s_last_change_us < 700 * 1000) return;   // the echo of our own change: nothing new
    values_forget();
    // One read for a burst of such notices, and at most one every 1.5 s: a pedal that keeps reporting must not have
    // the whole preset read again and again.
    static int64_t read_at_us;
    int64_t now = esp_timer_get_time(), at = now + 400 * 1000;
    if (at < read_at_us + 1500 * 1000 && now < read_at_us) at = read_at_us;                 // one is on its way: that one
    else if (at < read_at_us + 1500 * 1000) at = read_at_us + 1500 * 1000;                  // the last one was a moment ago
    read_at_us = at;
    esp_timer_stop(s_app_read_timer);
    esp_timer_start_once(s_app_read_timer, (uint64_t)(at - now));
    if (type == NANO_MSG_FX_VALUE && s_edit.slot >= 0 && !editor_reading()) editor_read_later(500);
    if (type == NANO_MSG_CAB_SETTING) cab_settings_command(2);
}

#ifdef NANO_DBG
static bool s_dbg_dump;   // console '`': every message from the Nano in the log, as it is

// Console "!<type, hex> <payload, hex>": send a message as it is - to ask the Nano things the controller has no
// function for (development only).
static void dbg_send(const char *line)
{
    char *end;
    uint32_t type = (uint32_t)strtoul(line, &end, 16);
    uint8_t payload[96];
    size_t n = 0;
    while (*end && n < sizeof(payload)) {
        while (*end == ' ') end++;
        if (!isxdigit((unsigned char)end[0]) || !isxdigit((unsigned char)end[1])) break;
        char byte[3] = { end[0], end[1], 0 };
        payload[n++] = (uint8_t)strtoul(byte, NULL, 16);
        end += 2;
    }
    ESP_LOGW(TAG, "Debug: sending type %u with %u payload bytes", (unsigned)type, (unsigned)n);
    send("Debug message", type, payload, n);
}
#endif

// The Nano's tempo menu / metronome as it last reported it (type 145). Sending it back needs this first: the message
// is a whole state, so guessed fields would change the tempo or its global setting.
static nano_tempo_t s_tempo;
static bool s_tempo_known;

static void handle_message(uint32_t type, const uint8_t *payload, size_t len)
{
#ifdef NANO_DBG
    if (s_dbg_dump) {
        ESP_LOGI(TAG, "<- type %u (%u bytes)", (unsigned)type, (unsigned)len);
        if (len) ESP_LOG_BUFFER_HEX(TAG, payload, len < 96 ? len : 96);
    }
#endif
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
        esp_timer_stop(s_state_timer);   // a complete reply: nothing overdue (the full state asks for the current one below)
        s_state_retries = 0;
        if (!s_waiting_full_state && s_expect_preset) {
            int reply_preset = (int)nano_field_varint(payload, len, 13) + 1;
            if ((reply_preset != s_expect_preset || s_expect_ack) && esp_timer_get_time() - s_expect_us < 2000000) {
                ESP_LOGI(TAG, "State of preset %d ignored (waiting for preset %d)", reply_preset, s_expect_preset);
                schedule_refresh(150);   // ask again for the new one
                break;
            }
            s_expect_preset = 0;
            s_expect_ack = false;
        }
        bool names = nano_state_apply(&s_state, payload, len);
        fx_follow_switches();
        // Right after an A/B swap a reply sent before the model change still names the previous model: the slot
        // holds the loaded one. Later a different model was chosen elsewhere (editor, app): it becomes A or B.
        for (int i = 0; i < AB_COUNT; i++) {
            ab_t *ab = &s_ab[i];
            if (ab->preset != s_state.current_preset || ab->slot < 0 || ab->phase != AB_IDLE) continue;
            uint32_t expected = ab->active ? ab->b.type : ab->a_type, now = s_state.fx_type[ab->slot];
            if (!expected || now == expected) continue;
            if (esp_timer_get_time() - ab->swapped_us < AB_SETTLE_US) {
                s_state.fx_type[ab->slot] = expected;
            } else if (ab->active && now == ab->a_type) {
                // The preset's own effect is back while B was running: the preset was loaded again somewhere else
                // (the app, MIDI at the Nano). B is not touched.
                ESP_LOGI(TAG, "%s: A is back (%s) - the preset was loaded again", ab_name(ab), nano_fx_name(now));
                ab->active = 0;
            } else if (now) {
                ESP_LOGI(TAG, "%s %c changed elsewhere: %s", ab_name(ab), ab->active ? 'B' : 'A', nano_fx_name(now));
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
            link_usable();
            static bool warned;
            if (!warned) {
                warned = true;
                store_warn();
            }
            if (s_refresh_library) {
                s_refresh_library = false;
                esp_timer_start_once(s_library_timer, 1500 * 1000);
            }
        } else {
            print_summary();
            if (s_reconnected) {   // the first preset after a lost link: everything else may come back too
                s_reconnected = false;
                link_usable();
                s_names_due = true;
                esp_timer_stop(s_library_timer);
                esp_timer_start_once(s_library_timer, 8000 * 1000);
            }
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
                    esp_timer_start_once(s_library_timer, 4000 * 1000);
                    request_settings();   // the Nano's global settings, once (shown in the log)
                }
                ab_capture_editor();   // edits of a second effect in the previous preset
                editor_close();
                if (s_preset_refused == s_state.current_preset) {
                    // The Nano names this preset but still plays the one before (it refused to load it): nothing the
                    // controller keeps for this preset belongs to what is in the slots. No second effects, reverb
                    // settings or mix positions - and above all nothing is read from the slots and stored for it.
                    // (In the run that showed this, the values of the preset that stuck were stored as the "A" and
                    // "B" of ten other presets.)
                    ESP_LOGW(TAG, "Preset %d is not loaded on the Nano: its stored extras stay out", s_state.current_preset);
                    for (int i = 0; i < AB_COUNT; i++) {
                        memset(&s_ab[i], 0, sizeof(s_ab[i]));
                        s_ab[i].slot = i;
                    }
                    memset(s_rp, 0, sizeof(s_rp));
                    s_rp_preset = 0;
                    s_mix.known = s_mix.own = s_mix.exp = false;
                    s_mix.active = -1;
                } else {
                    ab_load(s_state.current_preset);
                    mix_load();
                    rp_load(s_state.current_preset);
                    int cached = -1;
                    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {   // values of an A for a later swap, if it is on
                        if (ab_used(slot) && s_state.fx_on[slot]) {
                            ab_cache(slot, 700);
                            cached = slot;
                            break;   // (one read at a time: the others when they are switched, or at the swap)
                        }
                    }
                    // Which of the reverb's two settings the preset was stored with: from its values (that read above
                    // tells too, if it is the reverb's).
                    if (rp_used() && cached != s_mix.slot) rp_check_later(cached >= 0 ? 1300 : 700);
                }
            }
            show();
            setlist_start_scene();
            // Other preset or cab while the cab dialog is open: read its settings.
            if (s_src.cab_open && (s_src.cab_preset != s_state.current_preset || s_src.cab_slot != s_state.cab_slot)) {
                request_cab_settings();
            }
            if (s_edit.slot >= 0) {
                editor_show();
                if (s_state.fx_on[s_edit.slot] && !s_edit.known && !editor_reading()) editor_read_later(100);
            }
        }
        break;
    }
    case NANO_MSG_EXP_RESPONSE: {
        // Replies come in the order of the requests: this one belongs to the oldest request that still waits. Without
        // one the Nano reports on its own, and that is about the current preset.
        exp_expire();
        int asked = s_state.current_preset, purpose = EXP_ASK_READ;
        if (s_exp_asked_count) {
            asked = s_exp_asked[0].preset;
            purpose = s_exp_asked[0].purpose;
            s_exp_asked_count--;
            memmove(s_exp_asked, s_exp_asked + 1, sizeof(s_exp_asked[0]) * (size_t)s_exp_asked_count);
        }
        ESP_LOGI(TAG, "Expression assignments of preset %d (%u bytes)%s", asked, (unsigned)len,
                 asked != s_state.current_preset ? " - not the current preset any more, ignored" : "");
        ESP_LOG_BUFFER_HEX(TAG, payload, len < 96 ? len : 96);
        if (asked != s_state.current_preset) break;
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
        if (s_exp.write && purpose == EXP_ASK_WRITE) {
            int what = s_exp.write;
            s_exp.write = EXP_WRITE_NONE;
            exp_write(what);
        } else if (s_exp.verify && purpose == EXP_ASK_VERIFY) {
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
        if (s_exp.open && !s_exp.write && !s_exp.verify) {
            s_exp.shown = true;
            ui_set_expression(s_exp.ranges, s_exp.switches);
        }
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
        float values[NANO_MAX_PARAMS];
        int slot, count;
        int purpose = param_read_reply(payload, len, &slot, values, &count);
        if (purpose != READ_NONE) rp_match(slot, values, count);
        switch (purpose) {
        case READ_AB: ab_values_read(slot, values, count); break;
        case READ_RP: rp_values(slot, values, count); break;
        case READ_EDITOR: editor_values(slot, values, count); break;
        default: break;
        }
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
        ab_leave_preset();   // (before the number changes: edits belong to the preset that was on)
        s_read_busy_until_us = esp_timer_get_time() + PARAM_BUSY_US;   // the Nano is loading: no value reads meanwhile
        nano_state_apply_preset_change(&s_state, payload, len);
        s_expect_preset = 0;
        s_expect_ack = false;
        s_preset_refused = 0;
        ESP_LOGI(TAG, "Preset changed on the Nano: %d", s_state.current_preset);
        uint8_t buf[4];
        send("Preset change ack", NANO_MSG_SET_PRESET_SLOTS_RESPONSE, buf, nano_preset_change_ack(buf));
        schedule_refresh(150);
        break;
    }
    case NANO_MSG_SET_PRESET_SLOTS_RESPONSE: {
        bool ok = nano_field_varint(payload, len, 4) == 1;
        if (ok) {
            ESP_LOGI(TAG, "Preset change confirmed");
            s_preset_refused = 0;
        } else {
            ESP_LOGW(TAG, "Preset change FAILED - the Nano's reply (%u bytes):", (unsigned)len);
            ESP_LOG_BUFFER_HEX(TAG, payload, len < 64 ? len : 64);
            if (s_expect_preset && s_preset_tries < 2) {   // once more
                s_preset_tries++;
                preset_request(s_expect_preset);
                s_expect_us = esp_timer_get_time();
                break;
            }
            s_preset_refused = s_expect_preset ? s_expect_preset : s_state.current_preset;
            char text[80];
            snprintf(text, sizeof(text), "The Nano did not load preset %d - the sound is still the one before.", s_preset_refused);
            ui_show_message(text);
        }
        s_expect_ack = false;
        if (s_expect_preset) schedule_refresh(40);   // the new preset is ready: read it now (not after a fixed wait)
        break;
    }
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
        if (len <= 8) {   // (with its bytes: what the Nano says by itself is not all known yet)
            char bytes[3 * 8 + 1] = "";
            for (size_t i = 0; i < len; i++) snprintf(bytes + 3 * i, 4, "%02X ", payload[i]);
            ESP_LOGI(TAG, "Message type %lu: %s- changed elsewhere, reading again", (unsigned long)type, bytes);
        } else {
            ESP_LOGI(TAG, "Message type %lu (%u bytes) - changed elsewhere, reading again", (unsigned long)type, (unsigned)len);
        }
        external_change(type);
        break;
    case NANO_MSG_TEMPO_SCREEN: {   // tempo menu / metronome (NanOS 2.3.0): only kept and logged for now
        nano_tempo_read(&s_tempo, payload, len);
        s_tempo_known = true;
        const nano_tempo_t *t = &s_tempo;
        ESP_LOGI(TAG, "Tempo message: screen %d, global %d, %.1f BPM, MIDI clock %d, metronome %s %.1f dB, %s, %s, %s%s",
                 t->screen_active, t->global, (double)t->tempo, t->ext_clock, t->muted ? "muted" : "ON",
                 (double)t->volume_db, t->time_sig < NANO_TIME_SIG_COUNT ? NANO_TIME_SIGS[t->time_sig] : "?",
                 t->note_len < NANO_NOTE_LEN_COUNT ? NANO_NOTE_LENS[t->note_len] : "?",
                 t->sound < NANO_METRO_SOUND_COUNT ? NANO_METRO_SOUNDS[t->sound] : "?", t->has_volume ? "" : " (no volume field)");
        char bytes[3 * 48 + 1] = "";
        for (size_t i = 0; i < len && i < 48; i++) snprintf(bytes + 3 * i, 4, "%02X ", payload[i]);
        ESP_LOGI(TAG, "Tempo message bytes: %s", bytes);
        break;
    }
    default:
        ESP_LOGI(TAG, "Message type %lu (%u bytes)", (unsigned long)type, (unsigned)len);
        break;
    }
}

// ---- the settings store: how full it is, and its backup through the editor ----
// Everything the controller keeps (banks, scenes, second effects, the set list ...) is in one small store: 756
// entries, and a scene with settings alone takes dozens. When it is full nothing can be saved any more, so the
// controller says how full it is - and the editor can read every entry into a file and write such a file back
// (messages of APP_LINK_CONTROLLER_TYPE on, which stay between the app and the controller):
//   60000 app: entry request { index u16 }      -> 60001 { index u16, count u16, key length u8, key, value }
//                                                  or, past the last one, 60002 (see backup_info)
//   60003 app: restore begins { entries u16 }   -> 60004 { ok u8 }   (the store is emptied)
//   60005 app: { key length u8, key, total u16, offset u16, bytes } -> 60006 { ok u8 }   (a value in pieces)
//   60007 app: restore done                     -> 60008 { ok u8, entries written u16 }, then the controller restarts
//   60009 app: info request                     -> 60002 { entries u16, used u16, capacity u16, version }
// The editor's view of the board (its footswitch strip) and the FX presets shared with it:
//   60010 app: tiles, please                    -> 60011 per tile and 60013 settings, then again whenever they change
//                                                  (until the app goes): 60011 { switch 1-8, flags (UI_TILE_*), colour
//                                                  u24, filled row i8, caption, name, note, row 1, row 2 (each length
//                                                  u8 + text) }; 60013 see tiles_settings
//   60014 app: { command u8, arg i32 }          -> 60015 { ok u8 }   (the screen's commands the editor may give)
//   60016 app: { model u32 }                    -> 60017 { model u32, its FX presets as stored (FXP_COUNT x fxp_t) }
//   60018 app: which models have FX presets     -> 60019 { count u16, models u32 ... }
//   60020 app: { model u32, FX presets }        -> 60021 { ok u8 }   (all empty: removed)
//   60023 controller: { model u32 }             an FX preset of this model was saved or deleted on the board
#define STORE_WARN_PERCENT 85
#define BACKUP_VALUE_MAX 4000

// Used share of the settings store in per cent (of what can be written), -1 = not known.
static int store_percent(int *used, int *capacity)
{
#ifdef NANO_WEB
    return -1;
#else
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) != ESP_OK) return -1;
    int cap = (int)(stats.used_entries + stats.available_entries);
    if (used) *used = (int)stats.used_entries;
    if (capacity) *capacity = cap;
    return cap ? (int)(stats.used_entries * 100 / cap) : -1;
#endif
}

// Said once after the start, while there is still room to act.
static void store_warn(void)
{
    int percent = store_percent(NULL, NULL);
    ESP_LOGI(TAG, "Settings store %d %% full", percent);
    if (percent < STORE_WARN_PERCENT) return;
    char text[120];
    snprintf(text, sizeof(text), "Settings memory %d %% full - back it up with the editor and remove scenes or second effects you do not need.", percent);
    ui_show_message(text);
}

#ifdef NANO_WEB
static void backup_message(uint32_t type, const uint8_t *payload, size_t len) { }
static void editor_fxp_changed(uint32_t type) { }
#else
static void backup_reply(uint32_t type, const uint8_t *payload, size_t len)
{
    uint8_t *message = heap_caps_malloc(len + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!message) return;
    memcpy(message, payload, len);
    for (int i = 0; i < 4; i++) message[len + i] = (uint8_t)(type >> (8 * i));
    app_link_send_message(message, len + 4);   // (copies it)
    heap_caps_free(message);
}

static int backup_count(void)
{
    int count = 0;
    nvs_iterator_t it = NULL;
    for (esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, BANK_STORE_NAMESPACE, NVS_TYPE_BLOB, &it); err == ESP_OK; err = nvs_entry_next(&it)) count++;
    nvs_release_iterator(it);
    return count;
}

static void backup_info(void)
{
    int used = 0, capacity = 0;
    store_percent(&used, &capacity);
    uint8_t reply[6 + 32];
    int count = backup_count();
    reply[0] = (uint8_t)count; reply[1] = (uint8_t)(count >> 8);
    reply[2] = (uint8_t)used; reply[3] = (uint8_t)(used >> 8);
    reply[4] = (uint8_t)capacity; reply[5] = (uint8_t)(capacity >> 8);
    size_t n = strlcpy((char *)reply + 6, esp_app_get_description()->version, 32);
    backup_reply(60002, reply, 6 + (n < 31 ? n : 31));
}

// Entry `index` of the store, in the order the store lists them (the app asks for one after the other).
static void backup_entry(int index)
{
    static nvs_iterator_t it;
    static int at = -1;   // the entry `it` stands on
    if (index == 0 || index != at + 1 || !it) {   // from the start (the usual case is the next one)
        nvs_release_iterator(it);
        it = NULL;
        at = -1;
        if (nvs_entry_find(NVS_DEFAULT_PART_NAME, BANK_STORE_NAMESPACE, NVS_TYPE_BLOB, &it) == ESP_OK) at = 0;
        while (it && at < index && nvs_entry_next(&it) == ESP_OK) at++;
    } else if (nvs_entry_next(&it) == ESP_OK) {
        at++;
    } else {
        it = NULL;
    }
    if (!it || at != index) {   // past the end
        nvs_release_iterator(it);
        it = NULL;
        at = -1;
        backup_info();
        return;
    }
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    size_t key_len = strlen(info.key), size = 0;
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t *reply = NULL;
    if (nvs_get_blob(nvs, info.key, NULL, &size) == ESP_OK && size <= BACKUP_VALUE_MAX
        && (reply = heap_caps_malloc(5 + key_len + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != NULL) {
        int count = backup_count();
        reply[0] = (uint8_t)index; reply[1] = (uint8_t)(index >> 8);
        reply[2] = (uint8_t)count; reply[3] = (uint8_t)(count >> 8);
        reply[4] = (uint8_t)key_len;
        memcpy(reply + 5, info.key, key_len);
        if (nvs_get_blob(nvs, info.key, reply + 5 + key_len, &size) == ESP_OK) backup_reply(60001, reply, 5 + key_len + size);
    } else {
        ESP_LOGW(TAG, "Backup: entry %s not readable", info.key);
        uint8_t skip[5] = { (uint8_t)index, (uint8_t)(index >> 8), 0, 0, 0 };   // (an entry without a key: skipped by the app)
        backup_reply(60001, skip, sizeof(skip));
    }
    heap_caps_free(reply);
    nvs_close(nvs);
}


// ---- The editor's footswitch strip and the shared FX presets (60010-60023, see above) ----

static bool s_tiles_sub;                       // the editor asked for the tiles: they follow every change
static ui_tile_info_t s_tiles_sent[8];
static uint8_t s_tiles_sent_ok;                // bit n: tile n went out as in s_tiles_sent
static uint8_t s_settings_sent[96];
static size_t s_settings_sent_len;

static size_t put_text(uint8_t *out, const char *text, size_t max)
{
    size_t n = strnlen(text, max);
    out[0] = (uint8_t)n;
    memcpy(out + 1, text, n);
    return 1 + n;
}

// 60013 { fields u8, order u8 x 8, switch 1 u8, FX mode u8, reverb slot i8, mix known u8, reverb settings used u8,
// settings set u8, reverb B u32, reverb B kept u32, second effects u32 x 5, their switch u8 x 5, setting names 2 x
// (length u8 + text) }
static size_t tiles_settings(const ui_view_t *v, uint8_t *out)
{
    size_t n = 0;
    out[n++] = (uint8_t)s_layout_count;
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) out[n++] = s_layout[p];
    out[n++] = (uint8_t)s_switch1;
    out[n++] = v->fx_mode;
    out[n++] = (uint8_t)(int8_t)v->mix_slot;
    out[n++] = v->mix_known;
    out[n++] = v->rp_used;
    out[n++] = v->rp_set;
    uint32_t words[2 + NANO_FX_SLOTS] = { v->rev_b_type, v->rev_b_stored };
    for (int i = 0; i < NANO_FX_SLOTS; i++) words[2 + i] = v->ab_b_type[i];
    for (int w = 0; w < 2 + NANO_FX_SLOTS; w++) for (int b = 0; b < 4; b++) out[n++] = (uint8_t)(words[w] >> (8 * b));
    for (int i = 0; i < NANO_FX_SLOTS; i++) out[n++] = v->ab_mode[i];
    for (int r = 0; r < 2; r++) n += put_text(out + n, v->rp_names[r], 15);
    return n;
}

// Sends what changed since the last time (called by the app task after each event).
static void tiles_push(void)
{
    if (!s_tiles_sub) return;
    if (!app_link_connected()) { s_tiles_sub = false; return; }
    static ui_tile_info_t tiles[8];
    static ui_view_t view;
    if (!ui_tiles_get(tiles, &view)) return;
    for (int i = 0; i < 8; i++) {
        if ((s_tiles_sent_ok >> i & 1) && !memcmp(&tiles[i], &s_tiles_sent[i], sizeof(tiles[i]))) continue;
        const ui_tile_info_t *t = &tiles[i];
        uint8_t m[8 + 24 + 40 + 24 + 16 + 16];
        size_t n = 0;
        m[n++] = (uint8_t)(i + 1);
        m[n++] = t->flags;
        m[n++] = (uint8_t)t->color;
        m[n++] = (uint8_t)(t->color >> 8);
        m[n++] = (uint8_t)(t->color >> 16);
        m[n++] = (uint8_t)t->row_active;
        n += put_text(m + n, t->caption, 23);
        n += put_text(m + n, t->name, 39);
        n += put_text(m + n, t->note, 23);
        n += put_text(m + n, t->rows[0], 15);
        n += put_text(m + n, t->rows[1], 15);
        backup_reply(60011, m, n);
        s_tiles_sent[i] = *t;
        s_tiles_sent_ok |= 1 << i;
    }
    uint8_t settings[sizeof(s_settings_sent)];
    size_t len = tiles_settings(&view, settings);
    if (len != s_settings_sent_len || memcmp(settings, s_settings_sent, len)) {
        backup_reply(60013, settings, len);
        memcpy(s_settings_sent, settings, len);
        s_settings_sent_len = len;
    }
}

// The commands of the screen the editor's strip may give (pressing a tile, the second effects, footswitch 8, the
// layout) - nothing that opens a dialog on the board.
static bool editor_command(char c, int arg)
{
    switch (c) {
    case 'w': return (arg & 0xFF) >= 1 && (arg & 0xFF) <= FOOTSWITCH_COUNT && !(arg & ~(0xFF | UI_SWITCH_HOLD));
    case ']': case 'A': case '>': return true;
    case '-': return arg == 1 || arg == 2 || arg == 16 || arg == 17;
    default: return false;
    }
}

static void editor_fxp_changed(uint32_t type)
{
    if (!s_tiles_sub || !app_link_connected()) return;
    uint8_t m[4] = { (uint8_t)type, (uint8_t)(type >> 8), (uint8_t)(type >> 16), (uint8_t)(type >> 24) };
    backup_reply(60023, m, sizeof(m));
}

static uint32_t read_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void editor_message(uint32_t type, const uint8_t *payload, size_t len)
{
    switch (type) {
    case 60010:
        s_tiles_sub = true;
        s_tiles_sent_ok = 0;
        s_settings_sent_len = 0;
        tiles_push();
        break;
    case 60014: {
        uint8_t ok = 0;
        if (len >= 5) {
            char c = (char)payload[0];
            int arg = (int)read_u32(payload + 1);
            if (editor_command(c, arg)) {
                ESP_LOGI(TAG, "Editor: command %c %d", c, arg);
                command(c, arg);
                ok = 1;
            }
        }
        backup_reply(60015, &ok, 1);
        break;
    }
    case 60016: {
        if (len < 4) break;
        uint32_t model = read_u32(payload);
        static uint8_t reply[4 + sizeof(fxp_t) * FXP_COUNT];
        memcpy(reply, payload, 4);
        fxp_read(model, (fxp_t *)(reply + 4));
        backup_reply(60017, reply, sizeof(reply));
        break;
    }
    case 60018: {
        static uint8_t reply[2 + 4 * 128];
        int count = 0;
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, BANK_STORE_NAMESPACE, NVS_TYPE_BLOB, &it);
        while (err == ESP_OK && count < 128) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (!strncmp(info.key, "fxp", 3)) {
                uint32_t model = (uint32_t)strtoul(info.key + 3, NULL, 10);
                for (int b = 0; b < 4; b++) reply[2 + 4 * count + b] = (uint8_t)(model >> (8 * b));
                count++;
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        reply[0] = (uint8_t)count;
        reply[1] = (uint8_t)(count >> 8);
        backup_reply(60019, reply, 2 + 4 * count);
        break;
    }
    case 60020: {
        uint8_t ok = 0;
        if (len == 4 + sizeof(fxp_t) * FXP_COUNT) {
            uint32_t model = read_u32(payload);
            static fxp_t list[FXP_COUNT];
            memcpy(list, payload + 4, sizeof(list));
            bool empty = true;
            for (int i = 0; i < FXP_COUNT; i++) {
                list[i].name[sizeof(list[i].name) - 1] = 0;
                if (list[i].count > NANO_MAX_PARAMS) list[i].count = NANO_MAX_PARAMS;
                empty &= !list[i].name[0];
            }
            char key[16];
            fxp_key(model, key);
            nvs_handle_t nvs;
            if (model && nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                esp_err_t err = empty ? nvs_erase_key(nvs, key) : nvs_set_blob(nvs, key, list, sizeof(list));
                ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(nvs) == ESP_OK;
                nvs_close(nvs);
            }
            if (ok && model == s_fxp.type) {   // the FX editor shows this model: its list as it is now
                memcpy(s_fxp.list, list, sizeof(list));
                s_fxp.active = -1;
                if (s_edit.slot >= 0) editor_show();
            }
            ESP_LOGI(TAG, "Editor: FX presets of %s %s", nano_fx_name(model), ok ? (empty ? "removed" : "stored") : "NOT stored");
        }
        backup_reply(60021, &ok, 1);
        break;
    }
    default:
        break;
    }
}

static void backup_message(uint32_t type, const uint8_t *payload, size_t len)
{
    if (type >= 60010 && type < 60030) { editor_message(type, payload, len); return; }
    static uint8_t *value;        // the value being put together (PSRAM)
    static char key[NVS_KEY_NAME_MAX_SIZE];
    static size_t got, total;
    static int written;
    static bool restoring;
    uint8_t ok = 0;
    nvs_handle_t nvs;
    switch (type) {
    case 60000:
        if (len >= 2) backup_entry(payload[0] | payload[1] << 8);
        break;
    case 60009:
        backup_info();
        break;
    case 60003:   // restore: everything stored goes, the file's entries follow
        if (!value) value = heap_caps_malloc(BACKUP_VALUE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (value && nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            ok = nvs_erase_all(nvs) == ESP_OK && nvs_commit(nvs) == ESP_OK;
            nvs_close(nvs);
        }
        restoring = ok;
        written = 0;
        got = total = 0;
        key[0] = 0;
        ESP_LOGW(TAG, "Restore of the settings begins: %s", ok ? "store emptied" : "FAILED");
        if (ok) ui_show_message("Restoring the settings from the editor ...");
        backup_reply(60004, &ok, 1);
        break;
    case 60005: {   // a piece of a value
        size_t key_len = len ? payload[0] : 0;
        if (!restoring || !key_len || key_len >= sizeof(key) || len < 1 + key_len + 4) {
            backup_reply(60006, &ok, 1);
            break;
        }
        const uint8_t *p = payload + 1 + key_len;
        size_t all = p[0] | p[1] << 8, offset = p[2] | p[3] << 8, n = len - (1 + key_len + 4);
        if (offset == 0) {
            memcpy(key, payload + 1, key_len);
            key[key_len] = 0;
            total = all;
            got = 0;
        }
        if (strncmp(key, (const char *)payload + 1, key_len) || key[key_len] || offset != got || all != total || got + n > total
            || total > BACKUP_VALUE_MAX) {
            ESP_LOGW(TAG, "Restore: piece out of order for %s", key);
            backup_reply(60006, &ok, 1);
            break;
        }
        memcpy(value + got, p + 4, n);
        got += n;
        ok = 1;
        if (got == total) {   // complete: into the store
            ok = 0;
            if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                ok = nvs_set_blob(nvs, key, value, total) == ESP_OK;
                nvs_close(nvs);
            }
            if (ok) written++;
            else ESP_LOGW(TAG, "Restore: %s not written", key);
            key[0] = 0;
            got = total = 0;
        }
        backup_reply(60006, &ok, 1);
        break;
    }
    case 60007: {
        if (!restoring) break;
        restoring = false;
        if (nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            ok = nvs_commit(nvs) == ESP_OK;
            nvs_close(nvs);
        }
        uint8_t reply[3] = { ok, (uint8_t)written, (uint8_t)(written >> 8) };
        backup_reply(60008, reply, sizeof(reply));
        ESP_LOGW(TAG, "Restore done: %d entries written - restarting", written);
        ui_show_message("Settings restored - restarting ...");
        vTaskDelay(pdMS_TO_TICKS(1500));   // the reply goes out, the message is read
        esp_restart();
        break;
    }
    default:
        break;
    }
}
#endif

// ---- system page ----
static char s_start_reason[48] = "?";   // why the controller started (set in app_main)

static void put_duration(char *out, size_t size, int64_t us)
{
    long minutes = (long)(us / 60000000);
    if (minutes >= 60) snprintf(out, size, "%ld h %02ld min", minutes / 60, minutes % 60);
    else if (minutes >= 1) snprintf(out, size, "%ld min", minutes);
    else snprintf(out, size, "%ld s", (long)(us / 1000000));
}

// What the System button of the MIDI dialog shows: is everything well, and if not, since when.
static void system_page(void)
{
    char text[520], up[24], link[24], ago[24];
    int64_t now = esp_timer_get_time();
    nano_link_stats_t st;
    nano_link_stats(&st);
    put_duration(up, sizeof(up), now);
    put_duration(link, sizeof(link), st.ready_us ? now - st.ready_us : 0);
    put_duration(ago, sizeof(ago), now - st.last_drop_us);
    int used = 0, capacity = 0, percent = store_percent(&used, &capacity);
    // One line per row "what: value", a blank line between groups, '!' in front of what deserves a look (ui_show_system)
    bool hang = strstr(s_start_reason, "hang") != NULL;
    int n = snprintf(text, sizeof(text), "Firmware: %s\nRunning for: %s\n%sStarted after: %s\n\n", esp_app_get_description()->version, up,
                     hang ? "!" : "", s_start_reason);
    n += snprintf(text + n, sizeof(text) - (size_t)n, st.ready_us ? "Nano: connected for %s\n" : "!Nano: not connected\n", link);
    if (!st.drops) n += snprintf(text + n, sizeof(text) - (size_t)n, "Connection lost: never since the start\n\n");
    else n += snprintf(text + n, sizeof(text) - (size_t)n, "!Connection lost: %d time%s\nLast time: %s ago (reason 0x%X)\n\n", st.drops, st.drops == 1 ? "" : "s",
                       ago, (unsigned)st.last_reason);
#ifndef NANO_WEB
    n += snprintf(text + n, sizeof(text) - (size_t)n, "Internal memory: %u KB free, lowest %u KB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
#endif
    if (percent >= 0) snprintf(text + n, sizeof(text) - (size_t)n, "%sSettings memory: %d %% full (%d of %d)%s", percent >= STORE_WARN_PERCENT ? "!" : "",
                               percent, used, capacity, percent >= STORE_WARN_PERCENT ? " - back up" : "");
    ui_show_system(text);
}

#ifdef NANO_WEB
static void store_rescue(void) { }
static void store_cleanup(void) { }
#else
// A raw copy of the settings store in flash memory that nothing else uses (the board has 16 MB, the firmware ends at
// 0x610000). Made only before the store has to be emptied; a computer reads it back with
// "esptool read-flash 0xF00000 0x6000 store.bin" and can write it to 0x9000 again.
#define STORE_RESCUE_ADDRESS 0xF00000
static void store_rescue(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    uint32_t chip = 0;
    if (!part || esp_flash_get_size(NULL, &chip) != ESP_OK || chip < STORE_RESCUE_ADDRESS + part->size) return;
    uint8_t *page = heap_caps_malloc(4096, MALLOC_CAP_DEFAULT);
    if (!page) return;
    bool ok = esp_flash_erase_region(NULL, STORE_RESCUE_ADDRESS, (part->size + 4095) & ~4095u) == ESP_OK;
    for (size_t off = 0; ok && off < part->size; off += 4096) {
        size_t n = part->size - off < 4096 ? part->size - off : 4096;
        ok = esp_partition_read_raw(part, off, page, n) == ESP_OK && esp_flash_write(NULL, page, STORE_RESCUE_ADDRESS + off, n) == ESP_OK;
    }
    heap_caps_free(page);
    ESP_LOGW(TAG, "Copy of the settings store at 0x%X: %s", STORE_RESCUE_ADDRESS, ok ? "written" : "FAILED");
}

// What an earlier firmware on this board left in the store (Wi-Fi and Bluetooth settings of another controller
// firmware): a fifth of the store on the board this was written on, and nothing here reads it. Removed once.
static void store_cleanup(void)
{
    static const char *const LEFTOVERS[] = { "nvs.net80211", "storage", "bt_config.conf" };
    nvs_stats_t before, after;
    bool removed = false;
    if (nvs_get_stats(NULL, &before) != ESP_OK) return;
    for (size_t i = 0; i < sizeof(LEFTOVERS) / sizeof(LEFTOVERS[0]); i++) {
        nvs_handle_t nvs;
        if (nvs_open(LEFTOVERS[i], NVS_READONLY, &nvs) != ESP_OK) continue;   // not there (opening it for writing would make it)
        nvs_close(nvs);
        if (nvs_open(LEFTOVERS[i], NVS_READWRITE, &nvs) != ESP_OK) continue;
        size_t used = 0;
        nvs_get_used_entry_count(nvs, &used);
        if (used && nvs_erase_all(nvs) == ESP_OK && nvs_commit(nvs) == ESP_OK) removed = true;
        nvs_close(nvs);
    }
    if (removed && nvs_get_stats(NULL, &after) == ESP_OK) {
        ESP_LOGW(TAG, "Settings store: leftovers of an earlier firmware removed - %u entries free again",
                 (unsigned)(before.used_entries - after.used_entries));
    }
}
#endif

// ---- watchdog ----
// On stage a controller that hangs has to come back by itself. Twice a second the task that handles everything (app)
// and the task that draws the screen give a sign of life; if one of them is silent for WATCHDOG_MS the controller
// restarts - it is connected again a few seconds later - and says so after the start.
#ifndef NANO_WEB
#define WATCHDOG_MS 8000
#define WATCHDOG_MAGIC 0x57444F47u
static RTC_NOINIT_ATTR uint32_t s_watchdog_magic, s_watchdog_cause;   // survive the restart: 1 = app task, 2 = screen
static volatile uint32_t s_app_beat;   // tick of the app task's last round
static esp_timer_handle_t s_watchdog_timer;

static void watchdog_cb(void *arg)
{
    uint32_t cause = (uint32_t)(xTaskGetTickCount() - s_app_beat) > pdMS_TO_TICKS(WATCHDOG_MS) ? 1 : ui_stalled_ms() > WATCHDOG_MS ? 2 : 0;
    if (!cause) return;
    ESP_LOGE(TAG, "Watchdog: the %s task has not answered for %d s - restarting", cause == 1 ? "app" : "screen", WATCHDOG_MS / 1000);
    s_watchdog_magic = WATCHDOG_MAGIC;
    s_watchdog_cause = cause;
    esp_restart();
}

// After the start: 1 / 2 if the watchdog restarted the controller (which task hung), 0 otherwise.
static int watchdog_restarted(void)
{
    int cause = esp_reset_reason() == ESP_RST_SW && s_watchdog_magic == WATCHDOG_MAGIC ? (int)s_watchdog_cause : 0;
    s_watchdog_magic = 0;
    return cause;
}
#endif

// ---- tasks ----

static void app_event(app_event_t ev)
{
    switch (ev.kind) {
    case EV_LINK_UP:
        esp_timer_stop(s_pair_hint_timer);
        s_incomplete_retries = 0;
        s_state_retries = 0;
        ui_set_link(true);
        if (s_state.names_loaded) {   // back after a lost link: the current preset is enough for now
            ESP_LOGI(TAG, "Connected to the Nano Cortex again - reading the current preset");
            s_waiting_full_state = false;
            s_reconnected = true;
            request_current_state();
            break;
        }
        ESP_LOGI(TAG, "Connected to the Nano Cortex - reading presets");
        ui_splash_status("Connected - loading presets ...", NULL);
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
        esp_timer_stop(s_pair_hint_timer);
        esp_timer_start_once(s_pair_hint_timer, PAIR_HINT_US);
        s_tuner_on = false;
        s_last_preset = 0;
        s_exp_asked_count = 0;
        s_exp.write = s_exp.verify = EXP_WRITE_NONE;
        s_pedal_collect = s_cal.active = false;
        s_usb.known = false;
        memset(&s_src, 0, sizeof(s_src));   // also a throttle whose timer command came while disconnected
        s_edit.slot = -1;
        s_edit.pending_param = -1;
        s_read.purpose = READ_NONE;
        s_scene_queued = s_scene_sent = 0;
        esp_timer_stop(s_wah_timer);
        s_sweep.slot = -1;
        // Nothing that was under way goes on after the link comes back: swaps, value runs, reads, an expected preset
        ab_leave_preset();
        esp_timer_stop(s_state_timer);
        esp_timer_stop(s_refresh_timer);
        esp_timer_stop(s_read_timer);
        esp_timer_stop(s_scene_timer);
        esp_timer_stop(s_fxp_timer);
        esp_timer_stop(s_rp_timer);
        esp_timer_stop(s_app_read_timer);
        s_fxp.sent = s_fxp.send_count = 0;
        s_preset_refused = 0;
        s_expect_preset = 0;
        s_expect_ack = false;
        s_save_pending = 0;
        s_reconnected = false;
        for (int i = 0; i < NANO_FX_SLOTS; i++) s_fx_switched[i] = -1;
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
        else if (ev.command == 'T') setlist_edit((const char *)ev.data);
        else if (ev.command == 'R') rp_name((const char *)ev.data);
#ifdef NANO_DBG
        else if (ev.command == '!') dbg_send((const char *)ev.data);
#endif
        else if (ev.command == 'X') exp_dialog_save((const char *)ev.data);
        free(ev.data);
        break;
    case EV_MIDI:
        handle_midi(ev.arg);
        break;
    case EV_APP_MESSAGE:
        backup_message(ev.type, ev.data, ev.len);
        free(ev.data);
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
    size_t low_logged = SIZE_MAX;
    for (;;) {
        s_app_beat = xTaskGetTickCount();
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(2000)) == pdTRUE) app_event(ev);
        tiles_push();   // the editor's footswitch strip, if it asked for it
        // Internal RAM is what Bluetooth lives on: say so when its lowest free amount goes down (by 4 KB or more)
        size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        if (low + 4096 <= low_logged) {
            low_logged = low;
            ESP_LOGI(TAG, "Internal RAM: %u KB free, lowest so far %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(low / 1024));
        }
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
        if (c >= 'A' && c <= 'E') {   // footswitch 3-7 held (FX mode: second effect or settings swap)
            command('w', UI_SWITCH_HOLD | (c - 'A' + 3));
            continue;
        }
#ifdef NANO_BENCH
        if (c == '%') {
            ui_bench();
            continue;
        }
#endif
#ifdef NANO_DBG
        if (c == '!') {   // a message as it is, up to the end of the line (see dbg_send)
            char line[220];
            size_t n = 0;
            uint8_t ch;
            while (n < sizeof(line) - 1 && console_read(&ch, pdMS_TO_TICKS(2000)) == 1 && ch != '\n' && ch != '\r') line[n++] = (char)ch;
            line[n] = 0;
            on_text('!', line);
            continue;
        }
        if (c == '~') {   // "~<seconds>" + Enter: disconnect properly, stay away, then connect again
            char digits[6];
            size_t n = 0;
            uint8_t ch;
            while (n < sizeof(digits) - 1 && console_read(&ch, pdMS_TO_TICKS(2000)) == 1 && isdigit(ch)) digits[n++] = (char)ch;
            digits[n] = 0;
            nano_link_pause(n ? atoi(digits) : 30);
            continue;
        }
        if (c == '`') {
            s_dbg_dump = !s_dbg_dump;
            ESP_LOGW(TAG, "Debug: messages from the Nano %s", s_dbg_dump ? "are logged" : "are no longer logged");
            continue;
        }
#endif
#ifdef NANO_RPTEST
        if (c == '_' || c == '+' || c == '=' || c == '\\') {   // reverb settings: set 1 / set 2 / on the switch / forget
            command('-', c == '_' ? 1 : c == '+' ? 2 : c == '=' ? 17 : 32);
            continue;
        }
        if (c == '|' && s_mix.slot >= 0) {   // the reverb's own A / B, as its tile swaps it
            command('[', s_mix.slot);
            continue;
        }
#endif
#ifdef NANO_WDTEST
        if (c == 'W' || c == 'U') {   // hang the app task / the screen on purpose
            command('}', c == 'U');
            continue;
        }
#endif
        c = (uint8_t)tolower(c);
        if (c && strchr(CONSOLE_COMMANDS, c)) command((char)c, 0);   // other letters are used by the screen
    }
}
#endif

// Entries of firmware before 1.15 take more room in the store than they need: second effects were stored with all
// 24 values, the mix positions as an entry per preset. They are rewritten once (and again after a backup of such a
// firmware was written back); a copy of the store as it was is kept first (store_rescue).
static void store_compact(void)
{
    int old_mixes = mixes_load(false), rewritten = 0;   // (read, nothing written yet)
#ifdef NANO_WEB
    if (old_mixes) rewritten = mixes_load(true);
#else
    enum { OLD_B = sizeof(fx_b_t), OLD_A = sizeof(fx_a_t), OLD_RP = sizeof(rp_t), MAX_KEYS = 320 };
    static const struct { const char *prefix; int digits; size_t old; } FAMILIES[] = {
        { "rvb", 2, OLD_B }, { "pf1", 2, OLD_B }, { "ab", 3, OLD_B },
        { "rva", 2, OLD_A }, { "pfa", 2, OLD_A }, { "aa", 3, OLD_A },
        { "rvp", 2, OLD_RP }, { "rvq", 2, OLD_RP },
    };
    char (*keys)[NVS_KEY_NAME_MAX_SIZE] = malloc(MAX_KEYS * sizeof(*keys));
    uint8_t *family = malloc(MAX_KEYS);
    nvs_handle_t nvs;
    if (!keys || !family || nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        free(keys);
        free(family);
        return;
    }
    int count = 0;   // (the keys first: the store must not change while it is listed)
    nvs_iterator_t it = NULL;
    for (esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, BANK_STORE_NAMESPACE, NVS_TYPE_BLOB, &it);
         err == ESP_OK && count < MAX_KEYS; err = nvs_entry_next(&it)) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) != ESP_OK) continue;
        for (size_t f = 0; f < sizeof(FAMILIES) / sizeof(FAMILIES[0]); f++) {
            size_t n = strlen(FAMILIES[f].prefix);
            if (strncmp(info.key, FAMILIES[f].prefix, n) != 0 || strlen(info.key) != n + (size_t)FAMILIES[f].digits) continue;
            bool digits = true;
            for (const char *c = info.key + n; *c; c++) digits = digits && isdigit((unsigned char)*c);
            if (!digits) continue;
            strlcpy(keys[count], info.key, sizeof(keys[count]));
            family[count++] = (uint8_t)f;
            break;
        }
    }
    nvs_release_iterator(it);
    int old = 0;
    for (int i = 0; i < count; i++) {
        size_t size = 0;
        if (nvs_get_blob(nvs, keys[i], NULL, &size) == ESP_OK && size == FAMILIES[family[i]].old) old++;
        else family[i] = 0xFF;   // as it should be already
    }
    if (old || old_mixes) store_rescue();   // the store as it was, before anything is rewritten
    if (old_mixes) rewritten = mixes_load(true);
    for (int i = 0; i < count; i++) {
        if (family[i] == 0xFF) continue;
        uint8_t in[sizeof(rp_t)], out[sizeof(rp_t)];   // (the largest of the three)
        size_t size = sizeof(in), packed = 0;
        if (nvs_get_blob(nvs, keys[i], in, &size) != ESP_OK) continue;
        if (size == OLD_B) {
            fx_b_t b;
            if (fx_b_unpack(in, size, &b)) packed = fx_b_pack(&b, out);
        } else if (size == OLD_A) {
            fx_a_t a;
            if (fx_a_unpack(in, size, &a)) packed = fx_a_pack(&a, out);
        } else if (size == OLD_RP) {
            rp_t rp;
            if (rp_unpack(in, size, &rp)) packed = rp_pack(&rp, out);
        }
        if (!packed || packed >= size) continue;   // (not readable: left as it is)
        if (nvs_set_blob(nvs, keys[i], out, packed) == ESP_OK) rewritten++;
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    free(keys);
    free(family);
#endif
    if (rewritten) ESP_LOGW(TAG, "Settings store: %d entries of an earlier version rewritten to take less room", rewritten);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    bool store_reset = false;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The store cannot be used as it is and has to be emptied - everything the controller kept. Its contents are
        // copied aside first (store_rescue), so they are not gone for good.
        ESP_LOGE(TAG, "Settings store not usable (0x%X) - it is emptied", err);
        store_rescue();
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
        store_reset = true;
    }
    ESP_ERROR_CHECK(err);
    store_cleanup();
    store_compact();
#ifdef NANO_WDTEST
    store_rescue();   // development: write the copy now, to read it back and compare
#endif

    s_events = xQueueCreate(64, sizeof(app_event_t));
    s_state.tuner_base_hz = 440;
    const esp_timer_create_args_t timer = { .callback = refresh_cb, .name = "refresh" };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_refresh_timer));
    const esp_timer_create_args_t param_timer = { .callback = param_timer_cb, .name = "param" };
    ESP_ERROR_CHECK(esp_timer_create(&param_timer, &s_param_timer));
    const esp_timer_create_args_t read_timer = { .callback = read_timer_cb, .name = "read" };
    ESP_ERROR_CHECK(esp_timer_create(&read_timer, &s_read_timer));
    const esp_timer_create_args_t jack_timer = { .callback = jack_timer_cb, .name = "jack" };
    ESP_ERROR_CHECK(esp_timer_create(&jack_timer, &s_jack_timer));
    const esp_timer_create_args_t exp_retry_timer = { .callback = exp_retry_timer_cb, .name = "exp_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&exp_retry_timer, &s_exp_retry_timer));
    const esp_timer_create_args_t usb_timer = { .callback = usb_timer_cb, .name = "usb" };
    ESP_ERROR_CHECK(esp_timer_create(&usb_timer, &s_usb_timer));
    const esp_timer_create_args_t fxp_timer = { .callback = fxp_timer_cb, .name = "fxp" };
    ESP_ERROR_CHECK(esp_timer_create(&fxp_timer, &s_fxp_timer));
    const esp_timer_create_args_t rp_timer = { .callback = rp_timer_cb, .name = "rp" };
    ESP_ERROR_CHECK(esp_timer_create(&rp_timer, &s_rp_timer));
    const esp_timer_create_args_t source_timer = { .callback = source_timer_cb, .name = "source" };
    ESP_ERROR_CHECK(esp_timer_create(&source_timer, &s_src_timer));
    const esp_timer_create_args_t exp_timer = { .callback = exp_timer_cb, .name = "exp" };
    ESP_ERROR_CHECK(esp_timer_create(&exp_timer, &s_exp_timer));
    const esp_timer_create_args_t ab_timer = { .callback = ab_timer_cb, .name = "ab" };
    ESP_ERROR_CHECK(esp_timer_create(&ab_timer, &s_ab_timer));
    const esp_timer_create_args_t state_timer = { .callback = state_timer_cb, .name = "state" };
    ESP_ERROR_CHECK(esp_timer_create(&state_timer, &s_state_timer));
    const esp_timer_create_args_t gate_timer = { .callback = midi_gate_cb, .name = "midi_gate" };
    ESP_ERROR_CHECK(esp_timer_create(&gate_timer, &s_midi_gate_timer));
    const esp_timer_create_args_t app_read_timer = { .callback = app_read_cb, .name = "app_read" };
    ESP_ERROR_CHECK(esp_timer_create(&app_read_timer, &s_app_read_timer));
    const esp_timer_create_args_t app_notify_timer = { .callback = app_notify_cb, .name = "app_notify" };
    ESP_ERROR_CHECK(esp_timer_create(&app_notify_timer, &s_app_notify_timer));
    const esp_timer_create_args_t library_timer = { .callback = library_timer_cb, .name = "library" };
    ESP_ERROR_CHECK(esp_timer_create(&library_timer, &s_library_timer));
    banks_load();
    setlist_load();
    looper_tiles_load();
    layout_load();
    s_scenes = heap_caps_calloc(UI_SCENES, sizeof(scene_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    configASSERT(s_scenes);
    const esp_timer_create_args_t scene_timer = { .callback = scene_timer_cb, .name = "scene" };
    ESP_ERROR_CHECK(esp_timer_create(&scene_timer, &s_scene_timer));
    const esp_timer_create_args_t wah_timer = { .callback = wah_timer_cb, .name = "wah" };
    ESP_ERROR_CHECK(esp_timer_create(&wah_timer, &s_wah_timer));
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
    strlcpy(s_start_reason, reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[reason] : "?", sizeof(s_start_reason));
    printf("\nNano Cortex Controller %s - started after: %s\n", esp_app_get_description()->version, s_start_reason);
    // Bluetooth first: its host task and buffers need internal RAM, which the screen's many small objects would
    // otherwise take (they move to PSRAM when internal RAM runs short). Its events wait in the queue until the
    // app task runs, after the screen is built.
    app_link_start(on_app_write, on_app_state);
    nano_link_start(on_message, on_link);
    midi_ble_start(on_midi, on_midi_change);
    midi_ble_allow(false);   // first the Nano and its presets
    phone_midi_start(on_phone_midi, on_phone_state);
    esp_timer_start_once(s_midi_gate_timer, MIDI_GATE_US);
    const esp_timer_create_args_t pair_hint_timer = { .callback = pair_hint_cb, .name = "pair" };
    ESP_ERROR_CHECK(esp_timer_create(&pair_hint_timer, &s_pair_hint_timer));
    esp_timer_start_once(s_pair_hint_timer, PAIR_HINT_US);

    pedal_assets_init();   // pictures for the pedal view, if the flash memory holds the file with them
    ui_init(command, on_param, on_text);
    ui_set_scene_names_source(scene_names_read);
    uint8_t order[FOOTSWITCH_COUNT];
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) order[p] = s_layout[p];
    ui_set_layout(s_layout_count, order, s_switch1);
    ui_set_pedal_view(s_pedal_view);
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
    s_app_beat = xTaskGetTickCount();
    const esp_timer_create_args_t watchdog_timer = { .callback = watchdog_cb, .name = "watchdog" };
    ESP_ERROR_CHECK(esp_timer_create(&watchdog_timer, &s_watchdog_timer));
    esp_timer_start_periodic(s_watchdog_timer, 1000 * 1000);
    if (store_reset) ui_show_message("The settings memory could not be read and was emptied. A copy of the old one is kept - see the manual.");
    int hung = watchdog_restarted();
    if (hung) {
        ESP_LOGW(TAG, "The controller restarted itself: its %s task had hung", hung == 1 ? "app" : "screen");
        snprintf(s_start_reason, sizeof(s_start_reason), "a hang (%s) - restarted by itself", hung == 1 ? "app" : "screen");
        ui_show_message(hung == 1 ? "The controller restarted itself after a hang (app)." : "The controller restarted itself after a hang (screen).");
    }
#endif
    ESP_LOGI(TAG, "Free internal RAM %u KB (largest block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#ifndef NANO_WEB
    nvs_stats_t stats;   // banks, FX presets, second effects and scenes share this store
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        ESP_LOGI(TAG, "Settings store: %u entries used, room for %u more (%d %% full)", (unsigned)stats.used_entries,
                 (unsigned)stats.available_entries, store_percent(NULL, NULL));
    }
#endif
}
