#include "ui.h"
#include "ui_fonts.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_lvgl_port.h"
#include "fx_icons.h"
#include "fx_pedals.h"
#include "library.h"
#include "ui_icons.h"
#include "preset_icons.h"
#include "lvgl.h"

#define SCREEN_W 800
// Eight tiles in two rows, one per footswitch (1-4 top, 5-8 bottom), Quad Cortex style.
#define TILES 8
#define TILE_W 186
#define TILE_H 139
#define TILE_GAP 12
#define TILE_Y 186
// Swipe up: the gig view - a slim status bar (preset, bank, mode) and the tiles over the rest of the screen;
// swipe down: back.
#define GIG_BAR_H 44
#define FULL_TILE_Y (GIG_BAR_H + 8)
#define FULL_TILE_H ((480 - FULL_TILE_Y - 8 - 8) / 2)
#define TUNER_TRACK_W 640
#define TUNER_NOTE_SIZE 160   // px

// Look of the desktop editor (1.5): dark gradient cards with a thin border, dialogs as sheets with a title and a
// round close button, filled = on / outlined = off, one set of corner radii (controls 9, panels 12, cards 16,
// dialogs 18), IBM Plex Sans (ui_fonts.c).
#define CARD_TOP 0x17191B
#define CARD_BOTTOM 0x090A0B
#define CARD_BORDER 0x2A2E33
#define TEXT 0xF2F2F2
#define MUTED 0x8E9297
#define INK 0x0D0F11          // dark text on light fills
#define TILE_OFF_BG 0x111315
#define PANEL_BG 0x121416
#define GREEN 0x45F862
#define ORANGE 0xFFB02E       // unsaved changes (the dot next to the name)

static ui_command_cb s_on_command;
static ui_param_cb s_on_param;
static ui_text_cb s_on_text;
static lv_obj_t *s_save_button, *s_ask, *s_ask_text, *s_ask_ok, *s_rename, *s_rename_title, *s_rename_area;
static lv_obj_t *s_rename_ok;
static char s_rename_kind = 'N';   // 'N' preset name, '1'-'4' FX preset
static char s_ask_command;
static int s_ask_arg;
static int s_current_preset;
static bool s_fx_mode;
static bool s_looper_mode;   // the tiles are the switches of a looper app (footswitch 1 held)

static lv_obj_t *s_main;   // everything of the main screen (hidden under the full-screen editor and tuner)
static lv_obj_t *s_status_dot, *s_status, *s_edited, *s_app, *s_midi_icon;
static lv_obj_t *s_gig_bar, *s_gig_dot, *s_gig_number, *s_gig_bank, *s_gig_name, *s_gig_dirty, *s_gig_mode;
static lv_obj_t *s_capture_title;
static uint32_t s_tile_accent[TILES];   // colour of the pedal drawing on the tile
static lv_obj_t *s_preset_number, *s_preset_name, *s_capture, *s_cab;
static lv_obj_t *s_tile[TILES], *s_tile_caption[TILES], *s_tile_name[TILES], *s_tile_number[TILES];
static lv_obj_t *s_tile_square[TILES], *s_tile_pedal[TILES];
static lv_obj_t *s_tile_other[TILES];   // second line under the caption: the other effect of an A/B slot
static uint32_t s_tile_ink[TILES];
static uint32_t s_tile_pedal_type[TILES], s_tile_other_type[TILES];   // shown by show_tile_extras
static const char *s_tile_note[TILES];   // a line of text under the caption instead (looper mode, what holding does), NULL = none
static const lv_image_dsc_t *s_tile_note_icon[TILES];   // ... with this symbol in front of it, NULL = none
static lv_obj_t *s_tile_note_image[TILES];
static uint32_t s_tile_head[TILES];   // colour of a tile that is not active, dimmed behind its top row (0 = none)
static lv_obj_t *s_capture_square, *s_capture_slot_label, *s_cab_square, *s_cab_slot_label;
static lv_obj_t *s_picker, *s_picker_title, *s_picker_list, *s_picker_tab[2], *s_picker_chips, *s_picker_chip[LIB_CATEGORY_COUNT];
static lv_obj_t *s_picker_pager, *s_picker_page_label, *s_picker_info, *s_confirm, *s_confirm_text;
static lv_obj_t *s_bank_editor, *s_bank_title, *s_bank_swatch[UI_BANK_COLOR_COUNT], *s_bank_list;
static lv_obj_t *s_toast;
static lv_obj_t *s_usb, *s_usb_slider, *s_usb_value, *s_usb_info, *s_usb_minus, *s_usb_plus, *s_usb_reset;
static float s_usb_db;
static lv_obj_t *s_volume, *s_volume_info, *s_cab_settings, *s_cab_info, *s_amp, *s_amp_info;
static int s_capture_volume = NANO_CAPTURE_VOLUME_0DB;
static int s_amp_values[NANO_AMP_KNOBS];
static uint32_t s_volume_tick, s_amp_tick;
static lv_obj_t *s_mix_editor, *s_mix_model_label, *s_mix_info, *s_mix_slider[2], *s_mix_value[2];
static lv_obj_t *s_mix_tab[2], *s_mix_panel[2], *s_rev_dropdown, *s_rev_edit, *s_mix_free;
static char s_mix_model[48];
static bool s_mix_touched;                 // Pos 1 / Pos 2 moved: SAVE writes them
static int s_mix_tab_shown;                // the tab in front: saved, it is what footswitch 8 does
static uint32_t s_rev_types[16];           // reverb models for the dropdown (index + 1; 0 = none)
static int s_rev_type_count;
static lv_obj_t *s_learn, *s_learn_title, *s_learn_text, *s_learn_first;
// Looper tile dialog (long press on a tile in looper mode): name, colour and symbol of a switch
static lv_obj_t *s_looper_editor, *s_le_title, *s_le_hint, *s_le_name_label, *s_le_swatch[UI_BANK_COLOR_COUNT];
static lv_obj_t *s_le_symbol[UI_LOOPER_SYMBOLS];
static int s_le_tile, s_le_color, s_le_icon;   // tile 0-6 = footswitch 2-8
static char s_le_name[UI_LOOPER_NAME];
static const char *const LOOPER_SYMBOLS[UI_LOOPER_SYMBOLS] = {
    NULL, LV_SYMBOL_LOOP, LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE, LV_SYMBOL_STOP, LV_SYMBOL_REFRESH, LV_SYMBOL_AUDIO,
};
// Scenes: the button next to Save, the marks on a scene's tile (which effects it switches on) and its dialog
// (long press on a tile in scene mode): name, colour and the five effects
static lv_obj_t *s_scene_button;
// Expression pedal: its button in the top bar (green while the pedal does something in this preset) and its dialog -
// the values the pedal sweeps, one field each, and the heel and toe value of the chosen one
static lv_obj_t *s_exp_button, *s_exp_dialog, *s_ex_sub, *s_ex_field[UI_EXP_TARGETS], *s_ex_caption[UI_EXP_TARGETS];
static lv_obj_t *s_ex_name[UI_EXP_TARGETS], *s_ex_range_label[UI_EXP_TARGETS], *s_ex_target, *s_ex_toggle, *s_ex_toggle_label;
static lv_obj_t *s_ex_slider[2], *s_ex_value[2], *s_ex_save;
static ui_exp_range_t s_ex_range[UI_EXP_TARGETS];
static int s_ex_selected, s_ex_preset;
static bool s_ex_loaded;      // the preset's assignments have arrived
// ... its second page: what the pedal switches on and off, and how
static lv_obj_t *s_ex_pages[2], *s_ex_page_button, *s_ex_calibrate;
static lv_obj_t *s_ex_sw_field[NANO_EXP_SWITCHES], *s_ex_sw_caption[NANO_EXP_SWITCHES], *s_ex_sw_name[NANO_EXP_SWITCHES];
static lv_obj_t *s_ex_sw_way[NANO_EXP_SWITCHES], *s_ex_sw_target, *s_ex_sw_toggle, *s_ex_sw_toggle_label, *s_ex_sw_mode[NANO_EXP_MODES];
static lv_obj_t *s_ex_sw_invert, *s_ex_sw_latch, *s_ex_sw_delay_name, *s_ex_sw_delay, *s_ex_sw_delay_value;
static nano_exp_switch_t s_ex_switch[NANO_EXP_SWITCHES];
static int s_ex_page, s_ex_sw_selected;
// ... and the calibration of the pedal: its position while it is moved over its whole way
static lv_obj_t *s_cal_panel, *s_cal_bar, *s_cal_text, *s_cal_save;
static lv_obj_t *s_ex_jack[2];   // the Nano's EXP/MIDI connector: [0] expression pedal, [1] MIDI
static int s_jack_mode = -1;     // NANO_JACK_*, -1 = not known
static bool s_linked;         // connected to the Nano
static int s_tile_marks[TILES];                 // 0 = none, else 0x200 | 0x100 if the tile is filled | bit n = FX slot n on
                                                // | bit 16 + n = the scene carries settings for slot n
static uint32_t s_slot_type[NANO_FX_SLOTS];     // effects of the current preset (filled by ui_show_state)
static bool s_slot_on[NANO_FX_SLOTS];
static lv_obj_t *s_scene_editor, *s_se_title, *s_se_hint, *s_se_name_label, *s_se_swatch[UI_BANK_COLOR_COUNT], *s_se_clear;
static lv_obj_t *s_se_fx[NANO_FX_SLOTS], *s_se_fx_caption[NANO_FX_SLOTS], *s_se_fx_state[NANO_FX_SLOTS], *s_se_fx_name[NANO_FX_SLOTS];
static lv_obj_t *s_se_fx_set[NANO_FX_SLOTS];    // "with settings": the scene carries values for this effect
static int s_se_scene, s_se_color, s_se_preset;
static uint8_t s_se_fx_on;                      // bit n = FX slot n on in the edited scene
static char s_se_name[UI_SCENE_NAME];
static lv_obj_t *s_preset_card, *s_source_card[2];
static lv_obj_t *s_midi_label, *s_midi, *s_midi_status, *s_midi_list;
static ui_midi_t s_midi_state;
static bool s_fullscreen;
static uint32_t s_gesture_tick;   // a swipe ends with a click on the tile under the finger: ignore that one
static lv_timer_t *s_toast_timer;

static const nano_library_t *s_library;
static char s_picker_kind = 'C';      // 'C' captures, 'I' cabs
static bool s_picker_library;
static int s_picker_category, s_picker_page, s_confirm_code;
static char s_preset_names[NANO_PRESETS][65];
static ui_view_t s_view;
static int s_be_bank, s_be_tile, s_be_preset, s_be_color, s_be_icon;
static lv_obj_t *s_bank_symbol[PRESET_ICON_COUNT + 1];   // 0 = no symbol

#define LIB_PAGE 30

// Colours for own banks (index 0 = default white).
static const uint32_t BANK_COLORS[UI_BANK_COLOR_COUNT] = {
    0xF2F2F2, 0xFF4D4D, 0xFF7000, 0xFFD236, 0x45F862, 0x00FFDD, 0x3D8BFF, 0x8A5CFF, 0xFF5CC8, 0x9A9A9A,
};

// Copies for the capture / cab picker (filled by ui_show_state).
static char s_capture_names[NANO_CAPTURE_SLOTS][48], s_cab_names[NANO_CAB_SLOTS][48];
static int s_capture_slot, s_cab_slot;
static lv_obj_t *s_tuner, *s_tuner_note, *s_tuner_needle, *s_tuner_cents, *s_tuner_hz, *s_tuner_mute;

// FX editor
static lv_obj_t *s_editor, *s_editor_card, *s_editor_chip, *s_editor_icon, *s_editor_slot, *s_editor_model, *s_editor_pedal;
static lv_obj_t *s_editor_onoff, *s_editor_switch, *s_editor_ab, *s_editor_ab_seg[2];
static lv_obj_t *s_editor_body, *s_editor_info, *s_editor_info_text;
static lv_obj_t *s_panel_scrim, *s_panel, *s_panel_title, *s_panel_sub, *s_panel_list;   // model / option choice
static int s_panel_mode, s_panel_param;
static int s_panel_built_mode = -1, s_panel_built_slot;   // the entries in s_panel_list (-1 = none)
static intptr_t s_panel_current;                          // model or option drawn as the current one
static lv_obj_t *s_fxp_bar, *s_fxp_chip[UI_FX_PRESETS + 1];   // FX presets above the parameters: 0 = ORIGINAL
static lv_obj_t *s_fxp_scene, *s_fxp_scene_icon, *s_fxp_scene_count;   // at their end: this effect's settings for a scene
static char s_fxp_names[UI_FX_PRESETS][16];
static int s_fxp_active = -1;
static bool s_fxp_usable;
static lv_obj_t *s_param_bar[NANO_MAX_PARAMS];
static float s_param_norm[NANO_MAX_PARAMS];   // value of each parameter (0-1), < 0 = not known
static char s_param_text[NANO_MAX_PARAMS][16]; // its text as drawn (number or option)
static int16_t s_param_unit_w[NANO_MAX_PARAMS];
static int32_t s_drag_x;                       // a bar follows the finger sideways (relative, from s_drag_from)
static float s_drag_from;
static lv_obj_t *s_drag_bar;
static int s_drag_param;
static bool s_dragging;
static int s_editor_slot_index = -1;
static uint32_t s_editor_model_type, s_editor_color = 0x5A5A5A;
static const nano_fx_model_t *s_editor_model_def;

// Black text on light tiles, white text on dark ones.
static uint32_t ink_for(uint32_t rgb)
{
    float r = ((rgb >> 16) & 0xff) / 255.0f, g = ((rgb >> 8) & 0xff) / 255.0f, b = (rgb & 0xff) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b < 0.3f ? 0xFFFFFF : INK;
}

static void send(char command, int arg)
{
    if (s_on_command) s_on_command(command, arg);
}

static void on_previous(lv_event_t *e) { send('k', -1); }
static void on_refresh(lv_event_t *e) { send('r', 0); }
static void on_next(lv_event_t *e) { send('k', 1); }
static void on_tuner(lv_event_t *e) { send('w', 2); }
static void on_tuner_down(lv_event_t *e) { send('^', -1); }
static void on_tuner_up(lv_event_t *e) { send('^', 1); }
static void on_tuner_mute(lv_event_t *e) { send('~', 0); }
static void on_scene_button(lv_event_t *e) { send('$', -1); }
static void open_expression(lv_event_t *e);
static void on_tile(lv_event_t *e)
{
    if (lv_tick_elaps(s_gesture_tick) < 150) return;   // end of a swipe, not a tap
    send('w', 1 + (int)(intptr_t)lv_event_get_user_data(e));
}

static void set_fullscreen(bool on);

static void open_bank_editor(int tile);
static void open_looper_editor(int tile);
static void open_scene_editor(int scene);
static void open_mix_editor(void);
static void open_learn(int number);

static void on_tile_long(lv_event_t *e)
{
    int tile = (int)(intptr_t)lv_event_get_user_data(e);   // 0-7 = footswitch 1-8
    if (tile == 0) send('w', UI_SWITCH_HOLD | 1);   // looper mode on / off, as holding footswitch 1
    else if (s_looper_mode) open_looper_editor(tile - 1);
    else if (tile == 1) open_learn(2);              // (footswitch 1 is learned from this dialog too)
    else if (s_fx_mode && tile < 2 + NANO_FX_SLOTS) send('O', tile - 2);
    else if (s_fx_mode && tile == 2 + NANO_FX_SLOTS) open_mix_editor();
    else if (!s_fx_mode && tile >= 2 && s_view.scenes) open_scene_editor(tile - 2);
    else if (!s_fx_mode && tile >= 2) open_bank_editor(tile - 2);
}

static lv_obj_t *s_splash, *s_splash_status, *s_splash_version;

static void on_gesture(lv_event_t *e)
{
    s_gesture_tick = lv_tick_get();
    if (s_splash) return;   // start screen
    // Swipes work only on the main screen, not in an open dialog.
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_usb, s_mix_editor, s_learn, s_midi,
                                   s_volume, s_cab_settings, s_amp, s_looper_editor, s_scene_editor, s_exp_dialog, s_cal_panel };
    for (size_t i = 0; i < sizeof(overlays) / sizeof(overlays[0]); i++) {
        if (overlays[i] && !lv_obj_is_hidden(overlays[i])) return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) send('n', 0);
    else if (dir == LV_DIR_RIGHT) send('p', 0);
    else if (dir == LV_DIR_TOP) set_fullscreen(true);
    else if (dir == LV_DIR_BOTTOM) set_fullscreen(false);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x191B1D), 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0x111315), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x34383C), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x3A3D40), 0);
    lv_obj_set_style_radius(btn, 9, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// The main action of a dialog: filled light (as the editor's primary buttons).
static void primary(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xECECEC), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xECECEC), 0);
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(INK), 0);
}

// A UI symbol (ui_icons.c) at a size, in a colour.
static lv_obj_t *ui_icon(lv_obj_t *parent, int icon, int size, uint32_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, UI_ICONS[icon]);
    lv_image_set_scale(img, (uint32_t)(256 * size / 36));
    lv_obj_set_size(img, size, size);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(img, lv_color_hex(color), 0);
    return img;
}

// Round close button of a dialog (top right).
static lv_obj_t *close_button(lv_obj_t *parent, int x, int y, lv_event_cb_t cb, int data)
{
    lv_obj_t *btn = button(parent, x - 8, y - 4, 52, 52, cb);   // x, y: where a 44 px button would sit
    lv_obj_remove_event_cb(btn, cb);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x33383E), 0);
    lv_obj_center(ui_icon(btn, UI_ICON_CLOSE, 20, 0xC8CCD1));
    return btn;
}

// Dialog title (sentence case, as in the editor) and the line below it.
static lv_obj_t *dialog_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &ui_font_title_22, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TEXT), 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, 20, 16);
    return l;
}

static void style_slider(lv_obj_t *slider, uint32_t color)
{
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2B2F34), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, LV_OPA_50, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 9, LV_PART_KNOB);
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(CARD_TOP), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(CARD_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollable(c, false);
    return c;
}

// Dialog sheet over the screen.
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = card(parent, x, y, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_shadow_width(c, 40, 0);
    lv_obj_set_style_shadow_color(c, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_70, 0);
    return c;
}

// Rounded square in a block colour with the icon on it (black or white, like the editor's .fx-icon).
static lv_obj_t *icon_square(lv_obj_t *parent, int size)
{
    lv_obj_t *sq = lv_obj_create(parent);
    lv_obj_set_size(sq, size, size);
    lv_obj_set_style_radius(sq, size / 4, 0);
    lv_obj_set_style_border_width(sq, 0, 0);
    lv_obj_set_style_pad_all(sq, 0, 0);
    lv_obj_set_scrollable(sq, false);
    lv_obj_set_clickable(sq, false);
    lv_obj_t *img = lv_image_create(sq);
    lv_obj_center(img);
    lv_image_set_scale(img, (uint32_t)(256 * (size - 8) / 36));
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_t *symbol = lv_label_create(sq);   // used instead of an image for tiles without an effect icon
    lv_obj_set_style_text_font(symbol, &ui_font_28, 0);
    lv_label_set_text(symbol, "");
    lv_obj_center(symbol);
    return sq;
}

// a * k + b * (1 - k) per channel
static uint32_t blend(uint32_t a, uint32_t b, float k)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        float v = ((a >> shift) & 0xff) * k + ((b >> shift) & 0xff) * (1 - k);
        out |= (uint32_t)(v + 0.5f) << shift;
    }
    return out;
}

// ---- Changing only what differs ----
// Every style or text change draws an object again, and the Nano's state arrives several times a second (mostly
// unchanged): the main screen and the FX editor set only what differs.

static void set_color_prop(lv_obj_t *obj, lv_style_prop_t prop, uint32_t rgb)
{
    lv_style_value_t old, v = { .color = lv_color_hex(rgb) };
    if (lv_obj_get_local_style_prop(obj, prop, &old, 0) == LV_STYLE_RES_FOUND && lv_color_eq(old.color, v.color)) return;
    lv_obj_set_local_style_prop(obj, prop, v, 0);
}

static void set_num_prop(lv_obj_t *obj, lv_style_prop_t prop, int32_t num)
{
    lv_style_value_t old, v = { .num = num };
    if (lv_obj_get_local_style_prop(obj, prop, &old, 0) == LV_STYLE_RES_FOUND && old.num == num) return;
    lv_obj_set_local_style_prop(obj, prop, v, 0);
}

static void set_ptr_prop(lv_obj_t *obj, lv_style_prop_t prop, const void *ptr)
{
    lv_style_value_t old, v = { .ptr = ptr };
    if (lv_obj_get_local_style_prop(obj, prop, &old, 0) == LV_STYLE_RES_FOUND && old.ptr == ptr) return;
    lv_obj_set_local_style_prop(obj, prop, v, 0);
}

static void set_align_if(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y)
{
    set_num_prop(obj, LV_STYLE_ALIGN, align);
    set_num_prop(obj, LV_STYLE_X, x);
    set_num_prop(obj, LV_STYLE_Y, y);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_is_hidden(obj) != hidden) lv_obj_set_hidden(obj, hidden);
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void set_image(lv_obj_t *img, const void *src)
{
    if (lv_image_get_src(img) != src) lv_image_set_src(img, src);
}

// The main screen is hidden while the full-screen editor or tuner covers it: hidden objects are not drawn again
// when the state changes (covered ones would be, under the editor).
static void update_main_hidden(void)
{
    set_hidden(s_main, !lv_obj_is_hidden(s_editor) || !lv_obj_is_hidden(s_tuner));
}

// Fills the top h rows of a rounded rectangle (its top corners stay round): the colour of a field that is not
// active, shown along its top - as the colour bars over the switch labels of a Morningstar MC8 Pro.
static void draw_top(lv_layer_t *layer, const lv_area_t *a, int radius, int h, uint32_t color)
{
    lv_area_t old = layer->_clip_area;
    lv_area_t clip = { LV_MAX(a->x1, old.x1), LV_MAX(a->y1, old.y1), LV_MIN(a->x2, old.x2), LV_MIN(a->y1 + h - 1, old.y2) };
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;
    layer->_clip_area = clip;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.radius = radius;
    lv_draw_rect(layer, &r, a);
    layer->_clip_area = old;
}

static void set_icon_square(lv_obj_t *sq, uint32_t color, const lv_image_dsc_t *icon, bool active)
{
    lv_obj_t *img = lv_obj_get_child(sq, 0);
    set_color_prop(sq, LV_STYLE_BG_COLOR, color);
    set_num_prop(sq, LV_STYLE_OPA, active ? LV_OPA_COVER : 115);
    set_hidden(img, icon == NULL);
    set_text(lv_obj_get_child(sq, 1), "");
    if (icon) {
        set_image(img, icon);
        set_color_prop(img, LV_STYLE_IMAGE_RECOLOR, ink_for(color));
    }
}

static int tile_height(void)
{
    return s_fullscreen ? FULL_TILE_H : TILE_H;
}

static void place_tile(int i)
{
    int h = tile_height();
    lv_obj_set_size(s_tile[i], TILE_W, h);
    lv_obj_set_pos(s_tile[i], 10 + (i % 4) * (TILE_W + TILE_GAP), (s_fullscreen ? FULL_TILE_Y : TILE_Y) + (i / 4) * (h + 8));
}

// True if every word of the text fits into one line (a font that would break a word is not used).
static bool words_fit(const char *text, const lv_font_t *font, int width)
{
    char word[66];
    while (*text) {
        while (*text == ' ') text++;
        size_t n = strcspn(text, " ");
        if (!n) break;
        snprintf(word, sizeof(word), "%.*s", (int)(n < sizeof(word) - 1 ? n : sizeof(word) - 1), text);
        lv_point_t size;
        lv_text_get_size(&size, word, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > width) return false;
        text += n;
    }
    return true;
}

// One font size for all names (as large as every name allows: whole words, at most the room below the icon row),
// so the tiles read evenly - larger in the gig view.
static void fit_names(void)
{
    static const lv_font_t *const gig[] = { &ui_font_title_40, &ui_font_title_34, &ui_font_title_30, &ui_font_title_26 };
    static const lv_font_t *const normal[] = { &ui_font_title_26, &ui_font_24, &ui_font_20, &ui_font_16 };
    // Measuring takes a while and the names rarely change: only when a name or the view (gig / normal) changed.
    static char measured[TILES][65];
    static int measured_view = -1;
    bool same = measured_view == s_fullscreen;
    for (int i = 0; i < TILES && same; i++) same = !strcmp(measured[i], lv_label_get_text(s_tile_name[i]));
    if (same) return;
    measured_view = s_fullscreen;
    for (int i = 0; i < TILES; i++) strlcpy(measured[i], lv_label_get_text(s_tile_name[i]), sizeof(measured[i]));
    const lv_font_t *const *fonts = s_fullscreen ? gig : normal;
    int room = tile_height() - (s_fullscreen ? 66 : 56);
    const lv_font_t *font = fonts[3];
    for (int f = 0; f < 4; f++) {
        int line_space = -lv_font_get_line_height(fonts[f]) / 6;
        bool fits = true;
        for (int i = 0; i < TILES && fits; i++) {
            const char *name = lv_label_get_text(s_tile_name[i]);
            if (!name[0]) continue;
            lv_point_t size;
            lv_text_get_size(&size, name, fonts[f], 0, line_space, TILE_W - 24, LV_TEXT_FLAG_NONE);
            fits = size.y <= room && words_fit(name, fonts[f], TILE_W - 24);
        }
        if (fits) {
            font = fonts[f];
            break;
        }
    }
    for (int i = 0; i < TILES; i++) {
        set_ptr_prop(s_tile_name[i], LV_STYLE_TEXT_FONT, font);
        set_num_prop(s_tile_name[i], LV_STYLE_TEXT_LINE_SPACE, -lv_font_get_line_height(font) / 6);
        set_align_if(s_tile_name[i], LV_ALIGN_BOTTOM_LEFT, 12, -10);
    }
}

// A scene's tile: five marks in its top row, one per FX slot in the effect's colour - filled = the scene switches
// it on, outlined = off, a dot = the slot is empty. On the filled tile of the current scene they are in its ink.
static void draw_scene_marks(lv_layer_t *layer, const lv_area_t *tile, int i)
{
    bool filled = s_tile_marks[i] & 0x100;
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        const nano_fx_model_t *model = nano_fx_model(s_slot_type[slot]);
        bool on = (s_tile_marks[i] >> slot) & 1;
        uint32_t color = filled ? s_tile_ink[i] : model ? model->color : 0x4A4F55;
        lv_area_t a = { tile->x1 + 14 + slot * 21, tile->y1 + 16, tile->x1 + 14 + slot * 21 + 14, tile->y1 + 16 + 14 };
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = 4;
        if (!model) {
            a = (lv_area_t){ a.x1 + 5, a.y1 + 5, a.x2 - 5, a.y2 - 5 };
            r.radius = LV_RADIUS_CIRCLE;
            r.bg_color = lv_color_hex(color);
            r.bg_opa = filled ? LV_OPA_50 : LV_OPA_COVER;
        } else if (on) {
            r.bg_color = lv_color_hex(color);
        } else {
            r.bg_opa = LV_OPA_TRANSP;
            r.border_width = 2;
            r.border_color = lv_color_hex(color);
            r.border_opa = filled ? LV_OPA_50 : 150;
        }
        lv_draw_rect(layer, &r, &a);
        if (model && ((s_tile_marks[i] >> (16 + slot)) & 1)) {   // the scene carries settings for this effect
            lv_area_t bar = { a.x1 + 1, a.y2 + 4, a.x2 - 1, a.y2 + 6 };
            lv_draw_rect_dsc_init(&r);
            r.radius = 1;
            r.bg_color = lv_color_hex(color);
            r.bg_opa = on ? LV_OPA_COVER : filled ? LV_OPA_50 : 150;
            lv_draw_rect(layer, &r, &bar);
        }
    }
}

// A tile that is not active: its top row (symbol, caption, number) lies on the dimmed colour.
static void on_tile_draw(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_tile_head[i] && !s_tile_marks[i]) return;
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
    lv_area_t in = { a.x1 + 2, a.y1 + 2, a.x2 - 2, a.y2 - 2 };   // inside the border
    if (s_tile_head[i]) draw_top(lv_event_get_layer(e), &in, 14, 46, blend(s_tile_head[i], TILE_OFF_BG, 0.24f));
    if (s_tile_marks[i]) draw_scene_marks(lv_event_get_layer(e), &a, i);
}

