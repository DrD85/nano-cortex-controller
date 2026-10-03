// Nano Cortex Controller.
//
// Connects to the Nano Cortex over Bluetooth LE, reads the preset names, the current preset with its
// FX, capture and cab, and switches presets and FX. Footswitches (SX1509), the touch screen and the
// serial monitor (h = help) control it:
//   footswitch 1   preset mode <-> FX mode
//   footswitch 2   tuner on/off
//   preset mode    3-8 = the six presets of the bank; the arrow buttons on the screen change the bank
//                  (own banks: long press on a preset tile picks preset, colour and symbol; stored on the board)
//   FX mode        3-7 = FX slots 1-5 on/off, 8 = reverb mix Pos 1 <-> Pos 2, or reverb A <-> B
// A long press on an FX tile opens the FX editor (model and parameters of that slot), on tile 8 the reverb
// dialog, on tiles 1-2 the footswitch learn.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "board.h"
#include "footswitches.h"
#include "library.h"
#include "midi_ble.h"
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
#define MIDI_GATE_US (20 * 1000 * 1000)

static bool s_fx_mode;          // footswitches 3-8: FX (true) or bank + presets (false)
static bool s_footswitches;     // SX1509 found

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

// Reverb mix switch: Pos 1 / Pos 2 come from the preset's expression assignment for the reverb's Amount
// (set with the Pos 1 / Pos 2 sliders in the editor). The switch sets the reverb's Mix to that value.
static struct {
    int slot;          // FX slot of the reverb, -1 = none
    bool known;
    float pos[2];
    int active;        // -1 = as stored in the preset, 0 = Pos 1, 1 = Pos 2
    int pending;       // expression requests without reply; only the last reply counts
    int64_t request_us;
    // Long press on the mix tile: Pos 1 / Pos 2 are edited (moving a slider plays that mix) and saved.
    // The save rewrites all expression assignments of the preset, so the last reply is kept for it.
    uint8_t reply[256];
    size_t reply_len;
    bool reply_valid;      // reply belongs to the current preset
    bool touched;          // mix changed while editing: set back to the active position when closed
    int verify;            // after a save: expected (Pos 1 << 8 | Pos 2) + 1, 0 = none
    int verify_preset;
} s_mix = { .slot = -1, .active = -1 };

// FX editor (long press on an FX tile). Parameter values are only read for FX that are on;
// moving a control sends at most one value per PARAM_SEND_MS, plus the last one.
#define PARAM_SEND_MS 40
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

// USB playback volume (dB) from the Nano's settings; the slider sends at most one value per PARAM_SEND_MS.
static struct {
    bool known;
    float db;
    bool pending, throttling;
    float pending_db;
} s_usb;
static esp_timer_handle_t s_usb_timer, s_exp_timer, s_rev_timer;

// Reverb A/B (long press on the mix tile): the Nano has one reverb slot, so footswitch 8 swaps the model in
// that slot between the preset's reverb (A) and a second one (B) and sends the stored values of the other one.
// Reverb B (model and values) is stored per preset on the controller; it is edited in the FX editor while
// it is active. Before each swap the values of the running reverb are read, so edits are kept.
typedef struct {
    uint32_t type;                    // model of reverb B, 0 = off (footswitch 8 = mix Pos 1 / Pos 2)
    uint8_t count;                    // stored values, 0 = model defaults
    float values[NANO_MAX_PARAMS];
} rev_b_t;
enum { REV_IDLE, REV_TURNING_ON, REV_READING, REV_LOADING };
#define REV_SEND_CHUNK 4              // parameter values per step (the BLE write queue holds 16)
static struct {
    int preset;                       // preset the data belongs to
    rev_b_t b;
    uint32_t a_type;                  // preset's own reverb, read at the first swap
    uint8_t a_count;
    float a_values[NANO_MAX_PARAMS];
    int active;                       // 0 = A, 1 = B
    int slot, target, phase, sent;
    bool open_editor;                 // open the FX editor once B is loaded
} s_rev;

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

