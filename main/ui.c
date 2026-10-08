#include "ui.h"
#include "ui_fonts.h"

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

#define EDITOR_ROW_H 68

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
static lv_obj_t *s_save_button, *s_save_label, *s_ask, *s_ask_text, *s_rename, *s_rename_title, *s_rename_area;
static lv_obj_t *s_rename_ok;
static char s_rename_kind = 'N';   // 'N' preset name, '1'-'4' FX preset
static char s_ask_command;
static int s_current_preset;
static bool s_fx_mode;

static lv_obj_t *s_status_dot, *s_status, *s_edited, *s_app, *s_midi_icon;
static lv_obj_t *s_gig_bar, *s_gig_dot, *s_gig_number, *s_gig_bank, *s_gig_name, *s_gig_dirty, *s_gig_mode;
static lv_obj_t *s_capture_title;
static uint32_t s_tile_accent[TILES];   // colour of the pedal drawing on the tile
static lv_obj_t *s_preset_number, *s_preset_name, *s_capture, *s_cab;
static lv_obj_t *s_tile[TILES], *s_tile_caption[TILES], *s_tile_name[TILES], *s_tile_number[TILES];
static lv_obj_t *s_tile_square[TILES], *s_tile_pedal[TILES];
static lv_obj_t *s_tile_other[TILES];   // second line under the caption: the other effect of an A/B slot
static uint32_t s_tile_ink[TILES];
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
static lv_obj_t *s_mix_tab[2], *s_mix_panel[2], *s_rev_dropdown, *s_rev_edit;
static char s_mix_model[48];
static bool s_mix_touched;                 // Pos 1 / Pos 2 moved: SAVE writes them
static uint32_t s_rev_types[16];           // reverb models for the dropdown (index + 1; 0 = none)
static int s_rev_type_count;
static lv_obj_t *s_learn, *s_learn_title, *s_learn_text;
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
static lv_obj_t *s_editor, *s_editor_icon, *s_editor_slot, *s_editor_model, *s_editor_onoff, *s_editor_onoff_label;
static lv_obj_t *s_editor_switch;
static lv_obj_t *s_editor_body, *s_editor_info, *s_model_list, *s_editor_pedal;
static lv_obj_t *s_editor_model_button, *s_editor_second, *s_editor_second_label;
static bool s_model_list_second;   // the model list chooses the second effect (Pre FX 1)
static lv_obj_t *s_fxp_chip[UI_FX_PRESETS + 1];   // FX presets above the parameters: 0 = ORIGINAL
static char s_fxp_names[UI_FX_PRESETS][16];
static int s_fxp_active = -1;
static bool s_fxp_usable;
static lv_obj_t *s_param_control[NANO_MAX_PARAMS], *s_param_value[NANO_MAX_PARAMS];
static int s_editor_slot_index = -1;
static uint32_t s_editor_model_type;
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
static void on_tile(lv_event_t *e)
{
    if (lv_tick_elaps(s_gesture_tick) < 150) return;   // end of a swipe, not a tap
    send('w', 1 + (int)(intptr_t)lv_event_get_user_data(e));
}

static void set_fullscreen(bool on);

static void open_bank_editor(int tile);
static void open_mix_editor(void);
static void open_learn(int number);

static void on_tile_long(lv_event_t *e)
{
    int tile = (int)(intptr_t)lv_event_get_user_data(e);   // 0-7 = footswitch 1-8
    if (tile < 2) open_learn(tile + 1);
    else if (s_fx_mode && tile < 2 + NANO_FX_SLOTS) send('O', tile - 2);
    else if (s_fx_mode && tile == 2 + NANO_FX_SLOTS) open_mix_editor();
    else if (!s_fx_mode && tile >= 2) open_bank_editor(tile - 2);
}

static lv_obj_t *s_splash, *s_splash_status, *s_splash_version;

static void on_gesture(lv_event_t *e)
{
    s_gesture_tick = lv_tick_get();
    if (s_splash) return;   // start screen
    // Swipes work only on the main screen, not in an open dialog.
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_usb, s_mix_editor, s_learn, s_midi,
                                   s_volume, s_cab_settings, s_amp };
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
    lv_obj_t *btn = button(parent, x, y, 44, 44, cb);
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