static void build_tile(lv_obj_t *parent, int i)
{
    lv_obj_t *tile = lv_obj_create(parent);
    s_tile[i] = tile;
    place_tile(i);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_set_style_opa(tile, 190, LV_STATE_PRESSED);
    lv_obj_add_event_cb(tile, on_tile, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tile, on_tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tile, on_tile_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);

    // Line drawing of the pedal at the right edge, behind everything else (FX tiles only)
    s_tile_pedal[i] = lv_image_create(tile);
    lv_obj_set_style_image_recolor_opa(s_tile_pedal[i], LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(s_tile_pedal[i], 64, 0);   // 25 %
    lv_obj_align(s_tile_pedal[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_hidden(s_tile_pedal[i], true);

    // The symbol without a square (as on the editor's signal chain), in the effect colour or the text colour.
    s_tile_square[i] = icon_square(tile, 40);
    lv_obj_set_pos(s_tile_square[i], 8, 8);
    lv_obj_set_style_bg_opa(s_tile_square[i], LV_OPA_TRANSP, 0);

    s_tile_caption[i] = label(tile, &ui_font_12, TEXT);
    lv_obj_set_style_text_letter_space(s_tile_caption[i], 2, 0);
    lv_obj_set_width(s_tile_caption[i], TILE_W - 96);
    lv_label_set_long_mode(s_tile_caption[i], LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_tile_caption[i], 56, 14);

    s_tile_other[i] = label(tile, &ui_font_12, TEXT);
    lv_obj_set_size(s_tile_other[i], TILE_W - 66, lv_font_get_line_height(&ui_font_12));   // one line
    lv_label_set_long_mode(s_tile_other[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_tile_other[i], 56, 32);
    lv_obj_set_hidden(s_tile_other[i], true);
    s_tile_note_image[i] = lv_image_create(tile);   // a small symbol in front of that line
    lv_image_set_scale(s_tile_note_image[i], 256 * 15 / 36);
    lv_obj_set_size(s_tile_note_image[i], 15, 15);
    lv_image_set_inner_align(s_tile_note_image[i], LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_image_recolor_opa(s_tile_note_image[i], LV_OPA_COVER, 0);
    lv_obj_set_pos(s_tile_note_image[i], 55, 32);
    lv_obj_set_hidden(s_tile_note_image[i], true);

    s_tile_number[i] = label(tile, &ui_font_20, TEXT);
    lv_label_set_text_fmt(s_tile_number[i], "%d", i + 1);
    lv_obj_align(s_tile_number[i], LV_ALIGN_TOP_RIGHT, -12, 8);

    s_tile_name[i] = label(tile, &ui_font_title_26, TEXT);
    lv_obj_set_width(s_tile_name[i], TILE_W - 24);
    lv_label_set_long_mode(s_tile_name[i], LV_LABEL_LONG_WRAP);
    lv_obj_align(s_tile_name[i], LV_ALIGN_BOTTOM_LEFT, 12, -10);
}

// Footswitch pressed (or tile tapped): the tile lights up for a moment - the press arrived.
static void flash_cb(void *obj, int32_t v)
{
    lv_obj_set_style_outline_opa(obj, (lv_opa_t)v, 0);
}

void ui_flash_tile(int i)
{
    if (i < 0 || i >= TILES) return;
    lvgl_port_lock(0);
    lv_obj_t *t = s_tile[i];
    lv_obj_set_style_outline_width(t, 4, 0);
    lv_obj_set_style_outline_pad(t, 2, 0);
    lv_obj_set_style_outline_color(t, lv_color_white(), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_exec_cb(&a, flash_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lvgl_port_unlock();
}

// One footswitch tile, as on the editor's signal chain: filled with `color` when active (glowing a little), dark
// with a border and symbol in `color` when not - on stage the difference shows from afar. icon or symbol is drawn
// without a square; name is the big text at the bottom (one size for all tiles, fit_names).
static void set_tile(int i, const char *caption, const char *name, uint32_t color, bool active, bool empty,
                     const lv_image_dsc_t *icon, const char *symbol)
{
    uint32_t bg = empty ? 0x0D0E10 : active ? color : TILE_OFF_BG;
    uint32_t border = empty ? 0x24272B : active ? color : blend(color, TILE_OFF_BG, 0.62f);
    uint32_t ink = empty ? 0x5D6267 : active ? ink_for(color) : TEXT;
    uint32_t mark = empty ? 0x3A3F45 : active ? ink : color;   // symbol colour
    s_tile_ink[i] = ink;
    s_tile_accent[i] = mark;
    s_tile_pedal_type[i] = 0;   // set again by set_tile_pedal / set_tile_other, shown by show_tile_extras
    s_tile_other_type[i] = 0;
    s_tile_note[i] = NULL;
    s_tile_note_icon[i] = NULL;
    s_tile_marks[i] = 0;        // set again by show_scene_tiles
    lv_obj_t *t = s_tile[i];
    uint32_t head = empty || active ? 0 : color;
    if (head != s_tile_head[i]) {
        s_tile_head[i] = head;
        lv_obj_invalidate(t);
    }
    set_color_prop(t, LV_STYLE_BG_COLOR, bg);
    set_num_prop(t, LV_STYLE_BG_GRAD_DIR, LV_GRAD_DIR_NONE);
    set_color_prop(t, LV_STYLE_BORDER_COLOR, border);
    set_num_prop(t, LV_STYLE_SHADOW_WIDTH, active ? 24 : 0);
    set_color_prop(t, LV_STYLE_SHADOW_COLOR, color);
    set_num_prop(t, LV_STYLE_SHADOW_OPA, LV_OPA_40);

    lv_obj_t *sq = s_tile_square[i];
    set_hidden(sq, !icon && !symbol);
    lv_obj_t *img = lv_obj_get_child(sq, 0), *sym = lv_obj_get_child(sq, 1);
    set_hidden(img, icon == NULL);
    if (icon) {
        set_image(img, icon);
        set_color_prop(img, LV_STYLE_IMAGE_RECOLOR, mark);
    }
    set_text(sym, symbol ? symbol : "");
    set_color_prop(sym, LV_STYLE_TEXT_COLOR, mark);
    set_num_prop(s_tile_caption[i], LV_STYLE_X, (icon || symbol) ? 56 : 14);
    set_num_prop(s_tile_caption[i], LV_STYLE_Y, 14);
    set_num_prop(s_tile_caption[i], LV_STYLE_WIDTH, (icon || symbol) ? TILE_W - 96 : TILE_W - 54);

    char upper[40];   // captions in small capitals, as the editor's labels
    size_t n = 0;
    for (; caption[n] && n < sizeof(upper) - 1; n++) upper[n] = (char)((caption[n] >= 'a' && caption[n] <= 'z') ? caption[n] - 32 : caption[n]);
    upper[n] = 0;
    set_text(s_tile_caption[i], upper);
    set_color_prop(s_tile_caption[i], LV_STYLE_TEXT_COLOR, active ? ink : MUTED);
    set_num_prop(s_tile_caption[i], LV_STYLE_TEXT_OPA, active ? 210 : LV_OPA_COVER);
    set_color_prop(s_tile_number[i], LV_STYLE_TEXT_COLOR, active ? ink : 0x5D6267);
    set_num_prop(s_tile_number[i], LV_STYLE_TEXT_OPA, active ? 170 : LV_OPA_COVER);

    set_text(s_tile_name[i], name);
    set_color_prop(s_tile_name[i], LV_STYLE_TEXT_COLOR, ink);
}

// The other effect of an A/B slot, under the caption (after set_tile): "⇄ name".
static void set_tile_other(int i, uint32_t type)
{
    s_tile_other_type[i] = type;
}

// After the tiles are set: the pedal drawings (FX tiles), the A/B lines and the marks of the scenes.
static void show_tile_extras(void)
{
    char text[48];
    static int drawn_marks[TILES];
    static uint32_t drawn_types[NANO_FX_SLOTS];   // the marks have the colours of these effects
    bool types_changed = memcmp(drawn_types, s_slot_type, sizeof(drawn_types)) != 0;
    memcpy(drawn_types, s_slot_type, sizeof(drawn_types));
    for (int i = 0; i < TILES; i++) {
        if (s_tile_marks[i] != drawn_marks[i] || (types_changed && s_tile_marks[i])) {
            drawn_marks[i] = s_tile_marks[i];
            lv_obj_invalidate(s_tile[i]);
        }
        const lv_image_dsc_t *pedal = s_tile_pedal_type[i] ? nano_fx_pedal_tile(s_tile_pedal_type[i]) : NULL;
        set_hidden(s_tile_pedal[i], pedal == NULL);
        if (pedal) {
            set_image(s_tile_pedal[i], pedal);
            set_color_prop(s_tile_pedal[i], LV_STYLE_IMAGE_RECOLOR, s_tile_accent[i]);
        }
        uint32_t other = s_tile_other_type[i];
        const lv_image_dsc_t *note_icon = s_tile_note[i] ? s_tile_note_icon[i] : NULL;
        set_hidden(s_tile_other[i], !other && !s_tile_note[i]);
        set_hidden(s_tile_note_image[i], note_icon == NULL);
        if (other || s_tile_note[i]) {
            if (s_tile_note[i]) snprintf(text, sizeof(text), "%s", s_tile_note[i]);
            else snprintf(text, sizeof(text), LV_SYMBOL_SHUFFLE " %s", nano_fx_name(other));
            set_text(s_tile_other[i], text);
            uint32_t color = s_tile_ink[i] == TEXT ? 0xC8CCD1 : s_tile_ink[i];   // a little lighter than the caption: it is read
            set_color_prop(s_tile_other[i], LV_STYLE_TEXT_COLOR, color);
            set_num_prop(s_tile_other[i], LV_STYLE_TEXT_OPA, 230);
            set_num_prop(s_tile_other[i], LV_STYLE_X, note_icon ? 75 : 56);
            if (note_icon) {
                set_image(s_tile_note_image[i], note_icon);
                set_color_prop(s_tile_note_image[i], LV_STYLE_IMAGE_RECOLOR, color);
            }
        }
    }
}

// ---- capture / cab picker: slots of the Nano or its library ----

static void on_picker_close(lv_event_t *e)
{
    lv_obj_set_hidden(s_picker, true);
}

static void on_picker_choice(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);   // 'C' / 'I' << 8 | slot
    lv_obj_set_hidden(s_picker, true);
    send((char)(code >> 8), code & 0xFF);
}

static lv_obj_t *picker_entry(const char *text, const char *tag, bool current)
{
    lv_obj_t *btn = lv_button_create(s_picker_list);
    lv_obj_set_size(btn, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(btn, lv_color_hex(current ? 0x2A2E33 : 0x1A1D20), 0);
    lv_obj_set_style_border_width(btn, current ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(GREEN), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 9, 0);
    lv_obj_t *l = label(btn, &ui_font_20, current ? 0xFFFFFF : 0xD8D8D8);
    lv_obj_set_width(l, tag ? 440 : 520);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (tag) {
        lv_obj_t *t = label(btn, &ui_font_14, 0x45F862);
        lv_label_set_text(t, tag);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return btn;
}

static void picker_section(const char *text)
{
    lv_obj_t *l = label(s_picker_list, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    lv_label_set_text(l, text);
}

static void style_tab(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? 0xECECEC : 0x191B1D), 0);
    lv_obj_set_style_bg_grad_dir(btn, active ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(active ? 0xECECEC : 0x3A3D40), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_hex(active ? INK : TEXT), 0);
}

static const lib_list_t *picker_library_list(void)
{
    if (!s_library) return NULL;
    return s_picker_kind == 'I' ? &s_library->cabs : &s_library->captures;
}

static void on_library_entry(lv_event_t *e);

static void fill_picker(void)
{
    char text[80];
    lv_obj_clean(s_picker_list);
    lv_obj_set_hidden(s_confirm, true);
    bool cab = s_picker_kind == 'I';
    lv_label_set_text(s_picker_title, cab ? "Cab / IR" : "Capture");
    style_tab(s_picker_tab[0], !s_picker_library);
    style_tab(s_picker_tab[1], s_picker_library);
    lv_obj_set_hidden(s_picker_chips, !s_picker_library || cab);
    lv_obj_set_hidden(s_picker_pager, !s_picker_library);
    lv_obj_set_hidden(s_picker_info, !s_picker_library);
    int top = s_picker_library ? (cab ? 92 : 136) : 60;
    lv_obj_set_pos(s_picker_list, 10, top);
    lv_obj_set_size(s_picker_list, 580, (s_picker_library ? 384 : 432) - top);

    if (!s_picker_library) {
        if (cab) {
            lv_obj_add_event_cb(picker_entry("Bypass", NULL, s_cab_slot == 0), on_picker_choice, LV_EVENT_CLICKED, (void *)(intptr_t)('I' << 8));
            picker_section("SLOTS");
            for (int slot = 1; slot <= NANO_CAB_SLOTS; slot++) {
                snprintf(text, sizeof(text), "%d   %.47s", slot, s_cab_names[slot - 1][0] ? s_cab_names[slot - 1] : "-");
                lv_obj_add_event_cb(picker_entry(text, NULL, slot == s_cab_slot), on_picker_choice, LV_EVENT_CLICKED, (void *)(intptr_t)('I' << 8 | slot));
            }
        } else {
            lv_obj_add_event_cb(picker_entry("Bypass", NULL, s_capture_slot == 0), on_picker_choice, LV_EVENT_CLICKED, (void *)(intptr_t)('C' << 8));
            for (int bank = 0; bank < 5; bank++) {
                snprintf(text, sizeof(text), "BANK %d", bank + 1);
                picker_section(text);
                for (int pos = 0; pos < 5; pos++) {
                    int slot = bank * 5 + pos + 1;
                    snprintf(text, sizeof(text), "%d   %.47s", pos + 1, s_capture_names[slot - 1][0] ? s_capture_names[slot - 1] : "-");
                    lv_obj_add_event_cb(picker_entry(text, NULL, slot == s_capture_slot), on_picker_choice, LV_EVENT_CLICKED, (void *)(intptr_t)('C' << 8 | slot));
                }
            }
        }
        return;
    }

    // Library: filtered, one page at a time.
    int slot = cab ? s_cab_slot : s_capture_slot;
    if (!slot) snprintf(text, sizeof(text), "Choose a slot first - the library loads into the active slot.");
    else if (cab) snprintf(text, sizeof(text), "Loads into cab slot %d", slot);
    else snprintf(text, sizeof(text), "Loads into capture Bank %d - %d", (slot - 1) / 5 + 1, (slot - 1) % 5 + 1);
    lv_label_set_text(s_picker_info, text);
    for (int c = 0; c < LIB_CATEGORY_COUNT; c++) style_tab(s_picker_chip[c], c == s_picker_category);

    const lib_list_t *list = picker_library_list();
    if (!list) {
        picker_section("The library is still loading ...");
        lv_label_set_text(s_picker_page_label, "");
        return;
    }
    int matches = 0, shown = 0, first = s_picker_page * LIB_PAGE;
    for (int i = 0; i < list->count; i++) {
        if (!cab && s_picker_category != LIB_ALL && list->items[i].category != s_picker_category) continue;
        if (matches >= first && shown < LIB_PAGE) {
            lv_obj_t *btn = picker_entry(list->items[i].name, list->items[i].user ? "USER" : NULL, false);
            lv_obj_add_event_cb(btn, on_library_entry, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            shown++;
        }
        matches++;
    }
    if (!matches) picker_section("Nothing in this category");
    snprintf(text, sizeof(text), "%d-%d of %d", matches ? first + 1 : 0, first + shown, matches);
    lv_label_set_text(s_picker_page_label, text);
}

static void on_picker_tab(lv_event_t *e)
{
    s_picker_library = (int)(intptr_t)lv_event_get_user_data(e);
    s_picker_page = 0;
    fill_picker();
}

static void on_picker_chip(lv_event_t *e)
{
    s_picker_category = (int)(intptr_t)lv_event_get_user_data(e);
    s_picker_page = 0;
    fill_picker();
}

static void on_picker_page(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    const lib_list_t *list = picker_library_list();
    if (!list) return;
    int matches = 0;
    for (int i = 0; i < list->count; i++) {
        if (s_picker_kind == 'C' && s_picker_category != LIB_ALL && list->items[i].category != s_picker_category) continue;
        matches++;
    }
    int pages = (matches + LIB_PAGE - 1) / LIB_PAGE;
    int page = s_picker_page + step;
    if (page < 0 || page >= pages) return;
    s_picker_page = page;
    fill_picker();
}

static void on_library_entry(lv_event_t *e)
{
    int position = (int)(intptr_t)lv_event_get_user_data(e);
    const lib_list_t *list = picker_library_list();
    if (!list || position >= list->count) return;
    char text[120];
    int slot = s_picker_kind == 'I' ? s_cab_slot : s_capture_slot;
    if (!slot) {
        ui_show_message("Choose a slot first - the library loads into the active slot.");
        return;
    }
    snprintf(text, sizeof(text), "Load \"%.60s\" into this slot?", list->items[position].name);
    lv_label_set_text(s_confirm_text, text);
    s_confirm_code = (s_picker_kind == 'I' ? 1 << 16 : 0) | position;
    lv_obj_set_hidden(s_confirm, false);
}

static void on_confirm(lv_event_t *e)
{
    bool load = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_confirm, true);
    if (!load) return;
    lv_obj_set_hidden(s_picker, true);
    send('L', s_confirm_code);
}

static void open_picker(char kind)
{
    s_picker_kind = kind;
    s_picker_library = false;
    s_picker_page = 0;
    fill_picker();
    lv_obj_set_hidden(s_picker, false);
}

static void open_capture_picker(lv_event_t *e) { open_picker('C'); }
static void open_cab_picker(lv_event_t *e) { open_picker('I'); }

static lv_obj_t *small_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, lv_event_cb_t cb, int data)
{
    lv_obj_t *btn = button(parent, x, y, w, h, cb);
    lv_obj_remove_event_cb(btn, cb);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t *l = label(btn, &ui_font_16, TEXT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static void build_picker(lv_obj_t *screen)
{
    s_picker = panel(screen, 100, 20, 600, 450);
    s_picker_title = dialog_title(s_picker, "");
    s_picker_tab[0] = small_button(s_picker, 220, 12, 120, 44, "Slots", on_picker_tab, 0);
    s_picker_tab[1] = small_button(s_picker, 348, 12, 120, 44, "Library", on_picker_tab, 1);
    close_button(s_picker, 540, 12, on_picker_close, 0);

    s_picker_info = label(s_picker, &ui_font_14, MUTED);
    lv_obj_set_pos(s_picker_info, 20, 64);

    s_picker_chips = lv_obj_create(s_picker);
    lv_obj_set_pos(s_picker_chips, 10, 86);
    lv_obj_set_size(s_picker_chips, 580, 46);
    lv_obj_set_style_bg_opa(s_picker_chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_chips, 0, 0);
    lv_obj_set_style_pad_all(s_picker_chips, 0, 0);
    lv_obj_set_scrollable(s_picker_chips, false);
    for (int c = 0; c < LIB_CATEGORY_COUNT; c++) {
        s_picker_chip[c] = small_button(s_picker_chips, c * 97, 2, 92, 40, LIB_CATEGORY_NAMES[c], on_picker_chip, c);
    }

    s_picker_list = lv_obj_create(s_picker);
    lv_obj_set_style_bg_opa(s_picker_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_list, 0, 0);
    lv_obj_set_style_pad_all(s_picker_list, 4, 0);
    lv_obj_set_style_pad_row(s_picker_list, 6, 0);
    lv_obj_set_flex_flow(s_picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_picker_list, LV_DIR_VER);

    s_picker_pager = lv_obj_create(s_picker);
    lv_obj_set_pos(s_picker_pager, 10, 390);
    lv_obj_set_size(s_picker_pager, 580, 50);
    lv_obj_set_style_bg_opa(s_picker_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_pager, 0, 0);
    lv_obj_set_style_pad_all(s_picker_pager, 0, 0);
    lv_obj_set_scrollable(s_picker_pager, false);
    small_button(s_picker_pager, 0, 4, 120, 42, LV_SYMBOL_LEFT " Prev", on_picker_page, -1);
    small_button(s_picker_pager, 460, 4, 120, 42, "Next " LV_SYMBOL_RIGHT, on_picker_page, 1);
    s_picker_page_label = label(s_picker_pager, &ui_font_20, MUTED);
    lv_obj_align(s_picker_page_label, LV_ALIGN_CENTER, 0, 0);

    // Confirmation before a library item replaces the slot content
    s_confirm = card(s_picker, 10, 330, 580, 110);
    lv_obj_set_style_bg_color(s_confirm, lv_color_hex(0x1A1D20), 0);
    lv_obj_set_style_bg_grad_dir(s_confirm, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(s_confirm, lv_color_hex(0x3A3F45), 0);
    s_confirm_text = label(s_confirm, &ui_font_20, TEXT);
    lv_obj_set_width(s_confirm_text, 550);
    lv_label_set_long_mode(s_confirm_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_confirm_text, 14, 12);
    small_button(s_confirm, 300, 54, 130, 44, "Cancel", on_confirm, 0);
    primary(small_button(s_confirm, 440, 54, 130, 44, "Load", on_confirm, 1));
    lv_obj_set_hidden(s_confirm, true);

    lv_obj_set_hidden(s_picker, true);
}

// ---- own banks: preset and colour per switch (long press on a preset tile) ----

static void style_swatch(int i, bool selected)
{
    lv_obj_set_style_border_width(s_bank_swatch[i], selected ? 4 : 1, 0);
    lv_obj_set_style_border_color(s_bank_swatch[i], lv_color_hex(selected ? 0xFFFFFF : 0x3A3D40), 0);
}

// Symbol choices: the selected one in the chosen colour, the others grey.
static void style_symbols(void)
{
    for (int i = 0; i <= PRESET_ICON_COUNT; i++) {
        bool selected = i == s_be_icon;
        uint32_t color = selected ? BANK_COLORS[s_be_color] : 0x2A2D30;
        lv_obj_t *sq = s_bank_symbol[i];
        set_icon_square(sq, color, i ? PRESET_ICONS[i - 1].image : NULL, true);
        if (!selected) lv_obj_set_style_image_recolor(lv_obj_get_child(sq, 0), lv_color_hex(0xBDBDBD), 0);
        lv_obj_t *sym = lv_obj_get_child(sq, 1);
        lv_label_set_text(sym, i ? "" : LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(sym, lv_color_hex(selected ? ink_for(color) : 0xBDBDBD), 0);
        lv_obj_set_style_border_width(sq, selected ? 4 : 1, 0);
        lv_obj_set_style_border_color(sq, lv_color_hex(selected ? 0xFFFFFF : 0x3A3D40), 0);
    }
}

static void on_swatch(lv_event_t *e)
{
    s_be_color = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) style_swatch(i, i == s_be_color);
    style_symbols();
}

static void on_symbol(lv_event_t *e)
{
    s_be_icon = (int)(intptr_t)lv_event_get_user_data(e);
    style_symbols();
}

static void fill_bank_list(void);

static void on_bank_preset(lv_event_t *e)
{
    s_be_preset = (int)(intptr_t)lv_event_get_user_data(e);
    fill_bank_list();
}

static void fill_bank_list(void)
{
    char text[80];
    lv_obj_clean(s_bank_list);
    for (int preset = 1; preset <= NANO_PRESETS; preset++) {
        bool selected = preset == s_be_preset;
        lv_obj_t *btn = lv_button_create(s_bank_list);
        lv_obj_set_size(btn, LV_PCT(100), 46);
        lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x2A2E33 : 0x1A1D20), 0);
        lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(BANK_COLORS[s_be_color]), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 9, 0);
        lv_obj_add_event_cb(btn, on_bank_preset, LV_EVENT_CLICKED, (void *)(intptr_t)preset);
        lv_obj_t *l = label(btn, &ui_font_20, selected ? 0xFFFFFF : 0xD8D8D8);
        snprintf(text, sizeof(text), "%2d   %.60s", preset, s_preset_names[preset - 1][0] ? s_preset_names[preset - 1] : "-");
        lv_label_set_text(l, text);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        if (selected) lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
    }
}

static void open_bank_editor(int tile)
{
    char text[48];
    s_be_bank = s_view.bank;
    s_be_tile = tile;
    s_be_preset = s_view.bank_presets[tile];
    s_be_color = s_view.bank_colors[tile] < UI_BANK_COLOR_COUNT ? s_view.bank_colors[tile] : 0;
    s_be_icon = s_view.bank_icons[tile] <= PRESET_ICON_COUNT ? s_view.bank_icons[tile] : 0;
    snprintf(text, sizeof(text), "Bank %d  \xC2\xB7  switch %d", s_be_bank + 1, tile + 3);
    lv_label_set_text(s_bank_title, text);
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) style_swatch(i, i == s_be_color);
    style_symbols();
    fill_bank_list();
    lv_obj_set_hidden(s_bank_editor, false);
}

static void on_bank_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 cancel, 1 save, 2 default, 3 learn
    if (action == 3) {   // over the bank editor, which stays open
        open_learn(s_be_tile + 3);
        return;
    }
    lv_obj_set_hidden(s_bank_editor, true);
    int base = s_be_bank << 14 | s_be_tile << 11;
    if (action == 1 && s_be_preset) send('B', s_be_icon << 18 | base | s_be_color << 7 | s_be_preset);
    else if (action == 2) send('B', base);
}

static void build_bank_editor(lv_obj_t *screen)
{
    s_bank_editor = panel(screen, 50, 12, 700, 462);
    s_bank_title = dialog_title(s_bank_editor, "");
    lv_obj_t *hint = label(s_bank_editor, &ui_font_14, MUTED);
    lv_label_set_text(hint, "Colour, symbol and preset for this switch - stored on the controller");
    lv_obj_set_pos(hint, 20, 46);
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_bank_editor);
        s_bank_swatch[i] = sw;
        lv_obj_set_size(sw, 46, 46);
        lv_obj_set_pos(sw, 18 + i * 67, 72);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int i = 0; i <= PRESET_ICON_COUNT; i++) {   // "none" and the symbols, ten per row
        lv_obj_t *sq = icon_square(s_bank_editor, 46);
        s_bank_symbol[i] = sq;
        lv_obj_set_pos(sq, 18 + (i % 10) * 67, 126 + (i / 10) * 52);
        lv_obj_set_clickable(sq, true);
        lv_obj_add_event_cb(sq, on_symbol, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    s_bank_list = lv_obj_create(s_bank_editor);
    lv_obj_set_pos(s_bank_list, 10, 230);
    lv_obj_set_size(s_bank_list, 680, 164);
    lv_obj_set_style_bg_opa(s_bank_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bank_list, 0, 0);
    lv_obj_set_style_pad_all(s_bank_list, 4, 0);
    lv_obj_set_style_pad_row(s_bank_list, 6, 0);
    lv_obj_set_flex_flow(s_bank_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bank_list, LV_DIR_VER);
    small_button(s_bank_editor, 18, 404, 150, 46, "Default", on_bank_button, 2);
    small_button(s_bank_editor, 182, 404, 180, 46, "Learn switch", on_bank_button, 3);
    small_button(s_bank_editor, 380, 404, 150, 46, "Cancel", on_bank_button, 0);
    primary(small_button(s_bank_editor, 540, 404, 150, 46, "Save", on_bank_button, 1));
    lv_obj_set_hidden(s_bank_editor, true);
}

// ---- looper tiles: name, colour and symbol per switch (long press on a tile in looper mode) ----
// What a looper switch does is set in the app on the phone, so its tile is the user's to label.

static void show_rename(char kind, const char *title, const char *ok, const char *text, int max_length);
static void scene_editor_named(const char *text);

static void style_looper_editor(void)
{
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        bool selected = i == s_le_color;
        lv_obj_set_style_border_width(s_le_swatch[i], selected ? 4 : 1, 0);
        lv_obj_set_style_border_color(s_le_swatch[i], lv_color_hex(selected ? 0xFFFFFF : 0x3A3D40), 0);
    }
    for (int i = 0; i < UI_LOOPER_SYMBOLS; i++) {   // the chosen symbol in the chosen colour, the others grey
        bool selected = i == s_le_icon;
        uint32_t color = selected ? BANK_COLORS[s_le_color] : 0x2A2D30;
        lv_obj_t *sq = s_le_symbol[i], *sym = lv_obj_get_child(sq, 1);
        set_icon_square(sq, color, NULL, true);
        lv_label_set_text(sym, i ? LOOPER_SYMBOLS[i] : LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(sym, lv_color_hex(selected ? ink_for(color) : 0xBDBDBD), 0);
        lv_obj_set_style_border_width(sq, selected ? 4 : 1, 0);
        lv_obj_set_style_border_color(sq, lv_color_hex(selected ? 0xFFFFFF : 0x3A3D40), 0);
    }
    lv_label_set_text(s_le_name_label, s_le_name);
}

static void on_looper_swatch(lv_event_t *e)
{
    s_le_color = (int)(intptr_t)lv_event_get_user_data(e);
    style_looper_editor();
}

static void on_looper_symbol(lv_event_t *e)
{
    s_le_icon = (int)(intptr_t)lv_event_get_user_data(e);
    style_looper_editor();
}

static void on_looper_name(lv_event_t *e)
{
    char title[40];
    snprintf(title, sizeof(title), "Name of looper switch %d", s_le_tile + 2);
    show_rename('L', title, "OK", s_le_name, UI_LOOPER_NAME - 1);
}

// From the keyboard: the name stays in the dialog until Save.
static void looper_editor_named(const char *text)
{
    if (text[0]) strlcpy(s_le_name, text, sizeof(s_le_name));
    style_looper_editor();
}

static void open_looper_editor(int tile)
{
    char text[96];
    if (tile < 0 || tile >= UI_LOOPER_SWITCHES) return;
    s_le_tile = tile;
    s_le_color = s_view.looper_colors[tile] < UI_BANK_COLOR_COUNT ? s_view.looper_colors[tile] : 0;
    s_le_icon = s_view.looper_icons[tile] < UI_LOOPER_SYMBOLS ? s_view.looper_icons[tile] : 0;
    strlcpy(s_le_name, s_view.looper_names[tile], sizeof(s_le_name));
    snprintf(text, sizeof(text), "Looper switch %d", tile + 2);
    lv_label_set_text(s_le_title, text);
    snprintf(text, sizeof(text), "Sends CC %d on MIDI channel 16 - what it does is set in the app", 102 + tile);
    lv_label_set_text(s_le_hint, text);
    style_looper_editor();
    lv_obj_set_hidden(s_looper_editor, false);
    lv_obj_move_foreground(s_looper_editor);
}

static void on_looper_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 cancel, 1 save, 2 default
    char text[UI_LOOPER_NAME + 4];
    lv_obj_set_hidden(s_looper_editor, true);
    if (action == 1) snprintf(text, sizeof(text), "%c%c%c%s", '2' + s_le_tile, 'a' + s_le_color, 'a' + s_le_icon, s_le_name);
    else if (action == 2) snprintf(text, sizeof(text), "%c!", '2' + s_le_tile);
    else return;
    if (s_on_text) s_on_text('L', text);
}

static void build_looper_editor(lv_obj_t *screen)
{
    s_looper_editor = panel(screen, 50, 44, 700, 392);
    s_le_title = dialog_title(s_looper_editor, "");
    s_le_hint = label(s_looper_editor, &ui_font_14, MUTED);
    lv_obj_set_pos(s_le_hint, 20, 46);

    lv_obj_t *name = button(s_looper_editor, 18, 78, 662, 56, on_looper_name);   // tap: the keyboard
    s_le_name_label = label(name, &ui_font_title_22, TEXT);
    lv_obj_align(s_le_name_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *hint = label(name, &ui_font_14, MUTED);
    lv_label_set_text(hint, "Rename");
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -4, 0);

    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_looper_editor);
        s_le_swatch[i] = sw;
        lv_obj_set_size(sw, 52, 52);
        lv_obj_set_pos(sw, 18 + i * 68, 152);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_looper_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int i = 0; i < UI_LOOPER_SYMBOLS; i++) {   // "none" and the symbols
        lv_obj_t *sq = icon_square(s_looper_editor, 56);
        s_le_symbol[i] = sq;
        lv_obj_set_pos(sq, 18 + i * 68, 226);
        lv_obj_set_clickable(sq, true);
        lv_obj_add_event_cb(sq, on_looper_symbol, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    small_button(s_looper_editor, 18, 322, 150, 52, "Default", on_looper_button, 2);
    small_button(s_looper_editor, 372, 322, 150, 52, "Cancel", on_looper_button, 0);
    primary(small_button(s_looper_editor, 530, 322, 150, 52, "Save", on_looper_button, 1));
    lv_obj_set_hidden(s_looper_editor, true);
}

// ---- footswitch learn: the next pressed footswitch takes this switch's place ----

static void on_learn_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 cancel, -1 default order, 1 learn footswitch 1 instead
    if (action == 1) {
        open_learn(1);
        return;
    }
    lv_obj_set_hidden(s_learn, true);
    send('D', action);
}

static void open_learn(int number)
{
    lv_label_set_text_fmt(s_learn_title, "Learn footswitch %d", number);
    lv_label_set_text_fmt(s_learn_text, "Press the footswitch that should be switch %d now. Waiting 15 seconds.", number);
    lv_obj_set_hidden(s_learn_first, number != 2);   // tile 1 held is the looper mode: its switch is learned from here
    lv_obj_set_hidden(s_learn, false);
    lv_obj_move_foreground(s_learn);
    send('D', number);
}

void ui_learn_done(const char *message)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_learn, true);
    ui_show_message(message);
    lvgl_port_unlock();
}