static void on_footswitch(int number)
{
    command('w', number);
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

static void usb_timer_cb(void *arg)
{
    command('G', 0);
}

static void exp_timer_cb(void *arg)
{
    command('J', 0);
}

static void midi_gate_cb(void *arg)
{
    command('q', 0);
}

static void rev_timer_cb(void *arg)
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

static void show(void)
{
    ui_view_t view = {
        .fx_mode = s_fx_mode,
        .bank = s_bank,
        .mix_slot = s_mix.slot,
        .mix_known = s_mix.known,
        .mix_pos = { s_mix.pos[0], s_mix.pos[1] },
        .mix_active = s_mix.active,
        .rev_b_type = s_rev.b.type,
        .rev_active = s_rev.active,
    };
    for (int i = 0; i < BANK_SLOTS; i++) {
        view.bank_presets[i] = s_banks[s_bank][i].preset;
        view.bank_colors[i] = s_banks[s_bank][i].color;
        view.bank_icons[i] = s_banks[s_bank][i].icon;
    }
    ui_show_state(&s_state, &view);
}

static void editor_show(void)
{
    if (s_edit.slot < 0) return;
    ui_fx_editor_show(s_edit.slot, s_state.fx_type[s_edit.slot], s_state.fx_on[s_edit.slot],
                      s_edit.known ? s_edit.values : NULL, s_edit.count);
}

static void editor_close(void)
{
    if (s_edit.slot < 0) return;
    s_edit.slot = -1;
    ui_fx_editor_close();
}

// Reads again later (after a model change or after switching the FX on).
static void editor_read_later(int ms)
{
    esp_timer_stop(s_read_timer);
    esp_timer_start_once(s_read_timer, (uint64_t)ms * 1000);
}

// ---- actions ----

static void send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    if (!nano_link_send(label, type, payload, len)) ESP_LOGW(TAG, "Could not queue: %s", label);
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

static void select_preset(int preset)
{
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

static void toggle_fx(int slot)
{
    if (!s_state.fx_known || !s_state.fx_type[slot]) {
        ESP_LOGW(TAG, "%s is empty", NANO_FX_SLOT_NAMES[slot]);
        return;
    }
    bool on = !s_state.fx_on[slot];
    uint8_t buf[8];
    char label[32];
    snprintf(label, sizeof(label), "%s %s", NANO_FX_SLOT_NAMES[slot], on ? "ON" : "OFF");
    send(label, NANO_MSG_BYPASS, buf, nano_fx_bypass(slot, on, buf));
    s_state.fx_on[slot] = on;
    show();
    schedule_refresh(300);
    if (slot == s_edit.slot) {
        if (!on) s_edit.known = false;
        editor_show();
        if (on) editor_read_later(400);
    }
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

static void send_param(int slot, int param, float value)
{
    uint8_t buf[16];
    char label[40];
    snprintf(label, sizeof(label), "%s parameter %d", NANO_FX_SLOT_NAMES[slot], param);
    send(label, NANO_MSG_FX_VALUE, buf, nano_fx_param(slot, param, value, buf));
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

static void editor_open(int slot)
{
    if (slot < 0 || slot >= NANO_FX_SLOTS || !s_state.fx_known) return;
    s_edit.slot = slot;
    s_edit.known = false;
    s_edit.count = 0;
    editor_show();
    editor_read();
}

static void rev_model_chosen(int slot, uint32_t type);

static void editor_choose_model(uint32_t type)
{
    int slot = s_edit.slot;
    if (slot < 0 || !nano_fx_model(type)) return;
    rev_model_chosen(slot, type);
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

static void request_mix_range(void)
{
    uint8_t buf[8];
    s_mix.known = false;
    s_mix.active = -1;
    s_mix.reply_valid = false;
    s_mix.touched = false;
    s_mix.verify = 0;
    s_mix.pending++;
    s_mix.request_us = esp_timer_get_time();
    send("Expression request", NANO_MSG_EXP_REQUEST, buf, nano_exp_request(s_state.current_preset, buf));
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
                 : "no Pos 1 / Pos 2 set for the reverb (expression Amount) in this preset");
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

// Pos 1 / Pos 2 editor: SAVE (Pos 1 << 8 | Pos 2, 0-255 each).
static void mix_save(int arg)
{
    uint8_t pos1 = (uint8_t)(arg >> 8), pos2 = (uint8_t)arg;
    if (s_mix.slot < 0 || !s_mix.reply_valid) {
        ui_show_message("Not saved: the expression assignments of this preset are not read yet.");
        return;
    }
    uint8_t buf[256];
    size_t n = nano_exp_save_amount_range(s_mix.reply, s_mix.reply_len, s_state.current_preset, s_mix.slot,
                                          pos1, pos2, buf, 251);
    if (!n) {
        ui_show_message("Not saved: too many expression assignments for one message.");
        return;
    }
    char label[64];
    snprintf(label, sizeof(label), "Save reverb Pos 1 %ld%% / Pos 2 %ld%% (preset %d)",
             lroundf(pos1 * 100 / 255.0f), lroundf(pos2 * 100 / 255.0f), s_state.current_preset);
    send(label, NANO_MSG_EXP_SAVE, buf, n);
    s_mix.pos[0] = pos1 / 255.0f;
    s_mix.pos[1] = pos2 / 255.0f;
    s_mix.known = true;
    s_mix.touched = true;
    s_mix.verify = (pos1 << 8 | pos2) + 1;
    s_mix.verify_preset = s_state.current_preset;
    esp_timer_start_once(s_exp_timer, 400 * 1000);   // then read back and compare, as the editor does
    show();
}

static void mix_verify_request(void)
{
    if (!s_mix.verify || s_mix.verify_preset != s_state.current_preset) return;
    uint8_t buf[8];
    s_mix.pending++;
    s_mix.request_us = esp_timer_get_time();
    send("Expression check", NANO_MSG_EXP_REQUEST, buf, nano_exp_request(s_state.current_preset, buf));
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

// ---- reverb A/B ----

static void rev_key(int preset, char *key)
{
    snprintf(key, 16, "rvb%02d", preset);
}

static void rev_store(void)
{
    char key[16];
    nvs_handle_t nvs;
    if (!s_rev.preset || nvs_open(BANK_STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    rev_key(s_rev.preset, key);
    esp_err_t err = s_rev.b.type ? nvs_set_blob(nvs, key, &s_rev.b, sizeof(s_rev.b)) : nvs_erase_key(nvs, key);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(nvs) != ESP_OK) ESP_LOGW(TAG, "Could not store reverb B");
    nvs_close(nvs);
}

static void rev_load(int preset)
{
    char key[16];
    memset(&s_rev, 0, sizeof(s_rev));
    s_rev.preset = preset;
    nvs_handle_t nvs;
    if (nvs_open(BANK_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    rev_key(preset, key);
    size_t size = sizeof(s_rev.b);
    if (nvs_get_blob(nvs, key, &s_rev.b, &size) != ESP_OK || size != sizeof(s_rev.b) || !nano_fx_model(s_rev.b.type)) {
        memset(&s_rev.b, 0, sizeof(s_rev.b));
    }
    nvs_close(nvs);
    if (s_rev.b.type) ESP_LOGI(TAG, "Reverb B of preset %d: %s", preset, nano_fx_name(s_rev.b.type));
}

// The FX editor shows reverb B: keep its values (called when the editor closes or the preset changes).
// The model of B only changes by an explicit choice (dialog, or the editor's model list while B runs).
static void rev_capture_editor(void)
{
    if (s_rev.active != 1 || s_edit.slot < 0 || s_edit.slot != s_rev.slot || !s_edit.known) return;
    s_rev.b.count = (uint8_t)s_edit.count;
    memcpy(s_rev.b.values, s_edit.values, sizeof(s_rev.b.values));
    rev_store();
}

// A reverb model was chosen in the FX editor: it replaces the running reverb (A or B).
static void rev_model_chosen(int slot, uint32_t type)
{
    const nano_fx_model_t *model = nano_fx_model(type);
    if (!s_rev.b.type || slot != s_rev.slot || !model || model->icon != NANO_ICON_REVERB) return;
    if (s_rev.active == 1) {
        s_rev.b.type = type;
        s_rev.b.count = 0;
        rev_store();
    } else if (s_rev.a_type) {
        s_rev.a_type = type;
        s_rev.a_count = 0;
    }
}

static void rev_step_later(int ms)
{
    esp_timer_stop(s_rev_timer);
    esp_timer_start_once(s_rev_timer, (uint64_t)ms * 1000);
}

// Puts the target reverb into the slot: the model now, its values in the following steps.
static void rev_load_target(void)
{
    uint32_t type = s_rev.target ? s_rev.b.type : s_rev.a_type;
    uint8_t buf[16];
    char label[48];
    snprintf(label, sizeof(label), "Reverb %c: %s", s_rev.target ? 'B' : 'A', nano_fx_name(type));
    send(label, NANO_MSG_FX_TYPE, buf, nano_fx_model_select(s_rev.slot, type, buf));
    s_state.fx_type[s_rev.slot] = type;
    s_rev.active = s_rev.target;
    s_rev.phase = REV_LOADING;
    s_rev.sent = 0;
    rev_step_later(400);   // the Nano needs a moment for the model change
    show();
}

static void rev_read(void)
{
    uint8_t buf[8];
    s_rev.phase = REV_READING;
    send("Reverb values request", NANO_MSG_FX_PARAMS_REQUEST, buf, nano_fx_params_request(s_rev.slot, buf));
    rev_step_later(1500);   // timeout
}

// Footswitch 8 with a reverb B: swap A <-> B.
static void rev_toggle(void)
{
    if (s_rev.phase != REV_IDLE) return;
    if (s_mix.slot < 0) {
        ui_show_message("No reverb in this preset.");
        return;
    }
    s_rev.slot = s_mix.slot;
    s_rev.target = s_rev.active ? 0 : 1;
    rev_capture_editor();
    if (!s_state.fx_on[s_rev.slot]) {   // values can only be read from an active effect
        toggle_fx(s_rev.slot);
        s_rev.phase = REV_TURNING_ON;
        rev_step_later(400);
        return;
    }
    rev_read();
}

// Values of the running reverb arrived: keep them, then load the other one.
static void rev_values_read(const uint8_t *payload, size_t len)
{
    esp_timer_stop(s_rev_timer);
    float values[NANO_MAX_PARAMS];
    int count = nano_fx_params_values(payload, len, values, NANO_MAX_PARAMS);
    if (count <= 0) {
        s_rev.phase = REV_IDLE;
        ui_show_message("Reverb not switched: the Nano sent no values.");
        return;
    }
    if (s_rev.active == 0) {
        if (!s_rev.a_type) s_rev.a_type = s_state.fx_type[s_rev.slot];   // the preset's reverb, kept from now on
        s_rev.a_count = (uint8_t)count;
        memcpy(s_rev.a_values, values, sizeof(values));
    } else {   // values only: a state reply can still name the previous model
        s_rev.b.count = (uint8_t)count;
        memcpy(s_rev.b.values, values, sizeof(values));
        rev_store();
    }
    rev_load_target();
}

static void rev_step(void)
{
    switch (s_rev.phase) {
    case REV_TURNING_ON:
        rev_read();
        break;
    case REV_READING:
        s_rev.phase = REV_IDLE;
        ui_show_message("Reverb not switched: the Nano did not send the reverb's values.");
        break;
    case REV_LOADING: {
        int count = s_rev.target ? s_rev.b.count : s_rev.a_count;
        const float *values = s_rev.target ? s_rev.b.values : s_rev.a_values;
        for (int n = 0; n < REV_SEND_CHUNK && s_rev.sent < count; n++, s_rev.sent++) send_param(s_rev.slot, s_rev.sent, values[s_rev.sent]);
        if (s_rev.sent < count) {
            rev_step_later(60);
            break;
        }
        s_rev.phase = REV_IDLE;
        ESP_LOGI(TAG, "Reverb %c active (%d values)", s_rev.active ? 'B' : 'A', count);
        schedule_refresh(300);
        if (s_rev.open_editor) {
            s_rev.open_editor = false;
            editor_open(s_rev.slot);
        } else if (s_edit.slot == s_rev.slot) {
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

// From the reverb dialog: reverb B model for this preset (0 = none: footswitch 8 is the mix switch again).
static void rev_set_b(uint32_t type)
{
    if (type && !nano_fx_model(type)) return;
    if (s_rev.phase != REV_IDLE) {
        ui_show_message("Reverb is switching - try again in a moment.");
        return;
    }
    if (type == s_rev.b.type) return;
    s_rev.b.type = type;
    s_rev.b.count = 0;
    rev_store();
    if (s_rev.active == 1) {   // B is running: show the new choice, or go back to A
        s_rev.target = type ? 1 : 0;
        if (type || s_rev.a_type) rev_load_target();
        else s_rev.active = 0;
    }
    show();
}

// From the reverb dialog: edit reverb B in the FX editor (B is loaded first).
static void rev_edit_b(void)
{
    if (!s_rev.b.type) return;
    if (s_rev.active == 1) editor_open(s_rev.slot);
    else {
        s_rev.open_editor = true;
        rev_toggle();
    }
}

static void set_tuner(bool on)
{
    uint8_t buf[16];
    send(on ? "Tuner on" : "Tuner off", NANO_MSG_TUNER_MODE, buf, nano_tuner_mode(on, s_state.tuner_base_hz, buf));
    s_tuner_on = on;
    ui_show_tuner(on);
}

static void select_capture(int slot)
{
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
    int icon = (arg >> 18) & 0xF, bank = (arg >> 14) & 0xF, slot = (arg >> 11) & 0x7, color = (arg >> 7) & 0xF, preset = arg & 0x7F;
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

static void handle_switch(int number)
{
    if (number == 1) {
        s_fx_mode = !s_fx_mode;
        if (!s_fx_mode) follow_preset_with_bank(s_state.current_preset);
        ESP_LOGI(TAG, "%s mode", s_fx_mode ? "FX" : "Preset");
        show();
    } else if (number == 2) {
        set_tuner(!s_tuner_on);
    } else if (s_fx_mode) {
        if (number <= 7) toggle_fx(number - 3);
        else if (s_rev.b.type) rev_toggle();
        else toggle_mix();
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
// In addition CC 50-57 (value 64-127) press footswitches 1-8. All channels.
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
    if (!nano_link_ready()) return;
    if (type == 0xC0) {
        if (d1 < NANO_PRESETS) select_preset(d1 + 1);
    } else if (type == 0xB0 && d1 >= 37 && d1 <= 41) {
        int slot = d1 - 37;
        if (s_state.fx_known && s_state.fx_type[slot] && s_state.fx_on[slot] != (d2 >= 64)) toggle_fx(slot);
    } else if (type == 0xB0 && d1 >= 50 && d1 <= 57 && d2 >= 64) {
        handle_switch(d1 - 49);
    }
}

static void handle_command(char c, int arg)
{
    if (c == 'h') { print_help(); return; }
    if (c == 'l') { print_presets(); return; }
    if (c == 'D' || c == 'Z') { footswitch_learn(c, arg); return; }   // works without the Nano
    if (c == 'X' || c == 'P' || c == 'N') { midi_command(c, arg); return; }
    if (c == 'q') { midi_ble_allow(true); return; }   // MIDI gate timer: the Nano did not come
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
        if (s_rev.b.type) rev_toggle();
        else toggle_mix();
        break;
    case 'w':
        if (arg >= 1 && arg <= FOOTSWITCH_COUNT) handle_switch(arg);
        break;
    case 'O': editor_open(arg); break;
    case 'E':
        rev_capture_editor();
        s_edit.slot = -1;
        break;
    case 'A': rev_set_b((uint32_t)arg); break;
    case 'H': rev_edit_b(); break;
    case 'F': rev_step(); break;
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
    case 'Q': mix_preview(arg); break;
    case 'W': mix_save(arg); break;
    case 'J': mix_verify_request(); break;
    case 'K': mix_editor_closed(); break;
    default: break;
    }
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
                break;
            }
            s_expect_preset = 0;
        }
        bool names = nano_state_apply(&s_state, payload, len);
        // After a reverb swap the slot holds the reverb that was loaded (a reply sent before the model change
        // still names the previous one).
        if (s_rev.preset == s_state.current_preset) {
            if (s_rev.active == 1 && s_rev.b.type) s_state.fx_type[s_rev.slot] = s_rev.b.type;
            else if (s_rev.active == 0 && s_rev.a_type) s_state.fx_type[s_rev.slot] = s_rev.a_type;
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
            // Presets are in: MIDI may scan and connect now.
            esp_timer_stop(s_midi_gate_timer);
            midi_ble_allow(true);
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
                follow_preset_with_bank(s_state.current_preset);
                if (!s_library_requested) {
                    // The library is a large reply: ask once, after the first preset has been read.
                    s_library_requested = true;
                    esp_timer_start_once(s_library_timer, 1500 * 1000);
                }
                rev_capture_editor();   // reverb B edits of the previous preset
                esp_timer_stop(s_rev_timer);
                rev_load(s_state.current_preset);
                editor_close();
                request_mix_range();
            }
            show();
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
        s_mix.known = s_mix.slot >= 0 && nano_exp_amount_range(payload, len, s_mix.slot, s_mix.pos);
        s_mix.reply_valid = len <= sizeof(s_mix.reply);
        if (s_mix.reply_valid) {
            memcpy(s_mix.reply, payload, len);
            s_mix.reply_len = len;
        }
        if (s_mix.known) {
            ESP_LOGI(TAG, "Reverb mix Pos 1 %ld%%, Pos 2 %ld%%", lroundf(s_mix.pos[0] * 100), lroundf(s_mix.pos[1] * 100));
        }
        if (s_mix.verify) {
            int expected = s_mix.verify - 1;
            bool same = s_mix.known && lroundf(s_mix.pos[0] * 255) == expected >> 8 && lroundf(s_mix.pos[1] * 255) == (expected & 0xff);
            s_mix.verify = 0;
            ESP_LOGI(TAG, "Reverb Pos 1 / Pos 2 check: %s", same ? "matches" : "DIFFERENT");
            ui_show_message(same ? "Reverb Pos 1 / Pos 2 saved." : "Not confirmed: the preset reports other Pos 1 / Pos 2 values.");
        }
        show();
        break;
    }
    case NANO_MSG_TUNER_MODE: {   // tuner switched on the Nano itself
        bool on = nano_field_varint(payload, len, 4) != 0;
        ESP_LOGI(TAG, "Tuner %s on the Nano", on ? "opened" : "closed");
        s_tuner_on = on;
        ui_show_tuner(on);
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
        if (s_rev.phase == REV_READING) {
            rev_values_read(payload, len);
            break;
        }
        int slot = s_edit.request_slot;
        s_edit.request_slot = -1;
        if (slot < 0 || slot != s_edit.slot) break;   // editor closed or another slot meanwhile
        s_edit.count = nano_fx_params_values(payload, len, s_edit.values, NANO_MAX_PARAMS);
        s_edit.known = s_edit.count > 0;
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
        schedule_refresh(450);
        break;
    }
    case NANO_MSG_SET_PRESET_SLOTS_RESPONSE:
        ESP_LOGI(TAG, "Preset change %s", nano_field_varint(payload, len, 4) == 1 ? "confirmed" : "FAILED");
        break;
    case NANO_MSG_SETTINGS_RESPONSE: {
        // 0 dB is the protobuf default and is then left out of the reply.
        s_usb.db = 0;
        bool present = nano_settings_usb_gain(payload, len, &s_usb.db);
        s_usb.known = true;
        ESP_LOGI(TAG, "USB playback volume %.1f dB%s", s_usb.db, present ? "" : " (not in the reply)");
        ui_set_usb_gain(s_usb.db);
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
        break;
    default:
        ESP_LOGI(TAG, "Message type %lu (%u bytes)", (unsigned long)type, (unsigned)len);
        break;
    }
}

// ---- tasks ----

static void app_task(void *arg)
{
    app_event_t ev;
    for (;;) {
        xQueueReceive(s_events, &ev, portMAX_DELAY);
        switch (ev.kind) {
        case EV_LINK_UP:
            ESP_LOGI(TAG, "Connected to the Nano Cortex - reading presets");
            ui_set_link(true);
            s_incomplete_retries = 0;
            request_full_state();
            break;
        case EV_LINK_DOWN:
            ESP_LOGW(TAG, "Connection lost - searching again");
            s_waiting_full_state = false;
            midi_ble_allow(false);   // until the Nano is back (or the gate timer opens it)
            esp_timer_stop(s_midi_gate_timer);
            esp_timer_start_once(s_midi_gate_timer, MIDI_GATE_US);
            s_tuner_on = false;
            s_last_preset = 0;
            s_mix.pending = 0;
            s_usb.known = false;
            s_edit.slot = -1;
            s_edit.request_slot = -1;
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
            free(ev.data);
            break;
        case EV_MIDI:
            handle_midi(ev.arg);
            break;
        }
    }
}

// Characters typed into the serial monitor: letters are commands. Digits select a preset: the number is
// shown while typing and taken on Enter or after a short pause, so Enter is not required.
// The console runs on the board's native "USB" port (USB Serial/JTAG) or, if configured, on the "UART" port.
#define DIGIT_PAUSE_MS 1200

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
        command((char)tolower(c), 0);
    }
}

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
    const esp_timer_create_args_t usb_timer = { .callback = usb_timer_cb, .name = "usb" };
    ESP_ERROR_CHECK(esp_timer_create(&usb_timer, &s_usb_timer));
    const esp_timer_create_args_t exp_timer = { .callback = exp_timer_cb, .name = "exp" };
    ESP_ERROR_CHECK(esp_timer_create(&exp_timer, &s_exp_timer));
    const esp_timer_create_args_t rev_timer = { .callback = rev_timer_cb, .name = "rev" };
    ESP_ERROR_CHECK(esp_timer_create(&rev_timer, &s_rev_timer));
    const esp_timer_create_args_t gate_timer = { .callback = midi_gate_cb, .name = "midi_gate" };
    ESP_ERROR_CHECK(esp_timer_create(&gate_timer, &s_midi_gate_timer));
    const esp_timer_create_args_t library_timer = { .callback = library_timer_cb, .name = "library" };
    ESP_ERROR_CHECK(esp_timer_create(&library_timer, &s_library_timer));
    banks_load();

    board_display_init();
    ui_init(command, on_param, on_text);
    s_footswitches = footswitches_start(board_i2c_bus(), on_footswitch, on_footswitch_learned);

    xTaskCreate(app_task, "app", 6144, NULL, 4, NULL);
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);

    static const char *const reasons[] = { "unknown", "power-on", "external pin", "software", "panic (crash)",
                                           "interrupt watchdog", "task watchdog", "other watchdog", "deep sleep",
                                           "brownout (supply voltage too low)", "SDIO", "USB", "JTAG", "eFuse",
                                           "power glitch", "CPU lockup" };
    esp_reset_reason_t reason = esp_reset_reason();
    printf("\nNano Cortex Controller %s - started after: %s\n", esp_app_get_description()->version,
           reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[reason] : "?");
    nano_link_start(on_message, on_link);
    midi_ble_start(on_midi, on_midi_change);
    midi_ble_allow(false);   // first the Nano and its presets
    esp_timer_start_once(s_midi_gate_timer, MIDI_GATE_US);
}