static void set_icon_square(lv_obj_t *sq, uint32_t color, const lv_image_dsc_t *icon, bool active)
{
    lv_obj_t *img = lv_obj_get_child(sq, 0);
    lv_obj_set_style_bg_color(sq, lv_color_hex(color), 0);
    lv_obj_set_style_opa(sq, active ? LV_OPA_COVER : 115, 0);
    lv_obj_set_hidden(img, icon == NULL);
    lv_label_set_text(lv_obj_get_child(sq, 1), "");
    if (icon) {
        lv_image_set_src(img, icon);
        lv_obj_set_style_image_recolor(img, lv_color_hex(ink_for(color)), 0);
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
        lv_obj_set_style_text_font(s_tile_name[i], font, 0);
        lv_obj_set_style_text_line_space(s_tile_name[i], -lv_font_get_line_height(font) / 6, 0);
        lv_obj_align(s_tile_name[i], LV_ALIGN_BOTTOM_LEFT, 12, -10);
    }
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
    lv_obj_set_hidden(s_tile_pedal[i], true);   // shown again by set_tile_pedal
    lv_obj_set_style_bg_color(s_tile[i], lv_color_hex(bg), 0);
    lv_obj_set_style_bg_grad_dir(s_tile[i], LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(s_tile[i], lv_color_hex(border), 0);
    lv_obj_set_style_shadow_width(s_tile[i], active ? 24 : 0, 0);
    lv_obj_set_style_shadow_color(s_tile[i], lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(s_tile[i], LV_OPA_40, 0);

    lv_obj_t *sq = s_tile_square[i];
    lv_obj_set_hidden(sq, !icon && !symbol);
    lv_obj_t *img = lv_obj_get_child(sq, 0), *sym = lv_obj_get_child(sq, 1);
    lv_obj_set_hidden(img, icon == NULL);
    if (icon) {
        lv_image_set_src(img, icon);
        lv_obj_set_style_image_recolor(img, lv_color_hex(mark), 0);
    }
    lv_label_set_text(sym, symbol ? symbol : "");
    lv_obj_set_style_text_color(sym, lv_color_hex(mark), 0);
    lv_obj_set_pos(s_tile_caption[i], (icon || symbol) ? 56 : 14, 14);
    lv_obj_set_width(s_tile_caption[i], (icon || symbol) ? TILE_W - 96 : TILE_W - 54);

    char upper[40];   // captions in small capitals, as the editor's labels
    size_t n = 0;
    for (; caption[n] && n < sizeof(upper) - 1; n++) upper[n] = (char)((caption[n] >= 'a' && caption[n] <= 'z') ? caption[n] - 32 : caption[n]);
    upper[n] = 0;
    lv_label_set_text(s_tile_caption[i], upper);
    lv_obj_set_style_text_color(s_tile_caption[i], lv_color_hex(active ? ink : MUTED), 0);
    lv_obj_set_style_text_opa(s_tile_caption[i], active ? 210 : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_tile_number[i], lv_color_hex(active ? ink : 0x5D6267), 0);
    lv_obj_set_style_text_opa(s_tile_number[i], active ? 170 : LV_OPA_COVER, 0);

    lv_label_set_text(s_tile_name[i], name);
    lv_obj_set_style_text_color(s_tile_name[i], lv_color_hex(ink), 0);
    lv_obj_set_hidden(s_tile_other[i], true);   // shown again by set_tile_other
}

// The other effect of an A/B slot, under the caption (after set_tile): "⇄ name".
static void set_tile_other(int i, uint32_t type)
{
    lv_obj_set_hidden(s_tile_other[i], !type);
    if (!type) return;
    lv_label_set_text_fmt(s_tile_other[i], LV_SYMBOL_SHUFFLE " %s", nano_fx_name(type));
    lv_obj_set_style_text_color(s_tile_other[i], lv_color_hex(s_tile_ink[i] == TEXT ? MUTED : s_tile_ink[i]), 0);
    lv_obj_set_style_text_opa(s_tile_other[i], 210, 0);
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

// ---- footswitch learn: the next pressed footswitch takes this switch's place ----

static void on_learn_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 cancel, -1 default order
    lv_obj_set_hidden(s_learn, true);
    send('D', action);
}

static void open_learn(int number)
{
    lv_label_set_text_fmt(s_learn_title, "Learn footswitch %d", number);
    lv_label_set_text_fmt(s_learn_text, "Press the footswitch that should be switch %d now. Waiting 15 seconds.", number);
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
    small_button(s_learn, 20, 138, 180, 46, "Default order", on_learn_button, -1);
    small_button(s_learn, 220, 138, 180, 46, "Cancel", on_learn_button, 0);
    lv_obj_set_hidden(s_learn, true);
}

// ---- confirm dialog and rename keyboard ----

static void on_ask(lv_event_t *e)
{
    bool yes = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_ask, true);
    if (yes) send(s_ask_command, 0);
}

static void ask(const char *text, char command)
{
    lv_label_set_text(s_ask_text, text);
    s_ask_command = command;
    lv_obj_set_hidden(s_ask, false);
    lv_obj_move_foreground(s_ask);
}

static void build_ask(lv_obj_t *screen)
{
    s_ask = panel(screen, 150, 150, 500, 180);
    s_ask_text = label(s_ask, &ui_font_20, TEXT);
    lv_obj_set_width(s_ask_text, 460);
    lv_label_set_long_mode(s_ask_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ask_text, 20, 18);
    small_button(s_ask, 160, 116, 150, 48, "Cancel", on_ask, 0);
    primary(small_button(s_ask, 330, 116, 150, 48, "Save", on_ask, 1));
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
    if (s_on_text) s_on_text(s_rename_kind, text);
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
    lv_textarea_set_placeholder_text(s_rename_area, kind == 'N' ? "" : "Name (empty name = delete)");
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
    for (int i = 0; i < 2; i++) {
        style_tab(s_mix_tab[i], i == tab);
        lv_obj_set_hidden(s_mix_panel[i], i != tab);
    }
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

// 0 cancel, 1 save, 2 edit reverb B
static void on_mix_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_mix_editor, true);
    if (action && s_mix_touched) {
        send('W', (int)lv_slider_get_value(s_mix_slider[0]) << 8 | (int)lv_slider_get_value(s_mix_slider[1]));
    }
    if (action && chosen_rev_b() != s_view.rev_b_type) send('A', (int)chosen_rev_b());
    send('K', 0);
    if (action == 2) send('H', 0);
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
    lv_label_set_text(s_mix_info, s_view.mix_known
        ? "Moving a slider plays that mix. Save stores both in the preset."
        : "This preset has no Pos 1 / Pos 2 yet: Save adds them (expression Amount of the reverb).");
    int selected = 0;
    for (int i = 0; i < s_rev_type_count; i++) if (s_rev_types[i] == s_view.rev_b_type) selected = i + 1;
    lv_dropdown_set_selected(s_rev_dropdown, (uint32_t)selected);
    on_rev_dropdown(NULL);
    show_mix_tab(s_view.rev_b_type ? 1 : 0);
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
        int y = 30 + i * 54;
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
    lv_label_set_text(info, "Footswitch 8 switches between the preset's reverb (A) and a second one (B). "
                            "Reverb B and its settings are stored on the controller; Edit B sets it up.");
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

    // Top bar as in the editor: symbol buttons (refresh, capture volume, MIDI, USB) on the left, connection and
    // preset / bank in the middle, Save on the right (green when there is something to save). Below: the arrows and
    // the preset name, an orange dot next to it for unsaved changes. No card, no picture behind it.
    lv_obj_t *preset = lv_obj_create(screen);
    lv_obj_remove_style_all(preset);
    lv_obj_set_pos(preset, 0, 0);
    lv_obj_set_size(preset, 800, 122);
    lv_obj_set_scrollable(preset, false);
    s_preset_card = preset;
    static const struct { int icon; lv_event_cb_t cb; } TOP[] = {
        { UI_ICON_REFRESH, on_refresh }, { UI_ICON_VOLUME, open_volume }, { UI_ICON_MIDI, open_midi }, { UI_ICON_USB, open_usb },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = button(preset, 10 + i * 52, 6, 44, 40, TOP[i].cb);
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
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 17);
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

    s_save_button = button(preset, 676, 6, 114, 40, on_save_button);
    lv_obj_t *save_icon = ui_icon(s_save_button, UI_ICON_SAVE, 22, TEXT);
    lv_obj_align(save_icon, LV_ALIGN_LEFT_MID, 6, 0);
    s_save_label = label(s_save_button, &ui_font_16, TEXT);
    lv_label_set_text(s_save_label, "Save");
    lv_obj_align(s_save_label, LV_ALIGN_LEFT_MID, 38, 0);

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
    s_gig_bar = lv_obj_create(screen);
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
    lv_obj_set_hidden(s_gig_bar, true);

    // Capture and cab cards
    s_source_card[0] = source_card(screen, 10, "CAPTURE", NANO_ICON_CAPTURE, open_capture_picker, open_amp,
                                   &s_capture_square, &s_capture, &s_capture_slot_label, &s_capture_title);
    s_source_card[1] = source_card(screen, 405, "CAB / IR", NANO_ICON_CAB, open_cab_picker, open_cab_settings,
                                   &s_cab_square, &s_cab, &s_cab_slot_label, NULL);

    for (int i = 0; i < TILES; i++) build_tile(screen, i);
    build_picker(screen);
    build_bank_editor(screen);
    build_editor(screen);
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
        lv_obj_set_hidden(s_ask, true);
        lv_obj_set_hidden(s_rename, true);
        lv_label_set_text(s_capture_slot_label, "");
        lv_label_set_text(s_cab_slot_label, "");
        lv_label_set_text(s_preset_number, "");
        lv_label_set_text(s_preset_name, "");
        lv_label_set_text(s_capture, "");
        lv_label_set_text(s_cab, "");
        for (int i = 0; i < TILES; i++) set_tile(i, "", "", 0x2A2A2A, false, true, NULL, NULL);
        fit_names();
    }
    lvgl_port_unlock();
}