static void build_learn(lv_obj_t *screen)
{
    s_learn = panel(screen, 190, 140, 420, 200);
    s_learn_title = dialog_title(s_learn, "");
    s_learn_text = label(s_learn, &ui_font_16, 0xC8CCD1);
    lv_obj_set_width(s_learn_text, 380);
    lv_label_set_long_mode(s_learn_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_learn_text, 20, 52);
    s_learn_first = small_button(s_learn, 20, 138, 124, 46, "Switch 1", on_learn_button, 1);
    small_button(s_learn, 152, 138, 138, 46, "Default order", on_learn_button, -1);
    small_button(s_learn, 298, 138, 102, 46, "Cancel", on_learn_button, 0);
    lv_obj_set_hidden(s_learn, true);
}

// ---- confirm dialog and rename keyboard ----

static void on_ask(lv_event_t *e)
{
    bool yes = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_ask, true);
    if (yes) send(s_ask_command, s_ask_arg);
}

// A question with Cancel and a button that says what happens (ok); then command and arg are sent.
static void ask_for(const char *text, const char *ok, char command, int arg)
{
    lv_label_set_text(s_ask_text, text);
    lv_label_set_text(lv_obj_get_child(s_ask_ok, 0), ok);
    s_ask_command = command;
    s_ask_arg = arg;
    lv_obj_set_hidden(s_ask, false);
    lv_obj_move_foreground(s_ask);
}

static void ask(const char *text, char command)
{
    ask_for(text, "Save", command, 0);
}

static void build_ask(lv_obj_t *screen)
{
    s_ask = panel(screen, 150, 150, 500, 180);
    s_ask_text = label(s_ask, &ui_font_20, TEXT);
    lv_obj_set_width(s_ask_text, 460);
    lv_label_set_long_mode(s_ask_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ask_text, 20, 18);
    small_button(s_ask, 160, 116, 150, 48, "Cancel", on_ask, 0);
    s_ask_ok = small_button(s_ask, 330, 116, 150, 48, "Save", on_ask, 1);
    primary(s_ask_ok);
    lv_obj_set_hidden(s_ask, true);
}

static void on_save_button(lv_event_t *e)
{
    char text[200];
    const char *name = s_preset_names[s_current_preset - 1];
    snprintf(text, sizeof(text), "Save preset %d \"%.60s\" on the Nano?%s", s_current_preset, name[0] ? name : "-",
             s_view.rev_active ? " Reverb B is active and becomes the preset's reverb." : "");
    ask(text, 'S');
}

static void close_rename(void)
{
    lv_obj_set_hidden(s_rename, true);
}

static void submit_rename(void)
{
    const char *text = lv_textarea_get_text(s_rename_area);
    while (*text == ' ') text++;
    if (s_rename_kind == 'N' && strlen(text) < 4) {
        ui_show_message("Preset names need at least 4 characters.");
        return;
    }
    if (s_rename_kind == 'L') looper_editor_named(text);   // into the looper tile's dialog, sent with its Save
    else if (s_rename_kind == 'S') scene_editor_named(text);   // the same for a scene
    else if (s_on_text) s_on_text(s_rename_kind, text);
    close_rename();
}

static void on_rename_keyboard(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) submit_rename();
    else if (code == LV_EVENT_CANCEL) close_rename();
}

static void on_rename_button(lv_event_t *e)
{
    if ((int)(intptr_t)lv_event_get_user_data(e)) submit_rename();
    else close_rename();
}

static void show_rename(char kind, const char *title, const char *ok, const char *text, int max_length)
{
    s_rename_kind = kind;
    lv_label_set_text(s_rename_title, title);
    lv_label_set_text(lv_obj_get_child(s_rename_ok, 0), ok);
    lv_textarea_set_max_length(s_rename_area, max_length);
    lv_textarea_set_placeholder_text(s_rename_area, kind == 'N' || kind == 'L' || kind == 'S' ? "" : "Name (empty name = delete)");
    lv_textarea_set_text(s_rename_area, text);
    lv_obj_set_hidden(s_rename, false);
    lv_obj_move_foreground(s_rename);
}

static void open_rename(lv_event_t *e)
{
    char title[32];
    snprintf(title, sizeof(title), "Rename preset %d", s_current_preset);
    show_rename('N', title, "Rename", s_preset_names[s_current_preset - 1], 32);
}

static void build_rename(lv_obj_t *screen)
{
    s_rename = lv_obj_create(screen);
    lv_obj_set_size(s_rename, 800, 480);
    lv_obj_set_pos(s_rename, 0, 0);
    lv_obj_set_style_bg_color(s_rename, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_rename, 0, 0);
    lv_obj_set_style_radius(s_rename, 0, 0);
    lv_obj_set_style_pad_all(s_rename, 0, 0);
    lv_obj_set_scrollable(s_rename, false);

    s_rename_title = dialog_title(s_rename, "");
    lv_obj_set_pos(s_rename_title, 20, 22);
    small_button(s_rename, 470, 10, 150, 48, "Cancel", on_rename_button, 0);
    s_rename_ok = small_button(s_rename, 630, 10, 150, 48, "Rename", on_rename_button, 1);
    primary(s_rename_ok);

    s_rename_area = lv_textarea_create(s_rename);
    lv_obj_set_pos(s_rename_area, 20, 72);
    lv_obj_set_size(s_rename_area, 760, 70);
    lv_textarea_set_one_line(s_rename_area, true);
    lv_textarea_set_max_length(s_rename_area, 32);
    lv_obj_set_style_text_font(s_rename_area, &ui_font_28, 0);

    lv_obj_t *kb = lv_keyboard_create(s_rename);
    lv_obj_set_size(kb, 800, 320);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &ui_font_20, 0);
    lv_keyboard_set_textarea(kb, s_rename_area);
    lv_obj_add_event_cb(kb, on_rename_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_rename_keyboard, LV_EVENT_CANCEL, NULL);

    lv_obj_set_hidden(s_rename, true);
}

// ---- short messages ----

static void hide_toast(lv_timer_t *t)
{
    lv_obj_set_hidden(s_toast, true);
}

void ui_show_message(const char *text)
{
    lvgl_port_lock(0);
    lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
    lv_obj_set_hidden(s_toast, false);
    lv_obj_move_foreground(s_toast);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
    lvgl_port_unlock();
}

static void build_toast(lv_obj_t *screen)
{
    s_toast = card(screen, 100, 8, 600, 56);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1C1F22), 0);
    lv_obj_set_style_bg_grad_dir(s_toast, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x33383E), 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_shadow_width(s_toast, 30, 0);
    lv_obj_set_style_shadow_opa(s_toast, LV_OPA_60, 0);
    lv_obj_t *l = label(s_toast, &ui_font_20, TEXT);
    lv_obj_set_width(l, 570);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_center(l);
    lv_obj_set_hidden(s_toast, true);
    s_toast_timer = lv_timer_create(hide_toast, 3000, NULL);
    lv_timer_set_auto_delete(s_toast_timer, false);
    lv_timer_pause(s_toast_timer);
    lv_timer_set_repeat_count(s_toast_timer, -1);
}

static void on_midi_device(lv_event_t *e)
{
    send('P', (int)(intptr_t)lv_event_get_user_data(e));
}

static void fill_midi(void);

static void open_midi(lv_event_t *e)
{
    fill_midi();
    lv_obj_set_hidden(s_midi, false);
    lv_obj_move_foreground(s_midi);
    send('X', 1);
}

// ---- USB playback volume ----

#define USB_SLIDER_MAX 1000
#define USB_DEFAULT_DB (-6.0f)   // the app's reset value

// Slider position <-> dB as in the app: dB = 40 * sqrt(position) - 40, finer towards the top.
static float usb_db_from_position(int32_t position)
{
    return 40.0f * sqrtf(position / (float)USB_SLIDER_MAX) + NANO_USB_GAIN_MIN_DB;
}

static int32_t usb_position_from_db(float db)
{
    float k = (db - NANO_USB_GAIN_MIN_DB) / -NANO_USB_GAIN_MIN_DB;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    return (int32_t)lroundf(k * k * USB_SLIDER_MAX);
}

static void show_usb_value(void)
{
    int tenths = (int)lroundf(-s_usb_db * 10);
    if (s_usb_db <= NANO_USB_GAIN_MIN_DB + 0.05f) lv_label_set_text(s_usb_value, "Off");
    else if (tenths == 0) lv_label_set_text(s_usb_value, "0.0 dB");
    else lv_label_set_text_fmt(s_usb_value, "-%d.%d dB", tenths / 10, tenths % 10);
}

static void set_usb_enabled(bool enabled)
{
    lv_obj_t *const controls[] = { s_usb_slider, s_usb_minus, s_usb_plus, s_usb_reset };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (enabled) lv_obj_remove_state(controls[i], LV_STATE_DISABLED);
        else lv_obj_add_state(controls[i], LV_STATE_DISABLED);
    }
}

// New volume from the slider or the buttons: show it and send it (in tenths of a dB).
static void usb_changed(float db, bool move_slider)
{
    if (db < NANO_USB_GAIN_MIN_DB) db = NANO_USB_GAIN_MIN_DB;
    if (db > 0) db = 0;
    s_usb_db = roundf(db * 10) / 10;
    if (move_slider) lv_slider_set_value(s_usb_slider, usb_position_from_db(s_usb_db), LV_ANIM_OFF);
    show_usb_value();
    send('U', (int)lroundf(s_usb_db * 10));
}

static void on_usb_slider(lv_event_t *e)
{
    usb_changed(usb_db_from_position(lv_slider_get_value(s_usb_slider)), false);
}

static void on_usb_button(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    if (what == 0) lv_obj_set_hidden(s_usb, true);
    else if (what == 2) usb_changed(USB_DEFAULT_DB, true);
    else usb_changed(s_usb_db + (what > 0 ? 1.0f : -1.0f), true);
}

static void open_usb(lv_event_t *e)
{
    // The value can also be changed in the app: read it each time.
    set_usb_enabled(false);
    lv_label_set_text(s_usb_value, "");
    lv_label_set_text(s_usb_info, "Reading the Nano's settings...");
    lv_obj_set_hidden(s_usb, false);
    lv_obj_move_foreground(s_usb);
    send('V', 0);
}

void ui_set_usb_gain(float db)
{
    lvgl_port_lock(0);
    s_usb_db = db;
    lv_slider_set_value(s_usb_slider, usb_position_from_db(db), LV_ANIM_OFF);
    show_usb_value();
    lv_label_set_text(s_usb_info, "Volume of the audio played from the computer");
    set_usb_enabled(true);
    lvgl_port_unlock();
}

static void build_usb(lv_obj_t *screen)
{
    s_usb = panel(screen, 140, 128, 520, 214);
    dialog_title(s_usb, "USB audio");
    s_usb_info = label(s_usb, &ui_font_14, MUTED);
    lv_obj_set_pos(s_usb_info, 20, 46);
    s_usb_value = label(s_usb, &ui_font_28, 0xFFFFFF);
    lv_obj_set_width(s_usb_value, 160);
    lv_obj_set_style_text_align(s_usb_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_usb_value, LV_ALIGN_TOP_RIGHT, -76, 16);
    close_button(s_usb, 460, 12, on_usb_button, 0);

    s_usb_minus = small_button(s_usb, 20, 80, 60, 52, "", on_usb_button, -1);
    lv_obj_t *minus = lv_obj_get_child(s_usb_minus, 0);
    lv_obj_set_style_text_font(minus, &ui_font_28, 0);
    lv_label_set_text(minus, LV_SYMBOL_MINUS);
    s_usb_slider = lv_slider_create(s_usb);
    lv_slider_set_range(s_usb_slider, 0, USB_SLIDER_MAX);
    lv_obj_set_size(s_usb_slider, 316, 16);
    lv_obj_set_pos(s_usb_slider, 102, 98);
    lv_obj_set_ext_click_area(s_usb_slider, 22);
    style_slider(s_usb_slider, 0xECECEC);
    lv_obj_add_event_cb(s_usb_slider, on_usb_slider, LV_EVENT_VALUE_CHANGED, NULL);
    s_usb_plus = small_button(s_usb, 440, 80, 60, 52, "", on_usb_button, 1);
    lv_obj_t *plus = lv_obj_get_child(s_usb_plus, 0);
    lv_obj_set_style_text_font(plus, &ui_font_28, 0);
    lv_label_set_text(plus, LV_SYMBOL_PLUS);

    s_usb_reset = small_button(s_usb, 20, 152, 150, 46, "-6 dB", on_usb_button, 2);
    lv_obj_set_hidden(s_usb, true);
}

// ---- capture volume (VOL button), capture amp knobs (long press on the capture card) and cab settings ----

// Slider rows: 0 = capture volume (the Nano's 0-255), 1-3 = cab output, high pass, low pass (0-1000 = 0-1),
// 4-7 = capture gain, bass, mid, treble (0-255, shown 0-10). The row number is the 'v' command's "what".
#define AMP_ROW 4
#define AMP_ROWS 4
#define LEVEL_ROWS (AMP_ROW + AMP_ROWS)
static const int AMP_ROW_KNOB[AMP_ROWS] = { NANO_AMP_GAIN, NANO_AMP_BASS, NANO_AMP_MID, NANO_AMP_TREBLE };
#define CAB_SLIDER_MAX 1000
#define VOLUME_HOLD_MS 1500   // after a change on the board, state replies still on their way do not move the slider back

static lv_obj_t *s_level_slider[LEVEL_ROWS], *s_level_value[LEVEL_ROWS], *s_level_minus[LEVEL_ROWS], *s_level_plus[LEVEL_ROWS];

static void format_db(char *text, size_t size, float db)
{
    if (fabsf(db) < 0.05f) db = 0;
    snprintf(text, size, db > 0 ? "+%.1f dB" : "%.1f dB", db);
}

static void show_level_value(int row)
{
    int32_t position = lv_slider_get_value(s_level_slider[row]);
    char text[24];
    if (row == 0) {
        format_db(text, sizeof(text), nano_capture_volume_db(position));
    } else if (row >= AMP_ROW) {
        snprintf(text, sizeof(text), "%.1f", position / 25.5f);
    } else {
        float value = nano_cab_setting_value(row - 1, position / (float)CAB_SLIDER_MAX);
        if (row - 1 == NANO_CAB_OUTPUT) format_db(text, sizeof(text), value);
        else if (value >= 1000) snprintf(text, sizeof(text), "%.1f kHz", value / 1000);
        else snprintf(text, sizeof(text), "%d Hz", (int)lroundf(value));
    }
    lv_label_set_text(s_level_value[row], text);
}

// New position from a slider or a button: show it and send it.
static void level_changed(int row, int32_t position, bool move_slider)
{
    int32_t max = row == 0 || row >= AMP_ROW ? 255 : CAB_SLIDER_MAX;
    if (position < 0) position = 0;
    if (position > max) position = max;
    if (move_slider) lv_slider_set_value(s_level_slider[row], position, LV_ANIM_OFF);
    show_level_value(row);
    if (row == 0) {
        s_capture_volume = (int)position;
        s_volume_tick = lv_tick_get();
    } else if (row >= AMP_ROW) {
        s_amp_values[AMP_ROW_KNOB[row - AMP_ROW]] = (int)position;
        s_amp_tick = lv_tick_get();
    }
    send('v', row << 16 | (int)position);
}

static void on_level_slider(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    level_changed(row, lv_slider_get_value(s_level_slider[row]), false);
}

// Minus / plus (data = row << 1 | plus): to the next whole 1 dB, 10 Hz (high pass), 100 Hz (low pass) or 0.5 (amp).
static void on_level_step(lv_event_t *e)
{
    static const float STEPS[LEVEL_ROWS] = { 1, 1, 10, 100, 0.5f, 0.5f, 0.5f, 0.5f };
    int data = (int)(intptr_t)lv_event_get_user_data(e), row = data >> 1, dir = (data & 1) ? 1 : -1;
    int32_t position = lv_slider_get_value(s_level_slider[row]), next;
    if (row >= AMP_ROW) {
        float value = roundf(position / 25.5f / STEPS[row]) * STEPS[row];
        next = lroundf((value + dir * STEPS[row]) * 25.5f);
    } else if (row == 0) {
        float db = roundf(nano_capture_volume_db(position) / STEPS[row]) * STEPS[row];
        next = nano_capture_volume_raw(db + dir * STEPS[row]);
    } else {
        float value = roundf(nano_cab_setting_value(row - 1, position / (float)CAB_SLIDER_MAX) / STEPS[row]) * STEPS[row];
        next = lroundf(nano_cab_setting_normalized(row - 1, value + dir * STEPS[row]) * CAB_SLIDER_MAX);
    }
    if (next == position) next += dir;   // the Nano's resolution is coarser than one step here
    level_changed(row, next, true);
}

static void set_level_row_enabled(int row, bool enabled)
{
    lv_obj_t *const controls[] = { s_level_slider[row], s_level_minus[row], s_level_plus[row] };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (enabled) lv_obj_remove_state(controls[i], LV_STATE_DISABLED);
        else lv_obj_add_state(controls[i], LV_STATE_DISABLED);
    }
}

// Minus button, slider and plus button from x to x + w (as in the USB dialog).
static void build_level_row(lv_obj_t *parent, int row, int x, int y, int w)
{
    s_level_minus[row] = small_button(parent, x, y, 60, 52, "", on_level_step, row << 1);
    lv_obj_t *minus = lv_obj_get_child(s_level_minus[row], 0);
    lv_obj_set_style_text_font(minus, &ui_font_28, 0);
    lv_label_set_text(minus, LV_SYMBOL_MINUS);
    lv_obj_t *slider = lv_slider_create(parent);
    s_level_slider[row] = slider;
    lv_slider_set_range(slider, 0, row == 0 || row >= AMP_ROW ? 255 : CAB_SLIDER_MAX);
    lv_obj_set_size(slider, w - 164, 16);
    lv_obj_set_pos(slider, x + 82, y + 18);
    lv_obj_set_ext_click_area(slider, 22);
    style_slider(slider, 0xECECEC);   // capture and cab: white, as their blocks in the editor
    lv_obj_add_event_cb(slider, on_level_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)row);
    s_level_plus[row] = small_button(parent, x + w - 60, y, 60, 52, "", on_level_step, row << 1 | 1);
    lv_obj_t *plus = lv_obj_get_child(s_level_plus[row], 0);
    lv_obj_set_style_text_font(plus, &ui_font_28, 0);
    lv_label_set_text(plus, LV_SYMBOL_PLUS);
}

static void show_capture_volume(void)
{
    lv_slider_set_value(s_level_slider[0], s_capture_volume, LV_ANIM_OFF);
    show_level_value(0);
    lv_label_set_text(s_volume_info, s_capture_slot ? lv_label_get_text(s_capture) : "The capture is bypassed.");
}

static void on_volume_button(lv_event_t *e)
{
    if ((int)(intptr_t)lv_event_get_user_data(e) == 0) lv_obj_set_hidden(s_volume, true);
    else level_changed(0, NANO_CAPTURE_VOLUME_0DB, true);
}

static void open_volume(lv_event_t *e)
{
    show_capture_volume();
    lv_obj_set_hidden(s_volume, false);
    lv_obj_move_foreground(s_volume);
}

static void build_volume(lv_obj_t *screen)
{
    s_volume = panel(screen, 140, 128, 520, 214);
    dialog_title(s_volume, "Capture volume");
    s_volume_info = label(s_volume, &ui_font_14, MUTED);
    lv_obj_set_width(s_volume_info, 260);
    lv_label_set_long_mode(s_volume_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_volume_info, 20, 46);
    s_level_value[0] = label(s_volume, &ui_font_28, 0xFFFFFF);
    lv_obj_set_width(s_level_value[0], 150);
    lv_obj_set_style_text_align(s_level_value[0], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_level_value[0], LV_ALIGN_TOP_RIGHT, -76, 16);
    close_button(s_volume, 460, 12, on_volume_button, 0);
    build_level_row(s_volume, 0, 20, 82, 480);
    small_button(s_volume, 20, 152, 150, 46, "0 dB", on_volume_button, 1);
    lv_obj_set_hidden(s_volume, true);
}

static void on_cab_settings_close(lv_event_t *e)
{
    lv_obj_set_hidden(s_cab_settings, true);
    send('y', 0);
}

static void open_cab_settings(lv_event_t *e)
{
    for (int row = 1; row < LEVEL_ROWS; row++) {
        set_level_row_enabled(row, false);
        lv_label_set_text(s_level_value[row], "");
    }
    lv_label_set_text(s_cab_info, "Reading the cab settings...");
    lv_obj_set_hidden(s_cab_settings, false);
    lv_obj_move_foreground(s_cab_settings);
    send('y', 1);
}

void ui_set_cab_settings(const float *values, bool enabled, const char *info)
{
    lvgl_port_lock(0);
    lv_label_set_text(s_cab_info, info);
    for (int row = 1; row < LEVEL_ROWS; row++) {
        if (values) {
            lv_slider_set_value(s_level_slider[row], lroundf(values[row - 1] * CAB_SLIDER_MAX), LV_ANIM_OFF);
            show_level_value(row);
        } else {
            lv_label_set_text(s_level_value[row], "-");
        }
        set_level_row_enabled(row, enabled);
    }
    lvgl_port_unlock();
}

static void build_cab_settings(lv_obj_t *screen)
{
    static const char *const NAMES[NANO_CAB_SETTINGS] = { "OUTPUT", "HIGH PASS", "LOW PASS" };
    s_cab_settings = panel(screen, 100, 56, 600, 368);
    dialog_title(s_cab_settings, "Cab / IR");
    s_cab_info = label(s_cab_settings, &ui_font_14, MUTED);
    lv_obj_set_width(s_cab_info, 480);
    lv_label_set_long_mode(s_cab_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_cab_info, 20, 46);
    close_button(s_cab_settings, 540, 12, on_cab_settings_close, 0);
    for (int i = 0; i < NANO_CAB_SETTINGS; i++) {
        int row = 1 + i, y = 76 + i * 98;
        lv_obj_t *name = label(s_cab_settings, &ui_font_12, MUTED);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, NAMES[i]);
        lv_obj_set_pos(name, 20, y);
        s_level_value[row] = label(s_cab_settings, &ui_font_20, 0xFFFFFF);
        lv_obj_set_width(s_level_value[row], 200);
        lv_obj_set_style_text_align(s_level_value[row], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_level_value[row], LV_ALIGN_TOP_RIGHT, -20, y - 3);
        build_level_row(s_cab_settings, row, 20, y + 24, 560);
    }
    lv_obj_set_hidden(s_cab_settings, true);
}

// Capture dialog (long press on the capture card): gain, bass, mid, treble of the capture.
static void show_amp_values(void)
{
    for (int i = 0; i < AMP_ROWS; i++) {
        lv_slider_set_value(s_level_slider[AMP_ROW + i], s_amp_values[AMP_ROW_KNOB[i]], LV_ANIM_OFF);
        show_level_value(AMP_ROW + i);
    }
    lv_label_set_text(s_amp_info, s_capture_slot ? lv_label_get_text(s_capture) : "The capture is bypassed.");
}

static void on_amp_close(lv_event_t *e)
{
    lv_obj_set_hidden(s_amp, true);
}

static void open_amp(lv_event_t *e)
{
    show_amp_values();
    lv_obj_set_hidden(s_amp, false);
    lv_obj_move_foreground(s_amp);
}

static void build_amp(lv_obj_t *screen)
{
    static const char *const NAMES[AMP_ROWS] = { "GAIN", "BASS", "MID", "TREBLE" };
    s_amp = panel(screen, 100, 20, 600, 440);
    dialog_title(s_amp, "Capture");
    s_amp_info = label(s_amp, &ui_font_14, MUTED);
    lv_obj_set_width(s_amp_info, 480);
    lv_label_set_long_mode(s_amp_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_amp_info, 20, 46);
    close_button(s_amp, 540, 12, on_amp_close, 0);
    for (int i = 0; i < AMP_ROWS; i++) {
        int row = AMP_ROW + i, y = 78 + i * 88;
        lv_obj_t *name = label(s_amp, &ui_font_12, MUTED);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, NAMES[i]);
        lv_obj_set_pos(name, 20, y);
        s_level_value[row] = label(s_amp, &ui_font_20, 0xFFFFFF);
        lv_obj_set_width(s_level_value[row], 200);
        lv_obj_set_style_text_align(s_level_value[row], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_level_value[row], LV_ALIGN_TOP_RIGHT, -20, y - 3);
        build_level_row(s_amp, row, 20, y + 22, 560);
    }
    lv_obj_set_hidden(s_amp, true);
}

// ---- reverb mix Pos 1 / Pos 2 (long press on the mix tile) ----

static void show_mix_value(int which)
{
    lv_label_set_text_fmt(s_mix_value[which], "%d%%", (int)lroundf(lv_slider_get_value(s_mix_slider[which]) * 100 / 255.0f));
}

static void on_mix_slider(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    show_mix_value(which);
    s_mix_touched = true;
    send('Q', which << 8 | (int)lv_slider_get_value(s_mix_slider[which]));
}

static void show_mix_tab(int tab)
{
    s_mix_tab_shown = tab;
    for (int i = 0; i < 2; i++) {
        style_tab(s_mix_tab[i], i == tab);
        lv_obj_set_hidden(s_mix_panel[i], i != tab);
    }
    lv_obj_set_hidden(s_mix_free, tab != 0 || !s_view.mix_exp);
}

static void on_mix_tab(lv_event_t *e)
{
    show_mix_tab((int)(intptr_t)lv_event_get_user_data(e));
}

static uint32_t chosen_rev_b(void)
{
    int index = (int)lv_dropdown_get_selected(s_rev_dropdown);
    return index > 0 && index <= s_rev_type_count ? s_rev_types[index - 1] : 0;
}

