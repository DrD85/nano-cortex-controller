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

#ifndef SHOT_W   // the P4 board: 1024 x 600 (tools/screenshots/make_p4.sh)
#define SHOT_W 1024
#define SHOT_H 600
#endif
#define W SHOT_W
#define H SHOT_H
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
    view.ab_b_type[4] = type_of("Room");              // the reverb: Mind Hall (A) and Room (B) - footswitch 8 swaps them,
    view.ab_mode[4] = UI_AB_OFF;                      // the reverb's own switch only switches it on and off
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
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_mix_editor, s_learn,
                                   s_layout, s_settings, s_cab_settings, s_amp, s_toast, s_looper_editor, s_scene_editor, s_exp_dialog, s_cal_panel,
                                   s_setlist, s_scene_cap, s_metro, s_rot, s_presets };
    for (size_t i = 0; i < sizeof(overlays) / sizeof(overlays[0]); i++) lv_obj_set_hidden(overlays[i], true);
    update_main_hidden();
}

// P4: example looper / jam values for the screen's source
static ui_loop_t s_fake_loop;
static ui_jam_t s_fake_jam;
static ui_net_t s_fake_net;
static void fake_source(ui_loop_t *l, ui_jam_t *j, ui_net_t *n) { *l = s_fake_loop; *j = s_fake_jam; *n = s_fake_net; }
static const char *fake_name(int i)
{
    static const char *const N[] = { "Slow Blues in A", "Funk Groove E9", "Minor Swing Dm", "Rock Shuffle G", "Ballad in C", "Texas Blues E", "Modal Jam Dorian" };
    return i >= 0 && i < 7 ? N[i] : "";
}
static void fake_note(int i, char *out, size_t n)
{
    static const char *const N[] = { "\xE2\x88\x92" "17.3 LUFS \xC2\xB7 \xE2\x88\x92" "2.5", "\xE2\x88\x92" "14.1 LUFS", "\xE2\x88\x92" "19.8 LUFS \xC2\xB7 +1.5",
                                     "\xE2\x88\x92" "12.6 LUFS", "MEASURING", "MEASURING", "MEASURING \xC2\xB7 \xE2\x88\x92" "0.5" };
    snprintf(out, n, "%s", i >= 0 && i < 7 ? N[i] : "");
}
static void on_pad_corner_test(int k) { pad_move((float)(k % 2), (float)(k / 2)); }

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
    {   // P4: the phone and the Nano's output
        ui_phone_t ph = { .connected = true, .playing = true, .device = "iPhone", .title = "Slow Blues in A",
                          .artist = "Backing Tracks \xC2\xB7 Blues Jam Vol. 2", .pos_s = 134, .len_s = 330, .volume_db = -8.0f,
                          .mute_on_tuner = true };
        ui_set_phone(&ph);
        ui_set_output_level(true, -14.2f, 0.64f);
    }
    shot("01-preset-mode");
    open_phone(NULL);
    shot("50-phone");
    lv_obj_set_hidden(s_phone, true);

    view.fx_mode = true;
    ui_show_state(&st, &view);
    shot("02-fx-mode");

    // P4: the looper on the board and the jam tracks (example values through the screen's source)
    view.looper = true;
    s_fake_loop = (ui_loop_t){ .ready = true, .state = UI_LOOP_DUB, .pos_s = 5.0f, .len_s = 8.0f, .max_s = 45, .can_undo = true,
                               .bar_lines = true, .passes = 2, .bars = 4, .bpm = 120 };
    s_fake_jam = (ui_jam_t){ .card = true, .count = 7, .current = 0, .playing = true, .pos_s = 134, .len_s = 372, .volume_db = -6, .level_db = -2.5f, .measuring = 3, .target_lufs = -21 };
    ui_set_loop_source(fake_source, fake_name, fake_note);
    ui_show_state(&st, &view);
    shot("18-looper-mode");
    s_fake_loop = (ui_loop_t){ .ready = true, .state = UI_LOOP_EMPTY, .max_s = 45, .bar_lines = false };
    s_fake_jam.playing = false;
    s_fake_jam.current = -1;
    shot("18b-looper-empty");
    s_fake_loop = (ui_loop_t){ .ready = true, .state = UI_LOOP_REC, .pos_s = 6.4f, .max_s = 45, .bar_lines = true, .bpm = 120, .to_bar_line = true };
    shot("18c-looper-recording");
    s_fake_loop = (ui_loop_t){ .ready = true, .state = UI_LOOP_PLAY, .pos_s = 2.1f, .len_s = 12.0f, .max_s = 45, .can_undo = false, .can_redo = true,
                               .bar_lines = true, .bars = 6, .bpm = 120, .passes = 5 };
    s_fake_jam.current = 0;
    s_fake_jam.playing = true;
    shot("18d-looper-playing");
    // two parts and the drums
    s_fake_loop.part = 0, s_fake_loop.next_part = 1, s_fake_loop.drums_ready = true, s_fake_loop.drums_on = true, s_fake_loop.groove = 12;
    s_fake_loop.part_state[0] = UI_LOOP_PLAY, s_fake_loop.part_len_s[0] = 12.0f, s_fake_loop.part_state[1] = UI_LOOP_EMPTY;
    s_fake_loop.drum_sync = true, s_fake_loop.drum_level_db = -9;
    loop_poll(NULL);
    shot("18e-looper-to-part-b");
    s_fake_loop.next_part = -1, s_fake_loop.part_state[1] = UI_LOOP_PLAY, s_fake_loop.part_len_s[1] = 8.0f, s_fake_loop.paused = true;
    loop_poll(NULL);
    shot("18f-looper-paused");
    s_fake_loop.paused = false;
    loop_poll(NULL);
    ui_drum_menu_toggle();
    shot("18g-drum-menu");
    ui_drum_menu_toggle();
    {   // Settings > Wi-Fi and the list of networks around
        s_fake_net = (ui_net_t){ .state = UI_NET_STATION, .ssid = "FRITZ!Box 7590", .addr = "192.168.178.41", .scan_count = 5,
            .scan = { { "FRITZ!Box 7590", -48, true }, { "FRITZ!Box 7590 Gast", -55, true }, { "Telekom-8F21", -71, true },
                      { "Proberaum", -76, false }, { "Vodafone-2.4GHz", -84, true } } };
        loop_poll(NULL);
        lv_obj_set_hidden(s_settings, false);
        settings_select(SET_WIFI, false);
        loop_poll(NULL);
        shot("62-settings-wifi");
        netlist_open();
        s_fake_net.scanning = false;
        loop_poll(NULL);
        shot("63-wifi-networks");
        lv_obj_set_hidden(s_netlist, true);
        lv_obj_set_hidden(s_settings, true);
        s_fake_net = (ui_net_t){ 0 };
        loop_poll(NULL);
    }
    open_source(1);
    shot("51-jam");
    s_fake_net = (ui_net_t){ .state = UI_NET_STATION, .ssid = "FRITZ!Box 7590", .addr = "192.168.178.41", .received = 0,
                             .receiving = "Funk Groove E9", .percent = 46 };
    shot("51b-jam-wifi");
    s_fake_net = (ui_net_t){ .state = UI_NET_AP, .ssid = "Nano Board", .addr = "192.168.4.1", .ap_pass = "40718263" };
    s_fake_jam.measuring = 0;
    shot("51c-jam-own-network");
    s_fake_net = (ui_net_t){ 0 };
    open_source(0);
    shot("52-phone-chooser");
    lv_obj_set_hidden(s_phone, true);
    s_source = 0;
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
    {   // the reverb's FX presets: the two settings were taken from Room and Atmos
        static const char *const fxp[4] = { "Room", "Atmos", "Big Sky", "Hall" };
        view.rp_fxp_count = 4;
        for (int i = 0; i < 4; i++) snprintf(view.rp_fxp_names[i], sizeof(view.rp_fxp_names[i]), "%s", fxp[i]);
    }
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
    // The pad, the editor's second page: PAD beside the FX presets, or a long press on a pedal of the pedal view
    s_padv.prefer = true;
    ui_fx_editor_show(3, st.fx_type[3], true, values, 11);
    pad_move(0.80f, 0.24f);
    shot("56-fx-pad");
    on_pad_corner_test(3);
    shot("57-fx-pad-corner");
    s_padv.prefer = false;
    s_padv.waiting = 0;
    ui_fx_editor_show(3, st.fx_type[3], true, values, 11);
    // The info page, the editor's third page: "i" in the header
    info_set(true);
    shot("58-fx-info");
    lv_obj_update_layout(s_info_list);
    lv_obj_scroll_to_y(s_info_list, 330, LV_ANIM_OFF);
    shot("59-fx-info-sounds");
    lv_obj_send_event(s_info_apply[1], LV_EVENT_CLICKED, NULL);
    shot("60-fx-info-applied");
    s_padv.waiting = 0;
    info_set(false);
    ui_fx_editor_show(0, st.fx_type[0], true, values, 3);
    info_set(true);
    shot("61-fx-info-based-on");
    info_set(false);
    ui_fx_editor_show(3, st.fx_type[3], true, values, 11);
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
    view.ab_mode[4] = UI_AB_PRESS;                    // (chosen on the reverb's tile: footswitch 8 is the mix switch then)
    view.rev_b_type = 0;
    view.rev_b_stored = st.fx_type[4];
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
    view.ab_mode[4] = UI_AB_OFF;
    ui_show_state(&st, &view);

    // The reverb switch changes between two settings of the reverb: "Atmos" is in, "Room" comes with the next press
    uint32_t rev_was = view.rev_b_type;
    view.rev_b_type = 0;
    view.rp_used = true;
    view.rp_set = 3;
    view.rp_active = 1;
    snprintf(view.rp_names[0], sizeof(view.rp_names[0]), "Room");
    snprintf(view.rp_names[1], sizeof(view.rp_names[1]), "Atmos");
    {   // the reverb's FX presets: the two settings were taken from Room and Atmos
        static const char *const fxp[4] = { "Room", "Atmos", "Big Sky", "Hall" };
        view.rp_fxp_count = 4;
        for (int i = 0; i < 4; i++) snprintf(view.rp_fxp_names[i], sizeof(view.rp_fxp_names[i]), "%s", fxp[i]);
    }
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

    // Footswitch 8 as the mix switch at Pos 2, while the reverb's tile keeps its A / B (Room)
    view.rev_b_type = 0;
    view.mix_active = 1;
    ui_show_state(&st, &view);
    shot("41-mix-tile");
    open_mix_editor();
    shot("42-mix-dialog");
    close_all();
    view.rev_b_type = rev_was;
    view.mix_active = -1;
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
    ui_set_listening('C', 1, s_library->captures.items[1].name, false);   // listening without loading
    shot("43-capture-listening");
    ui_set_listening(0, 0, "", false);
    close_all();

    {   // the wheel while footswitch I of the Nano is turned
        ui_rotate_t rot = { .cab = false, .slot = 3, .from = 2, .prev = st.capture_names[1], .name = st.capture_names[2],
                            .next = st.capture_names[3], .from_name = st.capture_names[1] };
        ui_show_rotate(&rot);
        shot("48-rotate-capture");
        close_all();
    }

    {   // tempo and metronome (NanOS 2.3.0)
        ui_tempo_t tempo = { .supported = true, .global = true, .muted = false, .tempo = 120, .volume_db = -6, .time_sig = 2 };
        ui_set_tempo(&tempo);
        shot("44-tempo-button");
        tempo.open = true;
        ui_set_tempo(&tempo);
        shot("45-tempo-dialog");
        tempo.open = false;
        tempo.supported = false;
        ui_set_tempo(&tempo);
        close_all();
    }

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
    ui_set_link(true);
    ui_set_nano_firmware(2, 3);
    open_settings(NULL);   // the settings screen (1.17.0): Nano, MIDI, USB audio, tile layout, footswitches, system
    settings_select(SET_NANO, false);
    shot("46-settings-nano");
    settings_select(SET_USB, false);
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
    open_settings(NULL);
    settings_select(SET_MIDI, false);
    shot("10-bluetooth-midi");
    settings_select(SET_LAYOUT, false);
    shot("47-settings-layout");
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
    // Settings > Capture and the capture assistant (Neural Capture V1)
    ui_show_state(&st, &view);
    open_settings(NULL);
    ui_capture_t cap = { .step = UI_CAP_CLOSED, .gain = 160, .level = 200, .user_captures = 37 };
    ui_capture_show(&cap);
    settings_select(SET_CAPTURE, false);
    shot("49-settings-capture");
    on_settings_capture(NULL);
    cap.step = UI_CAP_CONNECT;
    ui_capture_show(&cap);
    shot("50-capture-connect");
    cap.step = UI_CAP_LEVELS;
    ui_capture_show(&cap);
    ui_capture_meter(0, 170);
    ui_capture_meter(1, 150);
    shot("51-capture-levels");
    cap.step = UI_CAP_TRAINING;
    cap.progress = 42;
    ui_capture_show(&cap);
    shot("52-capture-running");
    cap.step = UI_CAP_COMPARE;
    cap.progress = 100;
    cap_named("Plexi Crunch SM57");
    ui_capture_show(&cap);
    shot("53-capture-compare");
    on_cap_save_slot(NULL);
    s_capui.slot = 8;
    cap_slots_show();
    shot("54-capture-slot");
    cap.step = UI_CAP_DONE;
    cap.saved = "Plexi Crunch SM57 is saved\nMy Neural Captures - capture bank 2, slot 3";
    ui_capture_show(&cap);
    shot("55-capture-done");
    cap.step = UI_CAP_CLOSED;
    ui_capture_show(&cap);
    close_all();

    // The preset list (a tap on the preset's name), and a new preset: its place, what it starts from, its name
    view.fx_mode = false;
    ui_show_state(&st, &view);
    open_presets(NULL);
    shot("64-preset-list");
    lv_obj_send_event(s_pl_new, LV_EVENT_CLICKED, NULL);
    shot("65-new-preset-place");
    lv_obj_send_event(s_pl_cell[11], LV_EVENT_CLICKED, NULL);
    shot("66-new-preset");
    lv_obj_send_event(s_pl_start[1], LV_EVENT_CLICKED, NULL);   // "Copy of this preset" chosen
    shot("67-new-preset-copy");
    lv_obj_send_event(lv_obj_get_parent(s_pl_name_text), LV_EVENT_CLICKED, NULL);   // the name: the keyboard
    shot("68-new-preset-name");
    close_all();
    // ... and the list opened from the gig view's name, on the page of a preset further back
    {
        nano_state_t far = st;
        far.current_preset = 38;
        set_fullscreen(true);
        ui_show_state(&far, &view);
        open_presets(NULL);
        shot("69-preset-list-gig-view");
        close_all();
        set_fullscreen(false);
        ui_show_state(&st, &view);
    }
    // The pedal view with long names above the pedals (the slots' waiting effects): two lines in the P4's field
    {
        ui_view_t v = view;
        v.fx_mode = true;
        v.ab_b_type[1] = type_of("Solid State Comp (ST)");
        v.ab_b_type[3] = type_of("Dual Reverse Delay");
        set_fullscreen(true);
        ui_set_pedal_view(true);
        ui_show_state(&st, &v);
        shot("70-pedal-view-long-name");
        ui_set_pedal_view(false);
        set_fullscreen(false);
        ui_show_state(&st, &view);
    }
    // The info page's third group, only in an own build (`make_p4.sh private` with main/private/fx_neural_private.c):
    // Neural DSP's presets of the model. PRIVATE - this picture is never for the docs.
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        static float half[NANO_MAX_PARAMS];
        int n = 0;
        nano_fx_neural(st.fx_type[slot], &n);
        const nano_fx_model_t *m = nano_fx_model(st.fx_type[slot]);
        if (!n || !m || !nano_fx_info(m->type)) continue;
        for (int i = 0; i < NANO_MAX_PARAMS; i++) half[i] = 0.5f;
        view.fx_mode = true;
        ui_show_state(&st, &view);
        ui_fx_editor_show(slot, st.fx_type[slot], true, half, m->param_count);
        info_set(true);
        lv_obj_update_layout(s_info_list);
        lv_obj_scroll_to_view(lv_obj_get_parent(s_info_apply[s_info_sounds - n]), LV_ANIM_OFF);
        lv_obj_scroll_by(s_info_list, 0, 40, LV_ANIM_OFF);   // (the group's caption into sight)
        shot("71-fx-info-neural");
        lv_obj_scroll_to_y(s_info_list, LV_COORD_MAX, LV_ANIM_OFF);
        shot("72-fx-info-neural-end");
        info_set(false);
        ui_fx_editor_close();
        close_all();
        break;
    }
    return 0;
}
