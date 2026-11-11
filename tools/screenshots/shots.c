#include <stdlib.h>
// Renders screenshots of the controller UI on a computer: the real main/ui.c with the board's LVGL configuration,
// fed with example data. Output: shots/<name>.ppm; tools/screenshots/make_screenshots.sh turns them into
// docs/images/<name>.png.
#include <stdio.h>
#include <string.h>

#include "ui.c"

const char *const LIB_CATEGORY_NAMES[LIB_CATEGORY_COUNT] = { "All", "Amp", "Amp + Cab", "Cab", "Pedal", "Other" };   // as in main/library.c

// IBM Plex Sans TTF for the tuner note, as on the board (target_add_binary_data).
__asm__(".section __DATA,__const\n"
        ".globl _binary_IBMPlexSans_SemiBold_ttf_start\n_binary_IBMPlexSans_SemiBold_ttf_start:\n"
        ".incbin \"" TTF_PATH "\"\n"
        ".globl _binary_IBMPlexSans_SemiBold_ttf_end\n_binary_IBMPlexSans_SemiBold_ttf_end:\n"
        ".byte 0\n.text\n");

#define W 800
#define H 480
static uint8_t s_draw[W * H * 2];
static uint16_t s_fb[W * H];
static uint32_t s_ms;

static uint32_t tick(void) { return s_ms; }

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) memcpy(&s_fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        s_ms += 5;
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

static void shot(const char *name)
{
    run(600);
    char path[128];
    snprintf(path, sizeof(path), "shots/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = s_fb[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 0x1F) * 255 / 31), (uint8_t)(((c >> 5) & 0x3F) * 255 / 63), (uint8_t)((c & 0x1F) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", path);
}

static void on_command(char c, int arg) { (void)c; (void)arg; }
static void on_param_cb(int slot, int param, float value) { (void)slot; (void)param; (void)value; }
static void on_text_cb(char kind, const char *text) { (void)kind; (void)text; }

static uint32_t type_of(const char *name)
{
    for (unsigned i = 0; i < NANO_FX_MODEL_COUNT; i++) if (!strcmp(NANO_FX_MODELS[i].name, name)) return NANO_FX_MODELS[i].type;
    fprintf(stderr, "unknown model %s\n", name);
    return 0;
}

static nano_state_t st;
static ui_view_t view;

static void example_state(void)
{
    static const char *const names[] = {
        "Clean Glass", "Warm Jazz", "Funk Clean", "Edge Blues", "Crunch Rhythm", "Lead Boost",
        "Clean Sparkle", "Edge Of Breakup", "Crunch Rhythm", "Lead Boost", "Fuzz Wall", "Ambient Swell",
    };
    for (int i = 0; i < NANO_PRESETS; i++) {
        if (i < 12) snprintf(st.preset_names[i], sizeof(st.preset_names[i]), "%s", names[i]);
        else snprintf(st.preset_names[i], sizeof(st.preset_names[i]), "Preset %d", i + 1);
    }
    st.names_loaded = true;
    st.current_preset = 8;
    st.fx_known = true;
    const char *const fx[NANO_FX_SLOTS] = { "Green 808", "Legendary 87 (M)", "Chief CE2W (ST)", "Tape Delay", "Mind Hall" };
    const bool on[NANO_FX_SLOTS] = { true, true, false, true, true };
    for (int i = 0; i < NANO_FX_SLOTS; i++) {
        st.fx_type[i] = type_of(fx[i]);
        st.fx_on[i] = on[i];
    }
    snprintf(st.capture, sizeof(st.capture), "Revv D20 Clean NF53");
    snprintf(st.cab, sizeof(st.cab), "810 Amped VT Aln 70s");
    st.capture_slot = 3;
    st.cab_slot = 2;
    static const char *const captures[] = { "Plexi Crunch", "Brit 800 Drive", "Revv D20 Clean NF53", "Tweed Edge", "AC Top Boost" };
    for (int i = 0; i < NANO_CAPTURE_SLOTS; i++) snprintf(st.capture_names[i], sizeof(st.capture_names[i]), "%s", i < 5 ? captures[i] : "Empty");
    static const char *const cabs[] = { "412 Brit V30", "810 Amped VT Aln 70s", "212 Blue Alnico", "110 US PRN C10R", "412 Recto" };
    for (int i = 0; i < NANO_CAB_SLOTS; i++) snprintf(st.cab_names[i], sizeof(st.cab_names[i]), "%s", cabs[i]);
    st.tuner_base_hz = 440;
    st.capture_volume = nano_capture_volume_raw(1.5f);

    view.bank = 1;   // BANK 2: presets 7-12
    static const uint8_t colors[6] = { 0, 3, 2, 1, 8, 5 }, icons[6] = { 1, 2, 9, 16, 5, 19 };
    for (int i = 0; i < 6; i++) {
        view.bank_presets[i] = (uint8_t)(7 + i);
        view.bank_colors[i] = colors[i];
        view.bank_icons[i] = icons[i];
    }
    view.mix_slot = 4;
    view.mix_known = true;
    view.mix_pos[0] = 0.10f;
    view.mix_pos[1] = 0.40f;
    view.mix_active = -1;
    view.rev_b_type = view.rev_b_stored = type_of("Room");
    view.rev_active = 0;
    view.ab_b_type[0] = type_of("Envelope Filter");   // Pre FX 1: the Green 808 (A) and an envelope filter (B)
    view.ab_b_type[4] = type_of("Room");              // the reverb: Mind Hall (A) and Room (B), also on footswitch 8
}

static nano_library_t *example_library(void)
{
    static const struct { const char *name; uint8_t category; } items[] = {
        { "AC Top Boost", LIB_AMP }, { "Bass 800 Grit", LIB_AMP }, { "Brit 800 Drive", LIB_AMP },
        { "Clean Twin", LIB_AMP_CAB }, { "Fuzz Face Vintage", LIB_PEDAL }, { "JC Clean", LIB_AMP },
        { "Klon Boost", LIB_PEDAL }, { "Plexi Crunch", LIB_AMP_CAB }, { "Plexi Lead", LIB_AMP },
        { "Recto Modern", LIB_AMP }, { "Revv D20 Clean NF53", LIB_AMP }, { "Revv D20 Toxic Twins TS NF53", LIB_AMP },
        { "Tube Screamer 808", LIB_PEDAL }, { "Tweed Edge", LIB_AMP_CAB },
    };
    static lib_item_t list[sizeof(items) / sizeof(items[0])];
    static nano_library_t lib;
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        snprintf(list[i].name, sizeof(list[i].name), "%s", items[i].name);
        list[i].index = (uint16_t)i;
        list[i].category = items[i].category;
        list[i].user = i % 5 == 4;
    }
    lib.captures.items = list;
    lib.captures.count = (int)(sizeof(items) / sizeof(items[0]));
    return &lib;
}