static void on_rev_dropdown(lv_event_t *e)
{
    if (chosen_rev_b()) lv_obj_remove_state(s_rev_edit, LV_STATE_DISABLED);
    else lv_obj_add_state(s_rev_edit, LV_STATE_DISABLED);
}

// 0 cancel, 1 save, 2 edit reverb B, 3 save and take the reverb off the Nano's expression pedal.
// The tab in front decides what footswitch 8 does from now on: saved on the mix tab it switches the mix (a second
// reverb stays stored for later), saved on the 2nd reverb tab it swaps the two reverbs.
static void on_mix_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_mix_editor, true);
    bool decide = action == 1 || action == 2;   // (Free pedal leaves the switch as it is)
    bool mix = s_mix_tab_shown == 0;
    // Pos 1 / Pos 2: when moved - and the ones shown, if the preset has none yet and the switch is to use them
    if (action && (s_mix_touched || (decide && mix && !s_view.mix_known))) {
        send('W', (int)lv_slider_get_value(s_mix_slider[0]) << 8 | (int)lv_slider_get_value(s_mix_slider[1]));
    }
    if (decide) {
        uint32_t chosen = chosen_rev_b();
        bool parked = mix && chosen, was_parked = s_view.rev_b_stored && !s_view.rev_b_type;
        if (chosen != s_view.rev_b_stored || parked != was_parked) send('A', (parked ? 2 : 0) << 24 | (int)chosen);
    }
    send('K', 0);
    if (action == 2) send('H', 0);
    if (action == 3) send('!', 0);
}

static void open_mix_editor(void)
{
    if (s_view.mix_slot < 0) {
        ui_show_message("No reverb in this preset.");
        return;
    }
    static const int defaults[2] = { 26, 102 };   // 10 % / 40 % when the preset has no Pos 1 / Pos 2 yet
    for (int i = 0; i < 2; i++) {
        int value = s_view.mix_known ? (int)lroundf(s_view.mix_pos[i] * 255) : defaults[i];
        lv_slider_set_value(s_mix_slider[i], value, LV_ANIM_OFF);
        show_mix_value(i);
    }
    s_mix_touched = false;
    lv_label_set_text(s_mix_model_label, s_mix_model);
    // Pos 1 / Pos 2 are the controller's. A preset set up before 1.8 still has the reverb on the Nano's expression
    // pedal (they were stored there): Free pedal takes it off, the switch keeps its values.
    lv_label_set_text(s_mix_info, s_view.mix_exp
        ? "Save here: footswitch 8 switches the mix, Pos 1 / Pos 2. The Nano's preset still has this reverb on the expression pedal - Free pedal ends that."
        : s_view.rev_b_stored ? "Save here: footswitch 8 switches the reverb's mix between Pos 1 and Pos 2 again. The second reverb stays stored."
        : "Save here: footswitch 8 switches the reverb's mix between Pos 1 and Pos 2. Moving a slider plays that mix.");
    int selected = 0;
    for (int i = 0; i < s_rev_type_count; i++) if (s_rev_types[i] == s_view.rev_b_stored) selected = i + 1;
    lv_dropdown_set_selected(s_rev_dropdown, (uint32_t)selected);
    on_rev_dropdown(NULL);
    // The tab of what footswitch 8 does now is in front and carries a tick.
    int used = s_view.rev_b_type ? 1 : 0;
    lv_label_set_text(lv_obj_get_child(s_mix_tab[0], 0), used == 0 ? LV_SYMBOL_OK "  Mix Pos 1 / 2" : "Mix Pos 1 / 2");
    lv_label_set_text(lv_obj_get_child(s_mix_tab[1], 0), used == 1 ? LV_SYMBOL_OK "  2nd reverb" : "2nd reverb");
    show_mix_tab(used);
    lv_obj_set_hidden(s_mix_editor, false);
    lv_obj_move_foreground(s_mix_editor);
}

static lv_obj_t *mix_panel(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, y);
    lv_obj_set_size(p, 560, h);   // the reverb dialog's width
    lv_obj_set_scrollable(p, false);
    return p;
}

static void build_mix_editor(lv_obj_t *screen)
{
    s_mix_editor = panel(screen, 120, 90, 560, 316);
    dialog_title(s_mix_editor, "Reverb  \xC2\xB7  footswitch 8");
    s_mix_model_label = label(s_mix_editor, &ui_font_14, MUTED);
    lv_obj_align(s_mix_model_label, LV_ALIGN_TOP_RIGHT, -20, 20);
    static const char *const tabs[2] = { "Mix Pos 1 / 2", "2nd reverb" };
    for (int i = 0; i < 2; i++) {
        s_mix_tab[i] = small_button(s_mix_editor, 20 + i * 210, 54, 200, 40, tabs[i], on_mix_tab, i);
        s_mix_panel[i] = mix_panel(s_mix_editor, 106, 140);
    }

    // Tab 1: Pos 1 / Pos 2 of the expression assignment, played while moving
    lv_obj_t *mix = s_mix_panel[0];
    s_mix_info = label(mix, &ui_font_14, MUTED);
    lv_obj_set_width(s_mix_info, 520);
    lv_label_set_long_mode(s_mix_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_mix_info, 20, 0);
    static const char *const names[2] = { "POS 1", "POS 2" };
    for (int i = 0; i < 2; i++) {
        int y = 38 + i * 52;   // below the two lines of the text above
        lv_obj_t *name = label(mix, &ui_font_16, 0xC8CCD1);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, names[i]);
        lv_obj_set_pos(name, 20, y + 10);
        s_mix_slider[i] = lv_slider_create(mix);
        lv_slider_set_range(s_mix_slider[i], 0, 255);
        lv_obj_set_size(s_mix_slider[i], 310, 16);
        lv_obj_set_pos(s_mix_slider[i], 118, y + 16);
        lv_obj_set_ext_click_area(s_mix_slider[i], 20);
        style_slider(s_mix_slider[i], 0x00FFDD);   // reverb colour
        lv_obj_add_event_cb(s_mix_slider[i], on_mix_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        s_mix_value[i] = label(mix, &ui_font_28, 0xFFFFFF);
        lv_obj_set_width(s_mix_value[i], 90);
        lv_obj_set_style_text_align(s_mix_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_mix_value[i], 450, y + 6);
    }

    // Tab 2: a second reverb; footswitch 8 swaps the preset's reverb (A) and this one (B)
    lv_obj_t *rev = s_mix_panel[1];
    lv_obj_t *info = label(rev, &ui_font_14, MUTED);
    lv_obj_set_width(info, 520);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "Save here: footswitch 8 switches between the preset's reverb (A) and a second one (B). "
                            "B and its settings are stored on the controller; Edit B sets it up.");
    lv_obj_set_pos(info, 20, 0);
    static char options[512];
    size_t n = (size_t)snprintf(options, sizeof(options), "None (footswitch 8 = mix)");
    for (unsigned i = 0; i < NANO_SLOT_MODEL_COUNT[4] && s_rev_type_count < 16; i++) {
        const nano_fx_model_t *m = nano_fx_model(NANO_SLOT_MODELS[4][i]);
        if (!m || m->icon != NANO_ICON_REVERB || n + strlen(m->name) + 2 > sizeof(options)) continue;
        s_rev_types[s_rev_type_count++] = m->type;
        n += (size_t)snprintf(options + n, sizeof(options) - n, "\n%s", m->name);
    }
    s_rev_dropdown = lv_dropdown_create(rev);
    lv_dropdown_set_options_static(s_rev_dropdown, options);
    lv_obj_set_size(s_rev_dropdown, 320, 52);
    lv_obj_set_pos(s_rev_dropdown, 20, 56);
    lv_obj_set_style_text_font(s_rev_dropdown, &ui_font_20, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_rev_dropdown), &ui_font_20, 0);
    lv_obj_add_event_cb(s_rev_dropdown, on_rev_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    s_rev_edit = small_button(rev, 360, 56, 180, 52, "Edit B", on_mix_button, 2);

    s_mix_free = small_button(s_mix_editor, 20, 254, 150, 46, "Free pedal", on_mix_button, 3);
    small_button(s_mix_editor, 200, 254, 160, 46, "Cancel", on_mix_button, 0);
    primary(small_button(s_mix_editor, 380, 254, 160, 46, "Save", on_mix_button, 1));
    lv_obj_set_hidden(s_mix_editor, true);
}

// ---- Bluetooth MIDI ----

// The browser build (web/) uses the computer's MIDI inputs (Web MIDI) instead of a Bluetooth MIDI connection.
#ifdef NANO_WEB
#define MIDI_TITLE "MIDI"
#define MIDI_INFO "Tap a MIDI input, or Bluetooth MIDI device for a Bluetooth controller (MC6 / MC8 Pro, WIDI). It is used again by itself."
#define MIDI_SEARCHING "Looking for MIDI inputs ... (allow MIDI in the browser)"
#else
#define MIDI_TITLE "Bluetooth MIDI"
#define MIDI_INFO "Tap a device to connect it, e.g. an MC6 with a WIDI adapter. It reconnects by itself."
#define MIDI_SEARCHING "Searching for Bluetooth MIDI devices..."
#endif

static void fill_midi(void)
{
    const ui_midi_t *m = &s_midi_state;
    if (m->connected) lv_label_set_text_fmt(s_midi_status, LV_SYMBOL_BLUETOOTH " %s", m->name);
    else if (m->name[0]) lv_label_set_text_fmt(s_midi_status, "Waiting for %s", m->name);
    else lv_label_set_text(s_midi_status, "Not connected");
    lv_obj_set_style_text_color(s_midi_status, lv_color_hex(m->connected ? 0x45E35F : MUTED), 0);

    lv_obj_clean(s_midi_list);
    if (!m->count) {
        lv_obj_t *l = label(s_midi_list, &ui_font_20, MUTED);
        lv_label_set_text(l, MIDI_SEARCHING);
        return;
    }
    for (int i = 0; i < m->count; i++) {
        bool current = m->devices[i].remembered && m->connected;
        lv_obj_t *btn = lv_button_create(s_midi_list);
        lv_obj_set_size(btn, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current ? 0x15301C : 0x1A1D20), 0);
        lv_obj_set_style_border_width(btn, current ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(GREEN), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 9, 0);
        lv_obj_add_event_cb(btn, on_midi_device, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *name = label(btn, &ui_font_20, TEXT);
        lv_label_set_text_fmt(name, "%s%s", m->devices[i].name, current ? "  -  connected" : m->devices[i].remembered ? "  -  stored" : "");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
#ifndef NANO_WEB
        lv_obj_t *rssi = label(btn, &ui_font_14, MUTED);
        lv_label_set_text_fmt(rssi, "%d dBm", m->devices[i].rssi);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
#endif
    }
}

void ui_set_midi(const ui_midi_t *midi)
{
    lvgl_port_lock(0);
    s_midi_state = *midi;
    lv_obj_set_style_image_recolor(s_midi_icon, lv_color_hex(midi->connected ? GREEN : 0xC8CCD1), 0);
    if (!lv_obj_is_hidden(s_midi)) fill_midi();
    lvgl_port_unlock();
}

static void on_midi_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 close, -1 forget
    if (action < 0) {
        send('P', -1);
        return;
    }
    lv_obj_set_hidden(s_midi, true);
    send('X', 0);
}

static void build_midi(lv_obj_t *screen)
{
    s_midi = panel(screen, 110, 60, 580, 400);
    dialog_title(s_midi, MIDI_TITLE);
    s_midi_status = label(s_midi, &ui_font_14, MUTED);
    lv_obj_align(s_midi_status, LV_ALIGN_TOP_RIGHT, -76, 22);
    close_button(s_midi, 520, 12, on_midi_button, 0);
    lv_obj_t *info = label(s_midi, &ui_font_14, MUTED);
    lv_obj_set_width(info, 540);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, MIDI_INFO);
    lv_obj_set_pos(info, 20, 46);
    lv_obj_t *map = label(s_midi, &ui_font_14, MUTED);
    lv_obj_set_width(map, 540);
    lv_label_set_long_mode(map, LV_LABEL_LONG_WRAP);
    lv_label_set_text(map, "PC 0-63 = presets   CC 37-41 = FX 1-5   CC 1 = reverb mix   CC 50-57 = footswitches 1-8");
    lv_obj_set_pos(map, 20, 292);

    s_midi_list = lv_obj_create(s_midi);
    lv_obj_set_pos(s_midi_list, 10, 86);
    lv_obj_set_size(s_midi_list, 560, 204);
    lv_obj_set_style_bg_opa(s_midi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_midi_list, 0, 0);
    lv_obj_set_style_pad_all(s_midi_list, 6, 0);
    lv_obj_set_style_pad_row(s_midi_list, 6, 0);
    lv_obj_set_flex_flow(s_midi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_midi_list, LV_DIR_VER);

    small_button(s_midi, 20, 336, 180, 46, "Forget device", on_midi_button, -1);
    lv_obj_set_hidden(s_midi, true);
}

void ui_set_library(const nano_library_t *library)
{
    lvgl_port_lock(0);
    s_library = library;
    if (!lv_obj_is_hidden(s_picker) && s_picker_library) fill_picker();
    lvgl_port_unlock();
}

// Capture / cab card: icon square, title, current name, slot. Tapping opens the picker.
// Tap: cb (slot picker), long press: long_cb (capture volume / cab settings).
static lv_obj_t *source_card(lv_obj_t *screen, int x, const char *title, int icon, lv_event_cb_t cb, lv_event_cb_t long_cb,
                             lv_obj_t **square, lv_obj_t **name, lv_obj_t **slot, lv_obj_t **title_label)
{
    lv_obj_t *c = card(screen, x, 126, 385, 54);
    lv_obj_set_clickable(c, true);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x24272A), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(c, long_cb, LV_EVENT_LONG_PRESSED, NULL);
    *square = icon_square(c, 36);
    lv_obj_set_pos(*square, 10, 9);
    set_icon_square(*square, TEXT, NANO_ICONS[icon], true);
    lv_obj_t *t = label(c, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 56, 7);
    if (title_label) *title_label = t;
    *slot = label(c, &ui_font_14, MUTED);
    lv_obj_align(*slot, LV_ALIGN_TOP_RIGHT, -12, 7);
    *name = label(c, &ui_font_20, TEXT);
    lv_obj_set_width(*name, 310);
    lv_label_set_long_mode(*name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(*name, 56, 24);
    return c;
}

// Tuner as in the desktop editor: the note in the tuning colour (grey idle, green in tune, orange off), a scale with
// ticks every 10 cents and a green centre line, a glowing needle, the cents, and at the bottom the reference
// pitch and the output mute. A tap on the background or footswitch 2 closes it.
#define TUNER_IDLE 0x9A9A9A
#define TUNER_IN_TUNE 0x45F862
#define TUNER_OFF 0xFF7000
#define TUNER_SCALE_Y 46            // offset of the scale centre from the screen centre
#define TUNER_SCALE_H 60