// Pedal drawing on an FX tile (after set_tile), in the tile's text colour.
static void set_tile_pedal(int i, uint32_t type)
{
    const lv_image_dsc_t *pedal = nano_fx_pedal_tile(type);
    lv_obj_set_hidden(s_tile_pedal[i], pedal == NULL);
    if (!pedal) return;
    lv_image_set_src(s_tile_pedal[i], pedal);
    lv_obj_set_style_image_recolor(s_tile_pedal[i], lv_color_hex(s_tile_accent[i]), 0);
}

// Footswitch 1 (mode) and 2 (tuner) tiles.
static void show_fixed_tiles(const ui_view_t *view)
{
    char caption[24];
    if (view->fx_mode) snprintf(caption, sizeof(caption), "MODE");
    else snprintf(caption, sizeof(caption), "BANK %d", view->bank + 1);
    set_tile(0, caption, view->fx_mode ? "FX" : "Presets", 0x8A9099, false, false, NULL, LV_SYMBOL_LOOP);
    set_tile(1, "TUNER", "Tuner", 0xBDBDBD, false, false, NANO_ICONS[NANO_ICON_TUNER], NANO_ICONS[NANO_ICON_TUNER] ? NULL : LV_SYMBOL_AUDIO);
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

void ui_show_state(const nano_state_t *st, const ui_view_t *view)
{
    char text[96];
    lvgl_port_lock(0);

    s_fx_mode = view->fx_mode;
    show_fixed_tiles(view);
    if (view->mix_slot >= 0) {
        snprintf(s_mix_model, sizeof(s_mix_model), "%s  -  %s", nano_fx_name(st->fx_type[view->mix_slot]),
                 NANO_FX_SLOT_NAMES[view->mix_slot]);
    }

    snprintf(text, sizeof(text), "PRESET %d  \xC2\xB7  BANK %d", st->current_preset, view->bank + 1);
    lv_label_set_text(s_preset_number, text);
    lv_obj_set_hidden(s_preset_number, false);
    lv_obj_set_hidden(s_status, true);    // the green dot says "connected"
    s_current_preset = st->current_preset;
    // Save: neutral, green when there is something to save (as in the editor), plus the orange dot by the name.
    lv_obj_set_style_bg_color(s_save_button, lv_color_hex(st->dirty ? GREEN : 0x191B1D), 0);
    lv_obj_set_style_bg_grad_dir(s_save_button, st->dirty ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(s_save_button, lv_color_hex(st->dirty ? GREEN : 0x3A3D40), 0);
    lv_obj_set_style_text_color(s_save_label, lv_color_hex(st->dirty ? INK : TEXT), 0);
    lv_obj_set_style_image_recolor(lv_obj_get_child(s_save_button, 0), lv_color_hex(st->dirty ? INK : TEXT), 0);
    const char *name = st->preset_names[st->current_preset - 1];
    const char *shown = st->names_loaded && name[0] ? name : "-";
    lv_label_set_text(s_preset_name, shown);
    lv_point_t name_size;
    lv_text_get_size(&name_size, shown, &ui_font_title_46, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int half = (name_size.x < 600 ? name_size.x : 600) / 2;
    lv_obj_set_pos(s_edited, 400 + half + 10, 82);
    lv_obj_set_hidden(s_edited, !st->dirty);

    // Gig view bar
    lv_label_set_text_fmt(s_gig_number, "%d", st->current_preset);
    lv_label_set_text_fmt(s_gig_bank, "BANK %d", view->bank + 1);
    lv_label_set_text(s_gig_name, shown);
    lv_text_get_size(&name_size, shown, &ui_font_title_22, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    half = (name_size.x < 440 ? name_size.x : 440) / 2;
    lv_obj_align(s_gig_dirty, LV_ALIGN_CENTER, half + 12, 0);
    lv_obj_set_hidden(s_gig_dirty, !st->dirty);
    lv_label_set_text(s_gig_mode, view->fx_mode ? "FX" : "PRESETS");
    lv_obj_set_style_bg_color(s_gig_mode, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_gig_mode, view->fx_mode ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_gig_mode, lv_color_hex(view->fx_mode ? INK : TEXT), 0);
    memcpy(s_capture_names, st->capture_names, sizeof(s_capture_names));
    memcpy(s_cab_names, st->cab_names, sizeof(s_cab_names));
    s_capture_slot = st->capture_slot;
    memcpy(s_preset_names, st->preset_names, sizeof(s_preset_names));
    s_view = *view;
    s_cab_slot = st->cab_slot;
    lv_label_set_text(s_capture, st->capture_slot ? (st->capture[0] ? st->capture : "-") : "Bypassed");
    lv_obj_set_style_text_color(s_capture, lv_color_hex(st->capture_slot ? TEXT : MUTED), 0);
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
    if (KIND_NAMES[kind][0]) lv_label_set_text_fmt(s_capture_title, "CAPTURE  \xC2\xB7  %s", KIND_NAMES[kind]);
    else lv_label_set_text(s_capture_title, "CAPTURE");
    if (st->capture_slot) snprintf(text, sizeof(text), "%d-%d", (st->capture_slot - 1) / 5 + 1, (st->capture_slot - 1) % 5 + 1);
    else snprintf(text, sizeof(text), "BYPASS");
    lv_label_set_text(s_capture_slot_label, text);
    lv_label_set_text(s_cab, st->cab_slot ? (st->cab[0] ? st->cab : "-") : "Bypassed");
    lv_obj_set_style_text_color(s_cab, lv_color_hex(st->cab_slot ? TEXT : MUTED), 0);
    set_icon_square(s_cab_square, TEXT, NANO_ICONS[NANO_ICON_CAB], st->cab_slot != 0);
    if (st->cab_slot) snprintf(text, sizeof(text), "%d", st->cab_slot);
    else snprintf(text, sizeof(text), "BYPASS");
    lv_label_set_text(s_cab_slot_label, text);
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

    if (view->fx_mode) show_fx_tiles(st, view);
    else show_preset_tiles(st, view);
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

static float param_from_normalized(const nano_param_t *p, float n)
{
    float v = p->min + n * (p->max - p->min);
    if (p->step > 0) v = roundf(v / p->step) * p->step;
    return v < p->min ? p->min : v > p->max ? p->max : v;
}

static void format_param(char *buf, size_t size, const nano_param_t *p, float n)
{
    snprintf(buf, size, "%.*f %s", p->decimals < 0 ? 1 : p->decimals, (double)param_from_normalized(p, n), p->unit);
}

static void on_editor_back(lv_event_t *e)
{
    lv_obj_set_hidden(s_model_list, true);
    lv_obj_set_hidden(s_editor, true);
    s_editor_slot_index = -1;
    send('E', 0);
}

static void on_editor_onoff(lv_event_t *e)
{
    if (s_editor_slot_index >= 0) send('a' + s_editor_slot_index, 0);
}

static void build_model_list(int slot);

static void on_model_button(lv_event_t *e)
{
    if (s_model_list_second) {
        s_model_list_second = false;
        build_model_list(s_editor_slot_index);
        lv_obj_set_hidden(s_model_list, false);
        return;
    }
    lv_obj_set_hidden(s_model_list, !lv_obj_is_hidden(s_model_list));
}

// 2ND (Pre FX 1): the model list chooses the second effect, which footswitch 3 swaps in when held.
static void on_second_button(lv_event_t *e)
{
    bool open = !lv_obj_is_hidden(s_model_list) && s_model_list_second;
    s_model_list_second = !open;
    build_model_list(s_editor_slot_index);
    lv_obj_set_hidden(s_model_list, open);
}

static void on_second_choice(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);   // model, 0 = none, -1 = swap now
    lv_obj_set_hidden(s_model_list, true);
    s_model_list_second = false;
    if (choice < 0) send('w', UI_SWITCH_HOLD | 3);
    else if ((uint32_t)choice != s_view.pre1_b_type) send('A', 1 << 24 | choice);
}

static void on_model_choice(lv_event_t *e)
{
    uint32_t type = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_model_list, true);
    if (type != s_editor_model_type) send('M', (int)type);
}

static void on_slider(lv_event_t *e)
{
    int param = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_editor_model_def || param >= s_editor_model_def->param_count) return;
    const nano_param_t *p = &s_editor_model_def->params[param];
    float n = lv_slider_get_value(lv_event_get_target_obj(e)) / 1000.0f;
    float v = param_from_normalized(p, n);   // snap to the parameter's step
    n = p->max > p->min ? (v - p->min) / (p->max - p->min) : 0;
    char text[32];
    format_param(text, sizeof(text), p, n);
    lv_label_set_text(s_param_value[param], text);
    if (s_on_param) s_on_param(s_editor_slot_index, param, n);
}

static void on_dropdown(lv_event_t *e)
{
    int param = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_editor_model_def || param >= s_editor_model_def->param_count) return;
    int count = s_editor_model_def->params[param].option_count;
    float n = count > 1 ? (float)lv_dropdown_get_selected(lv_event_get_target_obj(e)) / (count - 1) : 0;
    if (s_on_param) s_on_param(s_editor_slot_index, param, n);
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

    lv_obj_t *back = button(s_editor, 8, 8, 120, 48, on_editor_back);
    lv_obj_t *back_label = label(back, &ui_font_20, 0xF2F2F2);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    s_editor_icon = lv_image_create(s_editor);
    lv_obj_set_pos(s_editor_icon, 142, 14);
    lv_obj_set_style_image_recolor_opa(s_editor_icon, LV_OPA_COVER, 0);
    s_editor_slot = label(s_editor, &ui_font_20, 0x9A9A9A);
    lv_obj_set_pos(s_editor_slot, 188, 20);

    s_editor_model_button = button(s_editor, 300, 8, 300, 48, on_model_button);
    s_editor_model = label(s_editor_model_button, &ui_font_20, 0xF2F2F2);
    lv_label_set_long_mode(s_editor_model, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_editor_model, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_editor_model);
    s_editor_second = button(s_editor, 530, 8, 70, 48, on_second_button);   // Pre FX 1 only
    s_editor_second_label = label(s_editor_second, &ui_font_20, TEXT);
    lv_label_set_text(s_editor_second_label, "A/B");
    lv_obj_center(s_editor_second_label);
    lv_obj_set_hidden(s_editor_second, true);

    // On/off: ON / OFF and a switch like the sliders (effect colour, white knob); the whole area is the button.
    s_editor_onoff = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_editor_onoff);
    lv_obj_set_pos(s_editor_onoff, 612, 8);
    lv_obj_set_size(s_editor_onoff, 180, 48);
    lv_obj_set_style_opa(s_editor_onoff, 180, LV_STATE_PRESSED);
    lv_obj_set_clickable(s_editor_onoff, true);
    lv_obj_add_event_cb(s_editor_onoff, on_editor_onoff, LV_EVENT_CLICKED, NULL);
    s_editor_onoff_label = label(s_editor_onoff, &ui_font_20, TEXT);
    lv_obj_set_style_text_letter_space(s_editor_onoff_label, 2, 0);
    lv_obj_align(s_editor_onoff_label, LV_ALIGN_LEFT_MID, 14, 0);
    s_editor_switch = lv_switch_create(s_editor_onoff);
    lv_obj_set_clickable(s_editor_switch, false);   // the state comes from the Nano (ui_fx_editor_show)
    lv_obj_set_size(s_editor_switch, 86, 42);
    lv_obj_align(s_editor_switch, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(0x2A2D30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(INK), LV_PART_KNOB | LV_STATE_CHECKED);   // as in the editor
    lv_obj_set_style_pad_all(s_editor_switch, -3, LV_PART_KNOB);

    // Pedal silhouette behind the parameters (fixed while the parameter list scrolls)
    s_editor_pedal = lv_image_create(s_editor);
    lv_obj_set_style_image_recolor_opa(s_editor_pedal, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(s_editor_pedal, 70, 0);
    lv_obj_align(s_editor_pedal, LV_ALIGN_CENTER, 31, 32);   // centre of the slider column
    lv_obj_set_hidden(s_editor_pedal, true);

    s_editor_body = lv_obj_create(s_editor);
    lv_obj_set_pos(s_editor_body, 0, 64);
    lv_obj_set_size(s_editor_body, 800, 416);
    lv_obj_set_style_bg_opa(s_editor_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_editor_body, 0, 0);
    lv_obj_set_style_radius(s_editor_body, 0, 0);
    lv_obj_set_style_pad_all(s_editor_body, 0, 0);
    lv_obj_set_style_pad_bottom(s_editor_body, 24, 0);
    lv_obj_set_scroll_dir(s_editor_body, LV_DIR_VER);

    // Model list: a scrollable column of buttons.
    s_model_list = lv_obj_create(s_editor);
    lv_obj_set_pos(s_model_list, 300, 60);
    lv_obj_set_size(s_model_list, 320, 414);
    lv_obj_set_style_bg_color(s_model_list, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_border_color(s_model_list, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_pad_all(s_model_list, 6, 0);
    lv_obj_set_style_pad_row(s_model_list, 4, 0);
    lv_obj_set_flex_flow(s_model_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_model_list, LV_DIR_VER);
    lv_obj_set_hidden(s_model_list, true);

    lv_obj_set_hidden(s_editor, true);
}

// ---- FX presets (row above the parameters) ----

static void style_fxp_chips(void)
{
    uint32_t color = s_editor_model_def ? s_editor_model_def->color : 0x5A5A5A;
    for (int i = 0; i <= UI_FX_PRESETS; i++) {
        lv_obj_t *chip = s_fxp_chip[i];
        if (!chip) continue;
        bool filled = i == 0 || s_fxp_names[i - 1][0];
        bool active = i == s_fxp_active && filled;
        lv_obj_t *l = lv_obj_get_child(chip, 0);
        const char *text = i == 0 ? "Orig" : filled ? s_fxp_names[i - 1] : "+";
        lv_label_set_text(l, text);
        // One line: the large font if the name fits, otherwise the small one (and dots if it is still too long).
        const lv_font_t *font = &ui_font_14;
        lv_point_t size;
        lv_text_get_size(&size, text, &ui_font_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (filled && size.x <= 130) font = &ui_font_20;
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_height(l, lv_font_get_line_height(font));
        lv_obj_set_style_text_color(l, lv_color_hex(active ? ink_for(color) : filled ? TEXT : 0x5D6267), 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(active ? color : 0x191B1D), 0);
        lv_obj_set_style_bg_grad_dir(chip, active ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
        lv_obj_set_hidden(chip, !s_fxp_usable);   // the info line ("effect is off", "reading values") is there then
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

// One row per parameter, in the model's display order.
static void build_param_rows(void)
{
    lv_obj_clean(s_editor_body);
    memset(s_param_control, 0, sizeof(s_param_control));
    memset(s_param_value, 0, sizeof(s_param_value));
    s_editor_info = label(s_editor_body, &ui_font_20, 0xFF7000);
    lv_obj_set_pos(s_editor_info, 16, 10);
    memset(s_fxp_chip, 0, sizeof(s_fxp_chip));

    const nano_fx_model_t *m = s_editor_model_def;
    if (!m) return;
    uint32_t color = m->color;
    // FX presets: ORIGINAL and the stored places of this model (tap = load, hold = save under a name).
    for (int i = 0; i <= UI_FX_PRESETS; i++) {
        lv_obj_t *chip = small_button(s_editor_body, 16 + i * 154, 8, 146, 46, "", on_fxp_chip, i);   // where the info line is
        lv_obj_remove_event_cb(chip, on_fxp_chip);
        lv_obj_add_event_cb(chip, on_fxp_chip, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        if (i) lv_obj_add_event_cb(chip, on_fxp_chip_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_obj_get_child(chip, 0);
        lv_obj_set_width(l, 132);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        s_fxp_chip[i] = chip;
    }
    style_fxp_chips();
    for (int row = 0; row < m->param_count; row++) {
        int idx = m->order ? m->order[row] : row;
        if (idx >= m->param_count || idx >= NANO_MAX_PARAMS) continue;
        const nano_param_t *p = &m->params[idx];
        int y = 64 + row * EDITOR_ROW_H;

        lv_obj_t *name = label(s_editor_body, &ui_font_20, 0xD8D8D8);
        lv_label_set_text(name, p->name);
        lv_obj_set_width(name, 196);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, 16, y + 20);

        if (p->kind == NANO_PARAM_ENUM) {
            lv_obj_t *dd = lv_dropdown_create(s_editor_body);
            lv_dropdown_set_options_static(dd, p->options);
            lv_obj_set_size(dd, 300, 52);
            lv_obj_set_pos(dd, 220, y + 6);
            lv_obj_set_style_text_font(dd, &ui_font_20, 0);
            lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &ui_font_20, 0);
            lv_obj_add_event_cb(dd, on_dropdown, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)idx);
            s_param_control[idx] = dd;
        } else {
            lv_obj_t *slider = lv_slider_create(s_editor_body);
            lv_slider_set_range(slider, 0, 1000);
            lv_obj_set_size(slider, 410, 16);
            lv_obj_set_pos(slider, 226, y + 26);
            lv_obj_set_ext_click_area(slider, 22);
            lv_obj_set_style_bg_color(slider, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
            lv_obj_set_style_pad_all(slider, 9, LV_PART_KNOB);
            lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)idx);
            s_param_control[idx] = slider;
            s_param_value[idx] = label(s_editor_body, &ui_font_20, 0xFFFFFF);
            lv_obj_set_width(s_param_value[idx], 130);
            lv_obj_set_style_text_align(s_param_value[idx], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(s_param_value[idx], 656, y + 20);
        }
    }
}

// The models that the Nano allows in this slot.
static lv_obj_t *model_list_entry(const char *text, uint32_t color, bool selected, lv_event_cb_t cb, intptr_t data)
{
    lv_obj_t *btn = lv_button_create(s_model_list);
    lv_obj_set_size(btn, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x3A3A3A : 0x262626), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)data);
    if (text) {
        lv_obj_t *l = label(btn, &ui_font_20, color);
        lv_label_set_text(l, text);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 44, 0);
    }
    return btn;
}

// The models allowed in the slot; for the second effect of Pre FX 1 also "swap now" and "none".
static void build_model_list(int slot)
{
    lv_obj_clean(s_model_list);
    bool second = s_model_list_second && slot == 0;
    uint32_t current = second ? s_view.pre1_b_type : s_editor_model_type;
    if (second) {
        lv_obj_t *title = label(s_model_list, &ui_font_14, MUTED);
        lv_obj_set_width(title, LV_PCT(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
        lv_label_set_text(title, "2ND EFFECT - hold switch 3 for A / B");
        if (s_view.pre1_b_type) model_list_entry(s_view.pre1_active ? LV_SYMBOL_SHUFFLE "  Back to A now" : LV_SYMBOL_SHUFFLE "  Swap to B now",
                                                 0xFF7000, false, on_second_choice, -1);
        model_list_entry("None", TEXT, current == 0, on_second_choice, 0);
    }
    for (int i = 0; i < NANO_SLOT_MODEL_COUNT[slot]; i++) {
        uint32_t type = NANO_SLOT_MODELS[slot][i];
        const nano_fx_model_t *m = nano_fx_model(type);
        if (!m) continue;
        lv_obj_t *btn = model_list_entry(NULL, 0, type == current, second ? on_second_choice : on_model_choice, (intptr_t)type);
        if (NANO_ICONS[m->icon]) {
            lv_obj_t *icon = lv_image_create(btn);
            lv_image_set_src(icon, NANO_ICONS[m->icon]);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(m->color), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, -4, 0);
        }
        lv_obj_t *name = label(btn, &ui_font_20, m->color);
        lv_label_set_text(name, m->name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 44, 0);
    }
}

void ui_fx_editor_show(int slot, uint32_t model, bool on, const float *values, int count)
{
    lvgl_port_lock(0);
    bool rebuild = lv_obj_is_hidden(s_editor) || slot != s_editor_slot_index || model != s_editor_model_type;
    s_editor_slot_index = slot;
    s_editor_model_type = model;
    s_editor_model_def = nano_fx_model(model);
    const nano_fx_model_t *m = s_editor_model_def;
    if (rebuild) {
        s_model_list_second = false;
        build_param_rows();
        build_model_list(slot);
    }

    uint32_t color = m ? m->color : 0x5A5A5A;
    char text[64];
    lv_obj_set_hidden(s_editor_icon, !m || !NANO_ICONS[m->icon]);
    if (m && NANO_ICONS[m->icon]) {
        lv_image_set_src(s_editor_icon, NANO_ICONS[m->icon]);
        lv_obj_set_style_image_recolor(s_editor_icon, lv_color_hex(color), 0);
    }
    const lv_image_dsc_t *pedal = m ? nano_fx_pedal(m->type) : NULL;
    lv_obj_set_hidden(s_editor_pedal, pedal == NULL);
    if (pedal) {
        lv_image_set_src(s_editor_pedal, pedal);
        lv_obj_set_style_image_recolor(s_editor_pedal, lv_color_hex(color), 0);
    }
    lv_label_set_text(s_editor_slot, NANO_FX_SLOT_NAMES[slot]);
    snprintf(text, sizeof(text), "%s  " LV_SYMBOL_DOWN, m ? m->name : "Choose a model");
    lv_label_set_text(s_editor_model, text);
    // Pre FX 1: the 2ND button (second effect) takes the right end of the model button.
    bool second = slot == 0;
    lv_obj_set_width(s_editor_model_button, second ? 224 : 300);
    lv_obj_set_width(s_editor_model, second ? 204 : 280);
    lv_obj_set_hidden(s_editor_second, !second);
    if (second) {
        bool b_active = s_view.pre1_b_type && s_view.pre1_active;
        lv_label_set_text(s_editor_second_label, s_view.pre1_b_type ? (b_active ? "B" : "A/B") : "2ND");
        lv_obj_set_style_bg_color(s_editor_second, lv_color_hex(b_active ? 0xFF7000 : 0x191B1D), 0);
        lv_obj_set_style_bg_grad_dir(s_editor_second, b_active ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_text_color(s_editor_second_label, lv_color_hex(b_active ? 0x000000 : TEXT), 0);
    }

    lv_label_set_text(s_editor_onoff_label, on ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_editor_onoff_label, lv_color_hex(on ? TEXT : MUTED), 0);
    lv_obj_set_style_bg_color(s_editor_switch, lv_color_hex(color), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(s_editor_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_editor_switch, LV_STATE_CHECKED);

    bool known = values != NULL;
    const char *info = !m ? "This slot is empty. Choose a model above."
                     : !on ? "Effect is off: switch it on to read and edit its values."
                     : !known ? "Reading values..." : "";
    lv_label_set_text(s_editor_info, info);
    lv_obj_set_hidden(s_editor_info, info[0] == 0);

    for (int idx = 0; m && idx < m->param_count && idx < NANO_MAX_PARAMS; idx++) {
        lv_obj_t *control = s_param_control[idx];
        if (!control) continue;
        const nano_param_t *p = &m->params[idx];
        if (known && idx < count) {
            lv_obj_remove_state(control, LV_STATE_DISABLED);
            if (p->kind == NANO_PARAM_ENUM) {
                int max = p->option_count > 1 ? p->option_count - 1 : 0;
                lv_dropdown_set_selected(control, (uint32_t)lroundf(values[idx] * max));
            } else {
                lv_slider_set_value(control, (int32_t)lroundf(values[idx] * 1000), LV_ANIM_OFF);
                format_param(text, sizeof(text), p, values[idx]);
                lv_label_set_text(s_param_value[idx], text);
            }
        } else {
            lv_obj_add_state(control, LV_STATE_DISABLED);
            if (s_param_value[idx]) lv_label_set_text(s_param_value[idx], "?");
        }
    }

    lv_obj_set_hidden(s_editor, false);
    lvgl_port_unlock();
}

void ui_fx_editor_close(void)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_model_list, true);
    lv_obj_set_hidden(s_editor, true);
    s_editor_slot_index = -1;
    lvgl_port_unlock();
}
