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
    view.rev_b_type = type_of("Room");
    view.rev_active = 0;
    view.pre1_b_type = type_of("Envelope Filter");
    view.pre1_active = 0;
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

static void close_all(void)
{
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_usb, s_mix_editor, s_learn, s_midi,
                                   s_volume, s_cab_settings, s_amp, s_toast, s_looper_editor, s_scene_editor, s_exp_dialog, s_cal_panel };
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

    ui_init(on_command, on_param_cb, on_text_cb);
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
    open_scene_editor(1);
    shot("21-scene");
    lv_obj_set_hidden(s_scene_editor, true);
    view.scenes = false;
    view.fx_mode = true;
    ui_show_state(&st, &view);

    set_fullscreen(true);
    view.fx_mode = false;
    ui_show_state(&st, &view);
    shot("03-fullscreen");
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
    return 0;
}
