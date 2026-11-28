// Host test of main/main.c: the controller's own logic run on the computer against a stand-in for the Nano.
// main.c is taken in as it is (its browser build: no tasks, the event queue is emptied by web_app_pump); the screen,
// Bluetooth and the store are small stand-ins here, time is the test's own (advance()).
//
// What it checks (1.25):
//   - the reverb's two switches: the reverb's own switch only switches on and off while the reverb switch
//     (footswitch 8) swaps A / B, data stored by earlier versions included; a B chosen on the tile is the tile's
//   - "New preset" from the preset list: the standard board and the copy, what goes to the Nano and in which
//     order, what the controller keeps for the place, the preset the run started on left as it was - and the run
//     when the Nano does not take the save
// Run from the project folder:  tools/test_main/run.sh
//
// P4 board: the same test as the 4.3" board's (tools/test_main there), run against this project's main.c built as
// the firmware builds it (NANO_P4). What only the P4 has (USB audio, looper, player, Wi-Fi, the link's two ways) is
// a stand-in below, under "the P4's parts".
#define MALLOC_CAP_SPM 0   // (P4: app_main keeps the DSI's memory; not in the browser build's esp_heap_caps.h)
#include "../../main/main.c"

#include <stdarg.h>

// ---- the test's clock, timers, queue, store ----

static int64_t t_now_us = 1000000;
int64_t esp_timer_get_time(void) { return t_now_us; }
uint32_t esp_log_timestamp(void) { return (uint32_t)(t_now_us / 1000); }
void esp_log_buffer_hex(const char *tag, const void *buffer, size_t len) { (void)tag; (void)buffer; (void)len; }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void esp_restart(void) { printf("esp_restart called\n"); exit(2); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "ESP_ERR"; }
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = { .version = "test", .project_name = "nano_controller" };
    return &desc;
}
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
void heap_caps_free(void *ptr) { free(ptr); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 1 << 20; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 1 << 20; }

struct esp_timer {
    esp_timer_create_args_t args;
    bool active;
    int64_t due_us;
    struct esp_timer *next;
};
static struct esp_timer *t_timers;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    struct esp_timer *t = calloc(1, sizeof(*t));
    t->args = *args;
    t->next = t_timers;
    t_timers = t;
    *out = t;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    if (t->active) return ESP_ERR_INVALID_STATE;   // (as ESP-IDF: a running timer is not started again)
    t->active = true;
    t->due_us = t_now_us + (int64_t)timeout_us;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us) { return esp_timer_start_once(t, period_us); }
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t->active) return ESP_ERR_INVALID_STATE;
    t->active = false;
    return ESP_OK;
}
bool esp_timer_is_active(esp_timer_handle_t t) { return t->active; }

struct web_queue { size_t item, length, count, head; uint8_t *data; };
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct web_queue *q = calloc(1, sizeof(*q));
    q->item = item_size;
    q->length = length;
    q->data = malloc((size_t)length * item_size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->count == q->length) return pdFALSE;
    memcpy(q->data + ((q->head + q->count) % q->length) * q->item, item, q->item);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->count) return pdFALSE;
    memcpy(item, q->data + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->length;
    q->count--;
    return pdTRUE;
}

#define STORE_MAX 256
static struct { char key[16]; uint8_t *data; size_t len; } t_store[STORE_MAX];
static int t_store_writes;

static int store_find(const char *key)
{
    for (int i = 0; i < STORE_MAX; i++) if (t_store[i].data && !strcmp(t_store[i].key, key)) return i;
    return -1;
}
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out) { (void)name; (void)mode; *out = 1; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    (void)handle;
    int i = store_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *length = t_store[i].len; return ESP_OK; }
    if (*length < t_store[i].len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, t_store[i].data, t_store[i].len);
    *length = t_store[i].len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    int i = store_find(key);
    for (int k = 0; i < 0 && k < STORE_MAX; k++) if (!t_store[k].data) i = k;
    if (i < 0) return ESP_ERR_NO_MEM;
    free(t_store[i].data);
    strlcpy(t_store[i].key, key, sizeof(t_store[i].key));
    t_store[i].data = malloc(length ? length : 1);
    memcpy(t_store[i].data, value, length);
    t_store[i].len = length;
    t_store_writes++;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    int i = store_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    free(t_store[i].data);
    t_store[i].data = NULL;
    t_store_writes++;
    return ESP_OK;
}

// ---- the screen, MIDI, footswitches, the app: stand-ins ----

static char t_message[200];       // the last short message on the screen
static ui_view_t t_view;          // the view of the last ui_show_state
static nano_state_t t_shown;
static int t_shows;

void ui_init(ui_command_cb on_command, ui_param_cb on_param, ui_text_cb on_text) { (void)on_command; (void)on_param; (void)on_text; }
uint32_t ui_stalled_ms(void) { return 0; }
void ui_set_scene_names_source(ui_scene_names_cb cb) { (void)cb; }
void ui_set_link(bool connected) { (void)connected; }
void ui_show_state(const nano_state_t *state, const ui_view_t *view) { t_shown = *state; t_view = *view; t_shows++; }
void ui_show_tuner(bool open) { (void)open; }
void ui_show_tuner_reading(const nano_tuner_reading_t *reading) { (void)reading; }
void ui_tuner_settings(float base_hz, bool muted) { (void)base_hz; (void)muted; }
void ui_fx_editor_show(int slot, uint32_t model, bool on, const float *values, int count) { (void)slot; (void)model; (void)on; (void)values; (void)count; }
void ui_fx_editor_close(void) { }
void ui_set_library(const nano_library_t *library) { (void)library; }
void ui_set_listening(char kind, int position, const char *name, bool pending) { (void)kind; (void)position; (void)name; (void)pending; }
void ui_set_tempo(const ui_tempo_t *t) { (void)t; }
void ui_show_rotate(const ui_rotate_t *r) { (void)r; }
void ui_set_cloud(bool on) { (void)on; }
void ui_set_nano_firmware(int major, int minor) { (void)major; (void)minor; }
void ui_set_usb_gain(float db) { (void)db; }
void ui_set_autowah(const ui_autowah_t *wah) { (void)wah; }
void ui_fx_presets_show(const char names[][16], int active, bool usable) { (void)names; (void)active; (void)usable; }
void ui_set_cab_settings(const float *values, bool enabled, const char *info) { (void)values; (void)enabled; (void)info; }
void ui_set_expression(const nano_exp_range_t *ranges, const nano_exp_switch_t *switches) { (void)ranges; (void)switches; }
void ui_set_pedal(int value, int min, int max) { (void)value; (void)min; (void)max; }
void ui_set_jack(int mode) { (void)mode; }
void ui_set_layout(int count, const uint8_t order[8], int switch1) { (void)count; (void)order; (void)switch1; }
void ui_set_layout_fx(const uint8_t order[8]) { (void)order; }
void ui_set_app(bool connected) { (void)connected; }
// (P4: the learn's answers are kept - learning all switches one after the other is checked below)
static int t_learn_next = -1;
static char t_learn_done[128];
void ui_learn_done(const char *message) { strlcpy(t_learn_done, message, sizeof(t_learn_done)); t_learn_next = 0; }
void ui_learn_next(int number, const char *message) { (void)message; t_learn_next = number; }
// (P4: the player on the screen and what the footswitches asked of it - checked below)
static int t_player_action = -1, t_player_toggles, t_player_hides;
void ui_player_action(int action) { t_player_action = action; }
void ui_player_hide(void) { t_player_hides++; }
void ui_set_midi(const ui_midi_t *midi) { (void)midi; }
void ui_splash_status(const char *status, const char *version) { (void)status; (void)version; }
void ui_splash_done(void) { }
void ui_splash_step(int step) { (void)step; }
void ui_show_message(const char *text) { strlcpy(t_message, text, sizeof(t_message)); }
void ui_capture_show(const ui_capture_t *c) { (void)c; }
void ui_capture_meter(int which, int value) { (void)which; (void)value; }
void ui_show_system(const char *text) { (void)text; }
void ui_flash_tile(int tile) { (void)tile; }
void ui_set_pedal_view(bool on) { (void)on; }