// The set list dialog reads scene names of any preset: every example preset has the same three scenes.
static void example_scene_names(int preset, char names[UI_SCENES][UI_SCENE_NAME])
{
    static const char *const scenes[] = { "Clean", "Crunch", "Lead" };
    for (int i = 0; i < 3; i++) snprintf(names[i], UI_SCENE_NAME, "%s", scenes[i]);
}

static void close_all(void)
{
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_usb, s_mix_editor, s_learn, s_midi,
                                   s_layout, s_volume, s_cab_settings, s_amp, s_toast, s_looper_editor, s_scene_editor, s_exp_dialog, s_cal_panel,
                                   s_setlist, s_scene_cap, s_system };
    for (size_t i = 0; i < sizeof(overlays) / sizeof(overlays[0]); i++) lv_obj_set_hidden(overlays[i], true);
    update_main_hidden();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_buffers(d, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);

    {   // PEDALBOARD=<file>: the own build's pictures for the pedal view (never for the published screenshots)
        const char *file = getenv("PEDALBOARD");
        FILE *f = file ? fopen(file, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            rewind(f);
            uint8_t *data = malloc((size_t)size);
            if (data && fread(data, 1, (size_t)size, f) == (size_t)size) pedal_assets_load(data, (size_t)size);
            fclose(f);
        }
    }
    ui_init(on_command, on_param_cb, on_text_cb);
    ui_set_scene_names_source(example_scene_names);
    ui_splash_status("Connected - loading presets ...", "v1.4.1");
    shot("15-start-screen");
    ui_splash_done();
    ui_set_link(true);
    example_state();
    ui_set_library(example_library());

    ui_show_state(&st, &view);
    shot("01-preset-mode");

    view.fx_mode = true;
    ui_show_state(&st, &view);
    shot("02-fx-mode");

    static const struct { const char *name; uint8_t color, icon; } looper[UI_LOOPER_SWITCHES] = {   // as main.c's defaults
        { "Pause", 9, 3 }, { "Loop 1", 2, 1 }, { "Loop 2", 2, 1 }, { "Loop 3", 3, 1 }, { "Loop 4", 3, 1 }, { "Loop 5", 6, 1 }, { "Loop 6", 6, 1 },
    };
    for (int i = 0; i < UI_LOOPER_SWITCHES; i++) {
        snprintf(view.looper_names[i], sizeof(view.looper_names[i]), "%s", looper[i].name);
        view.looper_colors[i] = looper[i].color;
        view.looper_icons[i] = looper[i].icon;
    }
    view.looper = true;
    view.phone = true;
    ui_show_state(&st, &view);
    shot("18-looper-mode");
    open_looper_editor(1);
    shot("19-looper-tile");
    lv_obj_set_hidden(s_looper_editor, true);
    view.looper = false;
    ui_show_state(&st, &view);

    // Scenes of the preset: which effects are on and which of them the scene carries settings for (bit n = FX
    // slot n); the fourth is what is on now.
    static const struct { const char *name; uint8_t color, fx, set; } scenes[UI_SCENES] = {
        { "Clean", 4, 0x12, 0 }, { "Crunch", 3, 0x13, 0x01 }, { "Chorus", 6, 0x16, 0 }, { "Lead", 1, 0x1B, 0x09 },
        { "Ambient", 7, 0x1E, 0x18 }, { "", 0, 0, 0 },
    };
    for (int i = 0; i < UI_SCENES; i++) {
        snprintf(view.scene_names[i], sizeof(view.scene_names[i]), "%s", scenes[i].name);
        view.scene_colors[i] = scenes[i].color;
        view.scene_fx[i] = scenes[i].fx;
        view.scene_set[i] = scenes[i].set;
    }
    // Expression pedal: a wah-like sweep on Pre FX 1, the delay's amount, a little more level
    view.exp_used = true;
    ui_show_state(&st, &view);
    ui_set_jack(NANO_JACK_EXPRESSION);
    open_expression(NULL);
    static const nano_exp_range_t sweeps[NANO_EXP_RANGES] = { [0] = { true, 0, 255 }, [3] = { true, 51, 153 }, [9] = { true, 128, 166 } };
    // ... and switches Pre FX 1 on with the pedal off the heel, the reverb with a footswitch
    static const nano_exp_switch_t switches[NANO_EXP_SWITCHES] = {
        { false, 0, false, false, 600 }, { false, 0, false, false, 600 }, { true, NANO_EXP_HEEL_TOE, false, false, 400 },
        { false, 0, false, false, 600 }, { false, 0, false, false, 600 }, { false, 0, false, false, 600 },
        { true, NANO_EXP_SWITCH, false, true, 600 }, { false, 0, false, false, 600 },
    };
    ui_set_expression(sweeps, switches);
    shot("23-expression");
    on_ex_page(NULL);
    s_ex_sw_selected = NANO_EXP_SW_POST3;
    style_expression();
    shot("24-expression-switches");
    close_all();

    view.scenes = true;
    view.scene_active = 3;
    view.fx_mode = false;
    ui_show_state(&st, &view);
    shot("20-scene-mode");
    view.scene_capture[1] = 3;   // scene 2 loads capture slot 3
    ui_show_state(&st, &view);
    open_scene_editor(1);
    shot("21-scene");
    on_scene_capture(NULL);
    shot("34-scene-capture");
    lv_obj_set_hidden(s_scene_cap, true);
    lv_obj_set_hidden(s_scene_editor, true);
    view.scenes = false;
    view.fx_mode = true;
    ui_show_state(&st, &view);

    set_fullscreen(true);
    view.fx_mode = false;
    ui_show_state(&st, &view);
    shot("03-fullscreen");
    // Pedal view: the FX slots as pedals (drawn here; the own build has pictures in the flash memory)
    view.fx_mode = true;
    ui_set_pedal_view(true);
    ui_show_state(&st, &view);
    shot("39-pedal-view");
    st.fx_on[2] = !st.fx_on[2];
    view.rp_used = true;   // the reverb switch changes between two settings: the one that is on, above the reverb
    view.rp_set = 3;
    view.rp_active = 1;
    snprintf(view.rp_names[0], sizeof(view.rp_names[0]), "Room");
    snprintf(view.rp_names[1], sizeof(view.rp_names[1]), "Atmos");
    ui_show_state(&st, &view);
    shot("40-pedal-view-off");
    st.fx_on[2] = !st.fx_on[2];
    view.rp_used = false;
    view.rp_set = 0;
    view.rp_active = 0;
    view.rp_names[0][0] = view.rp_names[1][0] = 0;
    ui_set_pedal_view(false);
    view.fx_mode = false;
    ui_show_state(&st, &view);
    set_fullscreen(false);

    view.fx_mode = true;
    ui_show_state(&st, &view);
    static const float values[] = { 0.42f, 0.55f, 0.30f, 0.65f, 0.25f, 0.38f, 0.18f, 0.12f, 0.0f, 0.0f, 0.4f };
    ui_fx_editor_show(3, st.fx_type[3], true, values, 11);
    static const char fx_presets[UI_FX_PRESETS][16] = { "Slapback", "Ambient Wash", "", "" };
    ui_fx_presets_show(fx_presets, 1, true);
    shot("04-fx-editor");
    open_panel(PANEL_SCENE, 0);
    shot("22-scene-settings");
    close_panel();
    ui_fx_editor_close();
    close_all();

    ui_fx_editor_show(0, st.fx_type[0], true, values, 3);
    open_panel(PANEL_SECOND, 0);
    shot("13-second-effect");
    close_panel();
    open_panel(PANEL_MODEL, 0);
    shot("16-fx-model");
    ui_fx_editor_close();
    close_all();

    ui_fx_editor_show(3, st.fx_type[3], false, NULL, 0);
    shot("17-fx-off");
    ui_fx_editor_close();
    close_all();

    // Second effects: Pre FX 1 runs its B (the envelope filter); the reverb has two settings of Mind Hall and a tap
    // on its switch swaps them
    view.fx_mode = true;
    uint32_t pre1 = st.fx_type[0], rev_b = view.ab_b_type[4];
    st.fx_type[0] = view.ab_b_type[0];
    view.ab_a_type[0] = pre1;
    view.ab_active = 1u << 0 | 1u << 4;
    view.ab_b_type[4] = st.fx_type[4];
    view.ab_a_type[4] = st.fx_type[4];
    view.ab_mode[4] = UI_AB_PRESS;
    view.rev_b_type = view.rev_b_stored = st.fx_type[4];   // (footswitch 8 swaps the same two)
    view.rev_active = 1;
    ui_show_state(&st, &view);
    shot("29-second-effects");
    view.ab_active = 1u << 0;
    view.ab_a_type[4] = 0;
    view.rev_b_type = view.rev_b_stored = rev_b;
    view.rev_active = 0;
    ui_show_state(&st, &view);
    ui_fx_editor_show(4, st.fx_type[4], true, values, 6);
    ui_fx_presets_show(fx_presets, -1, true);
    open_panel(PANEL_SECOND, 0);
    shot("30-second-effect-choice");
    close_panel();
    ui_fx_editor_close();
    close_all();
    st.fx_type[0] = pre1;
    view.ab_a_type[0] = 0;
    view.ab_active = 0;
    view.ab_b_type[4] = rev_b;
    view.ab_mode[4] = UI_AB_HOLD;
    ui_show_state(&st, &view);

    // The reverb switch changes between two settings of the reverb: "Atmos" is in, "Room" comes with the next press
    uint32_t rev_was = view.rev_b_type;
    view.rev_b_type = 0;
    view.rp_used = true;
    view.rp_set = 3;
    view.rp_active = 1;
    snprintf(view.rp_names[0], sizeof(view.rp_names[0]), "Room");
    snprintf(view.rp_names[1], sizeof(view.rp_names[1]), "Atmos");
    ui_show_state(&st, &view);
    shot("37-reverb-settings-tile");
    open_mix_editor();
    shot("38-reverb-settings");
    close_all();
    view.rp_used = false;
    view.rp_set = 0;
    view.rp_active = 0;
    view.rp_names[0][0] = view.rp_names[1][0] = 0;
    view.rev_b_type = rev_was;
    ui_show_state(&st, &view);

    // The Nano's expression pedal has Pre FX 1 (its amount) and Post FX 2 (amount and on / off) in this preset
    view.exp_amount = 1u << 0 | 1u << 3;
    view.exp_switch = 1u << 3;
    ui_show_state(&st, &view);
    shot("35-pedal-marks");
    view.exp_amount = view.exp_switch = 0;
    ui_show_state(&st, &view);
    ui_show_system("Firmware: 1.15.0\nRunning for: 2 h 14 min\nStarted after: power-on\n\nNano: connected for 2 h 13 min\n"
                   "Connection lost: never since the start\n\nInternal memory: 86 KB free, lowest 79 KB\n"
                   "Settings memory: 49 % full (309 of 630)");
    shot("36-system");
    close_all();

    // Auto-wah: a wah on Pre FX 2 that the controller moves in tempo while its footswitch is held
    uint32_t pre2 = st.fx_type[1];
    bool pre2_on = st.fx_on[1];
    st.fx_type[1] = type_of("Crying Wah");
    st.fx_on[1] = true;
    view.fx_mode = true;
    view.wah_mode[1] = UI_WAH_HELD;
    view.wah_rate[1] = 3;
    ui_show_state(&st, &view);
    shot("25-auto-wah-tile");
    static const ui_autowah_t wah = { .mode = UI_WAH_HELD, .rate = 3, .shape = 0, .heel = 15, .toe = 85, .own_tempo = false, .bpm = 120, .nano_bpm = 120 };
    ui_set_autowah(&wah);
    static const float wah_values[] = { 0.62f };
    ui_fx_editor_show(1, st.fx_type[1], true, wah_values, 1);
    static const char no_presets[UI_FX_PRESETS][16] = { "", "", "", "" };
    ui_fx_presets_show(no_presets, 0, true);
    shot("26-auto-wah");
    ui_fx_editor_close();
    close_all();
    st.fx_type[1] = pre2;
    st.fx_on[1] = pre2_on;
    view.wah_mode[1] = UI_WAH_OFF;

    view.fx_mode = false;
    ui_show_state(&st, &view);
    open_bank_editor(1);
    shot("05-bank-editor");
    close_all();

    open_picker('C');
    s_picker_library = true;
    fill_picker();
    shot("06-capture-library");
    close_all();

    view.fx_mode = true;
    ui_show_state(&st, &view);
    open_mix_editor();
    shot("07-reverb");
    close_all();

    ui_show_tuner(true);
    ui_tuner_settings(440, false);
    nano_tuner_reading_t reading = { .note = "E", .cents = -6.5f, .valid = true };
    ui_show_tuner_reading(&reading);
    shot("08-tuner");
    ui_show_tuner(false);

    view.fx_mode = false;
    ui_show_state(&st, &view);
    open_usb(NULL);
    ui_set_usb_gain(-6.0f);
    shot("09-usb-volume");
    close_all();

    ui_midi_t midi = { .connected = true, .name = "WIDI Jack", .count = 2 };
    snprintf(midi.devices[0].name, sizeof(midi.devices[0].name), "WIDI Jack");
    midi.devices[0].rssi = -56;
    midi.devices[0].remembered = true;
    snprintf(midi.devices[1].name, sizeof(midi.devices[1].name), "MIDI Footswitch");
    midi.devices[1].rssi = -71;
    ui_set_midi(&midi);
    open_midi(NULL);
    shot("10-bluetooth-midi");
    close_all();

    open_volume(NULL);
    shot("11-capture-volume");
    close_all();

    st.amp[NANO_AMP_GAIN] = 140; st.amp[NANO_AMP_BASS] = 115; st.amp[NANO_AMP_MID] = 150; st.amp[NANO_AMP_TREBLE] = 128;
    ui_show_state(&st, &view);
    open_amp(NULL);
    shot("14-capture-amp");
    close_all();

    open_cab_settings(NULL);
    const float cab[NANO_CAB_SETTINGS] = { nano_cab_setting_normalized(NANO_CAB_OUTPUT, -2.0f),
                                           nano_cab_setting_normalized(NANO_CAB_HIGH_PASS, 80),
                                           nano_cab_setting_normalized(NANO_CAB_LOW_PASS, 7500) };
    char info[96];
    snprintf(info, sizeof(info), "%s  -  slot %d", st.cab, st.cab_slot);
    ui_set_cab_settings(cab, true, info);
    shot("12-cab-settings");
    close_all();

    // Tile layout: 6 fields (mode, Pre FX 1-2, tuner, Post FX 1-2; Post FX 3 and the reverb switch left out)
    static const uint8_t six[8] = { 1, 3, 4, 2, 5, 6, 7, 8 };
    ui_set_layout(6, six, UI_SWITCH1_ALL);
    view.fx_mode = true;
    ui_show_state(&st, &view);
    shot("27-six-fields");
    open_layout(NULL);
    shot("28-tile-layout");
    close_all();
    static const uint8_t eight[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ui_set_layout(8, eight, UI_SWITCH1_FX_PRESETS);

    // Set list: eight songs, the fourth (preset 8) is on
    static const uint8_t songs[] = { 7, 9, 12, 8, 10, 11, 7, 9 };
    view.setlist_scenes[1] = 2;   // song 2 starts with its scene "Crunch", song 4 with scene 3
    snprintf(view.setlist_scene_names[1], sizeof(view.setlist_scene_names[1]), "Crunch");
    view.setlist_scenes[3] = 3;
    snprintf(view.setlist_scene_names[3], sizeof(view.setlist_scene_names[3]), "Lead");
    view.fx_mode = false;
    view.scenes = false;
    view.setlist_on = true;
    view.setlist_count = sizeof(songs);
    memcpy(view.setlist_presets, songs, sizeof(songs));
    view.setlist_first = 0;
    view.setlist_song = 3;
    uint8_t banked[6];
    memcpy(banked, view.bank_presets, sizeof(banked));
    for (int i = 0; i < 6; i++) view.bank_presets[i] = songs[i];
    ui_show_state(&st, &view);
    shot("32-setlist-mode");
    open_setlist_at(3);
    shot("33-setlist");
    close_all();
    view.setlist_on = false;
    memset(view.setlist_scenes, 0, sizeof(view.setlist_scenes));
    memcpy(view.bank_presets, banked, sizeof(banked));
    ui_show_state(&st, &view);
    return 0;
}