static void tuner_color(uint32_t color)
{
    lv_obj_set_style_text_color(s_tuner_note, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_tuner_needle, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_color(s_tuner_needle, lv_color_hex(color), 0);
}

static lv_obj_t *tuner_box(lv_obj_t *parent, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static void build_tuner(lv_obj_t *screen)
{
    s_tuner = lv_obj_create(screen);
    lv_obj_set_size(s_tuner, 800, 480);
    lv_obj_set_pos(s_tuner, 0, 0);
    lv_obj_set_style_bg_color(s_tuner, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_tuner, 0, 0);
    lv_obj_set_style_radius(s_tuner, 0, 0);
    lv_obj_set_style_pad_all(s_tuner, 0, 0);
    lv_obj_set_scrollable(s_tuner, false);
    lv_obj_set_clickable(s_tuner, true);
    lv_obj_add_event_cb(s_tuner, on_tuner, LV_EVENT_CLICKED, NULL);

    // Head: tuner symbol in green, title, how to close
    lv_obj_t *sq = icon_square(s_tuner, 40);
    lv_obj_set_pos(sq, 24, 18);
    set_icon_square(sq, TUNER_IN_TUNE, NANO_ICONS[NANO_ICON_TUNER], true);
    if (!NANO_ICONS[NANO_ICON_TUNER]) lv_label_set_text(lv_obj_get_child(sq, 1), LV_SYMBOL_AUDIO);
    lv_obj_t *title = label(s_tuner, &ui_font_28, 0xF2F2F2);
    lv_label_set_text(title, "Tuner");
    lv_obj_set_pos(title, 78, 22);
    lv_obj_t *hint = label(s_tuner, &ui_font_14, 0x6F747A);
    lv_label_set_text(hint, "Tap or footswitch 2 to close");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -24, 32);

    // The note in a large TTF font (a scaled bitmap font did not show up on the display).
#ifdef NANO_WEB   // browser build: the font is a C array (web/build.sh)
    extern const uint8_t note_ttf_start[];
    extern const size_t note_ttf_size;
    const uint8_t *note_ttf_end = note_ttf_start + note_ttf_size;
#else
    extern const uint8_t note_ttf_start[] asm("_binary_IBMPlexSans_SemiBold_ttf_start");
    extern const uint8_t note_ttf_end[] asm("_binary_IBMPlexSans_SemiBold_ttf_end");
#endif
    const lv_font_t *note_font = lv_tiny_ttf_create_data_ex(note_ttf_start, note_ttf_end - note_ttf_start,
                                                            TUNER_NOTE_SIZE, LV_FONT_KERNING_NORMAL, 16);
    s_tuner_note = label(s_tuner, note_font ? note_font : &ui_font_title_46, TUNER_IDLE);
    lv_obj_set_width(s_tuner_note, 600);
    lv_obj_set_style_text_align(s_tuner_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_tuner_note, LV_ALIGN_CENTER, 0, -80);

    // Scale: dark field with ticks every 10 cents and the green centre line
    lv_obj_t *scale = tuner_box(s_tuner, TUNER_TRACK_W, TUNER_SCALE_H, 0x151515);
    lv_obj_align(scale, LV_ALIGN_CENTER, 0, TUNER_SCALE_Y);
    lv_obj_set_style_radius(scale, 10, 0);
    lv_obj_set_style_border_width(scale, 1, 0);
    lv_obj_set_style_border_color(scale, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_clip_corner(scale, true, 0);
    for (int i = 1; i < 10; i++) {
        if (i == 5) continue;
        lv_obj_t *tick = tuner_box(scale, 1, TUNER_SCALE_H, 0x3A3A3A);
        lv_obj_set_pos(tick, TUNER_TRACK_W * i / 10, 0);
    }
    lv_obj_t *centre = tuner_box(scale, 2, TUNER_SCALE_H, TUNER_IN_TUNE);
    lv_obj_set_pos(centre, TUNER_TRACK_W / 2 - 1, 0);

    static const char *const marks[] = { "-50", "-25", "0", "+25", "+50" };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *m = label(s_tuner, &ui_font_14, 0x8E9297);
        lv_label_set_text(m, marks[i]);
        lv_obj_align(m, LV_ALIGN_CENTER, (i - 2) * TUNER_TRACK_W / 4, TUNER_SCALE_Y + TUNER_SCALE_H / 2 + 16);
    }

    // Needle: reaches 10 px over the scale, glows in the tuning colour
    s_tuner_needle = tuner_box(s_tuner, 8, TUNER_SCALE_H + 20, TUNER_IDLE);
    lv_obj_set_style_radius(s_tuner_needle, 4, 0);
    lv_obj_set_style_shadow_width(s_tuner_needle, 22, 0);
    lv_obj_set_style_shadow_opa(s_tuner_needle, LV_OPA_60, 0);
    lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, 0, TUNER_SCALE_Y);

    s_tuner_cents = label(s_tuner, &ui_font_28, 0xD8D8D8);
    lv_obj_align(s_tuner_cents, LV_ALIGN_CENTER, 0, TUNER_SCALE_Y + TUNER_SCALE_H / 2 + 56);

    // Bottom: reference pitch A4 (- / +, held repeats) and output mute
    lv_obj_t *ref = label(s_tuner, &ui_font_14, 0x8E9297);
    lv_label_set_text(ref, "REFERENCE A4");
    lv_obj_set_style_text_letter_space(ref, 2, 0);
    lv_obj_set_pos(ref, 24, 428);
    lv_obj_t *down = button(s_tuner, 166, 412, 56, 48, on_tuner_down);
    lv_obj_add_event_cb(down, on_tuner_down, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *down_label = label(down, &ui_font_28, 0xF2F2F2);
    lv_label_set_text(down_label, LV_SYMBOL_MINUS);
    lv_obj_center(down_label);
    s_tuner_hz = label(s_tuner, &ui_font_28, 0xF2F2F2);
    lv_obj_set_width(s_tuner_hz, 120);
    lv_obj_set_style_text_align(s_tuner_hz, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_tuner_hz, 226, 421);
    lv_obj_t *up = button(s_tuner, 350, 412, 56, 48, on_tuner_up);
    lv_obj_add_event_cb(up, on_tuner_up, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *up_label = label(up, &ui_font_28, 0xF2F2F2);
    lv_label_set_text(up_label, LV_SYMBOL_PLUS);
    lv_obj_center(up_label);

    // Mute: the whole area is the button; the switch only shows the state (it comes from the Nano)
    lv_obj_t *mute = lv_button_create(s_tuner);
    lv_obj_set_size(mute, 290, 48);
    lv_obj_set_pos(mute, 486, 412);
    lv_obj_set_style_bg_opa(mute, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(mute, 0, 0);
    lv_obj_set_style_border_width(mute, 0, 0);
    lv_obj_add_event_cb(mute, on_tuner_mute, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mute_label = label(mute, &ui_font_14, 0x8E9297);
    lv_label_set_text(mute_label, "MUTE OUTPUT");
    lv_obj_set_style_text_letter_space(mute_label, 2, 0);
    lv_obj_align(mute_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_tuner_mute = lv_switch_create(mute);
    lv_obj_set_clickable(s_tuner_mute, false);
    lv_obj_set_size(s_tuner_mute, 86, 42);
    lv_obj_align(s_tuner_mute, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_tuner_mute, lv_color_hex(0x2A2D30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_tuner_mute, lv_color_hex(TUNER_IN_TUNE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_tuner_mute, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_tuner_mute, lv_color_hex(0x0D0F11), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(s_tuner_mute, -3, LV_PART_KNOB);

    lv_obj_set_hidden(s_tuner, true);
}

static void build_editor(lv_obj_t *screen);
static void build_scene_editor(lv_obj_t *screen);
static void build_expression(lv_obj_t *screen);
static void style_scene_editor(void);
static void style_scene_chip(void);
static bool scene_panel_open(void);
static void close_expression(void);

// ---- start screen: shown until the Nano's presets are loaded (or a tap) ----

static void splash_glow(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)v, 0);
}

static void splash_hide(void)
{
    if (!s_splash) return;
    lv_obj_fade_out(s_splash, 400, 0);
    lv_obj_delete_delayed(s_splash, 450);
    s_splash = s_splash_status = s_splash_version = NULL;
}

static void on_splash_tap(lv_event_t *e)
{
    splash_hide();
}

static void build_splash(lv_obj_t *screen)
{
    // The effect categories in their colours, glowing one after another.
    static const struct { int icon; uint32_t color; } GLOW[] = {
        { NANO_ICON_OVERDRIVE, 0xFF7000 }, { NANO_ICON_COMPRESSOR, 0x45F862 }, { NANO_ICON_EQUALIZER, 0x0A74E0 },
        { NANO_ICON_MODULATION, 0x8A5CFF }, { NANO_ICON_PITCH, 0xFFD236 }, { NANO_ICON_FILTER, 0x87DAFF },
        { NANO_ICON_REVERB, 0x00FFDD },
    };
    const int count = sizeof(GLOW) / sizeof(GLOW[0]), size = 62, gap = 26;
    s_splash = lv_obj_create(screen);
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, 800, 480);
    lv_obj_set_style_bg_color(s_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_splash, true);
    lv_obj_add_event_cb(s_splash, on_splash_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = label(s_splash, &ui_font_title_46, 0xFFFFFF);
    lv_obj_set_style_text_letter_space(title, 8, 0);
    lv_label_set_text(title, "NANO CORTEX");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 4, 104);
    lv_obj_t *sub = label(s_splash, &ui_font_20, MUTED);
    lv_obj_set_style_text_letter_space(sub, 14, 0);
    lv_label_set_text(sub, "CONTROLLER");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 7, 168);

    int x = (800 - count * size - (count - 1) * gap) / 2;
    for (int i = 0; i < count; i++) {
        lv_obj_t *sq = icon_square(s_splash, size);
        lv_obj_set_pos(sq, x + i * (size + gap), 246);
        set_icon_square(sq, GLOW[i].color, NANO_ICONS[GLOW[i].icon], true);
        lv_obj_set_style_shadow_color(sq, lv_color_hex(GLOW[i].color), 0);
        lv_obj_set_style_shadow_width(sq, 34, 0);
        lv_obj_set_style_shadow_spread(sq, 3, 0);
        lv_obj_set_style_shadow_opa(sq, 30, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, sq);
        lv_anim_set_exec_cb(&a, splash_glow);
        lv_anim_set_values(&a, 30, 230);
        lv_anim_set_duration(&a, 1100);
        lv_anim_set_reverse_duration(&a, 1100);
        lv_anim_set_delay(&a, i * 220);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    s_splash_status = label(s_splash, &ui_font_14, MUTED);
    lv_obj_set_style_text_letter_space(s_splash_status, 2, 0);
    lv_obj_align(s_splash_status, LV_ALIGN_TOP_MID, 0, 360);
    s_splash_version = label(s_splash, &ui_font_14, 0x5A5A5A);
    lv_obj_align(s_splash_version, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
}

void ui_splash_status(const char *status, const char *version)
{
    lvgl_port_lock(0);
    if (s_splash && status) {
        lv_label_set_text(s_splash_status, status);
        lv_obj_align(s_splash_status, LV_ALIGN_TOP_MID, 0, 360);
    }
    if (s_splash && version) lv_label_set_text(s_splash_version, version);
    lvgl_port_unlock();
}

void ui_splash_done(void)
{
    lvgl_port_lock(0);
    splash_hide();
    lvgl_port_unlock();
}

void ui_init(ui_command_cb on_command, ui_param_cb on_param, ui_text_cb on_text)
{
    s_on_command = on_command;
    s_on_param = on_param;
    s_on_text = on_text;
    lvgl_port_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    s_main = lv_obj_create(screen);
    lv_obj_remove_style_all(s_main);
    lv_obj_set_size(s_main, 800, 480);
    lv_obj_set_scrollable(s_main, false);
    lv_obj_set_clickable(s_main, false);

    // Top bar as in the editor, symbol buttons on both sides: refresh, capture volume, MIDI and USB on the left,
    // connection and preset / bank in the middle, expression pedal, scenes and Save (green when there is something to
    // save) on the right. Below: the arrows and the preset name, an orange dot next to it for unsaved changes.
    lv_obj_t *preset = lv_obj_create(s_main);
    lv_obj_remove_style_all(preset);
    lv_obj_set_pos(preset, 0, 0);
    lv_obj_set_size(preset, 800, 122);
    lv_obj_set_scrollable(preset, false);
    s_preset_card = preset;
    static const struct { int icon; lv_event_cb_t cb; } TOP[] = {
        { UI_ICON_REFRESH, on_refresh }, { UI_ICON_VOLUME, open_volume }, { UI_ICON_MIDI, open_midi }, { UI_ICON_USB, open_usb },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = button(preset, 10 + i * 62, 4, 56, 44, TOP[i].cb);
        lv_obj_t *img = ui_icon(b, TOP[i].icon, 24, 0xC8CCD1);
        lv_obj_center(img);
        if (TOP[i].icon == UI_ICON_MIDI) s_midi_icon = img;
    }
    s_midi_label = NULL;

    lv_obj_t *status_row = lv_obj_create(preset);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 10, 0);
    lv_obj_set_clickable(status_row, false);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 18);
    s_status_dot = lv_obj_create(status_row);
    lv_obj_set_size(s_status_dot, 9, 9);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    s_status = label(status_row, &ui_font_14, MUTED);
    lv_obj_set_style_text_letter_space(s_status, 2, 0);
    s_preset_number = label(status_row, &ui_font_14, MUTED);
    lv_obj_set_style_text_letter_space(s_preset_number, 2, 0);
    s_app = label(status_row, &ui_font_14, 0x3D8BFF);    // editor connected through the controller
    lv_obj_set_style_text_letter_space(s_app, 2, 0);
    lv_label_set_text(s_app, "APP");
    lv_obj_set_hidden(s_app, true);

    // Expression pedal: what the Nano's pedal moves in this preset
    s_exp_button = button(preset, 610, 4, 56, 44, open_expression);
    lv_obj_center(ui_icon(s_exp_button, UI_ICON_EXP, 24, 0xC8CCD1));
    // Scenes: footswitches 3-8 switch between the scenes of this preset instead of between the bank's presets
    s_scene_button = button(preset, 672, 4, 56, 44, on_scene_button);
    lv_obj_center(ui_icon(s_scene_button, UI_ICON_SCENES, 24, 0xC8CCD1));
    s_save_button = button(preset, 734, 4, 56, 44, on_save_button);
    lv_obj_center(ui_icon(s_save_button, UI_ICON_SAVE, 24, 0xC8CCD1));

    lv_obj_t *prev = button(preset, 10, 52, 64, 66, on_previous);
    lv_obj_t *prev_label = label(prev, &ui_font_title_40, TEXT);
    lv_label_set_text(prev_label, LV_SYMBOL_LEFT);
    lv_obj_center(prev_label);
    lv_obj_t *next = button(preset, 726, 52, 64, 66, on_next);
    lv_obj_t *next_label = label(next, &ui_font_title_40, TEXT);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);
    s_preset_name = label(preset, &ui_font_title_46, 0xFFFFFF);
    lv_obj_set_width(s_preset_name, 600);
    lv_obj_set_style_text_align(s_preset_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_preset_name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_preset_name, 100, 56);
    lv_obj_set_clickable(s_preset_name, true);   // long press: rename
    lv_obj_add_event_cb(s_preset_name, open_rename, LV_EVENT_LONG_PRESSED, NULL);
    s_edited = lv_obj_create(preset);   // unsaved changes
    lv_obj_set_size(s_edited, 10, 10);
    lv_obj_set_style_radius(s_edited, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_edited, 0, 0);
    lv_obj_set_style_bg_color(s_edited, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_shadow_width(s_edited, 10, 0);
    lv_obj_set_style_shadow_color(s_edited, lv_color_hex(ORANGE), 0);
    lv_obj_set_hidden(s_edited, true);

    // Gig view (swipe up): slim status bar over the tiles - connection, preset and bank, name, unsaved dot, mode.
    s_gig_bar = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_gig_bar);
    lv_obj_set_pos(s_gig_bar, 0, 0);
    lv_obj_set_size(s_gig_bar, 800, GIG_BAR_H);
    lv_obj_set_style_bg_color(s_gig_bar, lv_color_hex(0x0B0C0E), 0);
    lv_obj_set_style_bg_opa(s_gig_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_gig_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_gig_bar, 1, 0);
    lv_obj_set_style_border_color(s_gig_bar, lv_color_hex(0x1D2024), 0);
    lv_obj_set_scrollable(s_gig_bar, false);
    s_gig_dot = lv_obj_create(s_gig_bar);
    lv_obj_set_size(s_gig_dot, 9, 9);
    lv_obj_set_style_radius(s_gig_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_gig_dot, 0, 0);
    lv_obj_align(s_gig_dot, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *badge = lv_obj_create(s_gig_bar);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_min_width(badge, 32, 0);
    lv_obj_set_style_pad_hor(badge, 9, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x1D2024), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 32, 0);
    s_gig_number = label(badge, &ui_font_16, TEXT);
    lv_obj_center(s_gig_number);
    s_gig_bank = label(s_gig_bar, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(s_gig_bank, 2, 0);
    lv_obj_align_to(s_gig_bank, badge, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    s_gig_name = label(s_gig_bar, &ui_font_title_22, TEXT);
    lv_obj_set_width(s_gig_name, 440);
    lv_obj_set_style_text_align(s_gig_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_gig_name, LV_LABEL_LONG_DOT);
    lv_obj_align(s_gig_name, LV_ALIGN_CENTER, 0, 0);
    s_gig_dirty = lv_obj_create(s_gig_bar);
    lv_obj_set_size(s_gig_dirty, 9, 9);
    lv_obj_set_style_radius(s_gig_dirty, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_gig_dirty, 0, 0);
    lv_obj_set_style_bg_color(s_gig_dirty, lv_color_hex(ORANGE), 0);
    lv_obj_set_hidden(s_gig_dirty, true);
    s_gig_mode = label(s_gig_bar, &ui_font_12, TEXT);
    lv_obj_set_style_text_letter_space(s_gig_mode, 2, 0);
    lv_obj_set_style_pad_hor(s_gig_mode, 11, 0);
    lv_obj_set_style_pad_ver(s_gig_mode, 5, 0);
    lv_obj_set_style_radius(s_gig_mode, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_gig_mode, 2, 0);
    lv_obj_set_style_border_color(s_gig_mode, lv_color_hex(TEXT), 0);
    lv_obj_align(s_gig_mode, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_clickable(s_gig_mode, true);   // a tap: as the Scenes button of the top bar, which is hidden here
    lv_obj_set_ext_click_area(s_gig_mode, 14);
    lv_obj_add_event_cb(s_gig_mode, on_scene_button, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_gig_bar, true);

    // Capture and cab cards
    s_source_card[0] = source_card(s_main, 10, "CAPTURE", NANO_ICON_CAPTURE, open_capture_picker, open_amp,
                                   &s_capture_square, &s_capture, &s_capture_slot_label, &s_capture_title);
    s_source_card[1] = source_card(s_main, 405, "CAB / IR", NANO_ICON_CAB, open_cab_picker, open_cab_settings,
                                   &s_cab_square, &s_cab, &s_cab_slot_label, NULL);

    for (int i = 0; i < TILES; i++) build_tile(s_main, i);
    build_picker(screen);
    build_bank_editor(screen);
    build_looper_editor(screen);
    build_editor(screen);
    build_scene_editor(screen);
    build_expression(screen);
    build_tuner(screen);
    build_ask(screen);
    build_rename(screen);
    build_usb(screen);
    build_volume(screen);
    build_cab_settings(screen);
    build_amp(screen);
    build_mix_editor(screen);
    build_learn(screen);
    build_midi(screen);
    build_toast(screen);
    build_splash(screen);

    lvgl_port_unlock();
    ui_set_link(false);
}

static void set_fullscreen(bool on)
{
    if (on == s_fullscreen) return;
    s_fullscreen = on;
    lv_obj_set_hidden(s_preset_card, on);
    lv_obj_set_hidden(s_source_card[0], on);
    lv_obj_set_hidden(s_source_card[1], on);
    lv_obj_set_hidden(s_gig_bar, !on);
    for (int i = 0; i < TILES; i++) place_tile(i);
    fit_names();
}

void ui_set_app(bool connected)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_app, !connected);
    lvgl_port_unlock();
}

void ui_set_link(bool connected)
{
    lvgl_port_lock(0);
    s_linked = connected;
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(connected ? GREEN : 0x555555), 0);
    lv_obj_set_style_bg_color(s_gig_dot, lv_color_hex(connected ? GREEN : 0x555555), 0);
#ifdef NANO_WEB   // the browser connects when CONNECT is clicked
    lv_label_set_text(s_status, connected ? "CONNECTED" : "NOT CONNECTED");
#else
    lv_label_set_text(s_status, connected ? "CONNECTED" : "SEARCHING FOR NANO");
#endif
    lv_obj_set_hidden(s_status, false);   // until the first preset is shown
    if (!connected) {
        set_fullscreen(false);   // show the search status
        lv_obj_set_hidden(s_edited, true);
        lv_obj_set_hidden(s_preset_number, true);
        lv_obj_set_hidden(s_usb, true);
        lv_obj_set_hidden(s_volume, true);
        lv_obj_set_hidden(s_cab_settings, true);
        lv_obj_set_hidden(s_amp, true);
        lv_obj_set_hidden(s_mix_editor, true);
        lv_obj_set_hidden(s_tuner, true);
        lv_obj_set_hidden(s_editor, true);
        lv_obj_set_hidden(s_picker, true);
        lv_obj_set_hidden(s_bank_editor, true);
        lv_obj_set_hidden(s_scene_editor, true);
        lv_obj_set_hidden(s_exp_dialog, true);
        lv_obj_set_hidden(s_cal_panel, true);
        lv_obj_set_hidden(s_ask, true);
        lv_obj_set_hidden(s_rename, true);
        lv_label_set_text(s_capture_slot_label, "");
        lv_label_set_text(s_cab_slot_label, "");
        lv_label_set_text(s_preset_number, "");
        lv_label_set_text(s_preset_name, "");
        lv_label_set_text(s_capture, "");
        lv_label_set_text(s_cab, "");
        for (int i = 0; i < TILES; i++) set_tile(i, "", "", 0x2A2A2A, false, true, NULL, NULL);
        show_tile_extras();
        fit_names();
        update_main_hidden();
    }
    lvgl_port_unlock();
}

// Pedal drawing on an FX tile (after set_tile), in the tile's symbol colour.
static void set_tile_pedal(int i, uint32_t type)
{
    s_tile_pedal_type[i] = type;
}

// Footswitch 1 (mode) and 2 (tuner) tiles.
static void show_fixed_tiles(const ui_view_t *view)
{
    char caption[24];
    if (view->fx_mode) snprintf(caption, sizeof(caption), "MODE");
    else if (view->scenes) snprintf(caption, sizeof(caption), "PRESET %d", s_current_preset);
    else snprintf(caption, sizeof(caption), "BANK %d", view->bank + 1);
    set_tile(0, caption, view->fx_mode ? "FX" : view->scenes ? "Scenes" : "Presets", 0x8A9099, false, false, NULL, LV_SYMBOL_LOOP);
    set_tile(1, "TUNER", "Tuner", 0xBDBDBD, false, false, NANO_ICONS[NANO_ICON_TUNER], NANO_ICONS[NANO_ICON_TUNER] ? NULL : LV_SYMBOL_AUDIO);
    // Under the caption: what holding the footswitch does (as the other effect of an A/B slot on its tile) - the
    // looper mode, and the scenes or the bank's presets with the symbol of the scenes button
    s_tile_note[0] = LV_SYMBOL_LOOP " Hold: Looper";
    s_tile_note[1] = view->scenes && !view->fx_mode ? "Hold: Presets" : "Hold: Scenes";
    s_tile_note_icon[1] = UI_ICONS[UI_ICON_SCENES];
}

static void show_fx_tiles(const nano_state_t *st, const ui_view_t *view)
{
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        const nano_fx_model_t *model = nano_fx_model(st->fx_type[slot]);
        bool empty = !st->fx_known || !st->fx_type[slot];
        bool on = !empty && st->fx_on[slot];
        char slot_caption[24];
        if (slot == 0 && view->pre1_b_type) snprintf(slot_caption, sizeof(slot_caption), "%s %c", NANO_FX_SLOT_NAMES[0], view->pre1_active ? 'B' : 'A');
        else snprintf(slot_caption, sizeof(slot_caption), "%s", NANO_FX_SLOT_NAMES[slot]);
        set_tile(2 + slot, slot_caption, empty ? "Empty" : nano_fx_name(st->fx_type[slot]),
                 model ? model->color : 0x6A6A6A, on, empty, model ? NANO_ICONS[model->icon] : NULL, NULL);
        if (!empty) set_tile_pedal(2 + slot, st->fx_type[slot]);
        if (slot == 0 && view->pre1_b_type) set_tile_other(2, view->pre1_active ? view->pre1_a_type : view->pre1_b_type);
    }

    // Footswitch 8: reverb A/B, or the reverb mix between Pos 1 and Pos 2 of the expression assignment.
    char caption[32];
    const lv_image_dsc_t *icon = NANO_ICONS[NANO_ICON_REVERB];
    if (view->rev_b_type) {   // the switch for reverb B: lit while B runs (tile 7 shows what is in the slot)
        bool none = view->mix_slot < 0;
        set_tile(7, "REVERB B", none ? "No reverb" : nano_fx_name(view->rev_b_type), 0x00FFDD, view->rev_active == 1, none, icon, NULL);
        if (!none) set_tile_pedal(7, view->rev_b_type);
    } else if (view->mix_slot < 0) {
        set_tile(7, "REV MIX", "No reverb", 0x00FFDD, false, true, icon, NULL);
    } else if (!view->mix_known) {
        set_tile(7, "REV MIX", "No Pos 1/2", 0x00FFDD, false, true, icon, NULL);
    } else {
        snprintf(caption, sizeof(caption), "MIX %d/%d%%", (int)lroundf(view->mix_pos[0] * 100), (int)lroundf(view->mix_pos[1] * 100));
        set_tile(7, caption, view->mix_active == 1 ? "Pos 2" : view->mix_active == 0 ? "Pos 1" : "Rev Mix",
                 0x00FFDD, view->mix_active == 1, false, icon, NULL);
    }
    if (view->mix_slot >= 0 && !view->rev_b_type && view->mix_known) set_tile_pedal(7, st->fx_type[view->mix_slot]);
}

// Looper mode: tile 1 is the mode (filled while the phone is connected), 2-8 the switches of the looper app with
// the name, colour and symbol the user gave them (long press; by default Pause and six loops in three colour pairs,
// as a two-column Loopy Pro project shows them). The caption is the controller a switch sends.
static void show_looper_tiles(const ui_view_t *view)
{
    // Under the caption: what the last press sent (to check a binding against), else the phone's state.
    static char note[24];
    if (view->looper_sent > 0) snprintf(note, sizeof(note), "Sent CC %d", view->looper_sent);
    else if (view->looper_sent < 0) snprintf(note, sizeof(note), "CC %d not sent", -view->looper_sent);
    else snprintf(note, sizeof(note), "%s", view->phone ? "Phone connected" : "No phone");
    set_tile(0, "MODE", "Looper", 0xFF4D4D, view->phone, false, NULL, LV_SYMBOL_LOOP);
    s_tile_note[0] = note;
    for (int i = 0; i < UI_LOOPER_SWITCHES; i++) {
        char caption[12];
        snprintf(caption, sizeof(caption), "CC %d", 102 + i);
        int color = view->looper_colors[i] < UI_BANK_COLOR_COUNT ? view->looper_colors[i] : 0;
        int icon = view->looper_icons[i] < UI_LOOPER_SYMBOLS ? view->looper_icons[i] : 0;
        set_tile(1 + i, caption, view->looper_names[i], BANK_COLORS[color], false, false, NULL, LOOPER_SYMBOLS[icon]);
    }
}

// Preset tiles carry no caption, so the name gets the whole tile.
static void show_preset_tiles(const nano_state_t *st, const ui_view_t *view)
{
    for (int i = 0; i < 6; i++) {
        int preset = view->bank_presets[i];
        if (preset < 1 || preset > NANO_PRESETS) {
            set_tile(2 + i, "EMPTY", "Hold to set", 0x3A3A3A, false, true, NULL, NULL);
            continue;
        }
        const char *name = st->preset_names[preset - 1];
        uint32_t color = BANK_COLORS[view->bank_colors[i] < UI_BANK_COLOR_COUNT ? view->bank_colors[i] : 0];
        int icon = view->bank_icons[i];
        set_tile(2 + i, "", st->names_loaded && name[0] ? name : "-", color, preset == st->current_preset, false,
                 icon >= 1 && icon <= PRESET_ICON_COUNT ? PRESET_ICONS[icon - 1].image : NULL, NULL);
    }
}

// Scene mode: the scenes of the current preset. The name gets the whole tile (as a preset's); the marks in the top
// row say which effects the scene switches on. Lit: the scene whose effects are on right now.
static void show_scene_tiles(const ui_view_t *view)
{
    for (int i = 0; i < UI_SCENES; i++) {
        if (!view->scene_names[i][0]) {
            char caption[12];
            snprintf(caption, sizeof(caption), "SCENE %d", i + 1);
            set_tile(2 + i, caption, "Hold to set", 0x3A3A3A, false, true, NULL, NULL);
            continue;
        }
        bool active = i == view->scene_active;
        uint32_t color = BANK_COLORS[view->scene_colors[i] < UI_BANK_COLOR_COUNT ? view->scene_colors[i] : 0];
        set_tile(2 + i, "", view->scene_names[i], color, active, false, NULL, NULL);
        s_tile_marks[2 + i] = 0x200 | (active ? 0x100 : 0) | (view->scene_fx[i] & ((1 << NANO_FX_SLOTS) - 1))
                              | (view->scene_set[i] & ((1 << NANO_FX_SLOTS) - 1)) << 16;
    }
}

void ui_show_state(const nano_state_t *st, const ui_view_t *view)
{
    char text[96];
    lvgl_port_lock(0);

    s_fx_mode = view->fx_mode;
    s_looper_mode = view->looper;
    s_current_preset = st->current_preset;
    bool slots_changed = false;
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        uint32_t type = st->fx_known ? st->fx_type[slot] : 0;
        slots_changed |= type != s_slot_type[slot];
        s_slot_type[slot] = type;
        s_slot_on[slot] = type && st->fx_on[slot];
    }
    if (!lv_obj_is_hidden(s_scene_editor)) {   // the dialog edits a scene of the preset it was opened in
        if (st->current_preset != s_se_preset) lv_obj_set_hidden(s_scene_editor, true);
        else if (slots_changed) style_scene_editor();
    }
    if (!lv_obj_is_hidden(s_exp_dialog) && st->current_preset != s_ex_preset) close_expression();   // it is the other preset's
    if (!lv_obj_is_hidden(s_editor)) {   // the FX editor's scene button and list say which scenes carry settings
        bool scenes_changed = memcmp(s_view.scene_set, view->scene_set, sizeof(s_view.scene_set)) || s_view.scene_active != view->scene_active
                           || memcmp(s_view.scene_names, view->scene_names, sizeof(s_view.scene_names));
        s_view = *view;
        style_scene_chip();
        if (scenes_changed && scene_panel_open()) lv_obj_invalidate(s_panel_list);
    }
    if (!view->looper) show_fixed_tiles(view);
    if (view->mix_slot >= 0) {
        snprintf(s_mix_model, sizeof(s_mix_model), "%s  -  %s", nano_fx_name(st->fx_type[view->mix_slot]),
                 NANO_FX_SLOT_NAMES[view->mix_slot]);
    }

    snprintf(text, sizeof(text), "PRESET %d  \xC2\xB7  BANK %d", st->current_preset, view->bank + 1);
    set_text(s_preset_number, text);
    set_hidden(s_preset_number, false);
    set_hidden(s_status, true);    // the green dot says "connected"
    // Scenes button: filled while the tiles show scenes (as every switch of the editor: filled = on)
    bool scenes_shown = view->scenes && !view->fx_mode && !view->looper;
    set_color_prop(s_scene_button, LV_STYLE_BG_COLOR, scenes_shown ? 0xECECEC : 0x191B1D);
    set_num_prop(s_scene_button, LV_STYLE_BG_GRAD_DIR, scenes_shown ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER);
    set_color_prop(s_scene_button, LV_STYLE_BORDER_COLOR, scenes_shown ? 0xECECEC : 0x3A3D40);
    set_color_prop(lv_obj_get_child(s_scene_button, 0), LV_STYLE_IMAGE_RECOLOR, scenes_shown ? INK : 0xC8CCD1);
    // Expression pedal: green while it does something in this preset (as MIDI while a controller is connected)
    set_color_prop(lv_obj_get_child(s_exp_button, 0), LV_STYLE_IMAGE_RECOLOR, view->exp_used ? GREEN : 0xC8CCD1);
    // Save: neutral, green when there is something to save (as in the editor), plus the orange dot by the name.
    set_color_prop(s_save_button, LV_STYLE_BG_COLOR, st->dirty ? GREEN : 0x191B1D);
    set_num_prop(s_save_button, LV_STYLE_BG_GRAD_DIR, st->dirty ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER);
    set_color_prop(s_save_button, LV_STYLE_BORDER_COLOR, st->dirty ? GREEN : 0x3A3D40);
    set_color_prop(lv_obj_get_child(s_save_button, 0), LV_STYLE_IMAGE_RECOLOR, st->dirty ? INK : 0xC8CCD1);
    const char *name = st->preset_names[st->current_preset - 1];
    const char *shown = st->names_loaded && name[0] ? name : "-";
    set_text(s_preset_name, shown);
    lv_point_t name_size;
    lv_text_get_size(&name_size, shown, &ui_font_title_46, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int half = (name_size.x < 600 ? name_size.x : 600) / 2;
    set_num_prop(s_edited, LV_STYLE_X, 400 + half + 10);
    set_num_prop(s_edited, LV_STYLE_Y, 82);
    set_hidden(s_edited, !st->dirty);

    // Gig view bar
    snprintf(text, sizeof(text), "%d", st->current_preset);
    set_text(s_gig_number, text);
    snprintf(text, sizeof(text), "BANK %d", view->bank + 1);
    set_text(s_gig_bank, text);
    set_text(s_gig_name, shown);
    lv_text_get_size(&name_size, shown, &ui_font_title_22, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    half = (name_size.x < 440 ? name_size.x : 440) / 2;
    set_align_if(s_gig_dirty, LV_ALIGN_CENTER, half + 12, 0);
    set_hidden(s_gig_dirty, !st->dirty);
    set_text(s_gig_mode, view->looper ? "LOOPER" : view->fx_mode ? "FX" : view->scenes ? "SCENES" : "PRESETS");
    set_color_prop(s_gig_mode, LV_STYLE_BG_COLOR, view->looper ? 0xFF4D4D : TEXT);
    set_color_prop(s_gig_mode, LV_STYLE_BORDER_COLOR, view->looper ? 0xFF4D4D : TEXT);
    set_num_prop(s_gig_mode, LV_STYLE_BG_OPA, view->looper || view->fx_mode ? LV_OPA_COVER : LV_OPA_TRANSP);
    set_color_prop(s_gig_mode, LV_STYLE_TEXT_COLOR, view->looper || view->fx_mode ? INK : TEXT);
    memcpy(s_capture_names, st->capture_names, sizeof(s_capture_names));
    memcpy(s_cab_names, st->cab_names, sizeof(s_cab_names));
    s_capture_slot = st->capture_slot;
    memcpy(s_preset_names, st->preset_names, sizeof(s_preset_names));
    s_view = *view;
    s_cab_slot = st->cab_slot;
    set_text(s_capture, st->capture_slot ? (st->capture[0] ? st->capture : "-") : "Bypassed");
    set_color_prop(s_capture, LV_STYLE_TEXT_COLOR, st->capture_slot ? TEXT : MUTED);
    // The capture's type (amp head, combo, pedal, ...) from the library, as in the editor.
    static const char *const KIND_NAMES[LIB_KIND_COUNT] = { "AMP HEAD", "COMBO", "AMP + CAB", "CAB", "PEDAL", "OVERDRIVE",
                                                            "FUZZ", "COMPRESSOR", "" };
    int kind = LIB_KIND_OTHER;
    if (s_library && st->capture_slot && st->capture[0]) {
        for (int i = 0; i < s_library->captures.count; i++) {
            if (!strcasecmp(s_library->captures.items[i].name, st->capture)) {
                kind = s_library->captures.items[i].kind;
                break;
            }
        }
    }
    const lv_image_dsc_t *kind_icon = kind <= LIB_KIND_OVERDRIVE ? UI_ICONS[UI_ICON_CAP_AMP_HEAD + kind]
                                    : kind == LIB_KIND_FUZZ ? NANO_ICONS[NANO_ICON_FUZZ]
                                    : kind == LIB_KIND_COMPRESSOR ? NANO_ICONS[NANO_ICON_COMPRESSOR] : NANO_ICONS[NANO_ICON_CAPTURE];
    set_icon_square(s_capture_square, TEXT, kind_icon ? kind_icon : NANO_ICONS[NANO_ICON_CAPTURE], st->capture_slot != 0);
    if (KIND_NAMES[kind][0]) snprintf(text, sizeof(text), "CAPTURE  \xC2\xB7  %s", KIND_NAMES[kind]);
    else snprintf(text, sizeof(text), "CAPTURE");
    set_text(s_capture_title, text);
    if (st->capture_slot) snprintf(text, sizeof(text), "%d-%d", (st->capture_slot - 1) / 5 + 1, (st->capture_slot - 1) % 5 + 1);
    else snprintf(text, sizeof(text), "BYPASS");
    set_text(s_capture_slot_label, text);
    set_text(s_cab, st->cab_slot ? (st->cab[0] ? st->cab : "-") : "Bypassed");
    set_color_prop(s_cab, LV_STYLE_TEXT_COLOR, st->cab_slot ? TEXT : MUTED);
    set_icon_square(s_cab_square, TEXT, NANO_ICONS[NANO_ICON_CAB], st->cab_slot != 0);
    if (st->cab_slot) snprintf(text, sizeof(text), "%d", st->cab_slot);
    else snprintf(text, sizeof(text), "BYPASS");
    set_text(s_cab_slot_label, text);
    // Capture volume: not while the slider is held or right after a change on the board (older replies).
    if (lv_obj_is_hidden(s_volume) ||
        (lv_tick_elaps(s_volume_tick) > VOLUME_HOLD_MS && !lv_obj_has_state(s_level_slider[0], LV_STATE_PRESSED))) {
        s_capture_volume = st->capture_volume;
        if (!lv_obj_is_hidden(s_volume)) show_capture_volume();
    }
    bool amp_held = false;
    for (int i = 0; i < AMP_ROWS; i++) amp_held |= lv_obj_has_state(s_level_slider[AMP_ROW + i], LV_STATE_PRESSED);
    if (lv_obj_is_hidden(s_amp) || (lv_tick_elaps(s_amp_tick) > VOLUME_HOLD_MS && !amp_held)) {
        memcpy(s_amp_values, st->amp, sizeof(s_amp_values));
        if (!lv_obj_is_hidden(s_amp)) show_amp_values();
    }

    if (view->looper) show_looper_tiles(view);
    else if (view->fx_mode) show_fx_tiles(st, view);
    else if (view->scenes) show_scene_tiles(view);
    else show_preset_tiles(st, view);
    show_tile_extras();
    fit_names();

    lvgl_port_unlock();
}

static void tuner_idle(void)
{
    lv_label_set_text(s_tuner_note, "\xE2\x80\x93");   // en dash
    lv_label_set_text(s_tuner_cents, "");
    tuner_color(TUNER_IDLE);
    lv_obj_set_style_opa(s_tuner_needle, LV_OPA_30, 0);
    lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, 0, TUNER_SCALE_Y);
}

void ui_show_tuner(bool open)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_tuner, !open);
    update_main_hidden();
    if (open) tuner_idle();
    lvgl_port_unlock();
}

void ui_tuner_settings(float base_hz, bool muted)
{
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_tuner_hz, "%d Hz", (int)lroundf(base_hz));
    if (muted) lv_obj_add_state(s_tuner_mute, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_tuner_mute, LV_STATE_CHECKED);
    lvgl_port_unlock();
}

void ui_show_tuner_reading(const nano_tuner_reading_t *reading)
{
    char text[24];
    lvgl_port_lock(0);
    if (!reading->valid) {
        tuner_idle();
    } else {
        float cents = reading->cents < -50 ? -50 : reading->cents > 50 ? 50 : reading->cents;
        bool in_tune = reading->centered || fabsf(cents) <= 2;
        lv_label_set_text(s_tuner_note, reading->note);
        tuner_color(in_tune ? TUNER_IN_TUNE : TUNER_OFF);
        lv_obj_set_style_opa(s_tuner_needle, LV_OPA_COVER, 0);
        snprintf(text, sizeof(text), "%+.1f ct", (double)cents);
        lv_label_set_text(s_tuner_cents, text);
        lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, (int)(cents / 50 * (TUNER_TRACK_W / 2)), TUNER_SCALE_Y);
    }
    lvgl_port_unlock();
}

// ---- FX editor: model and parameters of one FX slot (long press on an FX tile) ----
// As the desktop editor: a header card (symbol chip filled = on / outlined = off, slot, model, pedal picture), the
// FX presets, and the parameters as fill bars in two columns. A bar follows the finger once it moves sideways (a tap
// or a scroll never jumps a value); enum parameters with up to three options show them as a segmented switch,
// longer lists open a panel. Fields that are not active have a frame in the dimmed effect colour (as the switch
// labels of a Morningstar MC8 Pro), the active ones are filled.
// Speed: bars and panel entries are single objects drawn in one go (LV_EVENT_DRAW_MAIN_END), the model list is
// built once per slot, and ui_fx_editor_show only touches what changed (the Nano's state arrives every few hundred
// ms while editing).

#define ED_BAR_W 384
#define ED_BAR_H 64
#define ED_BAR_PITCH 72
#define ED_BODY_Y 140
#define ED_DRAG_START 8       // px sideways before a bar follows the finger
#define ED_PANEL_W 716
#define ED_PANEL_H 398
#define ED_ENTRY_H 64         // panel entries: large enough to hit on stage
#define ED_ENTRY_W 342        // model entry: two columns in the panel
#define ED_OPTION_W 168       // option entry: four columns
#define ED_DIM 0.42f          // frame of a field that is not active: effect colour mixed with the background
#define ED_NEUTRAL 0x2E3237   // frame of an empty field or an unknown value

enum { PANEL_MODEL, PANEL_SECOND, PANEL_OPTION, PANEL_SCENE };

static void on_fxp_chip(lv_event_t *e);
static void on_fxp_chip_long(lv_event_t *e);
static void on_scene_chip(lv_event_t *e);

static float param_from_normalized(const nano_param_t *p, float n)
{
    float v = p->min + n * (p->max - p->min);
    if (p->step > 0) v = roundf(v / p->step) * p->step;
    return v < p->min ? p->min : v > p->max ? p->max : v;
}

// Option i of an enum parameter (the options are separated by "\n").
static void option_text(const nano_param_t *p, int i, char *buf, size_t size)
{
    const char *o = p->options ? p->options : "";
    for (; i > 0 && o; i--) {
        o = strchr(o, '\n');
        if (o) o++;
    }
    size_t len = o ? strcspn(o, "\n") : 0;
    if (len >= size) len = size - 1;
    if (o) memcpy(buf, o, len);
    buf[len] = 0;
}