const char *nano_fx_real_name(uint32_t type) { (void)type; return NULL; }   // (the published names)
lv_display_t *board_display_init(void) { return NULL; }
i2c_master_bus_handle_t board_i2c_bus(void) { return NULL; }
void pedal_assets_init(void) { }
bool footswitches_start(i2c_master_bus_handle_t bus, footswitch_cb on_press, footswitch_learn_cb on_learn) { (void)bus; (void)on_press; (void)on_learn; return true; }
static int t_fsw_learn = -1;   // (P4: what the footswitch task was last asked to learn)
void footswitches_learn(int number) { t_fsw_learn = number; }
void footswitches_reset(void) { }
void midi_switches_start(footswitch_cb on_event, footswitch_learn_cb on_learn) { (void)on_event; (void)on_learn; }
bool midi_switches_take(uint8_t status, uint8_t data1, uint8_t data2) { (void)status; (void)data1; (void)data2; return false; }
void midi_switches_learn(int number) { (void)number; }
void midi_switches_reset(void) { }
bool midi_switches_describe(int number, char *text, size_t size) { (void)number; if (size) text[0] = 0; return false; }
void midi_ble_start(midi_message_cb on_message, midi_change_cb on_change) { (void)on_message; (void)on_change; }
void midi_ble_search(bool on) { (void)on; }
void midi_ble_allow(bool allowed) { (void)allowed; }
int midi_ble_devices(midi_device_t *out, int max) { (void)out; (void)max; return 0; }
bool midi_ble_connect(int index) { (void)index; return false; }
void midi_ble_forget(void) { }
bool midi_ble_status(char *name, size_t size) { if (size) name[0] = 0; return false; }
bool midi_ble_chocolate(void) { return false; }
void phone_midi_start(midi_message_cb on_message, phone_state_cb on_state) { (void)on_message; (void)on_state; }
bool phone_midi_connected(void) { return false; }
float phone_midi_interval_ms(void) { return 0; }
bool phone_midi_send(uint8_t status, uint8_t data1, uint8_t data2) { (void)status; (void)data1; (void)data2; return false; }
void app_link_start(app_write_cb on_write, app_state_cb on_state) { (void)on_write; (void)on_state; }
void app_link_enable(bool enabled) { (void)enabled; }
void app_link_set_cloud(bool on) { (void)on; }
void app_link_allow_phone(bool allowed) { (void)allowed; }
bool app_link_connected(void) { return false; }
void app_link_send(uint32_t type, const uint8_t *payload, size_t len) { (void)type; (void)payload; (void)len; }

// ---- the P4's parts: USB audio with looper, drums and player, Wi-Fi, updates, the link's two ways ----
// (none of it takes part in what is checked here)
const char *nano_link_way(void) { return "USB"; }
void ui_set_link_way(bool usb) { (void)usb; }
void ui_player_toggle(void) { t_player_toggles++; }
void ui_drum_menu_toggle(void) { }
void p4_audio_start(void) { }
void p4_audio_show(void) { }
float p4_audio_di_peak(void) { return 0; }
bool p4_looper_ready(void) { return false; }
void p4_looper_command(looper_cmd_t cmd) { (void)cmd; }
void p4_looper_info(looper_info_t *out) { memset(out, 0, sizeof(*out)); }
void p4_looper_set_level_db(float db) { (void)db; }
void p4_drums_start(bool on) { (void)on; }
bool p4_drums_on(void) { return false; }
void p4_parts_choose(int part) { (void)part; }
void p4_parts_pause(void) { }
void p4_net_text(char kind, const char *text) { (void)kind; (void)text; }
void p4_ota_start(void) { }

// ---- the Nano's stand-in ----
// 64 presets (name, five effects with on / off, the capture's knobs, what the pedal has), the one that plays as an
// edit buffer. It answers what the controller asks for, a moment later (replies are queued with their time).

typedef struct {
    char name[33];
    uint32_t fx[NANO_FX_SLOTS];
    bool on[NANO_FX_SLOTS];
    int amp[NANO_AMP_KNOBS], volume;
    bool pedal;                       // an assignment of the expression pedal (Pre FX 1's Amount)
} t_preset_t;
static struct {
    t_preset_t preset[NANO_PRESETS], now;
    int current;                      // 0-based
    bool dirty;
    bool ignore_save;                 // the Nano does not take a save (the name list stays as it was)
} t_nano;

#define SENT_MAX 256
static struct { uint32_t type; uint8_t payload[64]; size_t len; int64_t at_us; } t_sent[SENT_MAX];
static int t_sent_count;
static struct { uint32_t type; uint8_t *payload; size_t len; int64_t due_us; } t_replies[32];
static int t_reply_count;
static nano_message_cb t_on_message;
static nano_link_cb t_on_link;
static bool t_link;

static size_t put_v(uint8_t *out, uint64_t v)
{
    size_t n = 0;
    do {
        out[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) out[n] |= 0x80;
        n++;
    } while (v);
    return n;
}

static size_t put_field(uint8_t *out, int field, uint64_t v)
{
    size_t n = put_v(out, (uint64_t)field << 3);
    return n + put_v(out + n, v);
}