static int option_index(const nano_param_t *p, float n)
{
    return n < 0 ? -1 : (int)lroundf(n * (p->option_count > 1 ? p->option_count - 1 : 0));
}

// One line of text; y1 is the top of the line box. Text that does not live on (a local buffer) is copied.
static void draw_text(lv_layer_t *layer, const char *text, bool copy, const lv_font_t *font, uint32_t color, int32_t x1,
                      int32_t y1, int32_t x2, lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.text_local = copy;
    d.font = font;
    d.color = lv_color_hex(color);
    d.align = align;
    lv_area_t area = { x1, y1, x2, y1 + lv_font_get_line_height(font) - 1 };
    lv_draw_label(layer, &d, &area);
}

#define NO_FILL 0xFF000000u   // draw_frame: only the frame (the background is there already)

static void draw_frame(lv_layer_t *layer, const lv_area_t *a, uint32_t bg, uint32_t border, int radius)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(bg);
    if (bg == NO_FILL) r.bg_opa = LV_OPA_TRANSP;
    r.radius = radius;
    r.border_width = 2;
    r.border_color = lv_color_hex(border);
    lv_draw_rect(layer, &r, a);
}

// ---- Panel (model / second effect / option) ----

static void panel_clean_later(void *unused)
{
    if (lv_obj_is_hidden(s_panel)) {
        lv_obj_clean(s_panel_list);
        s_panel_built_mode = -1;
    }
}

static void close_panel(void)
{
    set_hidden(s_panel_scrim, true);
    set_hidden(s_panel, true);
}

// The editor closes or shows another slot: the panel's entries go (after the click that may have caused it).
static void drop_panel(void)
{
    close_panel();
    s_panel_built_mode = -1;
    lv_async_call(panel_clean_later, NULL);
}

static void on_editor_back(lv_event_t *e)
{
    drop_panel();
    lv_obj_set_hidden(s_editor, true);
    update_main_hidden();
    s_editor_slot_index = -1;
    send('E', 0);
}

static void on_editor_onoff(lv_event_t *e)
{
    if (s_editor_slot_index >= 0) send('a' + s_editor_slot_index, 0);
}

static void open_panel(int mode, int param);

static void on_model_button(lv_event_t *e)
{
    if (s_editor_slot_index >= 0) open_panel(PANEL_MODEL, 0);
}

// Pre FX 1, A | B: the other one swaps (as holding footswitch 3); the running one - or B while there is no second
// effect - chooses the second effect.
static void on_ab(lv_event_t *e)
{
    int seg = (int)(intptr_t)lv_event_get_user_data(e);
    int running = s_view.pre1_b_type && s_view.pre1_active ? 1 : 0;
    if (s_view.pre1_b_type && seg != running) send('w', UI_SWITCH_HOLD | 3);
    else open_panel(PANEL_SECOND, 0);
}

static void on_panel_close(lv_event_t *e) { close_panel(); }

static bool scene_panel_open(void)
{
    return s_panel_mode == PANEL_SCENE && !lv_obj_is_hidden(s_panel);
}

static void set_param_norm(int idx, float n);

static void set_enum(int idx, int choice)
{
    const nano_param_t *p = &s_editor_model_def->params[idx];
    if (choice == option_index(p, s_param_norm[idx])) return;
    float n = p->option_count > 1 ? (float)choice / (p->option_count - 1) : 0;
    set_param_norm(idx, n);
    if (s_on_param) s_on_param(s_editor_slot_index, idx, n);
}

// A tap on an entry: data is the model (0 = none), the option or the scene (| 0x100: held).
static void panel_choice(intptr_t data)
{
    close_panel();
    if (s_panel_mode == PANEL_SCENE) {
        // The scene gets the effect's settings as they are now; held: it leaves the effect alone again.
        int scene = (int)data & 0xFF;
        bool carries = (s_view.scene_set[scene] >> s_editor_slot_index) & 1;
        if (!(data & 0x100) || carries) send('&', (int)data);
    } else if (s_panel_mode == PANEL_MODEL) {
        if ((uint32_t)data != s_editor_model_type) send('M', (int)data);
    } else if (s_panel_mode == PANEL_SECOND) {
        if ((uint32_t)data != s_view.pre1_b_type) send('A', 1 << 24 | (int)data);
    } else if (s_editor_model_def && s_panel_param < s_editor_model_def->param_count) {
        set_enum(s_panel_param, (int)data);
    }
}

static void draw_entry(lv_obj_t *obj, lv_layer_t *layer, intptr_t data)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t cy = (a.y1 + a.y2) / 2;
    bool pressed = lv_obj_has_state(obj, LV_STATE_PRESSED);
    if (s_panel_mode == PANEL_SCENE) {
        // A scene of the preset in its colour: filled = it carries settings for this effect.
        int scene = (int)data;
        uint32_t tone = BANK_COLORS[s_view.scene_colors[scene] < UI_BANK_COLOR_COUNT ? s_view.scene_colors[scene] : 0];
        bool carries = (s_view.scene_set[scene] >> s_editor_slot_index) & 1, now = scene == s_view.scene_active;
        if (carries) draw_frame(layer, &a, tone, tone, 10);
        else {
            uint32_t dim = blend(tone, PANEL_BG, ED_DIM);
            draw_frame(layer, &a, pressed ? 0x23272B : NO_FILL, dim, 10);
            draw_top(layer, &a, 10, 7, dim);
        }
        uint32_t ink = carries ? ink_for(tone) : TEXT;
        draw_text(layer, s_view.scene_names[scene], true, &ui_font_20, ink, a.x1 + 16, cy - 13, a.x2 - 150, LV_TEXT_ALIGN_LEFT);
        const char *state = carries ? (now ? "ON NOW  \xC2\xB7  SAVED" : "SAVED") : now ? "ON NOW" : "";
        draw_text(layer, state, false, &ui_font_12, carries ? ink : MUTED, a.x2 - 150, cy - 7, a.x2 - 14, LV_TEXT_ALIGN_RIGHT);
        return;
    }
    bool option = s_panel_mode == PANEL_OPTION;
    const nano_fx_model_t *m = option || !data ? NULL : nano_fx_model((uint32_t)data);
    uint32_t color = option ? s_editor_color : m ? m->color : 0x9AA0A6;
    bool current = data == s_panel_current;
    // The current one filled in its colour, the others framed in the dimmed colour.
    if (current) draw_frame(layer, &a, color, color, 10);
    else {
        // Only the frame: the panel is behind it (a fill of every entry costs as much as the panel's own).
        uint32_t dim = blend(color, PANEL_BG, ED_DIM);
        draw_frame(layer, &a, pressed ? 0x23272B : NO_FILL, dim, 10);
        draw_top(layer, &a, 10, 7, dim);   // its colour along the top
    }
    uint32_t ink = current ? ink_for(color) : TEXT;
    if (option) {
        char text[24];
        option_text(&s_editor_model_def->params[s_panel_param], (int)data, text, sizeof(text));
        draw_text(layer, text, true, &ui_font_20, ink, a.x1 + 4, cy - 13, a.x2 - 4, LV_TEXT_ALIGN_CENTER);
        return;
    }
    int32_t x = a.x1 + 16;
    if (m && NANO_ICONS[m->icon]) {   // the symbol at its own size (no scaling while the list scrolls)
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = NANO_ICONS[m->icon];
        d.recolor = lv_color_hex(current ? ink : color);
        d.recolor_opa = LV_OPA_COVER;
        lv_area_t ia = { a.x1 + 6, cy - 18, a.x1 + 41, cy + 17 };
        lv_draw_image(layer, &d, &ia);
        x = a.x1 + 48;
    }
    draw_text(layer, m ? m->name : "None", false, &ui_font_16, ink, x, cy - 11, a.x2 - 10, LV_TEXT_ALIGN_LEFT);
}

static void on_entry_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    intptr_t data = (intptr_t)lv_event_get_user_data(e);
    if (code == LV_EVENT_DRAW_MAIN_END) draw_entry(obj, lv_event_get_layer(e), data);
    else if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_invalidate(obj);
    else if (s_panel_mode == PANEL_SCENE) {   // a scene: tap = save for it, hold = remove
        if (code == LV_EVENT_SHORT_CLICKED) panel_choice(data);
        else if (code == LV_EVENT_LONG_PRESSED) panel_choice(data | 0x100);
    }
    else if (code == LV_EVENT_CLICKED) panel_choice(data);
}

static void panel_entry(int w, intptr_t data)
{
    lv_obj_t *obj = lv_obj_create(s_panel_list);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, ED_ENTRY_H);
    lv_obj_set_clickable(obj, true);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_user_data(obj, (void *)data);
    lv_obj_add_event_cb(obj, on_entry_event, LV_EVENT_ALL, (void *)data);
}

static void panel_heading(const char *text)
{
    lv_obj_t *l = label(s_panel_list, &ui_font_12, MUTED);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    lv_obj_set_style_pad_left(l, 4, 0);
}

static const char *model_group(int icon)
{
    switch (icon) {
    case NANO_ICON_OVERDRIVE: case NANO_ICON_BASS_OVERDRIVE: case NANO_ICON_FUZZ: return "DRIVE";
    case NANO_ICON_GATE: case NANO_ICON_EQUALIZER: case NANO_ICON_UTILITY: return "EQ & UTILITY";
    case NANO_ICON_WAH: case NANO_ICON_FILTER: return "WAH & FILTER";
    case NANO_ICON_COMPRESSOR: return "COMPRESSOR";
    case NANO_ICON_PITCH: return "PITCH";
    case NANO_ICON_DOUBLER: return "DOUBLER";
    case NANO_ICON_MODULATION: return "MODULATION";
    case NANO_ICON_DELAY: return "DELAY";
    case NANO_ICON_REVERB: return "REVERB";
    default: return "OTHER";
    }
}

static void open_panel(int mode, int param)
{
    const nano_fx_model_t *cur = s_editor_model_def;
    if (mode == PANEL_OPTION && (!cur || param >= cur->param_count)) return;
    int slot = mode == PANEL_SECOND ? 0 : s_editor_slot_index;
    char sub[120];
    // The model lists stay built while the editor shows this slot; an option list is built each time (it is short).
    if (mode == PANEL_OPTION || mode == PANEL_SCENE || mode != s_panel_built_mode || slot != s_panel_built_slot) {
        lv_async_call_cancel(panel_clean_later, NULL);
        lv_obj_clean(s_panel_list);
        s_panel_built_mode = mode;
        s_panel_built_slot = slot;
        s_panel_mode = mode;
        s_panel_param = param;
        if (mode == PANEL_OPTION) {
            for (int i = 0; i < cur->params[param].option_count; i++) panel_entry(ED_OPTION_W, i);
        } else if (mode == PANEL_SCENE) {
            for (int i = 0; i < UI_SCENES; i++) if (s_view.scene_names[i][0]) panel_entry(ED_ENTRY_W, i);
        } else {
            if (mode == PANEL_SECOND) panel_entry(ED_ENTRY_W, 0);   // None
            const char *group = NULL;
            for (int i = 0; i < NANO_SLOT_MODEL_COUNT[slot]; i++) {
                const nano_fx_model_t *m = nano_fx_model(NANO_SLOT_MODELS[slot][i]);
                if (!m) continue;
                const char *g = model_group(m->icon);
                if (!group || strcmp(g, group)) panel_heading(group = g);
                panel_entry(ED_ENTRY_W, (intptr_t)m->type);
            }
        }
    }
    s_panel_mode = mode;
    s_panel_param = param;
    if (mode == PANEL_OPTION) {
        const nano_param_t *p = &cur->params[param];
        lv_label_set_text(s_panel_title, p->name);
        snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", cur->name, NANO_FX_SLOT_NAMES[s_editor_slot_index]);
        s_panel_current = option_index(p, s_param_norm[param]);
    } else if (mode == PANEL_SCENE) {
        lv_label_set_text(s_panel_title, "Settings for a scene");
        snprintf(sub, sizeof(sub), "Tap a scene: it sets %.24s as it is now  \xC2\xB7  hold: remove", cur ? cur->name : "this effect");
        s_panel_current = -1;
    } else if (mode == PANEL_SECOND) {
        lv_label_set_text(s_panel_title, "Second effect (B)");
        snprintf(sub, sizeof(sub), "%s  \xC2\xB7  hold footswitch 3 to swap A and B", NANO_FX_SLOT_NAMES[0]);
        s_panel_current = (intptr_t)s_view.pre1_b_type;
    } else {
        lv_label_set_text(s_panel_title, "Choose a model");
        snprintf(sub, sizeof(sub), "%s", NANO_FX_SLOT_NAMES[slot]);
        s_panel_current = (intptr_t)s_editor_model_type;
    }
    lv_label_set_text(s_panel_sub, sub);
    lv_obj_invalidate(s_panel_list);
    set_hidden(s_panel_scrim, false);
    set_hidden(s_panel, false);
    lv_obj_update_layout(s_panel_list);
    lv_obj_scroll_to_y(s_panel_list, 0, LV_ANIM_OFF);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_panel_list); i++) {
        lv_obj_t *c = lv_obj_get_child(s_panel_list, (int32_t)i);
        if (lv_obj_get_user_data(c) == (void *)s_panel_current && lv_obj_is_clickable(c)) {
            lv_obj_scroll_to_view(c, LV_ANIM_OFF);
            break;
        }
    }
}

// ---- Parameter bars ----

// Value and its text (the number of a range, the option of a long enum) for drawing.
static void set_param_norm(int idx, float n)
{
    const nano_param_t *p = &s_editor_model_def->params[idx];
    s_param_norm[idx] = n;
    if (n < 0) strlcpy(s_param_text[idx], "\xE2\x80\x93", sizeof(s_param_text[idx]));   // en dash: not known
    else if (p->kind == NANO_PARAM_RANGE)
        snprintf(s_param_text[idx], sizeof(s_param_text[idx]), "%.*f", p->decimals < 0 ? 1 : p->decimals, (double)param_from_normalized(p, n));
    else option_text(p, option_index(p, n), s_param_text[idx], sizeof(s_param_text[idx]));
    if (s_param_bar[idx]) lv_obj_invalidate(s_param_bar[idx]);
}

// Segmented switch of an enum with up to three options, at the right end of its bar.
static void enum_switch_area(const lv_area_t *bar, int count, lv_area_t *out)
{
    int32_t cy = (bar->y1 + bar->y2) / 2;
    out->x2 = bar->x2 - 7;
    out->x1 = out->x2 - (count == 2 ? 180 : 246) + 1;
    out->y1 = cy - 25;
    out->y2 = cy + 24;
}

static void draw_bar(lv_obj_t *bar, int idx, lv_layer_t *layer)
{
    const nano_param_t *p = &s_editor_model_def->params[idx];
    lv_area_t a;
    lv_obj_get_coords(bar, &a);
    int32_t cy = (a.y1 + a.y2) / 2;
    float n = s_param_norm[idx];
    bool known = n >= 0;
    bool touched = lv_obj_has_state(bar, LV_STATE_PRESSED);
    uint32_t color = s_editor_color;
    lv_draw_rect_dsc_t r;

    // Frame in the dimmed effect colour (brighter while touched), grey while the value is not known.
    // No fill of its own: the editor's black is behind it (a second fill of every bar costs as much as the first).
    draw_frame(layer, &a, NO_FILL, known ? blend(color, TILE_OFF_BG, touched ? 0.8f : ED_DIM) : ED_NEUTRAL, 12);

    if (p->kind == NANO_PARAM_RANGE && known) {
        // Fill inside the frame up to the value, cut straight there; a bright line marks the value.
        lv_area_t in = { a.x1 + 2, a.y1 + 2, a.x2 - 2, a.y2 - 2 };
        int32_t x = in.x1 + (int32_t)lroundf(n * (lv_area_get_width(&in) - 1));
        lv_area_t old = layer->_clip_area;
        lv_area_t clip = { LV_MAX(in.x1, old.x1), LV_MAX(in.y1, old.y1), LV_MIN(x, old.x2), LV_MIN(in.y2, old.y2) };
        if (clip.x1 <= clip.x2 && clip.y1 <= clip.y2) {
            layer->_clip_area = clip;
            lv_draw_rect_dsc_init(&r);
            r.bg_color = lv_color_hex(blend(color, TILE_OFF_BG, 0.30f));
            r.radius = 10;
            lv_draw_rect(layer, &r, &in);
            layer->_clip_area = old;
        }
        lv_area_t glow = { x - 5, a.y1 + 7, x + 4, a.y2 - 7 }, edge = { x - 2, a.y1 + 9, x + 1, a.y2 - 9 };
        int32_t shift = edge.x1 < a.x1 + 4 ? a.x1 + 4 - edge.x1 : edge.x2 > a.x2 - 4 ? a.x2 - 4 - edge.x2 : 0;
        lv_area_move(&glow, shift, 0);
        lv_area_move(&edge, shift, 0);
        lv_draw_rect_dsc_init(&r);   // a soft glow without a shadow (shadows are slow to draw)
        r.bg_color = lv_color_hex(color);
        r.bg_opa = LV_OPA_30;
        r.radius = 5;
        lv_draw_rect(layer, &r, &glow);
        r.bg_opa = LV_OPA_COVER;
        r.radius = 2;
        lv_draw_rect(layer, &r, &edge);
    }

    int32_t name_end = a.x1 + 220;
    lv_area_t sw;
    bool segmented = p->kind == NANO_PARAM_ENUM && p->option_count <= 3;
    if (segmented) {
        enum_switch_area(&a, p->option_count, &sw);
        name_end = sw.x1 - 8;
    }
    draw_text(layer, p->name, false, &ui_font_16, known ? 0xE6E8EA : 0x6B7076, a.x1 + 16, cy - 11, name_end, LV_TEXT_ALIGN_LEFT);

    if (p->kind == NANO_PARAM_RANGE) {
        // Value with the unit small next to it (both on one baseline)
        int32_t right = a.x2 - 16;
        if (known && s_param_unit_w[idx]) {
            draw_text(layer, p->unit, false, &ui_font_14, MUTED, right - s_param_unit_w[idx] - 2, cy - 7, right, LV_TEXT_ALIGN_RIGHT);
            right -= s_param_unit_w[idx] + 3;
        }
        draw_text(layer, s_param_text[idx], false, &ui_font_title_22, known ? TEXT : 0x5D6267, right - 150, cy - 15, right,
                  LV_TEXT_ALIGN_RIGHT);
        return;
    }
    if (p->option_count > 3) {   // the option and a chevron: a tap opens the list
        draw_text(layer, s_param_text[idx], false, &ui_font_20, known ? TEXT : 0x5D6267, a.x2 - 200, cy - 13, a.x2 - 40,
                  LV_TEXT_ALIGN_RIGHT);
        draw_text(layer, LV_SYMBOL_DOWN, false, &ui_font_16, MUTED, a.x2 - 36, cy - 11, a.x2 - 16, LV_TEXT_ALIGN_RIGHT);
        return;
    }
    // Up to three options: the chosen one filled in the effect colour.
    int current = option_index(p, n);
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(0x0B0C0E);
    r.border_width = 1;
    r.border_color = lv_color_hex(0x2A2E33);
    r.radius = 10;
    lv_draw_rect(layer, &r, &sw);
    int32_t seg_w = (lv_area_get_width(&sw) - 6) / p->option_count;
    char text[24];
    for (int i = 0; i < p->option_count; i++) {
        lv_area_t s = { sw.x1 + 3 + i * seg_w, sw.y1 + 3, sw.x1 + 3 + (i + 1) * seg_w - 1, sw.y2 - 3 };
        if (i == current) {
            lv_draw_rect_dsc_init(&r);
            r.bg_color = lv_color_hex(color);
            r.radius = 8;
            lv_draw_rect(layer, &r, &s);
        }
        option_text(p, i, text, sizeof(text));
        draw_text(layer, text, true, &ui_font_16, i == current ? ink_for(color) : known ? 0xC8CCD1 : 0x5D6267, s.x1, cy - 11,
                  s.x2, LV_TEXT_ALIGN_CENTER);
    }
}

static void set_bar_value(int idx, float n)
{
    const nano_param_t *p = &s_editor_model_def->params[idx];
    float v = param_from_normalized(p, n < 0 ? 0 : n > 1 ? 1 : n);   // snap to the parameter's step
    n = p->max > p->min ? (v - p->min) / (p->max - p->min) : 0;
    if (fabsf(n - s_param_norm[idx]) < 0.0001f) return;
    set_param_norm(idx, n);
    if (s_on_param) s_on_param(s_editor_slot_index, idx, n);
}

static void stop_drag(void)
{
    if (s_dragging && s_drag_bar) lv_obj_set_scroll_chain_ver(s_drag_bar, true);
    s_dragging = false;
    s_drag_bar = NULL;
}

static void on_bar_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *bar = lv_event_get_current_target_obj(e);
    if (!s_editor_model_def || idx >= s_editor_model_def->param_count) return;
    if (code == LV_EVENT_DRAW_MAIN_END) {
        draw_bar(bar, idx, lv_event_get_layer(e));
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST
        && code != LV_EVENT_CLICKED) return;
    const nano_param_t *p = &s_editor_model_def->params[idx];
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        stop_drag();
        lv_obj_invalidate(bar);   // frame back to dimmed
        return;
    }
    if (s_param_norm[idx] < 0) return;   // not known (effect off, still reading)
    if (p->kind == NANO_PARAM_ENUM) {
        if (code != LV_EVENT_CLICKED) return;
        if (p->option_count > 3) {
            open_panel(PANEL_OPTION, idx);
            return;
        }
        // On the switch: that option; elsewhere on the bar: the next one.
        lv_area_t a, sw;
        lv_obj_get_coords(bar, &a);
        enum_switch_area(&a, p->option_count, &sw);
        int choice = (option_index(p, s_param_norm[idx]) + 1) % p->option_count;
        if (point.x >= sw.x1 && point.x <= sw.x2) choice = (point.x - sw.x1) * p->option_count / lv_area_get_width(&sw);
        set_enum(idx, choice < 0 ? 0 : choice >= p->option_count ? p->option_count - 1 : choice);
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        s_drag_x = point.x;
        s_dragging = false;
        lv_obj_invalidate(bar);   // brighter frame
    } else if (code == LV_EVENT_PRESSING) {
        if (!s_dragging) {
            if (LV_ABS(point.x - s_drag_x) < ED_DRAG_START) return;
            s_dragging = true;
            s_drag_bar = bar;
            s_drag_param = idx;
            s_drag_x = point.x;
            s_drag_from = s_param_norm[idx];
            lv_obj_set_scroll_chain_ver(bar, false);   // the list stays put while a value moves
            return;
        }
        set_bar_value(idx, s_drag_from + (float)(point.x - s_drag_x) / ED_BAR_W);
    }
}

// One bar per parameter, two columns, in the model's display order.
static void build_param_rows(void)
{
    stop_drag();
    lv_obj_clean(s_editor_body);
    memset(s_param_bar, 0, sizeof(s_param_bar));
    const nano_fx_model_t *m = s_editor_model_def;
    for (int i = 0; m && i < m->param_count && i < NANO_MAX_PARAMS; i++) {
        lv_point_t size = { 0, 0 };
        const char *unit = m->params[i].unit;
        if (m->params[i].kind == NANO_PARAM_RANGE && unit && unit[0])
            lv_text_get_size(&size, unit, &ui_font_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        s_param_unit_w[i] = (int16_t)size.x;
        set_param_norm(i, -1);
    }
    if (!m) return;
    int pos = 0;
    for (int row = 0; row < m->param_count; row++) {
        int idx = m->order ? m->order[row] : row;
        if (idx >= m->param_count || idx >= NANO_MAX_PARAMS) continue;
        lv_obj_t *bar = lv_obj_create(s_editor_body);
        lv_obj_remove_style_all(bar);   // drawn in on_bar_event
        lv_obj_set_size(bar, ED_BAR_W, ED_BAR_H);
        lv_obj_set_pos(bar, 8 + (pos % 2) * (ED_BAR_W + 8), (pos / 2) * ED_BAR_PITCH);
        lv_obj_set_scrollable(bar, false);
        lv_obj_set_scroll_on_focus(bar, false);
        lv_obj_set_scroll_chain_hor(bar, false);   // sideways is the bar's own (else the screen would scroll)
        lv_obj_set_clickable(bar, true);
        lv_obj_add_event_cb(bar, on_bar_event, LV_EVENT_ALL, (void *)(intptr_t)idx);
        s_param_bar[idx] = bar;
        pos++;
    }
}

// ---- Building the editor ----

// A field that is framed in the dimmed colour when not active and filled when active (FX presets, A | B).
// Not active: the frame's colour as a strip along the top (the filled and the empty ones have none).
static void on_frame_button_draw(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    uint32_t border = lv_color_to_u32(lv_obj_get_style_border_color(btn, 0)) & 0xFFFFFF;
    uint32_t bg = lv_color_to_u32(lv_obj_get_style_bg_color(btn, 0)) & 0xFFFFFF;
    if (border == bg || border == ED_NEUTRAL) return;
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    draw_top(lv_event_get_layer(e), &a, 10, 7, border);
}

static lv_obj_t *frame_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_event_cb(btn, on_frame_button_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(TILE_OFF_BG), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_opa(btn, 170, LV_STATE_PRESSED);
    return btn;
}

static void style_frame_button(lv_obj_t *btn, uint32_t color, bool active, bool empty)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? color : TILE_OFF_BG), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(active ? color : empty ? ED_NEUTRAL : blend(color, TILE_OFF_BG, ED_DIM)), 0);
}