static void reply(uint32_t type, const uint8_t *payload, size_t len, int after_ms)
{
    if (t_reply_count == 32) { printf("too many replies waiting\n"); exit(2); }
    t_replies[t_reply_count].type = type;
    t_replies[t_reply_count].payload = malloc(len ? len : 1);
    memcpy(t_replies[t_reply_count].payload, payload, len);
    t_replies[t_reply_count].len = len;
    t_replies[t_reply_count].due_us = t_now_us + after_ms * 1000LL;
    t_reply_count++;
}

static void reply_state(bool full)
{
    static uint8_t buf[8192];
    size_t n = 0;
    const t_preset_t *p = &t_nano.now;
    for (int i = 0; i < NANO_AMP_KNOBS; i++) n += put_field(buf + n, 3 + i, (uint64_t)p->amp[i]);
    n += put_field(buf + n, 13, (uint64_t)t_nano.current);
    if (full) {
        for (int i = 0; i < NANO_PRESETS; i++) {
            size_t len = strlen(t_nano.preset[i].name);
            n += put_v(buf + n, 18 << 3 | 2);
            n += put_v(buf + n, len + 2);
            buf[n++] = 0x0A;
            buf[n++] = (uint8_t)len;
            memcpy(buf + n, t_nano.preset[i].name, len);
            n += len;
        }
    }
    n += put_v(buf + n, 31 << 3 | 2);
    buf[n++] = NANO_FX_SLOTS;
    for (int i = 0; i < NANO_FX_SLOTS; i++) buf[n++] = p->on[i] ? 0 : 1;
    if (t_nano.dirty) n += put_field(buf + n, 41, 1);
    n += put_field(buf + n, 44, (uint64_t)p->volume);
    for (int i = 0; i < NANO_FX_SLOTS; i++) n += put_field(buf + n, 48 + i, p->fx[i]);
    reply(NANO_MSG_STATE_RESPONSE, buf, n, full ? 300 : 30);
}

static uint64_t field_of(const uint8_t *payload, size_t len, uint32_t field) { return nano_field_varint(payload, len, field); }

static void nano_takes(uint32_t type, const uint8_t *payload, size_t len)
{
    switch (type) {
    case NANO_MSG_STATE_REQUEST:
        reply_state(len <= 2);
        break;
    case NANO_MSG_SET_PRESET_SLOTS: {
        t_nano.current = (int)field_of(payload, len, 4);
        t_nano.now = t_nano.preset[t_nano.current];
        t_nano.dirty = false;
        static const uint8_t ok[] = { 0x20, 0x01 };
        reply(NANO_MSG_SET_PRESET_SLOTS_RESPONSE, ok, sizeof(ok), 20);
        break;
    }
    case NANO_MSG_FX_TYPE:
        t_nano.now.fx[field_of(payload, len, 3) % NANO_FX_SLOTS] = (uint32_t)field_of(payload, len, 4);
        t_nano.dirty = true;
        break;
    case NANO_MSG_BYPASS: {
        int slot = (int)field_of(payload, len, 3) - 4;
        if (slot >= 0 && slot < NANO_FX_SLOTS) t_nano.now.on[slot] = field_of(payload, len, 4) == 0;
        t_nano.dirty = true;
        break;
    }
    case NANO_MSG_VALUE: {
        int id = (int)field_of(payload, len, 3), value = (int)field_of(payload, len, 4);
        if (id < NANO_AMP_KNOBS) t_nano.now.amp[id] = value;
        else if (id == 10) t_nano.now.volume = value;
        t_nano.dirty = true;
        break;
    }
    case NANO_MSG_SAVE_PRESET: {
        if (t_nano.ignore_save) break;
        int index = (int)field_of(payload, len, 3);
        t_preset_t *p = &t_nano.preset[index % NANO_PRESETS];
        *p = t_nano.now;   // (what the pedal has goes along, as on the Nano)
        const uint8_t *name;
        size_t name_len;
        if (nano_field_bytes(payload, len, 5, &name, &name_len) && name_len < sizeof(p->name)) {
            memcpy(p->name, name, name_len);
            p->name[name_len] = 0;
        }
        if (index == t_nano.current) t_nano.dirty = false;
        break;
    }
    case NANO_MSG_EXP_REQUEST: {
        // (as read from a Nano: Pre FX 1's Amount 0-255 on the pedal - or nothing)
        static const uint8_t assigned[] = { 0x08, 0x01, 0x3a, 0x03, 0x18, 0xff, 0x01 }, none[] = { 0x08, 0x01 };
        bool pedal = t_nano.preset[field_of(payload, len, 3) % NANO_PRESETS].pedal;
        reply(NANO_MSG_EXP_RESPONSE, pedal ? assigned : none, pedal ? sizeof(assigned) : sizeof(none), 30);
        break;
    }
    case NANO_MSG_EXP_SAVE: {
        // (the test only ever saves "nothing on the pedal")
        t_preset_t *p = &t_nano.preset[field_of(payload, len, 3) % NANO_PRESETS];
        p->pedal = false;
        if ((int)field_of(payload, len, 3) == t_nano.current) {
            t_nano.now.pedal = false;
            t_nano.dirty = true;   // (as the Nano: the preset counts as edited after it)
        }
        break;
    }
    case NANO_MSG_FX_PARAMS_REQUEST: {
        // (as many values as the model in that slot has controls)
        const nano_fx_model_t *m = nano_fx_model(t_nano.now.fx[field_of(payload, len, 3) % NANO_FX_SLOTS]);
        int count = m ? m->param_count : 0;
        uint8_t buf[2 + 4 * NANO_MAX_PARAMS] = { 0x22, (uint8_t)(4 * count) };
        for (int i = 0; i < count; i++) { float v = 0.5f; memcpy(buf + 2 + 4 * i, &v, 4); }
        reply(NANO_MSG_FX_PARAMS_RESPONSE, buf, 2 + 4 * (size_t)count, 30);
        break;
    }
    default:
        break;   // (library, settings, values of effects: not answered)
    }
}

void nano_link_start(nano_message_cb on_message, nano_link_cb on_link) { t_on_message = on_message; t_on_link = on_link; }
bool nano_link_ready(void) { return t_link; }
void nano_link_restart(void) { }
void nano_link_log_status(void) { }
void nano_link_stats(nano_link_stats_t *out) { memset(out, 0, sizeof(*out)); }
bool nano_link_send(const char *label, uint32_t type, const uint8_t *payload, size_t len)
{
    (void)label;
    if (!t_link) return false;
    if (t_sent_count < SENT_MAX) {
        t_sent[t_sent_count].type = type;
        t_sent[t_sent_count].len = len < sizeof(t_sent[0].payload) ? len : sizeof(t_sent[0].payload);
        memcpy(t_sent[t_sent_count].payload, payload, t_sent[t_sent_count].len);
        t_sent[t_sent_count].at_us = t_now_us;
        t_sent_count++;
    }
    nano_takes(type, payload, len);
    return true;
}