static void build_editor(lv_obj_t *screen)
{
    s_editor = lv_obj_create(screen);
    lv_obj_set_size(s_editor, 800, 480);
    lv_obj_set_pos(s_editor, 0, 0);
    lv_obj_set_style_bg_color(s_editor, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_editor, 0, 0);
    lv_obj_set_style_radius(s_editor, 0, 0);
    lv_obj_set_style_pad_all(s_editor, 0, 0);
    lv_obj_set_scrollable(s_editor, false);

    lv_obj_t *back = button(s_editor, 8, 8, 60, 60, on_editor_back);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_center(ui_icon(back, UI_ICON_BACK, 30, TEXT));

    // Header card (tap: choose a model)
    s_editor_card = button(s_editor, 76, 8, 596, 60, on_model_button);
    lv_obj_set_style_radius(s_editor_card, 12, 0);
    lv_obj_set_style_bg_color(s_editor_card, lv_color_hex(CARD_TOP), 0);
    lv_obj_set_style_bg_grad_color(s_editor_card, lv_color_hex(0x0E0F11), 0);
    lv_obj_set_style_border_color(s_editor_card, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_style_pad_all(s_editor_card, 0, 0);
    s_editor_chip = lv_obj_create(s_editor_card);
    lv_obj_remove_style_all(s_editor_chip);
    lv_obj_set_size(s_editor_chip, 44, 44);
    lv_obj_set_style_radius(s_editor_chip, 11, 0);
    lv_obj_align(s_editor_chip, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_set_clickable(s_editor_chip, false);
    s_editor_icon = lv_image_create(s_editor_chip);   // at its own size (36 px)
    lv_obj_set_style_image_recolor_opa(s_editor_icon, LV_OPA_COVER, 0);
    lv_obj_center(s_editor_icon);
    s_editor_slot = label(s_editor_card, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(s_editor_slot, 2, 0);
    lv_obj_set_pos(s_editor_slot, 64, 7);
    s_editor_model = label(s_editor_card, &ui_font_title_22, TEXT);
    lv_label_set_long_mode(s_editor_model, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_editor_model, 64, 22);
    s_editor_pedal = lv_image_create(s_editor_card);
    lv_obj_set_style_image_recolor_opa(s_editor_pedal, LV_OPA_COVER, 0);
    lv_obj_align(s_editor_pedal, LV_ALIGN_RIGHT_MID, -42, 0);
    lv_obj_set_hidden(s_editor_pedal, true);
    lv_obj_align(ui_icon(s_editor_card, UI_ICON_CHEVRON_DOWN, 22, MUTED), LV_ALIGN_RIGHT_MID, -12, 0);

    // Pre FX 1: A | B
    s_editor_ab = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_editor_ab);
    lv_obj_set_pos(s_editor_ab, 556, 8);
    lv_obj_set_size(s_editor_ab, 118, 60);
    lv_obj_set_scrollable(s_editor_ab, false);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *seg = frame_button(s_editor_ab);
        lv_obj_set_pos(seg, i * 62, 0);
        lv_obj_set_size(seg, 56, 60);
        lv_obj_add_event_cb(seg, on_ab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(label(seg, &ui_font_20, TEXT));
        s_editor_ab_seg[i] = seg;
    }
    lv_obj_set_hidden(s_editor_ab, true);

    // On / off: a large switch in the effect colour; the area around it is the button.
    s_editor_onoff = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_editor_onoff);
    lv_obj_set_pos(s_editor_onoff, 680, 0);
    lv_obj_set_size(s_editor_onoff, 120, 76);
    lv_obj_set_style_opa(s_editor_onoff, 180, LV_STATE_PRESSED);
    lv_obj_set_clickable(s_editor_onoff, true);
    lv_obj_add_event_cb(s_editor_onoff, on_editor_onoff, LV_EVENT_CLICKED, NULL);
    s_editor_switch = lv_switch_create(s_editor_onoff);
    lv_obj_set_clickable(s_editor_switch, false);   // the state comes from the Nano (ui_fx_editor_show)
    lv_obj_set_size(s_editor_switch, 96, 52);
    lv_obj_align(s_editor_switch, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(0x2A2D30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(INK), LV_PART_KNOB | LV_STATE_CHECKED);   // as in the editor
    lv_obj_set_style_pad_all(s_editor_switch, -5, LV_PART_KNOB);

    // FX presets: Original and the stored places of this model (tap = load, hold = save under a name).
    s_fxp_bar = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_fxp_bar);
    lv_obj_set_pos(s_fxp_bar, 8, 76);
    lv_obj_set_size(s_fxp_bar, 784, 56);
    lv_obj_set_style_pad_column(s_fxp_bar, 8, 0);
    lv_obj_set_flex_flow(s_fxp_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(s_fxp_bar, false);
    for (int i = 0; i <= UI_FX_PRESETS; i++) {
        lv_obj_t *chip = frame_button(s_fxp_bar);
        lv_obj_set_height(chip, LV_PCT(100));
        lv_obj_set_flex_grow(chip, 1);
        lv_obj_add_event_cb(chip, on_fxp_chip, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        if (i) lv_obj_add_event_cb(chip, on_fxp_chip_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *l = label(chip, &ui_font_16, TEXT);
        lv_obj_set_width(l, 120);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        if (i) lv_obj_center(ui_icon(chip, UI_ICON_PLUS, 20, 0x5D6267));   // empty place
        s_fxp_chip[i] = chip;
    }
    // At the end of the row: these settings for a scene of the preset (and how many scenes carry some).
    s_fxp_scene = frame_button(s_fxp_bar);
    lv_obj_set_size(s_fxp_scene, 84, LV_PCT(100));
    lv_obj_add_event_cb(s_fxp_scene, on_scene_chip, LV_EVENT_CLICKED, NULL);
    s_fxp_scene_icon = ui_icon(s_fxp_scene, UI_ICON_SCENES, 26, TEXT);
    lv_obj_center(s_fxp_scene_icon);
    s_fxp_scene_count = label(s_fxp_scene, &ui_font_16, TEXT);
    lv_obj_align(s_fxp_scene_count, LV_ALIGN_RIGHT_MID, -12, 0);

    // Instead of the presets: why there is nothing to edit (effect off, reading, empty slot).
    s_editor_info = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_editor_info);
    lv_obj_set_pos(s_editor_info, 8, 76);
    lv_obj_set_size(s_editor_info, 784, 56);
    lv_obj_set_style_border_width(s_editor_info, 1, 0);
    lv_obj_set_style_border_color(s_editor_info, lv_color_hex(0x3A3F45), 0);
    lv_obj_set_style_radius(s_editor_info, 12, 0);
    lv_obj_set_clickable(s_editor_info, false);
    lv_obj_set_scrollable(s_editor_info, false);
    lv_obj_t *dot = lv_obj_create(s_editor_info);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 9, 9);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(MUTED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 16, 0);
    s_editor_info_text = label(s_editor_info, &ui_font_16, 0xC8CCD1);
    lv_obj_align(s_editor_info_text, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_set_hidden(s_editor_info, true);

    s_editor_body = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_editor_body);
    lv_obj_set_pos(s_editor_body, 0, ED_BODY_Y);
    lv_obj_set_size(s_editor_body, 800, 480 - ED_BODY_Y);
    lv_obj_set_style_pad_bottom(s_editor_body, 8, 0);
    lv_obj_set_scroll_dir(s_editor_body, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_editor_body, lv_color_hex(0x3A3F45), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_editor_body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_editor_body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_editor_body, 2, LV_PART_SCROLLBAR);

    // Model / option panel. Below the header the editor is covered in plain black (no see-through layer to blend);
    // a tap there closes the panel.
    s_panel_scrim = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_panel_scrim);
    lv_obj_set_pos(s_panel_scrim, 0, 74);
    lv_obj_set_size(s_panel_scrim, 800, 480 - 74);
    lv_obj_set_style_bg_color(s_panel_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel_scrim, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_panel_scrim, true);
    lv_obj_add_event_cb(s_panel_scrim, on_panel_close, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_panel_scrim, true);
    s_panel = panel(s_editor, 76, 76, ED_PANEL_W, ED_PANEL_H);
    lv_obj_set_style_shadow_width(s_panel, 0, 0);   // nothing behind it to lift it from, and shadows are slow
    s_panel_title = dialog_title(s_panel, "");
    s_panel_sub = label(s_panel, &ui_font_14, 0x9AA0A6);
    lv_obj_set_pos(s_panel_sub, 20, 46);
    close_button(s_panel, ED_PANEL_W - 60, 12, on_panel_close, 0);
    s_panel_list = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_panel_list);
    // 6 px inside the panel, clear of its round corners: the panel then covers the list's whole area, so a scroll
    // step fills that area once (panel) instead of twice (black underneath, then the panel) - filling is what
    // takes the time on this display.
    lv_obj_set_pos(s_panel_list, 6, 74);
    lv_obj_set_size(s_panel_list, ED_PANEL_W - 2 - 12, ED_PANEL_H - 2 - 74 - 6);
    lv_obj_set_style_pad_hor(s_panel_list, 6, 0);
    lv_obj_set_style_pad_bottom(s_panel_list, 6, 0);
    lv_obj_set_style_pad_row(s_panel_list, 6, 0);
    lv_obj_set_style_pad_column(s_panel_list, 6, 0);
    lv_obj_set_flex_flow(s_panel_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(s_panel_list, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_panel_list, lv_color_hex(0x3A3F45), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_panel_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_panel_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_hidden(s_panel, true);

    lv_obj_set_hidden(s_editor, true);
}

// ---- FX presets (bar above the parameters) ----

// The scene button at the end of the FX preset row: filled while the scene that is on carries settings for this
// effect, with the number of scenes that do; grey while the preset has no scenes.
static void style_scene_chip(void)
{
    static int shown = -1;
    int scenes = 0, carrying = 0, slot = s_editor_slot_index;
    if (slot < 0) return;
    for (int i = 0; i < UI_SCENES; i++) {
        scenes += s_view.scene_names[i][0] != 0;
        carrying += s_view.scene_names[i][0] && ((s_view.scene_set[i] >> slot) & 1);
    }
    bool active = s_view.scene_active >= 0 && ((s_view.scene_set[s_view.scene_active] >> slot) & 1);
    int state = (int)(s_editor_color << 8) | carrying << 2 | (scenes ? 2 : 0) | active;
    if (state == shown) return;   // (this runs with every state update of the Nano)
    shown = state;
    uint32_t ink = active ? ink_for(s_editor_color) : scenes ? TEXT : 0x5D6267;
    style_frame_button(s_fxp_scene, s_editor_color, active, !scenes);
    lv_obj_set_style_image_recolor(s_fxp_scene_icon, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(s_fxp_scene_count, lv_color_hex(ink), 0);
    if (carrying) lv_label_set_text_fmt(s_fxp_scene_count, "%d", carrying);
    else lv_label_set_text(s_fxp_scene_count, "");
    lv_obj_align(s_fxp_scene_icon, carrying ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, carrying ? 14 : 0, 0);
}

static void on_scene_chip(lv_event_t *e)
{
    for (int i = 0; i < UI_SCENES; i++) {
        if (!s_view.scene_names[i][0]) continue;
        open_panel(PANEL_SCENE, 0);
        return;
    }
    ui_show_message("This preset has no scenes yet - tap the scenes button (top right of the main screen) and hold a tile.");
}

static void style_fxp_chips(void)
{
    // Only when something changed (this runs with every state update of the Nano).
    static uint32_t shown_color;
    static char shown_names[UI_FX_PRESETS][16];
    static int shown_active = -2;
    static bool shown_hidden;
    bool hidden = !s_fxp_usable || !lv_obj_is_hidden(s_editor_info);   // the info line is there then
    set_hidden(s_fxp_bar, hidden);
    style_scene_chip();
    if (hidden == shown_hidden && shown_active == s_fxp_active && shown_color == s_editor_color
        && !memcmp(shown_names, s_fxp_names, sizeof(shown_names))) return;
    shown_hidden = hidden;
    shown_active = s_fxp_active;
    shown_color = s_editor_color;
    memcpy(shown_names, s_fxp_names, sizeof(shown_names));

    uint32_t color = s_editor_color;
    for (int i = 0; i <= UI_FX_PRESETS; i++) {
        lv_obj_t *chip = s_fxp_chip[i];
        bool filled = i == 0 || s_fxp_names[i - 1][0];
        bool active = i == s_fxp_active && filled;
        lv_obj_t *l = lv_obj_get_child(chip, 0);
        const char *text = i == 0 ? "Original" : filled ? s_fxp_names[i - 1] : "";
        set_text(l, text);
        // The large font if the name fits, otherwise the small one (and dots if it is still too long).
        lv_point_t size;
        lv_text_get_size(&size, text, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_style_text_font(l, size.x <= 116 ? &ui_font_16 : &ui_font_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(active ? ink_for(color) : i == 0 ? 0xC8CCD1 : 0xE6E8EA), 0);
        style_frame_button(chip, color, active, !filled);
        if (i) set_hidden(lv_obj_get_child(chip, 1), filled);
    }
}

void ui_fx_presets_show(const char names[][16], int active, bool usable)
{
    lvgl_port_lock(0);
    memcpy(s_fxp_names, names, sizeof(s_fxp_names));
    s_fxp_active = active;
    s_fxp_usable = usable;
    style_fxp_chips();
    lvgl_port_unlock();
}

static void on_fxp_chip(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i && !s_fxp_names[i - 1][0]) {
        ui_show_message("Empty - hold to save the current settings here.");
        return;
    }
    send('z', i);
}

// Hold: save the current values here under a name (the keyboard opens).
static void on_fxp_chip_long(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char title[48], name[16];
    snprintf(title, sizeof(title), "FX preset %d  \xC2\xB7  %.24s", i, s_editor_model_def ? s_editor_model_def->name : "");
    if (s_fxp_names[i - 1][0]) strlcpy(name, s_fxp_names[i - 1], sizeof(name));
    else snprintf(name, sizeof(name), "Preset %d", i);
    show_rename((char)('0' + i), title, "Save", name, 15);
}

// Header: symbol chip, slot, model, pedal picture, A | B, switch.
static void show_editor_header(int slot, const nano_fx_model_t *m, bool on)
{
    uint32_t color = s_editor_color;
    bool lit = on && m;   // the chip: filled in the effect colour when on, outlined when off (as the tiles)
    lv_obj_set_style_bg_color(s_editor_chip, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_editor_chip, lit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_editor_chip, lit ? 0 : 2, 0);
    lv_obj_set_style_border_color(s_editor_chip, lv_color_hex(blend(color, TILE_OFF_BG, 0.62f)), 0);
    set_hidden(s_editor_icon, !m || !NANO_ICONS[m->icon]);
    if (m && NANO_ICONS[m->icon]) {
        lv_image_set_src(s_editor_icon, NANO_ICONS[m->icon]);
        lv_obj_set_style_image_recolor(s_editor_icon, lv_color_hex(lit ? ink_for(color) : color), 0);
    }
    char caption[32];
    size_t n = 0;
    for (const char *c = NANO_FX_SLOT_NAMES[slot]; *c && n + 1 < sizeof(caption); c++) caption[n++] = (char)toupper((unsigned char)*c);
    caption[n] = 0;
    set_text(s_editor_slot, caption);
    set_text(s_editor_model, m ? m->name : "Choose a model");
    const lv_image_dsc_t *pedal = m ? nano_fx_pedal(m->type) : NULL;
    set_hidden(s_editor_pedal, pedal == NULL);
    if (pedal) {
        lv_image_set_src(s_editor_pedal, pedal);
        lv_obj_set_style_image_recolor(s_editor_pedal, lv_color_hex(color), 0);
        lv_obj_set_style_image_opa(s_editor_pedal, on ? LV_OPA_90 : LV_OPA_40, 0);
    }
    // Pre FX 1: A | B takes the right end of the header card.
    bool ab = slot == 0;
    int card_w = ab ? 472 : 596;
    lv_obj_set_width(s_editor_card, card_w);
    lv_obj_set_width(s_editor_model, card_w - 64 - (pedal ? 116 : 50));
    set_hidden(s_editor_ab, !ab);
    if (ab) {
        int running = s_view.pre1_b_type && s_view.pre1_active ? 1 : 0;
        for (int i = 0; i < 2; i++) {
            lv_obj_t *seg = s_editor_ab_seg[i];
            bool none = i == 1 && !s_view.pre1_b_type;
            set_text(lv_obj_get_child(seg, 0), i == 0 ? "A" : none ? "+B" : "B");
            style_frame_button(seg, color, i == running, none);
            lv_obj_set_style_text_color(lv_obj_get_child(seg, 0), lv_color_hex(i == running ? ink_for(color) : none ? 0x8E9297 : TEXT), 0);
        }
    }
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(color), LV_PART_INDICATOR | LV_STATE_CHECKED);
}

void ui_fx_editor_show(int slot, uint32_t model, bool on, const float *values, int count)
{
    static struct { int slot; uint32_t model, b_type; bool on; int b_active; } shown = { .slot = -1 };
    lvgl_port_lock(0);
    bool rebuild = lv_obj_is_hidden(s_editor) || slot != s_editor_slot_index || model != s_editor_model_type;
    if (slot != s_editor_slot_index || lv_obj_is_hidden(s_editor)) drop_panel();   // the model list is per slot
    else if (rebuild) close_panel();
    s_editor_slot_index = slot;
    s_editor_model_type = model;
    s_editor_model_def = nano_fx_model(model);
    const nano_fx_model_t *m = s_editor_model_def;
    s_editor_color = m ? m->color : 0x5A5A5A;
    if (rebuild) build_param_rows();

    if (rebuild || shown.slot != slot || shown.model != model || shown.on != on || shown.b_type != s_view.pre1_b_type
        || shown.b_active != s_view.pre1_active) {
        show_editor_header(slot, m, on);
        shown.slot = slot;
        shown.model = model;
        shown.on = on;
        shown.b_type = s_view.pre1_b_type;
        shown.b_active = s_view.pre1_active;
    }
    if (on) lv_obj_add_state(s_editor_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_editor_switch, LV_STATE_CHECKED);

    bool known = values != NULL;
    const char *info = !m ? "This slot is empty. Choose a model above."
                     : !on ? "Effect is off \xE2\x80\x93 switch it on to read and edit its values"
                     : !known ? "Reading values..." : "";
    set_text(s_editor_info_text, info);
    set_hidden(s_editor_info, info[0] == 0);
    style_fxp_chips();

    for (int idx = 0; m && idx < m->param_count && idx < NANO_MAX_PARAMS; idx++) {
        if (s_dragging && idx == s_drag_param) continue;   // the finger has it
        float v = known && idx < count ? values[idx] : -1;
        v = v < 0 ? -1 : v > 1 ? 1 : v;
        if (v != s_param_norm[idx]) set_param_norm(idx, v);   // only what changed is drawn again
    }

    set_hidden(s_editor, false);
    update_main_hidden();
    lvgl_port_unlock();
}

void ui_fx_editor_close(void)
{
    lvgl_port_lock(0);
    drop_panel();
    stop_drag();
    lv_obj_set_hidden(s_editor, true);
    update_main_hidden();
    s_editor_slot_index = -1;
    lvgl_port_unlock();
}

// ---- scenes: name, colour and effects of a scene (long press on a tile in scene mode) ----
// A scene is which of the preset's five effects are on. The dialog starts from the stored scene - or, for an empty
// one, from what is on right now - and Save stores it on the controller and switches to it.

static void style_scene_editor(void)
{
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        bool selected = i == s_se_color;
        lv_obj_set_style_border_width(s_se_swatch[i], selected ? 4 : 1, 0);
        lv_obj_set_style_border_color(s_se_swatch[i], lv_color_hex(selected ? 0xFFFFFF : 0x3A3D40), 0);
    }
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {   // as everywhere: filled = on, framed in the dimmed colour = off
        const nano_fx_model_t *model = nano_fx_model(s_slot_type[slot]);
        bool on = model && ((s_se_fx_on >> slot) & 1);
        uint32_t color = model ? model->color : 0x6A6A6A;
        uint32_t ink = on ? ink_for(color) : model ? TEXT : 0x5D6267;
        style_frame_button(s_se_fx[slot], color, on, !model);
        lv_obj_set_style_text_color(s_se_fx_caption[slot], lv_color_hex(on ? ink : MUTED), 0);
        lv_obj_set_style_text_color(s_se_fx_state[slot], lv_color_hex(on ? ink : MUTED), 0);
        lv_label_set_text(s_se_fx_state[slot], !model ? "" : on ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_se_fx_name[slot], lv_color_hex(ink), 0);
        lv_label_set_text(s_se_fx_name[slot], model ? nano_fx_name(s_slot_type[slot]) : "Empty");
        bool carries = model && s_view.scene_names[s_se_scene][0] && ((s_view.scene_set[s_se_scene] >> slot) & 1);
        lv_obj_set_style_text_color(s_se_fx_set[slot], lv_color_hex(on ? ink : MUTED), 0);
        lv_label_set_text(s_se_fx_set[slot], carries ? "WITH SETTINGS" : "");
    }
    lv_label_set_text(s_se_name_label, s_se_name);
}

static void on_scene_swatch(lv_event_t *e)
{
    s_se_color = (int)(intptr_t)lv_event_get_user_data(e);
    style_scene_editor();
}

static void on_scene_fx(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (!nano_fx_model(s_slot_type[slot])) return;
    s_se_fx_on ^= (uint8_t)(1u << slot);
    style_scene_editor();
}

static void on_scene_name(lv_event_t *e)
{
    char title[40];
    snprintf(title, sizeof(title), "Name of scene %d", s_se_scene + 1);
    show_rename('S', title, "OK", s_se_name, UI_SCENE_NAME - 1);
}

// From the keyboard: the name stays in the dialog until Save.
static void scene_editor_named(const char *text)
{
    if (text[0]) strlcpy(s_se_name, text, sizeof(s_se_name));
    style_scene_editor();
}

static void open_scene_editor(int scene)
{
    static const uint8_t COLORS[UI_SCENES] = { 4, 3, 2, 1, 6, 7 };   // of a new scene: green, yellow, orange, red, blue, violet
    char text[128];
    if (scene < 0 || scene >= UI_SCENES) return;
    bool stored = s_view.scene_names[scene][0] != 0;
    s_se_scene = scene;
    s_se_preset = s_current_preset;
    s_se_color = !stored ? COLORS[scene] : s_view.scene_colors[scene] < UI_BANK_COLOR_COUNT ? s_view.scene_colors[scene] : 0;
    s_se_fx_on = stored ? s_view.scene_fx[scene] : 0;
    for (int slot = 0; slot < NANO_FX_SLOTS && !stored; slot++) s_se_fx_on |= (uint8_t)(s_slot_on[slot] << slot);
    if (stored) strlcpy(s_se_name, s_view.scene_names[scene], sizeof(s_se_name));
    else snprintf(s_se_name, sizeof(s_se_name), "Scene %d", scene + 1);
    snprintf(text, sizeof(text), "Scene %d  \xC2\xB7  switch %d", scene + 1, scene + 3);
    lv_label_set_text(s_se_title, text);
    snprintf(text, sizeof(text), "Preset %d: tap the effects that are on in this scene. Their settings: scene button in the FX editor", s_se_preset);
    lv_label_set_text(s_se_hint, text);
    lv_obj_set_hidden(s_se_clear, !stored);
    style_scene_editor();
    lv_obj_set_hidden(s_scene_editor, false);
    lv_obj_move_foreground(s_scene_editor);
}

static void on_scene_editor_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 cancel, 1 save, 2 remove the scene
    char text[UI_SCENE_NAME + NANO_FX_SLOTS + 4];
    lv_obj_set_hidden(s_scene_editor, true);
    if (action == 1) {
        int n = snprintf(text, sizeof(text), "%c%c", '3' + s_se_scene, 'a' + s_se_color);
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) text[n++] = (s_se_fx_on >> slot) & 1 ? '1' : '0';
        snprintf(text + n, sizeof(text) - (size_t)n, "%s", s_se_name);
    } else if (action == 2) {
        snprintf(text, sizeof(text), "%c!", '3' + s_se_scene);
    } else {
        return;
    }
    if (s_on_text) s_on_text('S', text);
}

static void build_scene_editor(lv_obj_t *screen)
{
    s_scene_editor = panel(screen, 50, 26, 700, 428);
    s_se_title = dialog_title(s_scene_editor, "");
    s_se_hint = label(s_scene_editor, &ui_font_14, MUTED);
    lv_obj_set_pos(s_se_hint, 20, 46);

    lv_obj_t *name = button(s_scene_editor, 18, 78, 662, 56, on_scene_name);   // tap: the keyboard
    s_se_name_label = label(name, &ui_font_title_22, TEXT);
    lv_obj_align(s_se_name_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *hint = label(name, &ui_font_14, MUTED);
    lv_label_set_text(hint, "Rename");
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -4, 0);

    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_scene_editor);
        s_se_swatch[i] = sw;
        lv_obj_set_size(sw, 52, 52);
        lv_obj_set_pos(sw, 18 + i * 68, 150);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_scene_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {   // the five effects in the order of the signal chain
        lv_obj_t *fx = frame_button(s_scene_editor);
        s_se_fx[slot] = fx;
        lv_obj_set_pos(fx, 18 + slot * 134, 220);
        lv_obj_set_size(fx, 126, 112);
        lv_obj_add_event_cb(fx, on_scene_fx, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
        s_se_fx_caption[slot] = label(fx, &ui_font_12, MUTED);
        lv_obj_set_style_text_letter_space(s_se_fx_caption[slot], 1, 0);
        char caption[16];   // small capitals, as the captions of the tiles
        size_t n = 0;
        for (const char *c = NANO_FX_SLOT_NAMES[slot]; *c && n < sizeof(caption) - 1; c++) caption[n++] = (char)toupper((unsigned char)*c);
        caption[n] = 0;
        lv_label_set_text(s_se_fx_caption[slot], caption);
        lv_obj_set_pos(s_se_fx_caption[slot], 10, 15);
        s_se_fx_state[slot] = label(fx, &ui_font_12, MUTED);
        lv_obj_set_style_text_letter_space(s_se_fx_state[slot], 1, 0);
        lv_obj_align(s_se_fx_state[slot], LV_ALIGN_TOP_RIGHT, -10, 15);
        s_se_fx_name[slot] = label(fx, &ui_font_16, TEXT);
        lv_obj_set_pos(s_se_fx_name[slot], 10, 38);
        lv_obj_set_size(s_se_fx_name[slot], 106, 2 * lv_font_get_line_height(&ui_font_16));
        lv_label_set_long_mode(s_se_fx_name[slot], LV_LABEL_LONG_DOT);
        s_se_fx_set[slot] = label(fx, &ui_font_12, MUTED);
        lv_obj_set_style_text_letter_space(s_se_fx_set[slot], 1, 0);
        lv_obj_set_pos(s_se_fx_set[slot], 10, 86);
    }
    s_se_clear = small_button(s_scene_editor, 18, 358, 150, 52, "Remove", on_scene_editor_button, 2);
    small_button(s_scene_editor, 372, 358, 150, 52, "Cancel", on_scene_editor_button, 0);
    primary(small_button(s_scene_editor, 530, 358, 150, 52, "Save", on_scene_editor_button, 1));
    lv_obj_set_hidden(s_scene_editor, true);
}

// ---- expression pedal: what the Nano's pedal does in this preset (the pedal button of the top bar) ----
// First page: eleven values can follow the pedal, each from its heel to its toe value - the Amount of the five
// effects (the wah position of a wah, the mix of a reverb ...) and the capture's knobs, its level and the input
// gate. Second page: what the pedal switches on and off - the capture, the cab, the effects, the gate - and in which
// way (the Cortex Cloud app's three: Heel-Toe, Switch for a footswitch on the connector, Stop). A field is filled
// while it is assigned; the chosen one shows its settings below. Save writes the assignments into the Nano's preset
// (they belong to it, as in the editor's Expression panel). Top right: what the Nano's EXP/MIDI connector takes;
// Calibrate teaches the Nano the pedal's lowest and highest position.

static const char *const EXP_WAYS[NANO_EXP_MODES] = { "Heel-Toe", "Switch", "Stop" };

static uint32_t exp_color(int i)
{
    if (i >= NANO_FX_SLOTS) return 0xF2F2F2;
    const nano_fx_model_t *model = nano_fx_model(s_slot_type[i]);
    return model ? model->color : 0x6A6A6A;
}

// The FX slot of an on/off assignment, -1 for the capture, the cab and the gate.
static int exp_switch_slot(int i)
{
    return i >= NANO_EXP_SW_PRE1 && i <= NANO_EXP_SW_POST3 ? i - NANO_EXP_SW_PRE1 : -1;
}

static const char *exp_switch_name(int i)
{
    int slot = exp_switch_slot(i);
    if (slot >= 0) return nano_fx_model(s_slot_type[slot]) ? nano_fx_name(s_slot_type[slot]) : "Empty";
    return i == NANO_EXP_SW_CAPTURE ? "Capture" : i == NANO_EXP_SW_CAB ? "Cab / IR" : "Gate";
}

static int exp_percent(uint8_t value)
{
    return (int)lroundf(value * 100 / 255.0f);
}

static void style_exp_field(int i)
{
    static const char *const NAMES[UI_EXP_TARGETS - NANO_FX_SLOTS] = { "Gain", "Bass", "Mid", "Treble", "Level", "Gate" };
    const ui_exp_range_t *range = &s_ex_range[i];
    bool fx = i < NANO_FX_SLOTS, empty = fx && !nano_fx_model(s_slot_type[i]), on = s_ex_loaded && range->on;
    uint32_t color = exp_color(i), ink = on ? ink_for(color) : empty || !s_ex_loaded ? 0x8E9297 : TEXT;
    style_frame_button(s_ex_field[i], color, on, empty && !on);
    lv_obj_set_style_text_color(s_ex_caption[i], lv_color_hex(on ? ink : MUTED), 0);
    lv_obj_set_style_text_color(s_ex_name[i], lv_color_hex(ink), 0);
    lv_label_set_text(s_ex_name[i], !fx ? NAMES[i - NANO_FX_SLOTS] : empty ? "Empty" : nano_fx_name(s_slot_type[i]));
    lv_obj_set_style_text_color(s_ex_range_label[i], lv_color_hex(ink), 0);
    if (on) lv_label_set_text_fmt(s_ex_range_label[i], "%d\xE2\x80\x93%d", exp_percent(range->heel), exp_percent(range->toe));
    else lv_label_set_text(s_ex_range_label[i], "");
    bool selected = s_ex_loaded && i == s_ex_selected;
    lv_obj_set_style_outline_width(s_ex_field[i], selected ? 3 : 0, 0);
}

// The chosen value: its name, whether it is on the pedal, its heel and toe value.
static void style_exp_detail(void)
{
    static const char *const NAMES[UI_EXP_TARGETS - NANO_FX_SLOTS] = { "Capture gain", "Capture bass", "Capture mid", "Capture treble",
                                                                      "Capture level", "Input gate amount" };
    int i = s_ex_selected;
    const ui_exp_range_t *range = &s_ex_range[i];
    char text[96];
    if (i >= NANO_FX_SLOTS) snprintf(text, sizeof(text), "%s", NAMES[i - NANO_FX_SLOTS]);
    else snprintf(text, sizeof(text), "%s Amount  \xC2\xB7  %s", NANO_FX_SLOT_NAMES[i], nano_fx_model(s_slot_type[i]) ? nano_fx_name(s_slot_type[i]) : "empty slot");
    lv_label_set_text(s_ex_target, s_ex_loaded ? text : "");
    bool on = s_ex_loaded && range->on;
    uint32_t color = exp_color(i);
    style_frame_button(s_ex_toggle, color, on, !s_ex_loaded);
    lv_label_set_text(s_ex_toggle_label, on ? "On" : "Off");
    lv_obj_set_style_text_color(s_ex_toggle_label, lv_color_hex(on ? ink_for(color) : s_ex_loaded ? TEXT : 0x5D6267), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_ex_toggle, 0), lv_color_hex(on ? ink_for(color) : MUTED), 0);
    for (int k = 0; k < 2; k++) {
        uint8_t value = k ? range->toe : range->heel;
        style_slider(s_ex_slider[k], on ? color : 0x6A6F75);
        lv_slider_set_value(s_ex_slider[k], value, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_ex_value[k], "%d%%", exp_percent(value));
        lv_obj_set_style_text_color(s_ex_value[k], lv_color_hex(on ? 0xFFFFFF : 0x8E9297), 0);
        if (s_ex_loaded) lv_obj_remove_state(s_ex_slider[k], LV_STATE_DISABLED);
        else lv_obj_add_state(s_ex_slider[k], LV_STATE_DISABLED);
    }
}

static uint32_t exp_switch_color(int i)
{
    int slot = exp_switch_slot(i);
    return slot >= 0 ? exp_color(slot) : 0xF2F2F2;
}

static void style_exp_switch_field(int i)
{
    const nano_exp_switch_t *sw = &s_ex_switch[i];
    int slot = exp_switch_slot(i);
    bool empty = slot >= 0 && !nano_fx_model(s_slot_type[slot]), on = s_ex_loaded && sw->on;
    uint32_t color = exp_switch_color(i), ink = on ? ink_for(color) : empty || !s_ex_loaded ? 0x8E9297 : TEXT;
    style_frame_button(s_ex_sw_field[i], color, on, empty && !on);
    lv_obj_set_style_text_color(s_ex_sw_caption[i], lv_color_hex(on ? ink : MUTED), 0);
    lv_obj_set_style_text_color(s_ex_sw_name[i], lv_color_hex(ink), 0);
    lv_label_set_text(s_ex_sw_name[i], exp_switch_name(i));
    lv_obj_set_style_text_color(s_ex_sw_way[i], lv_color_hex(ink), 0);
    char way[12] = "";
    for (size_t n = 0; on && EXP_WAYS[sw->mode][n] && n < sizeof(way) - 1; n++) {
        way[n] = (char)toupper((unsigned char)EXP_WAYS[sw->mode][n]);
        way[n + 1] = 0;
    }
    lv_label_set_text(s_ex_sw_way[i], way);
    lv_obj_set_style_outline_width(s_ex_sw_field[i], s_ex_loaded && i == s_ex_sw_selected ? 3 : 0, 0);
}

// The chosen on/off assignment: its name, whether the pedal switches it, the way, and what belongs to that way.
static void style_exp_switch_detail(void)
{
    int i = s_ex_sw_selected, slot = exp_switch_slot(i);
    const nano_exp_switch_t *sw = &s_ex_switch[i];
    char text[96];
    if (slot >= 0) snprintf(text, sizeof(text), "%s on / off  \xC2\xB7  %s", NANO_FX_SLOT_NAMES[slot], exp_switch_name(i));
    else snprintf(text, sizeof(text), "%s on / off", i == NANO_EXP_SW_GATE ? "Input gate" : exp_switch_name(i));
    lv_label_set_text(s_ex_sw_target, s_ex_loaded ? text : "");
    bool on = s_ex_loaded && sw->on;
    uint32_t color = exp_switch_color(i);
    style_frame_button(s_ex_sw_toggle, color, on, !s_ex_loaded);
    lv_label_set_text(s_ex_sw_toggle_label, on ? "On" : "Off");
    lv_obj_set_style_text_color(s_ex_sw_toggle_label, lv_color_hex(on ? ink_for(color) : s_ex_loaded ? TEXT : 0x5D6267), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_ex_sw_toggle, 0), lv_color_hex(on ? ink_for(color) : MUTED), 0);
    for (int k = 0; k < NANO_EXP_MODES; k++) style_tab(s_ex_sw_mode[k], on && k == sw->mode);
    bool delay = sw->mode != NANO_EXP_SWITCH;
    lv_obj_set_hidden(s_ex_sw_invert, sw->mode == NANO_EXP_STOP);
    style_tab(s_ex_sw_invert, on && sw->inverted);
    lv_obj_set_hidden(s_ex_sw_latch, delay);
    style_tab(s_ex_sw_latch, on && sw->latch);
    lv_label_set_text(lv_obj_get_child(s_ex_sw_latch, 0), sw->latch ? "Latch emulation: on" : "Latch emulation: off");
    lv_obj_set_hidden(s_ex_sw_delay_name, !delay);
    lv_obj_set_hidden(s_ex_sw_delay, !delay);
    lv_obj_set_hidden(s_ex_sw_delay_value, !delay);
    style_slider(s_ex_sw_delay, on ? color : 0x6A6F75);
    lv_slider_set_value(s_ex_sw_delay, sw->delay_ms / 10, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ex_sw_delay_value, "%d ms", sw->delay_ms);
    lv_obj_set_style_text_color(s_ex_sw_delay_value, lv_color_hex(on ? 0xFFFFFF : 0x8E9297), 0);
}

static void style_expression(void)
{
    char text[96];
    const char *by = s_jack_mode == NANO_JACK_MIDI ? "MIDI CC 1" : "the pedal";
    if (!s_ex_loaded) snprintf(text, sizeof(text), "Preset %d  \xC2\xB7  reading its assignments ...", s_ex_preset);
    else if (s_ex_page) snprintf(text, sizeof(text), "Preset %d  \xC2\xB7  what %s switches on and off", s_ex_preset, by);
    else if (s_jack_mode == NANO_JACK_MIDI) snprintf(text, sizeof(text), "Preset %d  \xC2\xB7  what MIDI CC 1 moves: 0 = heel, 127 = toe", s_ex_preset);
    else snprintf(text, sizeof(text), "Preset %d  \xC2\xB7  what the pedal moves, from heel to toe", s_ex_preset);
    lv_label_set_text(s_ex_sub, text);
    style_tab(s_ex_jack[0], s_jack_mode == NANO_JACK_EXPRESSION);
    style_tab(s_ex_jack[1], s_jack_mode == NANO_JACK_MIDI);
    lv_obj_set_hidden(s_ex_pages[0], s_ex_page != 0);
    lv_obj_set_hidden(s_ex_pages[1], s_ex_page != 1);
    int sweeps = 0, switches = 0;
    for (int i = 0; i < UI_EXP_TARGETS; i++) {
        style_exp_field(i);
        sweeps += s_ex_loaded && s_ex_range[i].on;
    }
    for (int i = 0; i < NANO_EXP_SWITCHES; i++) {
        style_exp_switch_field(i);
        switches += s_ex_loaded && s_ex_switch[i].on;
    }
    style_exp_detail();
    style_exp_switch_detail();
    // The button to the other page says how much is assigned there.
    if (s_ex_page) lv_label_set_text_fmt(lv_obj_get_child(s_ex_page_button, 0), "Sweeps  \xC2\xB7  %d", sweeps);
    else lv_label_set_text_fmt(lv_obj_get_child(s_ex_page_button, 0), "On / off  \xC2\xB7  %d", switches);
    lv_obj_set_hidden(s_ex_calibrate, s_jack_mode != NANO_JACK_EXPRESSION);   // a pedal on the connector only
    if (s_ex_loaded) lv_obj_remove_state(s_ex_save, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ex_save, LV_STATE_DISABLED);
}

static void on_ex_field(lv_event_t *e)
{
    if (!s_ex_loaded) return;
    int before = s_ex_selected;
    s_ex_selected = (int)(intptr_t)lv_event_get_user_data(e);
    style_exp_field(before);
    style_exp_field(s_ex_selected);
    style_exp_detail();
}

static void on_ex_toggle(lv_event_t *e)
{
    if (!s_ex_loaded) return;
    ui_exp_range_t *range = &s_ex_range[s_ex_selected];
    range->on = !range->on;
    if (range->on && range->heel == range->toe) {   // never set: the whole way
        range->heel = 0;
        range->toe = 255;
    }
    style_exp_field(s_ex_selected);
    style_exp_detail();
}

// A slider moved: that is the value at the heel (0) or the toe (1) - and the value is on the pedal.
static void on_ex_slider(lv_event_t *e)
{
    if (!s_ex_loaded) return;
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    ui_exp_range_t *range = &s_ex_range[s_ex_selected];
    uint8_t value = (uint8_t)lv_slider_get_value(s_ex_slider[which]);
    bool was_on = range->on;
    if (which) range->toe = value;
    else range->heel = value;
    range->on = true;
    lv_label_set_text_fmt(s_ex_value[which], "%d%%", exp_percent(value));
    style_exp_field(s_ex_selected);
    if (!was_on) style_exp_detail();
}

// On/off page: a field chosen (0-7), the assignment switched (8), a way chosen (10-12: that also assigns), invert
// (20) or latch emulation (21) switched.
static void on_ex_switch(lv_event_t *e)
{
    if (!s_ex_loaded) return;
    int what = (int)(intptr_t)lv_event_get_user_data(e), before = s_ex_sw_selected;
    nano_exp_switch_t *sw = &s_ex_switch[s_ex_sw_selected];
    if (what < NANO_EXP_SWITCHES) {
        s_ex_sw_selected = what;
        style_exp_switch_field(before);
    } else if (what == 8) {
        sw->on = !sw->on;
    } else if (what >= 10 && what < 10 + NANO_EXP_MODES) {
        sw->mode = (uint8_t)(what - 10);
        sw->on = true;
    } else if (what == 20) {
        sw->inverted = !sw->inverted;
        sw->on = true;
    } else if (what == 21) {
        sw->latch = !sw->latch;
        sw->on = true;
    }
    style_exp_switch_field(s_ex_sw_selected);
    style_exp_switch_detail();
}

static void on_ex_delay(lv_event_t *e)
{
    if (!s_ex_loaded) return;
    nano_exp_switch_t *sw = &s_ex_switch[s_ex_sw_selected];
    bool was_on = sw->on;
    sw->delay_ms = (uint16_t)(lv_slider_get_value(s_ex_sw_delay) * 10);
    sw->on = true;
    lv_label_set_text_fmt(s_ex_sw_delay_value, "%d ms", sw->delay_ms);
    if (!was_on) {
        style_exp_switch_field(s_ex_sw_selected);
        style_exp_switch_detail();
    }
}

static void on_ex_page(lv_event_t *e)
{
    s_ex_page = !s_ex_page;
    style_expression();
}

// The connector of the Nano takes an expression pedal or TRS MIDI, not both: a tap on the other one asks first.
static void on_ex_jack(lv_event_t *e)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    if (mode == s_jack_mode) return;
    if (mode == NANO_JACK_MIDI) {
        ask_for("Set the Nano's EXP/MIDI connector to MIDI? An expression pedal plugged into it stops working.", "Switch", '+', mode);
    } else {
        ask_for("Set the Nano's EXP/MIDI connector to expression pedal? MIDI through it stops working.", "Switch", '+', mode);
    }
}

void ui_set_jack(int mode)
{
    lvgl_port_lock(0);
    if (mode != s_jack_mode) {
        s_jack_mode = mode;
        if (!lv_obj_is_hidden(s_exp_dialog)) style_expression();
    }
    lvgl_port_unlock();
}

// Calibrate: the Nano forgets what it knew of the pedal and reports where it is; the lowest and highest position
// of a few sweeps over the whole way are saved as the new calibration.
static void on_ex_calibrate(lv_event_t *e)
{
    ask_for("Calibrate the expression pedal? The Nano forgets its calibration first.", "Start", '<', 1);
}

static void on_cal_button(lv_event_t *e)
{
    int save = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_cal_panel, true);
    send('<', save ? 2 : 0);
}

void ui_set_pedal(int value, int min, int max)
{
    char text[96];
    lvgl_port_lock(0);
    bool seen = max > min;
    lv_bar_set_range(s_cal_bar, seen ? min : 0, seen ? max : 1);
    lv_bar_set_value(s_cal_bar, seen ? value : 0, LV_ANIM_OFF);
    if (seen) snprintf(text, sizeof(text), "Position %d  \xC2\xB7  lowest %d  \xC2\xB7  highest %d", value, min, max);
    else snprintf(text, sizeof(text), "Waiting for the pedal to move ...");
    lv_label_set_text(s_cal_text, text);
    if (seen) lv_obj_remove_state(s_cal_save, LV_STATE_DISABLED);
    else lv_obj_add_state(s_cal_save, LV_STATE_DISABLED);
    if (lv_obj_is_hidden(s_cal_panel)) {
        lv_obj_set_hidden(s_cal_panel, false);
        lv_obj_move_foreground(s_cal_panel);
    }
    lvgl_port_unlock();
}

static void close_expression(void)
{
    if (lv_obj_is_hidden(s_exp_dialog)) return;
    lv_obj_set_hidden(s_exp_dialog, true);
    if (!lv_obj_is_hidden(s_cal_panel)) {
        lv_obj_set_hidden(s_cal_panel, true);
        send('<', 0);
    }
    send('=', 0);
}

static void on_ex_button(lv_event_t *e)
{
    bool save = (int)(intptr_t)lv_event_get_user_data(e) && s_ex_loaded;
    close_expression();
    if (!save || !s_on_text) return;
    char text[5 * UI_EXP_TARGETS + 8 * NANO_EXP_SWITCHES + 1];
    for (int i = 0; i < UI_EXP_TARGETS; i++) {
        snprintf(text + 5 * i, 6, "%c%02X%02X", s_ex_range[i].on ? '1' : '0', s_ex_range[i].heel, s_ex_range[i].toe);
    }
    for (int i = 0; i < NANO_EXP_SWITCHES; i++) {
        const nano_exp_switch_t *sw = &s_ex_switch[i];
        snprintf(text + 5 * UI_EXP_TARGETS + 8 * i, 9, "%c%c%c%c%04X", sw->on ? '1' : '0', '0' + sw->mode, sw->inverted ? '1' : '0',
                 sw->latch ? '1' : '0', sw->delay_ms);
    }
    s_on_text('X', text);
}

static void open_expression(lv_event_t *e)
{
    if (!s_linked) {
        ui_show_message("Not connected - still searching for the Nano.");
        return;
    }
    s_ex_loaded = false;
    s_ex_preset = s_current_preset;
    s_ex_selected = s_ex_sw_selected = 0;
    s_ex_page = 0;
    memset(s_ex_range, 0, sizeof(s_ex_range));
    memset(s_ex_switch, 0, sizeof(s_ex_switch));
    style_expression();
    lv_obj_set_hidden(s_exp_dialog, false);
    lv_obj_move_foreground(s_exp_dialog);
    send('=', 1);
}

void ui_set_expression(const nano_exp_range_t *ranges, const nano_exp_switch_t *switches)
{
    lvgl_port_lock(0);
    if (!lv_obj_is_hidden(s_exp_dialog)) {
        bool first = !s_ex_loaded;
        memcpy(s_ex_range, ranges, sizeof(s_ex_range));
        memcpy(s_ex_switch, switches, sizeof(s_ex_switch));
        s_ex_loaded = true;
        if (first) {   // in front: the first one that is assigned
            s_ex_selected = s_ex_sw_selected = 0;
            for (int i = UI_EXP_TARGETS - 1; i >= 0; i--) if (ranges[i].on) s_ex_selected = i;
            for (int i = NANO_EXP_SWITCHES - 1; i >= 0; i--) if (switches[i].on) s_ex_sw_selected = i;
        }
        style_expression();
    }
    lvgl_port_unlock();
}

// One field of the two grids: a caption, a name, a short note at the top right.
static lv_obj_t *exp_field(lv_obj_t *parent, int index, const char *caption, lv_event_cb_t cb, lv_obj_t **caption_label,
                           lv_obj_t **name_label, lv_obj_t **note_label)
{
    lv_obj_t *field = frame_button(parent);
    lv_obj_set_pos(field, 18 + (index % 4) * 178, 74 + (index / 4) * 66);
    lv_obj_set_size(field, 170, 58);
    lv_obj_set_style_outline_color(field, lv_color_white(), 0);
    lv_obj_set_style_outline_pad(field, 2, 0);
    lv_obj_add_event_cb(field, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    *caption_label = label(field, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(*caption_label, 1, 0);
    char upper[16];   // small capitals, as the captions of the tiles
    size_t n = 0;
    for (const char *c = caption; *c && n < sizeof(upper) - 1; c++) upper[n++] = (char)toupper((unsigned char)*c);
    upper[n] = 0;
    lv_label_set_text(*caption_label, upper);
    lv_obj_set_pos(*caption_label, 10, 12);
    *name_label = label(field, &ui_font_16, TEXT);
    lv_obj_set_pos(*name_label, 10, 29);
    lv_obj_set_size(*name_label, 148, lv_font_get_line_height(&ui_font_16));
    lv_label_set_long_mode(*name_label, LV_LABEL_LONG_DOT);
    *note_label = label(field, &ui_font_12, TEXT);
    lv_obj_set_style_text_letter_space(*note_label, 1, 0);
    lv_obj_align(*note_label, LV_ALIGN_TOP_RIGHT, -10, 12);
    return field;
}

// "ON THE PEDAL" with On / Off below: the assignment of the chosen field.
static lv_obj_t *exp_toggle(lv_obj_t *parent, int y, lv_event_cb_t cb, int data, lv_obj_t **state_label)
{
    lv_obj_t *toggle = frame_button(parent);
    lv_obj_set_pos(toggle, 18, y);
    lv_obj_set_size(toggle, 150, 74);
    lv_obj_add_event_cb(toggle, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t *caption = label(toggle, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(caption, 1, 0);
    lv_label_set_text(caption, "ON THE PEDAL");
    lv_obj_set_pos(caption, 10, 14);
    *state_label = label(toggle, &ui_font_title_22, TEXT);
    lv_obj_set_pos(*state_label, 10, 34);
    return toggle;
}

static lv_obj_t *exp_page(void)
{
    lv_obj_t *page = lv_obj_create(s_exp_dialog);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 740, 386);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_clickable(page, false);
    return page;
}

static void build_expression(lv_obj_t *screen)
{
    s_exp_dialog = panel(screen, 30, 16, 740, 448);
    dialog_title(s_exp_dialog, "Expression pedal");
    s_ex_sub = label(s_exp_dialog, &ui_font_14, MUTED);
    lv_obj_set_pos(s_ex_sub, 20, 46);
    // Top right: what the Nano's EXP/MIDI connector takes (a setting of the Nano, the same for all presets)
    lv_obj_t *jack = label(s_exp_dialog, &ui_font_12, MUTED);
    lv_obj_set_style_text_letter_space(jack, 1, 0);
    lv_label_set_text(jack, "EXP/MIDI JACK");
    lv_obj_set_width(jack, 130);
    lv_obj_set_style_text_align(jack, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(jack, 340, 21);
    s_ex_jack[0] = small_button(s_exp_dialog, 480, 8, 118, 38, "Expression", on_ex_jack, NANO_JACK_EXPRESSION);
    s_ex_jack[1] = small_button(s_exp_dialog, 604, 8, 118, 38, "MIDI", on_ex_jack, NANO_JACK_MIDI);

    // Page 1: the sweeps, four in a row - the effects in the order of the signal chain first
    lv_obj_t *page = s_ex_pages[0] = exp_page();
    static const char *const CAPTIONS[UI_EXP_TARGETS - NANO_FX_SLOTS] = { "Capture", "Capture", "Capture", "Capture", "Capture", "Input" };
    for (int i = 0; i < UI_EXP_TARGETS; i++) {
        s_ex_field[i] = exp_field(page, i, i < NANO_FX_SLOTS ? NANO_FX_SLOT_NAMES[i] : CAPTIONS[i - NANO_FX_SLOTS], on_ex_field,
                                  &s_ex_caption[i], &s_ex_name[i], &s_ex_range_label[i]);
    }
    s_ex_target = label(page, &ui_font_16, 0xC8CCD1);
    lv_obj_set_pos(s_ex_target, 20, 276);
    s_ex_toggle = exp_toggle(page, 304, on_ex_toggle, 0, &s_ex_toggle_label);
    static const char *const ENDS[2] = { "HEEL", "TOE" };
    for (int k = 0; k < 2; k++) {
        int y = 304 + k * 40;
        lv_obj_t *name = label(page, &ui_font_16, 0xC8CCD1);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, ENDS[k]);
        lv_obj_set_pos(name, 192, y + 8);
        s_ex_slider[k] = lv_slider_create(page);
        lv_slider_set_range(s_ex_slider[k], 0, 255);
        lv_obj_set_size(s_ex_slider[k], 330, 16);
        lv_obj_set_pos(s_ex_slider[k], 270, y + 12);
        lv_obj_set_ext_click_area(s_ex_slider[k], 14);
        style_slider(s_ex_slider[k], 0x6A6F75);
        lv_obj_add_event_cb(s_ex_slider[k], on_ex_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        s_ex_value[k] = label(page, &ui_font_28, 0xFFFFFF);
        lv_obj_set_width(s_ex_value[k], 100);
        lv_obj_set_style_text_align(s_ex_value[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_ex_value[k], 620, y + 2);
    }

    // Page 2: on / off - the capture, the cab, the five effects, the gate
    page = s_ex_pages[1] = exp_page();
    for (int i = 0; i < NANO_EXP_SWITCHES; i++) {
        int slot = exp_switch_slot(i);
        s_ex_sw_field[i] = exp_field(page, i, slot >= 0 ? NANO_FX_SLOT_NAMES[slot] : i == NANO_EXP_SW_GATE ? "Input" : "On / off", on_ex_switch,
                                     &s_ex_sw_caption[i], &s_ex_sw_name[i], &s_ex_sw_way[i]);
    }
    s_ex_sw_target = label(page, &ui_font_16, 0xC8CCD1);
    lv_obj_set_pos(s_ex_sw_target, 20, 214);
    s_ex_sw_toggle = exp_toggle(page, 242, on_ex_switch, 8, &s_ex_sw_toggle_label);
    for (int k = 0; k < NANO_EXP_MODES; k++) {   // the way the pedal switches it
        s_ex_sw_mode[k] = small_button(page, 192 + k * 116, 242, 110, 34, EXP_WAYS[k], on_ex_switch, 10 + k);
    }
    s_ex_sw_invert = small_button(page, 580, 242, 142, 34, "Invert", on_ex_switch, 20);
    s_ex_sw_latch = small_button(page, 192, 284, 250, 34, "Latch emulation: off", on_ex_switch, 21);
    s_ex_sw_delay_name = label(page, &ui_font_16, 0xC8CCD1);
    lv_obj_set_style_text_letter_space(s_ex_sw_delay_name, 2, 0);
    lv_label_set_text(s_ex_sw_delay_name, "DELAY");
    lv_obj_set_pos(s_ex_sw_delay_name, 192, 292);
    s_ex_sw_delay = lv_slider_create(page);
    lv_slider_set_range(s_ex_sw_delay, 0, NANO_EXP_DELAY_MAX / 10);   // steps of 10 ms
    lv_obj_set_size(s_ex_sw_delay, 300, 16);
    lv_obj_set_pos(s_ex_sw_delay, 282, 296);
    lv_obj_set_ext_click_area(s_ex_sw_delay, 14);
    style_slider(s_ex_sw_delay, 0x6A6F75);
    lv_obj_add_event_cb(s_ex_sw_delay, on_ex_delay, LV_EVENT_VALUE_CHANGED, NULL);
    s_ex_sw_delay_value = label(page, &ui_font_title_22, 0xFFFFFF);
    lv_obj_set_width(s_ex_sw_delay_value, 120);
    lv_obj_set_style_text_align(s_ex_sw_delay_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ex_sw_delay_value, 600, 288);

    s_ex_page_button = small_button(s_exp_dialog, 18, 390, 170, 46, "On / off", on_ex_page, 0);
    s_ex_calibrate = small_button(s_exp_dialog, 196, 390, 150, 46, "Calibrate", on_ex_calibrate, 0);
    small_button(s_exp_dialog, 410, 390, 150, 46, "Cancel", on_ex_button, 0);
    s_ex_save = small_button(s_exp_dialog, 570, 390, 150, 46, "Save", on_ex_button, 1);
    primary(s_ex_save);
    lv_obj_set_hidden(s_exp_dialog, true);

    // Calibration: the pedal's position between the lowest and the highest seen
    s_cal_panel = panel(screen, 170, 126, 460, 228);
    dialog_title(s_cal_panel, "Calibrate the pedal");
    lv_obj_t *how = label(s_cal_panel, &ui_font_16, 0xC8CCD1);
    lv_obj_set_width(how, 420);
    lv_label_set_long_mode(how, LV_LABEL_LONG_WRAP);
    lv_label_set_text(how, "Move the pedal from heel to toe a few times, then tap Save.");
    lv_obj_set_pos(how, 20, 52);
    s_cal_bar = lv_bar_create(s_cal_panel);
    lv_obj_set_pos(s_cal_bar, 20, 108);
    lv_obj_set_size(s_cal_bar, 420, 14);
    lv_obj_set_style_bg_color(s_cal_bar, lv_color_hex(0x2B2F34), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cal_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cal_bar, lv_color_hex(GREEN), LV_PART_INDICATOR);
    s_cal_text = label(s_cal_panel, &ui_font_14, MUTED);
    lv_obj_set_pos(s_cal_text, 20, 132);
    small_button(s_cal_panel, 150, 166, 140, 46, "Cancel", on_cal_button, 0);
    s_cal_save = small_button(s_cal_panel, 300, 166, 140, 46, "Save", on_cal_button, 1);
    primary(s_cal_save);
    lv_obj_set_hidden(s_cal_panel, true);
}

#ifdef NANO_BENCH
// ---- Development: timing on the board ----
// Console command '%' (firmware built with "idf.py -DNANO_BENCH=1 build"): opens and scrolls the FX editor's lists
// by itself and prints how long a frame takes - the drawing and the wait for the display. The display shows a new
// frame every 26.5 ms; a list that draws longer than that gets every second one (18.7 fps), and filling the list's
// area once already takes about 16 ms (the frame buffers are in PSRAM).
#include "esp_timer.h"

static struct { int64_t refr_t0, render_t0, flush_t0, refr_us, render_us, flush_us, max_us; int frames; bool drawn; } s_bench;
static int s_bench_step;

static void bench_display_event(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        s_bench.refr_t0 = now;
        s_bench.drawn = false;
    } else if (code == LV_EVENT_RENDER_START) {
        s_bench.render_t0 = now;
        s_bench.drawn = true;
    } else if (code == LV_EVENT_FLUSH_START) {
        s_bench.flush_t0 = now;
    } else if (code == LV_EVENT_FLUSH_FINISH) {
        s_bench.flush_us += now - s_bench.flush_t0;
    } else if (code == LV_EVENT_RENDER_READY) {
        s_bench.render_us += now - s_bench.render_t0;
    } else if (code == LV_EVENT_REFR_READY && s_bench.drawn) {
        int64_t dt = now - s_bench.refr_t0;
        s_bench.refr_us += dt;
        if (dt > s_bench.max_us) s_bench.max_us = dt;
        s_bench.frames++;
    }
}

static void bench_scroll(void *obj, int32_t v)
{
    lv_obj_scroll_to_y(obj, v, LV_ANIM_OFF);
}

static void bench_step(lv_timer_t *timer);

static void bench_report(lv_anim_t *a)
{
    int f = s_bench.frames ? s_bench.frames : 1;
    printf("BENCH %-11s %3d frames in 3 s = %4.1f fps | frame %5.1f ms (max %5.1f) = draw %5.1f + wait %4.1f\n",
           s_bench_step ? "parameters" : "model list", s_bench.frames, (double)(s_bench.frames / 3.0f),
           (double)((float)s_bench.refr_us / f / 1000), (double)((float)s_bench.max_us / 1000),
           (double)((float)(s_bench.render_us - s_bench.flush_us) / f / 1000), (double)((float)s_bench.flush_us / f / 1000));
    s_bench_step++;
    lv_timer_set_repeat_count(lv_timer_create(bench_step, 300, NULL), 1);
}

// Scrolls a list to its end and back in 3 s and counts the frames.
static void bench_run(lv_obj_t *list)
{
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    int32_t max = lv_obj_get_scroll_bottom(list);
    lv_refr_now(NULL);
    memset(&s_bench, 0, sizeof(s_bench));
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, list);
    lv_anim_set_values(&a, 0, max);
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_reverse_duration(&a, 1500);
    lv_anim_set_exec_cb(&a, bench_scroll);
    lv_anim_set_completed_cb(&a, bench_report);
    lv_anim_start(&a);
}

static void bench_step(lv_timer_t *timer)
{
    static float values[NANO_MAX_PARAMS];
    for (int i = 0; i < NANO_MAX_PARAMS; i++) values[i] = 0.1f + 0.035f * i;
    if (s_bench_step == 0) {   // the model list of Pre FX 1: opening it (building, first frame), then scrolling
        ui_fx_editor_show(0, NANO_SLOT_MODELS[0][6], true, values, 3);
        lv_refr_now(NULL);
        int64_t t = esp_timer_get_time();
        open_panel(PANEL_MODEL, 0);
        int64_t build = esp_timer_get_time() - t;
        t = esp_timer_get_time();
        lv_refr_now(NULL);
        printf("BENCH open model list: build %.1f ms + first frame %.1f ms\n", (double)(build / 1000.0f),
               (double)((esp_timer_get_time() - t) / 1000.0f));
        bench_run(s_panel_list);
    } else if (s_bench_step == 1) {   // the parameters of the model with the most of them
        close_panel();
        uint32_t type = NANO_SLOT_MODELS[3][0];
        for (int i = 0; i < NANO_SLOT_MODEL_COUNT[3]; i++) {
            const nano_fx_model_t *m = nano_fx_model(NANO_SLOT_MODELS[3][i]), *best = nano_fx_model(type);
            if (m && best && m->param_count > best->param_count) type = m->type;
        }
        ui_fx_editor_show(3, type, true, values, NANO_MAX_PARAMS);
        bench_run(s_editor_body);
    } else {
        ui_fx_editor_close();
        printf("BENCH done\n");
    }
}

void ui_bench(void)
{
    static bool hooked;
    lvgl_port_lock(0);
    if (!hooked) {
        lv_display_add_event_cb(lv_display_get_default(), bench_display_event, LV_EVENT_ALL, NULL);
        hooked = true;
    }
    s_bench_step = 0;
    lv_timer_set_repeat_count(lv_timer_create(bench_step, 100, NULL), 1);
    lvgl_port_unlock();
}
#endif