// Time goes on: timers that are due, replies that are due, the controller's events.
static void advance(int ms)
{
    for (int step = 0; step < ms; step++) {
        t_now_us += 1000;
        for (struct esp_timer *t = t_timers; t; t = t->next) {
            if (!t->active || t->due_us > t_now_us) continue;
            t->active = false;
            t->args.callback(t->args.arg);
        }
        for (int i = 0; i < t_reply_count;) {
            if (t_replies[i].due_us > t_now_us) { i++; continue; }
            uint32_t type = t_replies[i].type;
            uint8_t *payload = t_replies[i].payload;
            size_t len = t_replies[i].len;
            memmove(t_replies + i, t_replies + i + 1, sizeof(t_replies[0]) * (size_t)(t_reply_count - i - 1));
            t_reply_count--;
            t_on_message(type, payload, len);
            free(payload);
        }
        web_app_pump();
    }
}

// ---- checks ----

static int t_checks, t_failed;

static void check(bool ok, const char *format, ...)
{
    t_checks++;
    if (ok) return;
    t_failed++;
    va_list args;
    va_start(args, format);
    printf("FAILED: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static int sent_of(uint32_t type, int from)
{
    int count = 0;
    for (int i = from; i < t_sent_count; i++) count += t_sent[i].type == type;
    return count;
}

static int first_of(uint32_t type, int from)
{
    for (int i = from; i < t_sent_count; i++) if (t_sent[i].type == type) return i;
    return -1;
}

static uint32_t type_of(const char *name)
{
    for (unsigned i = 0; i < NANO_FX_MODEL_COUNT; i++) if (!strcmp(NANO_FX_MODELS[i].name, name)) return NANO_FX_MODELS[i].type;
    printf("unknown model %s\n", name);
    exit(2);
}

static void press(int number)
{
    command('w', number);
    advance(400);
}

static bool store_same(const char *key, const void *data, size_t len)
{
    int i = store_find(key);
    return i >= 0 && t_store[i].len == len && !memcmp(t_store[i].data, data, len);
}


// ---- soak: random presses, holds and preset changes, then everything has to agree ----
// After every burst the controller is left alone for a few seconds; then what it shows must be what the stand-in
// Nano plays (preset, the five models, on / off), and nothing may still be under way. Run: <test> soak <seed> <bursts>
static uint32_t t_seed;
static uint32_t rnd(uint32_t n)
{
    t_seed = t_seed * 1664525u + 1013904223u;
    return (t_seed >> 8) % n;
}

static int soak(uint32_t seed, int bursts)
{
    t_seed = seed;
    int failed = 0, actions = 0;
    char trail[400];
    for (int b = 0; b < bursts && failed < 5; b++) {
        size_t tn = 0;
        trail[0] = 0;
        int count = 1 + (int)rnd(6);
        for (int a = 0; a < count; a++, actions++) {
            uint32_t what = rnd(100);
            int arg;
            if (what < 55) {          // a footswitch 3-8 (FX on / off, the reverb switch; presets and scenes in the other modes)
                arg = 3 + (int)rnd(6);
                command('w', arg);
                tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "w%d ", arg);
            } else if (what < 65) {   // ... held
                arg = 3 + (int)rnd(5);
                command('w', UI_SWITCH_HOLD | arg);
                tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "h%d ", arg);
            } else if (what < 80) {   // another preset (5 has a second reverb; 23 / 24 may be the ones made before)
                static const int PRESETS[] = { 5, 6, 7, 5, 23, 24 };
                arg = PRESETS[rnd(6)];
                command('g', arg);
                tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "g%d ", arg);
            } else if (what < 86) {   // the mode
                command('m', 0);
                tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "m ");
            } else if (what < 92) {   // next / previous preset
                command(rnd(2) ? 'n' : 'p', 0);
                tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "np ");
            } else {                  // an effect on / off directly (the console's a-e)
                arg = (int)rnd(5);
                command((char)('a' + arg), 0);
                tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "%c ", 'a' + arg);
            }
            int gap = (int)rnd(8) ? 20 + (int)rnd(500) : 1 + (int)rnd(15);   // mostly a foot's pace, sometimes at once
            advance(gap);
            tn += (size_t)snprintf(trail + tn, sizeof(trail) - tn, "+%d ", gap);
            if (tn > sizeof(trail) - 24) break;
        }
        advance(5000);
        bool ok = s_state.current_preset == t_nano.current + 1 && !ab_running() && !fxp_sending() && !s_expect_preset
                  && s_newp.phase == NEWP_IDLE;
        for (int i = 0; i < NANO_FX_SLOTS; i++) {
            ok = ok && s_state.fx_type[i] == t_nano.now.fx[i] && s_state.fx_on[i] == t_nano.now.on[i];
            if (ab_used(i)) ok = ok && t_nano.now.fx[i] == (s_ab[i].active ? s_ab[i].b.type : s_ab[i].a_type ? s_ab[i].a_type : t_nano.now.fx[i]);
        }
        if (!ok) {
            failed++;
            printf("SOAK seed %u burst %d FAILED after: %s\n", (unsigned)seed, b, trail);
            printf("  preset: controller %d, Nano %d%s%s%s\n", s_state.current_preset, t_nano.current + 1, ab_running() ? ", a swap still runs" : "",
                   fxp_sending() ? ", values still going out" : "", s_expect_preset ? ", still waiting for a preset" : "");
            for (int i = 0; i < NANO_FX_SLOTS; i++) {
                printf("  %-9s controller %-18s %-3s | Nano %-18s %-3s%s\n", NANO_FX_SLOT_NAMES[i], nano_fx_name(s_state.fx_type[i]),
                       s_state.fx_on[i] ? "ON" : "off", nano_fx_name(t_nano.now.fx[i]), t_nano.now.on[i] ? "ON" : "off",
                       ab_used(i) ? (s_ab[i].active ? "  (B runs)" : "  (A runs)") : "");
            }
            // (bring both back in step for the next burst)
            command('g', 6);
            advance(4000);
        }
    }
    printf("soak seed %u: %d bursts, %d actions, %d failed\n", (unsigned)seed, bursts, actions, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t wah = type_of("Crying Wah"), drive = type_of("Rodent Drive"), chorus = type_of("Dream Chorus"),
             delay = type_of("Tape Delay"), hall = type_of("Mind Hall"), modulated = type_of("Modulated"), room = type_of("Room");
    for (int i = 0; i < NANO_PRESETS; i++) {
        t_preset_t *p = &t_nano.preset[i];
        snprintf(p->name, sizeof(p->name), "Preset %d", i + 1);
        const uint32_t fx[NANO_FX_SLOTS] = { wah, drive, chorus, delay, hall };
        memcpy(p->fx, fx, sizeof(fx));
        p->on[1] = p->on[4] = true;
        for (int k = 0; k < NANO_AMP_KNOBS; k++) p->amp[k] = 200;
        p->volume = 150;
    }
    t_nano.preset[4].pedal = true;   // preset 5: the wah's Amount on the pedal
    t_nano.current = 4;
    t_nano.now = t_nano.preset[4];

    // What earlier versions stored for preset 5: a second reverb that footswitch 8 swaps (1.14: tap = hold, not
    // parked), and scenes for place 23, which gets the new preset later.
    fx_b_t old_b = { .type = modulated };
    uint8_t packed[sizeof(fx_b_t)];
    size_t packed_len = fx_b_pack(&old_b, packed);
    nvs_set_blob(1, "rvb05", packed, packed_len);
    static const uint8_t leftover[40] = { 1, 2, 3 };
    nvs_set_blob(1, "scn23", leftover, sizeof(leftover));
    nvs_set_blob(1, "rvb23", packed, packed_len);

    app_main();
    t_link = true;
    t_on_link(true);
    advance(3000);
    check(s_state.current_preset == 5 && s_state.names_loaded && s_state.fx_known, "start: preset %d read", s_state.current_preset);
    check(!strcmp(s_state.preset_names[22], "Preset 23"), "start: names read");
    command('m', 0);   // FX mode
    advance(200);
    check(s_fx_mode, "FX mode");
    if (argc > 1 && !strcmp(argv[1], "soak")) return soak(argc > 2 ? (uint32_t)atoi(argv[2]) : 1, argc > 3 ? atoi(argv[3]) : 300);
    int rslot = s_mix.slot;
    check(rslot == 4, "the reverb is in Post FX 3 (slot %d)", rslot);

    // ---- the reverb's two switches ----
    // Stored by an earlier version, footswitch 8 swaps: the reverb's own switch is on / off only, at the press
    check(ab_used(rslot) && reverb_b_used(), "old data: reverb B is there and footswitch 8 swaps it");
    check(ab_mode(rslot) == UI_AB_OFF, "old data: the reverb's own switch is on / off only (mode %d)", ab_mode(rslot));
    check(!(s_hold_switches & 1u << (rslot + 3)), "old data: its switch does not wait for the release");
    check(t_view.ab_b_type[rslot] == modulated && t_view.ab_mode[rslot] == UI_AB_OFF && t_view.rev_b_type == modulated,
          "the screen is told: B there, the tile plain, footswitch 8 swaps");
    int mark = t_sent_count;
    press(7);   // the reverb's own switch: off
    check(sent_of(NANO_MSG_BYPASS, mark) == 1 && sent_of(NANO_MSG_FX_TYPE, mark) == 0 && !t_nano.now.on[rslot],
          "the reverb's switch switches it off (bypass %d, models %d)", sent_of(NANO_MSG_BYPASS, mark), sent_of(NANO_MSG_FX_TYPE, mark));
    command('w', UI_SWITCH_HOLD | 7);   // (a held switch, should one be reported: nothing)
    advance(300);
    check(sent_of(NANO_MSG_FX_TYPE, mark) == 0, "holding the reverb's switch swaps nothing");
    press(7);   // on again
    check(t_nano.now.on[rslot], "... and on again");
    advance(1500);
    mark = t_sent_count;
    press(8);   // the reverb switch: B
    advance(1500);
    check(sent_of(NANO_MSG_FX_TYPE, mark) == 1 && t_nano.now.fx[rslot] == modulated && s_ab[rslot].active == 1,
          "footswitch 8 swaps to B (%s)", nano_fx_name(t_nano.now.fx[rslot]));
    press(8);
    advance(1500);
    check(t_nano.now.fx[rslot] == hall && s_ab[rslot].active == 0, "... and back to A");

    // The reverb dialog saved on its mix tab: B is kept, nobody swaps it - the tile stays plain
    command('W', 26 << 8 | 102);
    command('A', 2 << 24 | (int)modulated);
    advance(300);
    check(ab_used(rslot) && !reverb_b_used() && ab_mode(rslot) == UI_AB_OFF, "mix on footswitch 8: B kept, the tile plain");
    mark = t_sent_count;
    press(8);
    check(sent_of(NANO_MSG_FX_TYPE, mark) == 0 && sent_of(NANO_MSG_FX_VALUE, mark) >= 1, "footswitch 8 is the mix switch");
    press(7);
    press(7);
    check(sent_of(NANO_MSG_FX_TYPE, mark) == 0 && t_nano.now.on[rslot], "the reverb's switch: off and on, no swap");

    // Chosen for the tile in its FX editor: the tile swaps (held), footswitch 8 stays the mix switch
    command('>', rslot << 8 | UI_AB_HOLD);
    advance(300);
    check(ab_mode(rslot) == UI_AB_HOLD && !reverb_b_used() && (s_hold_switches & 1u << (rslot + 3)), "chosen on the tile: it swaps when held");
    mark = t_sent_count;
    command('w', UI_SWITCH_HOLD | 7);
    advance(1500);
    check(t_nano.now.fx[rslot] == modulated, "the reverb's switch held: B");
    command('w', UI_SWITCH_HOLD | 7);
    advance(1500);
    check(t_nano.now.fx[rslot] == hall, "... held again: A");
    // The dialog saved on Reverb A / B: footswitch 8 takes the swap, the tile is plain again
    command('A', (int)modulated);
    advance(300);
    check(reverb_b_used() && ab_mode(rslot) == UI_AB_OFF && !(s_hold_switches & 1u << (rslot + 3)), "Reverb A / B on footswitch 8: the tile plain again");
    // "On / off only" chosen on a tile that swapped; a tile that swaps takes the A / B from footswitch 8
    command('>', rslot << 8 | UI_AB_TAP);
    advance(300);
    check(ab_mode(rslot) == UI_AB_TAP && !reverb_b_used(), "a tap on the tile swaps: footswitch 8 is the mix switch again");
    command('>', rslot << 8 | UI_AB_OFF);
    advance(300);
    check(ab_mode(rslot) == UI_AB_OFF && !reverb_b_used() && ab_used(rslot), "On / off only: B kept, nothing swaps it");
    // Another slot keeps what it had: hold by default, never "off"
    command('A', (8 + 1) << 24 | (int)type_of("Green 808"));
    advance(300);
    check(ab_used(1) && ab_mode(1) == UI_AB_HOLD, "Pre FX 2 with a second effect: its switch swaps when held");
    command('>', 1 << 8 | UI_AB_OFF);
    advance(300);
    check(ab_mode(1) == UI_AB_HOLD, "... and cannot be set to on / off only");
    command('A', (8 + 1) << 24);
    advance(300);
    // A layout without the reverb switch: the reverb's own switch has to swap
    command('A', (int)modulated);
    advance(300);
    s_layout_count = 6;   // (FX mode's order for six fields, as the dialog sets it: no reverb switch either)
    uint8_t fx_eight[FOOTSWITCH_COUNT];
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) fx_eight[p] = s_layout_fx[p], s_layout_fx[p] = LAYOUT_FX_DEFAULT[0][p];
    check(ab_mode(rslot) == UI_AB_HOLD, "six fields, no reverb switch: the reverb's own switch swaps");
    s_layout_count = 8;
    for (int p = 0; p < FOOTSWITCH_COUNT; p++) s_layout_fx[p] = fx_eight[p];
    check(ab_mode(rslot) == UI_AB_OFF, "eight fields again: on / off only");
    advance(2000);

    // ---- the switch pressed again while a swap is under way: the press is not lost ----
    check(reverb_b_used() && s_ab[rslot].active == 0 && t_nano.now.fx[rslot] == hall, "before: A runs, footswitch 8 swaps");
    t_message[0] = 0;
    command('w', 8);
    advance(250);   // (the Nano is loading B)
    check(ab_running() != NULL, "a swap is under way");
    command('w', 8);
    advance(3000);
    check(!strstr(t_message, "Still switching"), "no \"Still switching\" (\"%s\")", t_message);
    check(t_nano.now.fx[rslot] == hall && s_ab[rslot].active == 0 && !ab_running(), "two presses in a row: B, then A again (%s)",
          nano_fx_name(t_nano.now.fx[rslot]));
    command('w', 8);
    advance(200);
    command('w', 8);
    advance(100);
    command('w', 8);
    advance(3000);
    check(t_nano.now.fx[rslot] == modulated && s_ab[rslot].active == 1 && !s_ab[rslot].again, "three presses in a row: B (%s)",
          nano_fx_name(t_nano.now.fx[rslot]));
    command('w', 8);
    advance(3000);
    check(t_nano.now.fx[rslot] == hall, "... and one more: A");

    // ... and while values are going out to the reverb (an FX preset just loaded): the swap follows them
    s_fxp.send_slot = rslot;
    s_fxp.send_count = 12;   // (more than one step's worth, so the run is still going when the switch comes)
    s_fxp.sent = 0;
    for (int i = 0; i < 12; i++) s_fxp.send_values[i] = 0.25f;
    fxp_send_step();
    check(fxp_sending(), "values are going out to the reverb");
    t_message[0] = 0;
    command('w', 8);
    advance(3000);
    check(!strstr(t_message, "Still switching") && t_nano.now.fx[rslot] == modulated && s_ab[rslot].active == 1,
          "the swap came after them (%s, \"%s\")", nano_fx_name(t_nano.now.fx[rslot]), t_message);
    command('w', 8);
    advance(3000);
    check(t_nano.now.fx[rslot] == hall, "... back on A");

    // ---- an effect switched on and off quickly: a state reply from before the second press does not take it back ----
    int fslot = 3;   // Post FX 2, footswitch 6
    check(!s_state.fx_on[fslot] && !t_nano.now.on[fslot], "before: the delay is off");
    command('w', 6);
    advance(310);   // (the controller has asked for the state: the Nano's reply says "on" and is on its way)
    check(t_reply_count == 1, "a state reply is on its way (%d)", t_reply_count);
    command('w', 6);
    advance(60);    // (that reply has arrived)
    check(!t_nano.now.on[fslot] && !s_state.fx_on[fslot], "off on the Nano and on the controller (controller: %s)", s_state.fx_on[fslot] ? "on" : "off");
    mark = t_sent_count;
    command('w', 6);
    advance(600);
    check(t_nano.now.on[fslot] && s_state.fx_on[fslot] && sent_of(NANO_MSG_BYPASS, mark) == 1, "the next press switches it on (Nano: %s)",
          t_nano.now.on[fslot] ? "on" : "off");
    command('w', 6);
    advance(1500);
    check(!t_nano.now.on[fslot] && !s_state.fx_on[fslot], "... and off again");

    // ---- a new preset: the standard board ----
    uint8_t before[sizeof(fx_b_t)];
    size_t before_len = sizeof(before);
    nvs_get_blob(1, "rvb05", before, &before_len);
    t_preset_t five = t_nano.preset[4];
    mark = t_sent_count;
    on_text('P', "170Fresh Start");
    advance(100);
    check(s_newp.phase != NEWP_IDLE, "the run started (%s)", t_message);
    press(7);   // (somebody steps on a switch meanwhile: the run goes on)
    advance(12000);
    check(s_newp.phase == NEWP_IDLE, "the run ended (phase %d, \"%s\")", s_newp.phase, t_message);
    const t_preset_t *made = &t_nano.preset[22];
    check(!strcmp(made->name, "Fresh Start"), "place 23 is called \"%s\"", made->name);
    bool board = true, off = true;
    for (int i = 0; i < NANO_FX_SLOTS; i++) {
        board = board && made->fx[i] == NEW_PRESET_BOARD[i];
        off = off && !made->on[i];
    }
    check(board, "place 23 has the standard board (%s, %s, %s, %s, %s)", nano_fx_name(made->fx[0]), nano_fx_name(made->fx[1]),
          nano_fx_name(made->fx[2]), nano_fx_name(made->fx[3]), nano_fx_name(made->fx[4]));
    check(off, "... all effects off");
    check(made->amp[NANO_AMP_GAIN] == 128 && made->amp[NANO_AMP_BASS] == 128 && made->amp[NANO_AMP_MID] == 128
          && made->amp[NANO_AMP_TREBLE] == 128 && made->amp[NANO_AMP_LEVEL] == 200 && made->volume == 128,
          "tone controls in the middle, the master level untouched");
    check(!made->pedal, "nothing on the expression pedal (the preset before had the wah on it)");
    check(t_nano.current == 22 && s_state.current_preset == 23 && !t_nano.dirty, "the Nano is on the new preset, nothing unsaved");
    check(!memcmp(&t_nano.preset[4], &five, sizeof(five)), "preset 5 on the Nano is as it was");
    check(store_same("rvb05", before, before_len), "what the controller keeps for preset 5 is as it was");
    check(store_find("scn23") < 0 && store_find("rvb23") < 0, "what it kept for place 23 is gone");
    check(!ab_used(rslot) && s_ab[rslot].preset == 23, "no second reverb in the new preset");
    int model_at = first_of(NANO_MSG_FX_TYPE, mark), save_at = first_of(NANO_MSG_SAVE_PRESET, mark), change_at = -1;
    for (int i = mark; i < t_sent_count; i++) if (t_sent[i].type == NANO_MSG_SET_PRESET_SLOTS) change_at = i;
    // (two saves: into the place, and once more after the pedal's assignments of the preset before were taken off)
    check(sent_of(NANO_MSG_FX_TYPE, mark) == 5 && sent_of(NANO_MSG_SAVE_PRESET, mark) == 2, "five models set, saved twice (%d, %d)",
          sent_of(NANO_MSG_FX_TYPE, mark), sent_of(NANO_MSG_SAVE_PRESET, mark));
    check(model_at >= 0 && save_at > model_at && change_at > save_at, "order: models, save, change of preset");
    bool spaced = true;
    for (int i = mark, last = -1; i < t_sent_count; i++) {
        if (t_sent[i].type != NANO_MSG_FX_TYPE) continue;
        if (last >= 0 && t_sent[i].at_us - t_sent[last].at_us < 400 * 1000) spaced = false;
        last = i;
    }
    check(spaced, "the models go out one at a time, 0.4 s apart at least");
    check(strstr(t_message, "is new") != NULL, "said on the screen: \"%s\"", t_message);

    // ---- a new preset: a copy ----
    command('g', 5);
    advance(3000);
    check(s_state.current_preset == 5 && ab_used(rslot), "back on preset 5 with its second reverb");
    mark = t_sent_count;
    on_text('P', "181Copy Of Five");
    advance(8000);
    check(s_newp.phase == NEWP_IDLE && !strcmp(t_nano.preset[23].name, "Copy Of Five"), "the copy is in place 24 (\"%s\")", t_message);
    check(!memcmp(t_nano.preset[23].fx, five.fx, sizeof(five.fx)) && t_nano.preset[23].pedal, "... with preset 5's effects and pedal");
    check(sent_of(NANO_MSG_FX_TYPE, mark) == 0 && sent_of(NANO_MSG_BYPASS, mark) == 0, "nothing of the sound was touched for it");
    check(store_find("rvb24") >= 0 && s_state.current_preset == 24 && ab_used(rslot), "the second reverb went along; the Nano is on the copy");

    // ---- what is refused ----
    on_text('P', "190Copy Of Five");
    advance(200);
    check(s_newp.phase == NEWP_IDLE && strstr(t_message, "already exists"), "a name that exists: \"%s\"", t_message);
    on_text('P', "181Same Place");
    advance(200);
    check(s_newp.phase == NEWP_IDLE && strstr(t_message, "another place"), "a copy onto itself: \"%s\"", t_message);
    on_text('P', "190abc");
    advance(200);
    check(s_newp.phase == NEWP_IDLE && strstr(t_message, "4 to 32"), "a name too short: \"%s\"", t_message);

    // ---- right after a preset with a second effect was loaded (its values are being read for later): no refusal ----
    command('g', 5);
    advance(715);
    check(ab_running() != NULL && ab_running()->cache_only, "a read for later is under way");
    t_message[0] = 0;
    on_text('P', "211Quick Copy");
    advance(8000);
    check(s_newp.phase == NEWP_IDLE && !strcmp(t_nano.preset[32].name, "Quick Copy") && s_state.current_preset == 33,
          "a copy made 0.7 s after a preset change (\"%s\")", t_message);

    // ---- the Nano does not take the save: nothing is created, the preset before is loaded again ----
    command('g', 5);
    advance(3000);
    t_nano.ignore_save = true;
    t_preset_t thirty = t_nano.preset[29];
    nvs_set_blob(1, "scn30", leftover, sizeof(leftover));
    on_text('P', "1E0Never Made");
    advance(15000);
    check(s_newp.phase == NEWP_IDLE && strstr(t_message, "Not created"), "not created: \"%s\"", t_message);
    check(!memcmp(&t_nano.preset[29], &thirty, sizeof(thirty)) && store_find("scn30") >= 0, "place 30 and what is kept for it untouched");
    check(t_nano.current == 4 && !t_nano.dirty && s_state.current_preset == 5 && t_nano.now.fx[0] == wah,
          "preset 5 is loaded again as it is stored (on %d, %s)", t_nano.current + 1, nano_fx_name(t_nano.now.fx[0]));
    check(store_same("rvb05", before, before_len), "what the controller keeps for preset 5 is still as it was");
    t_nano.ignore_save = false;

    // ---- the link goes while a run is under way ----
    on_text('P', "200Cut Off");
    advance(600);
    check(s_newp.phase != NEWP_IDLE, "a run under way");
    t_link = false;
    t_on_link(false);
    advance(3000);
    check(s_newp.phase == NEWP_IDLE, "the link lost: the run is over");

    // ---- (P4) the settings' footswitch learn goes through all switches, one after the other ----
    command('D', 1 | UI_LEARN_ALL);
    advance(50);
    check(s_learning == 1 && t_fsw_learn == 1, "learn all: waits for switch 1 (%d, footswitch task %d)", s_learning, t_fsw_learn);
    for (int n = 1; n < FOOTSWITCH_COUNT; n++) {
        command('Z', n << 8 | (n == 1 ? 2 : 0));   // (switch 1 was on switch 2's pin: swapped)
        advance(50);
        if (s_learning != n + 1 || t_fsw_learn != n + 1 || t_learn_next != n + 1) break;
    }
    check(s_learning == FOOTSWITCH_COUNT && t_fsw_learn == FOOTSWITCH_COUNT && t_learn_next == FOOTSWITCH_COUNT,
          "learn all: after each press the next switch is waited for, up to the last (%d, %d, %d)", s_learning, t_fsw_learn, t_learn_next);
    command('Z', FOOTSWITCH_COUNT << 8);
    advance(50);
    check(!s_learning && t_learn_next == 0 && strstr(t_learn_done, "learned"), "learn all: the last press ends it (\"%s\")", t_learn_done);
    command('D', 3 | UI_LEARN_ALL);
    advance(50);
    command('Z', 0);   // nothing pressed in 15 s
    advance(50);
    check(!s_learning && !s_learn_all && t_learn_next == 0, "learn all: nothing pressed ends it (\"%s\")", t_learn_done);
    command('D', 4);   // one switch only (the bank editor's Learn switch): as before
    advance(50);
    command('Z', 4 << 8);
    advance(50);
    check(!s_learning && t_learn_next == 0 && t_fsw_learn == 4, "one switch learned: no next one (footswitch task %d)", t_fsw_learn);

    // ---- (P4) player mode: footswitch 1 held - the switches are the player's, each on its press; 8 back out ----
    int preset_before = s_state.current_preset, toggles = t_player_toggles, hides = t_player_hides;
    command('w', UI_SWITCH_HOLD | 1);
    advance(50);
    check(s_player && s_player_live && s_hold_switches == 0, "player mode: on, and no switch waits for a hold (%u)", (unsigned)s_hold_switches);
    // (player draft B, 2026-11-28: 5 / 7 select the song before / after, 6 plays the selected one)
    static const int WANT[3] = { UI_PLAYER_SONG_PREV, UI_PLAYER_START, UI_PLAYER_SONG_NEXT };
    bool row_ok = true;
    for (int k = 0; k < 3; k++) {
        t_player_action = -1;
        command('w', 5 + k);
        advance(50);
        if (t_player_action != WANT[k]) row_ok = false;
    }
    check(row_ok && s_state.current_preset == preset_before, "player mode: 5-7 select the song before, play the selected one, select the one after - no preset changed");
    t_player_action = -1;
    command('w', 1);
    advance(50);
    check(t_player_action == UI_PLAYER_SOURCE && s_player && s_state.current_preset == preset_before, "player mode: 1 switches jam tracks / phone");
    t_player_action = -1;
    command('w', 3);
    advance(50);
    check(t_player_action == UI_PLAYER_ALBUM_NEXT && s_player && t_player_toggles == toggles, "player mode: 3 is the next album (no page)");
    command('w', 2);
    advance(50);
    check(t_player_toggles == toggles + 1 && s_player && !s_tuner_on && s_state.current_preset == preset_before,
          "player mode: 2 opens / closes the list (not the tuner, 2026-11-28)");
    check(layout_switch(6) == 6, "player mode: the switches in fixed places, whatever the layout");
    command('w', 8);
    advance(50);
    check(!s_player && !s_player_live && t_player_hides == hides + 1 && s_state.current_preset == preset_before,
          "player mode: 8 goes back out (no preset, the page away)");
    t_player_action = -1;
    command('w', 6);
    advance(50);
    check(t_player_action == -1, "out of player mode: footswitch 6 is not the player's");
    advance(3000);
    // ---- (P4) looper mode: 8 back out too; 5 is undo (held: clear) ----
    command('w', UI_SWITCH_HOLD | 1);
    advance(50);
    command('w', 4);   // player mode -> the looper
    advance(50);
    check(s_looper && !s_player && (s_hold_switches & (1u << 5)) && !(s_hold_switches & (1u << 8)),
          "player mode, switch 4: into looper mode (5 waits for a hold, 8 does not: %X)", (unsigned)s_hold_switches);
    command('w', 8);
    advance(50);
    check(!s_looper && !s_player && s_state.current_preset == preset_before, "looper mode: 8 goes back out");
    advance(3000);

    // ---- (P4, 2026-11-27) switch 2: a press = looper mode, held = the tuner ----
    t_link = true;   // (the link came back after the run above)
    t_on_link(true);
    advance(8000);
    command('w', 2);
    advance(50);
    check(s_looper && !s_tuner_on, "switch 2 pressed: looper mode");
    command('w', 8);
    advance(50);
    command('w', UI_SWITCH_HOLD | 2);
    advance(50);
    check(!s_looper && s_tuner_on, "switch 2 held: the tuner");
    command('w', 2);
    advance(50);
    check(!s_looper && !s_tuner_on, "switch 2 pressed while the tuner shows: the tuner closes (no looper)");
    advance(2000);
    // ---- (P4) a second preset on a switch: held and let go = that preset; a short press = the first ----
    s_fx_mode = false, s_scene_mode = false;
    int slot = 5, first = s_banks[s_bank][slot].preset ? s_banks[s_bank][slot].preset : 6, second = first == 12 ? 13 : 12;
    command('B', second << 23 | s_bank << 14 | slot << 11 | 0 << 7 | first);
    advance(200);
    check(s_bank2[s_bank][slot] == second && ((s_release_hold_switches >> (slot + 3)) & 1) && ((s_hold_switches >> (slot + 3)) & 1),
          "second preset %d on switch %d: the switch waits, its held function comes on the release", second, slot + 3);
    on_footswitch(slot + 3, FOOTSWITCH_PRESS);
    advance(10);
    on_footswitch(slot + 3, FOOTSWITCH_HOLD);
    advance(300);
    check(s_state.current_preset != second, "held: nothing yet while the switch is down (preset %d)", s_state.current_preset);
    on_footswitch(slot + 3, FOOTSWITCH_RELEASE);
    advance(3000);
    check(s_state.current_preset == second, "held and let go: the second preset (%d, wanted %d)", s_state.current_preset, second);
    on_footswitch(slot + 3, FOOTSWITCH_PRESS);
    advance(10);
    on_footswitch(slot + 3, FOOTSWITCH_RELEASE);
    advance(3000);
    check(s_state.current_preset == first, "a short press: the first preset (%d, wanted %d)", s_state.current_preset, first);
    command('B', s_bank << 14 | slot << 11 | 0 << 7 | first);   // (no second preset again)
    advance(200);
    check(!s_bank2[s_bank][slot] && !((s_release_hold_switches >> (slot + 3)) & 1), "second preset removed: the switch acts on its press again");

    // ---- (P4, 2026-11-27) FX mode has its own order; a footswitch's release goes where its press went ----
    s_fx_mode = true;
    show();
    check(layout_switch(1) == 3 && layout_switch(3) == 7 && layout_switch(4) == 2 && layout_switch(5) == 5 && layout_switch(7) == 8
          && layout_switch(8) == 1, "FX mode: its own order (1 Pre FX 1, 3 reverb on / off, 4 tuner, 5 Post FX 1, 7 reverb switch, 8 mode)");
    s_fx_mode = false;
    show();
    check(layout_switch(1) == 1 && layout_switch(4) == 4 && layout_switch(8) == 8, "preset mode: the presets' order");
    {
        switch_track_t tr = { 0 };
        s_fx_mode = true;
        show();
        switch_event(&tr, 4, FOOTSWITCH_PRESS);   // FX mode: footswitch 4 is the tuner's switch (2) - it waits for a hold
        bool waited = tr.waiting[2] && tr.pressed[4] == 2;
        s_fx_mode = false;   // the mode changes while the footswitch is down (position 4 is preset 2 there)
        show();
        switch_event(&tr, 4, FOOTSWITCH_RELEASE);
        check(waited && !tr.waiting[2] && !tr.waiting[4] && tr.pressed[4] == 0,
              "mode changed while a footswitch was down: its release went to the switch of its press (2), not to 4");
        advance(50);
        if (s_looper) command('w', 8);   // (switch 2 pressed = looper mode: back out)
        advance(50);
    }

    printf("%d checks, %d failed\n", t_checks, t_failed);
    printf(t_failed ? "main: FAILED\n" : "main: all checks passed\n");
    return t_failed ? 1 : 0;
}
