#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "fx_icons.h"
#include "fx_pedals.h"
#include "preset_icons.h"
#include "lvgl.h"

#define SCREEN_W 800
// Eight tiles in two rows, one per footswitch (1-4 top, 5-8 bottom), Quad Cortex style.
#define TILES 8
#define TILE_W 186
#define TILE_H 136
#define TILE_GAP 12
#define TILE_Y 188
// Swipe up: the tiles fill the screen (preset, capture and cab cards hidden); swipe down: back.
#define FULL_TILE_Y 10
#define FULL_TILE_H ((480 - 2 * FULL_TILE_Y - 8) / 2)
#define TUNER_TRACK_W 640
#define TUNER_NOTE_SIZE 160   // px

#define EDITOR_ROW_H 68

// Look of the desktop editor: dark gradient cards with a thin border, coloured icon squares.
#define CARD_TOP 0x17191B
#define CARD_BOTTOM 0x090A0B
#define CARD_BORDER 0x2E2E2E
#define TEXT 0xF2F2F2
#define MUTED 0x9A9A9A

static ui_command_cb s_on_command;
static ui_param_cb s_on_param;
static ui_text_cb s_on_text;
static lv_obj_t *s_save_button, *s_save_label, *s_ask, *s_ask_text, *s_rename, *s_rename_title, *s_rename_area;
static char s_ask_command;
static int s_current_preset;
static bool s_fx_mode;

static lv_obj_t *s_status_dot, *s_status, *s_edited, *s_app;
static lv_obj_t *s_preset_number, *s_preset_name, *s_capture, *s_cab;
static lv_obj_t *s_tile[TILES], *s_tile_caption[TILES], *s_tile_name[TILES], *s_tile_number[TILES];
static lv_obj_t *s_tile_square[TILES], *s_tile_pedal[TILES];
static uint32_t s_tile_ink[TILES];
static lv_obj_t *s_capture_square, *s_capture_slot_label, *s_cab_square, *s_cab_slot_label;
static lv_obj_t *s_picker, *s_picker_title, *s_picker_list, *s_picker_tab[2], *s_picker_chips, *s_picker_chip[LIB_CATEGORY_COUNT];
static lv_obj_t *s_picker_pager, *s_picker_page_label, *s_picker_info, *s_confirm, *s_confirm_text;
static lv_obj_t *s_bank_editor, *s_bank_title, *s_bank_swatch[UI_BANK_COLOR_COUNT], *s_bank_list;
static lv_obj_t *s_toast;
static lv_obj_t *s_usb, *s_usb_slider, *s_usb_value, *s_usb_info, *s_usb_minus, *s_usb_plus, *s_usb_reset;
static float s_usb_db;
static lv_obj_t *s_volume, *s_volume_info, *s_cab_settings, *s_cab_info;
static int s_capture_volume = NANO_CAPTURE_VOLUME_0DB;
static uint32_t s_volume_tick;
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
static lv_obj_t *s_tuner, *s_tuner_note, *s_tuner_needle, *s_tuner_cents;

// FX editor
static lv_obj_t *s_editor, *s_editor_icon, *s_editor_slot, *s_editor_model, *s_editor_onoff, *s_editor_onoff_label;
static lv_obj_t *s_editor_body, *s_editor_info, *s_model_list, *s_editor_pedal;
static lv_obj_t *s_param_control[NANO_MAX_PARAMS], *s_param_value[NANO_MAX_PARAMS];
static int s_editor_slot_index = -1;
static uint32_t s_editor_model_type;
static const nano_fx_model_t *s_editor_model_def;

// Black text on light tiles, white text on dark ones.
static uint32_t ink_for(uint32_t rgb)
{
    float r = ((rgb >> 16) & 0xff) / 255.0f, g = ((rgb >> 8) & 0xff) / 255.0f, b = (rgb & 0xff) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b < 0.3f ? 0xFFFFFF : 0x000000;
}

static void send(char command, int arg)
{
    if (s_on_command) s_on_command(command, arg);
}

static void on_previous(lv_event_t *e) { send('k', -1); }
static void on_refresh(lv_event_t *e) { send('r', 0); }
static void on_next(lv_event_t *e) { send('k', 1); }
static void on_tuner(lv_event_t *e) { send('w', 2); }
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

static void on_gesture(lv_event_t *e)
{
    s_gesture_tick = lv_tick_get();
    // Swipes work only on the main screen, not in an open dialog.
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_usb, s_mix_editor, s_learn, s_midi,
                                   s_volume, s_cab_settings };
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
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_text_letter_space(btn, 1, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
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
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollable(c, false);
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
    lv_obj_set_style_text_font(symbol, &lv_font_montserrat_28, 0);
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

// Largest font in which the name fits below the icon row without breaking a word (48 only in full screen).
static void fit_name(int i)
{
    static const lv_font_t *const fonts[] = { &lv_font_montserrat_48, &lv_font_montserrat_28, &lv_font_montserrat_20 };
    const char *name = lv_label_get_text(s_tile_name[i]);
    int room = s_fullscreen ? tile_height() - 66 : 2 * lv_font_get_line_height(&lv_font_montserrat_28);
    const lv_font_t *font = fonts[2];
    for (size_t f = s_fullscreen ? 0 : 1; f < 2; f++) {
        lv_point_t size;
        lv_text_get_size(&size, name, fonts[f], 0, 0, TILE_W - 20, LV_TEXT_FLAG_NONE);
        if (size.y <= room && words_fit(name, fonts[f], TILE_W - 20)) {
            font = fonts[f];
            break;
        }
    }
    lv_obj_set_style_text_font(s_tile_name[i], font, 0);
    lv_obj_align(s_tile_name[i], LV_ALIGN_BOTTOM_LEFT, 10, -8);
}

static void build_tile(lv_obj_t *parent, int i)
{
    lv_obj_t *tile = lv_obj_create(parent);
    s_tile[i] = tile;
    place_tile(i);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
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

    s_tile_square[i] = icon_square(tile, 46);
    lv_obj_set_pos(s_tile_square[i], 10, 10);
    lv_obj_set_style_border_width(s_tile_square[i], 2, 0);

    s_tile_caption[i] = label(tile, &lv_font_montserrat_14, TEXT);
    lv_obj_set_width(s_tile_caption[i], TILE_W - 100);
    lv_label_set_long_mode(s_tile_caption[i], LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_tile_caption[i], 64, 12);

    s_tile_number[i] = label(tile, &lv_font_montserrat_28, TEXT);
    lv_label_set_text_fmt(s_tile_number[i], "%d", i + 1);
    lv_obj_align(s_tile_number[i], LV_ALIGN_TOP_RIGHT, -12, 6);

    s_tile_name[i] = label(tile, &lv_font_montserrat_28, TEXT);
    lv_obj_set_width(s_tile_name[i], TILE_W - 20);
    lv_label_set_long_mode(s_tile_name[i], LV_LABEL_LONG_WRAP);
    lv_obj_align(s_tile_name[i], LV_ALIGN_BOTTOM_LEFT, 10, -8);
}

// One footswitch tile: filled with `color` when active, a muted version of it when not (like bypassed blocks
// on the Quad Cortex). icon or symbol go into the square; name is the big text at the bottom.
static void set_tile(int i, const char *caption, const char *name, uint32_t color, bool active, bool empty,
                     const lv_image_dsc_t *icon, const char *symbol)
{
    uint32_t bg = empty ? 0x1A1A1A : active ? color : blend(color, 0x1C1C1C, 0.36f);
    uint32_t ink = empty ? 0x5A5A5A : ink_for(bg);
    s_tile_ink[i] = ink;
    lv_obj_set_hidden(s_tile_pedal[i], true);   // shown again by set_tile_pedal
    lv_obj_set_style_bg_color(s_tile[i], lv_color_hex(bg), 0);
    lv_obj_set_style_bg_grad_dir(s_tile[i], LV_GRAD_DIR_NONE, 0);

    // Icon square: dimmed with the tile while the switch is not active, the icon in the tile's text colour.
    lv_obj_t *sq = s_tile_square[i];
    lv_obj_set_hidden(sq, !icon && !symbol);
    if (icon || symbol) {
        uint32_t square = empty ? 0x2A2A2A : active ? color : bg;
        set_icon_square(sq, square, icon, true);
        uint32_t border = empty ? 0x2A2A2A : active ? blend(color, 0x000000, 0.55f) : blend(color, 0x1C1C1C, 0.55f);
        lv_obj_set_style_border_color(sq, lv_color_hex(border), 0);
        lv_obj_t *sym = lv_obj_get_child(sq, 1);
        lv_label_set_text(sym, symbol ? symbol : "");
        lv_obj_set_style_text_color(sym, lv_color_hex(ink_for(square)), 0);
    }
    lv_obj_set_pos(s_tile_caption[i], (icon || symbol) ? 64 : 12, 12);
    lv_obj_set_width(s_tile_caption[i], (icon || symbol) ? TILE_W - 100 : TILE_W - 48);

    lv_label_set_text(s_tile_caption[i], caption);
    lv_obj_set_style_text_color(s_tile_caption[i], lv_color_hex(ink), 0);
    lv_obj_set_style_text_opa(s_tile_caption[i], 200, 0);
    lv_obj_set_style_text_color(s_tile_number[i], lv_color_hex(ink), 0);
    lv_obj_set_style_text_opa(s_tile_number[i], 170, 0);

    lv_label_set_text(s_tile_name[i], name);
    lv_obj_set_style_text_color(s_tile_name[i], lv_color_hex(ink), 0);
    fit_name(i);
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
    lv_obj_set_style_bg_color(btn, lv_color_hex(current ? 0x3A3D40 : 0x1C1E20), 0);
    lv_obj_set_style_border_width(btn, current ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(TEXT), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *l = label(btn, &lv_font_montserrat_20, current ? 0xFFFFFF : 0xD8D8D8);
    lv_obj_set_width(l, tag ? 440 : 520);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (tag) {
        lv_obj_t *t = label(btn, &lv_font_montserrat_14, 0x45F862);
        lv_label_set_text(t, tag);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return btn;
}

static void picker_section(const char *text)
{
    lv_obj_t *l = label(s_picker_list, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    lv_label_set_text(l, text);
}

static void style_tab(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? 0x3A3D40 : 0x191B1D), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(active ? TEXT : 0x3A3D40), 0);
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
    lv_label_set_text(s_picker_title, cab ? "CAB / IR" : "CAPTURE");
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
        picker_section("THE LIBRARY IS STILL LOADING...");
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
    if (!matches) picker_section("NOTHING IN THIS CATEGORY");
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
    lv_obj_t *l = label(btn, &lv_font_montserrat_14, TEXT);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static void build_picker(lv_obj_t *screen)
{
    s_picker = card(screen, 100, 20, 600, 450);
    lv_obj_set_style_border_color(s_picker, lv_color_hex(0x4A4D50), 0);
    s_picker_title = label(s_picker, &lv_font_montserrat_20, TEXT);
    lv_obj_set_style_text_letter_space(s_picker_title, 2, 0);
    lv_obj_set_pos(s_picker_title, 18, 18);
    s_picker_tab[0] = small_button(s_picker, 170, 8, 120, 44, "SLOTS", on_picker_tab, 0);
    s_picker_tab[1] = small_button(s_picker, 298, 8, 120, 44, "LIBRARY", on_picker_tab, 1);
    lv_obj_t *close = button(s_picker, 470, 8, 120, 44, on_picker_close);
    lv_obj_t *close_label = label(close, &lv_font_montserrat_20, TEXT);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    s_picker_info = label(s_picker, &lv_font_montserrat_14, MUTED);
    lv_obj_set_pos(s_picker_info, 18, 64);

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
    small_button(s_picker_pager, 0, 4, 120, 42, LV_SYMBOL_LEFT " PREV", on_picker_page, -1);
    small_button(s_picker_pager, 460, 4, 120, 42, "NEXT " LV_SYMBOL_RIGHT, on_picker_page, 1);
    s_picker_page_label = label(s_picker_pager, &lv_font_montserrat_20, MUTED);
    lv_obj_align(s_picker_page_label, LV_ALIGN_CENTER, 0, 0);

    // Confirmation before a library item replaces the slot content
    s_confirm = card(s_picker, 10, 330, 580, 110);
    lv_obj_set_style_border_color(s_confirm, lv_color_hex(TEXT), 0);
    s_confirm_text = label(s_confirm, &lv_font_montserrat_20, TEXT);
    lv_obj_set_width(s_confirm_text, 550);
    lv_label_set_long_mode(s_confirm_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_confirm_text, 14, 12);
    small_button(s_confirm, 300, 54, 130, 44, "CANCEL", on_confirm, 0);
    small_button(s_confirm, 440, 54, 130, 44, "LOAD", on_confirm, 1);
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
        lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x3A3D40 : 0x1C1E20), 0);
        lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(BANK_COLORS[s_be_color]), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_bank_preset, LV_EVENT_CLICKED, (void *)(intptr_t)preset);
        lv_obj_t *l = label(btn, &lv_font_montserrat_20, selected ? 0xFFFFFF : 0xD8D8D8);
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
    snprintf(text, sizeof(text), "BANK %d  -  SWITCH %d", s_be_bank + 1, tile + 3);
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
    s_bank_editor = card(screen, 50, 14, 700, 456);
    lv_obj_set_style_border_color(s_bank_editor, lv_color_hex(0x4A4D50), 0);
    s_bank_title = label(s_bank_editor, &lv_font_montserrat_20, TEXT);
    lv_obj_set_style_text_letter_space(s_bank_title, 2, 0);
    lv_obj_set_pos(s_bank_title, 18, 16);
    lv_obj_t *hint = label(s_bank_editor, &lv_font_montserrat_14, MUTED);
    lv_label_set_text(hint, "Colour, symbol and preset for this switch - stored on the controller");
    lv_obj_set_pos(hint, 18, 44);
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_bank_editor);
        s_bank_swatch[i] = sw;
        lv_obj_set_size(sw, 46, 46);
        lv_obj_set_pos(sw, 18 + i * 67, 66);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int i = 0; i <= PRESET_ICON_COUNT; i++) {   // "none" and the symbols, ten per row
        lv_obj_t *sq = icon_square(s_bank_editor, 46);
        s_bank_symbol[i] = sq;
        lv_obj_set_pos(sq, 18 + (i % 10) * 67, 120 + (i / 10) * 52);
        lv_obj_set_clickable(sq, true);
        lv_obj_add_event_cb(sq, on_symbol, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    s_bank_list = lv_obj_create(s_bank_editor);
    lv_obj_set_pos(s_bank_list, 10, 224);
    lv_obj_set_size(s_bank_list, 680, 164);
    lv_obj_set_style_bg_opa(s_bank_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bank_list, 0, 0);
    lv_obj_set_style_pad_all(s_bank_list, 4, 0);
    lv_obj_set_style_pad_row(s_bank_list, 6, 0);
    lv_obj_set_flex_flow(s_bank_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bank_list, LV_DIR_VER);
    small_button(s_bank_editor, 18, 398, 150, 46, "DEFAULT", on_bank_button, 2);
    small_button(s_bank_editor, 182, 398, 180, 46, "LEARN SWITCH", on_bank_button, 3);
    small_button(s_bank_editor, 380, 398, 150, 46, "CANCEL", on_bank_button, 0);
    small_button(s_bank_editor, 540, 398, 150, 46, "SAVE", on_bank_button, 1);
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
    lv_label_set_text_fmt(s_learn_title, "LEARN FOOTSWITCH %d", number);
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
    s_learn = card(screen, 190, 150, 420, 190);
    lv_obj_set_style_border_color(s_learn, lv_color_hex(0xFF7000), 0);
    s_learn_title = label(s_learn, &lv_font_montserrat_20, TEXT);
    lv_obj_set_style_text_letter_space(s_learn_title, 2, 0);
    lv_obj_set_pos(s_learn_title, 20, 16);
    s_learn_text = label(s_learn, &lv_font_montserrat_20, 0xD8D8D8);
    lv_obj_set_width(s_learn_text, 380);
    lv_label_set_long_mode(s_learn_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_learn_text, 20, 50);
    small_button(s_learn, 20, 128, 180, 46, "DEFAULT ORDER", on_learn_button, -1);
    small_button(s_learn, 220, 128, 180, 46, "CANCEL", on_learn_button, 0);
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
    s_ask = card(screen, 150, 150, 500, 180);
    lv_obj_set_style_border_color(s_ask, lv_color_hex(0xFF7000), 0);
    s_ask_text = label(s_ask, &lv_font_montserrat_20, TEXT);
    lv_obj_set_width(s_ask_text, 460);
    lv_label_set_long_mode(s_ask_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ask_text, 20, 18);
    small_button(s_ask, 160, 116, 150, 48, "CANCEL", on_ask, 0);
    small_button(s_ask, 330, 116, 150, 48, "SAVE", on_ask, 1);
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
    if (strlen(text) < 4) {
        ui_show_message("Preset names need at least 4 characters.");
        return;
    }
    if (s_on_text) s_on_text('N', text);
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

static void open_rename(lv_event_t *e)
{
    const char *name = s_preset_names[s_current_preset - 1];
    lv_label_set_text_fmt(s_rename_title, "RENAME PRESET %d", s_current_preset);
    lv_textarea_set_text(s_rename_area, name);
    lv_obj_set_hidden(s_rename, false);
    lv_obj_move_foreground(s_rename);
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

    s_rename_title = label(s_rename, &lv_font_montserrat_20, MUTED);
    lv_obj_set_style_text_letter_space(s_rename_title, 2, 0);
    lv_obj_set_pos(s_rename_title, 20, 22);
    small_button(s_rename, 470, 10, 150, 48, "CANCEL", on_rename_button, 0);
    small_button(s_rename, 630, 10, 150, 48, "RENAME", on_rename_button, 1);

    s_rename_area = lv_textarea_create(s_rename);
    lv_obj_set_pos(s_rename_area, 20, 72);
    lv_obj_set_size(s_rename_area, 760, 70);
    lv_textarea_set_one_line(s_rename_area, true);
    lv_textarea_set_max_length(s_rename_area, 32);
    lv_obj_set_style_text_font(s_rename_area, &lv_font_montserrat_28, 0);

    lv_obj_t *kb = lv_keyboard_create(s_rename);
    lv_obj_set_size(kb, 800, 320);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0xFF7000), 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2A1A0A), 0);
    lv_obj_t *l = label(s_toast, &lv_font_montserrat_20, TEXT);
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
    if (s_usb_db <= NANO_USB_GAIN_MIN_DB + 0.05f) lv_label_set_text(s_usb_value, "OFF");
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
    s_usb = card(screen, 140, 128, 520, 214);
    lv_obj_set_style_border_color(s_usb, lv_color_hex(0x4A4D50), 0);
    lv_obj_t *title = label(s_usb, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "USB AUDIO");
    lv_obj_set_pos(title, 20, 16);
    s_usb_info = label(s_usb, &lv_font_montserrat_14, MUTED);
    lv_obj_set_pos(s_usb_info, 20, 40);
    s_usb_value = label(s_usb, &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_set_width(s_usb_value, 200);
    lv_obj_set_style_text_align(s_usb_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_usb_value, LV_ALIGN_TOP_RIGHT, -20, 14);

    s_usb_minus = small_button(s_usb, 20, 80, 60, 52, "", on_usb_button, -1);
    lv_obj_t *minus = lv_obj_get_child(s_usb_minus, 0);
    lv_obj_set_style_text_font(minus, &lv_font_montserrat_28, 0);
    lv_label_set_text(minus, LV_SYMBOL_MINUS);
    s_usb_slider = lv_slider_create(s_usb);
    lv_slider_set_range(s_usb_slider, 0, USB_SLIDER_MAX);
    lv_obj_set_size(s_usb_slider, 316, 16);
    lv_obj_set_pos(s_usb_slider, 102, 98);
    lv_obj_set_ext_click_area(s_usb_slider, 22);
    lv_obj_set_style_bg_color(s_usb_slider, lv_color_hex(0xFF7000), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_usb_slider, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_usb_slider, 9, LV_PART_KNOB);
    lv_obj_add_event_cb(s_usb_slider, on_usb_slider, LV_EVENT_VALUE_CHANGED, NULL);
    s_usb_plus = small_button(s_usb, 440, 80, 60, 52, "", on_usb_button, 1);
    lv_obj_t *plus = lv_obj_get_child(s_usb_plus, 0);
    lv_obj_set_style_text_font(plus, &lv_font_montserrat_28, 0);
    lv_label_set_text(plus, LV_SYMBOL_PLUS);

    s_usb_reset = small_button(s_usb, 20, 152, 150, 46, "-6 dB", on_usb_button, 2);
    small_button(s_usb, 350, 152, 150, 46, "CLOSE", on_usb_button, 0);
    lv_obj_set_hidden(s_usb, true);
}

// ---- capture volume (VOL button, long press on the capture card) and cab settings (long press on the cab card) ----

// Slider rows: 0 = capture volume (the Nano's 0-255), 1-3 = cab output, high pass, low pass (0-1000 = 0-1).
#define LEVEL_ROWS (1 + NANO_CAB_SETTINGS)
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
    int32_t max = row == 0 ? 255 : CAB_SLIDER_MAX;
    if (position < 0) position = 0;
    if (position > max) position = max;
    if (move_slider) lv_slider_set_value(s_level_slider[row], position, LV_ANIM_OFF);
    show_level_value(row);
    if (row == 0) {
        s_capture_volume = (int)position;
        s_volume_tick = lv_tick_get();
    }
    send('v', row << 16 | (int)position);
}

static void on_level_slider(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    level_changed(row, lv_slider_get_value(s_level_slider[row]), false);
}

// Minus / plus (data = row << 1 | plus): to the next whole 1 dB, 10 Hz (high pass) or 100 Hz (low pass).
static void on_level_step(lv_event_t *e)
{
    static const float STEPS[LEVEL_ROWS] = { 1, 1, 10, 100 };
    int data = (int)(intptr_t)lv_event_get_user_data(e), row = data >> 1, dir = (data & 1) ? 1 : -1;
    int32_t position = lv_slider_get_value(s_level_slider[row]), next;
    if (row == 0) {
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
    lv_obj_set_style_text_font(minus, &lv_font_montserrat_28, 0);
    lv_label_set_text(minus, LV_SYMBOL_MINUS);
    lv_obj_t *slider = lv_slider_create(parent);
    s_level_slider[row] = slider;
    lv_slider_set_range(slider, 0, row == 0 ? 255 : CAB_SLIDER_MAX);
    lv_obj_set_size(slider, w - 164, 16);
    lv_obj_set_pos(slider, x + 82, y + 18);
    lv_obj_set_ext_click_area(slider, 22);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF7000), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 9, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_level_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)row);
    s_level_plus[row] = small_button(parent, x + w - 60, y, 60, 52, "", on_level_step, row << 1 | 1);
    lv_obj_t *plus = lv_obj_get_child(s_level_plus[row], 0);
    lv_obj_set_style_text_font(plus, &lv_font_montserrat_28, 0);
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
    s_volume = card(screen, 140, 128, 520, 214);
    lv_obj_set_style_border_color(s_volume, lv_color_hex(0x4A4D50), 0);
    lv_obj_t *title = label(s_volume, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "CAPTURE VOLUME");
    lv_obj_set_pos(title, 20, 16);
    s_volume_info = label(s_volume, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(s_volume_info, 290);
    lv_label_set_long_mode(s_volume_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_volume_info, 20, 40);
    s_level_value[0] = label(s_volume, &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_set_width(s_level_value[0], 180);
    lv_obj_set_style_text_align(s_level_value[0], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_level_value[0], LV_ALIGN_TOP_RIGHT, -20, 14);
    build_level_row(s_volume, 0, 20, 80, 480);
    small_button(s_volume, 20, 152, 150, 46, "0 dB", on_volume_button, 1);
    small_button(s_volume, 350, 152, 150, 46, "CLOSE", on_volume_button, 0);
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
    s_cab_settings = card(screen, 100, 56, 600, 368);
    lv_obj_set_style_border_color(s_cab_settings, lv_color_hex(0x4A4D50), 0);
    lv_obj_t *title = label(s_cab_settings, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "CAB / IR SETTINGS");
    lv_obj_set_pos(title, 20, 16);
    s_cab_info = label(s_cab_settings, &lv_font_montserrat_14, TEXT);
    lv_obj_set_width(s_cab_info, 420);
    lv_label_set_long_mode(s_cab_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_cab_info, 20, 40);
    small_button(s_cab_settings, 470, 12, 110, 44, "CLOSE", on_cab_settings_close, 0);
    for (int i = 0; i < NANO_CAB_SETTINGS; i++) {
        int row = 1 + i, y = 76 + i * 98;
        lv_obj_t *name = label(s_cab_settings, &lv_font_montserrat_14, MUTED);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, NAMES[i]);
        lv_obj_set_pos(name, 20, y);
        s_level_value[row] = label(s_cab_settings, &lv_font_montserrat_20, 0xFFFFFF);
        lv_obj_set_width(s_level_value[row], 200);
        lv_obj_set_style_text_align(s_level_value[row], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_level_value[row], LV_ALIGN_TOP_RIGHT, -20, y - 3);
        build_level_row(s_cab_settings, row, 20, y + 24, 560);
    }
    lv_obj_set_hidden(s_cab_settings, true);
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
        ? "Moving a slider plays that mix. SAVE stores both in the preset."
        : "This preset has no Pos 1 / Pos 2 yet: SAVE adds them (expression Amount of the reverb).");
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
    s_mix_editor = card(screen, 120, 96, 560, 304);
    lv_obj_set_style_border_color(s_mix_editor, lv_color_hex(0x00FFDD), 0);
    lv_obj_t *title = label(s_mix_editor, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "REVERB  -  FOOTSWITCH 8");
    lv_obj_set_pos(title, 20, 16);
    s_mix_model_label = label(s_mix_editor, &lv_font_montserrat_14, TEXT);
    lv_obj_align(s_mix_model_label, LV_ALIGN_TOP_RIGHT, -20, 16);
    static const char *const tabs[2] = { "MIX POS 1 / 2", "2ND REVERB" };
    for (int i = 0; i < 2; i++) {
        s_mix_tab[i] = small_button(s_mix_editor, 20 + i * 210, 42, 200, 40, tabs[i], on_mix_tab, i);
        s_mix_panel[i] = mix_panel(s_mix_editor, 94, 140);
    }

    // Tab 1: Pos 1 / Pos 2 of the expression assignment, played while moving
    lv_obj_t *mix = s_mix_panel[0];
    s_mix_info = label(mix, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(s_mix_info, 520);
    lv_label_set_long_mode(s_mix_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_mix_info, 20, 0);
    static const char *const names[2] = { "POS 1", "POS 2" };
    for (int i = 0; i < 2; i++) {
        int y = 30 + i * 54;
        lv_obj_t *name = label(mix, &lv_font_montserrat_20, 0xD8D8D8);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, names[i]);
        lv_obj_set_pos(name, 20, y + 10);
        s_mix_slider[i] = lv_slider_create(mix);
        lv_slider_set_range(s_mix_slider[i], 0, 255);
        lv_obj_set_size(s_mix_slider[i], 310, 16);
        lv_obj_set_pos(s_mix_slider[i], 118, y + 16);
        lv_obj_set_ext_click_area(s_mix_slider[i], 20);
        lv_obj_set_style_bg_color(s_mix_slider[i], lv_color_hex(0x00FFDD), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_mix_slider[i], lv_color_hex(0xF2F2F2), LV_PART_KNOB);
        lv_obj_set_style_pad_all(s_mix_slider[i], 9, LV_PART_KNOB);
        lv_obj_add_event_cb(s_mix_slider[i], on_mix_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        s_mix_value[i] = label(mix, &lv_font_montserrat_28, 0xFFFFFF);
        lv_obj_set_width(s_mix_value[i], 90);
        lv_obj_set_style_text_align(s_mix_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_mix_value[i], 450, y + 6);
    }

    // Tab 2: a second reverb; footswitch 8 swaps the preset's reverb (A) and this one (B)
    lv_obj_t *rev = s_mix_panel[1];
    lv_obj_t *info = label(rev, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(info, 520);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "Footswitch 8 switches between the preset's reverb (A) and a second one (B). "
                            "Reverb B and its settings are stored on the controller; EDIT B sets it up.");
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
    lv_obj_set_style_text_font(s_rev_dropdown, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_rev_dropdown), &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(s_rev_dropdown, on_rev_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    s_rev_edit = small_button(rev, 360, 56, 180, 52, "EDIT B", on_mix_button, 2);

    small_button(s_mix_editor, 200, 244, 160, 46, "CANCEL", on_mix_button, 0);
    small_button(s_mix_editor, 380, 244, 160, 46, "SAVE", on_mix_button, 1);
    lv_obj_set_hidden(s_mix_editor, true);
}

// ---- Bluetooth MIDI ----

static void fill_midi(void)
{
    const ui_midi_t *m = &s_midi_state;
    if (m->connected) lv_label_set_text_fmt(s_midi_status, LV_SYMBOL_BLUETOOTH " %s", m->name);
    else if (m->name[0]) lv_label_set_text_fmt(s_midi_status, "Waiting for %s", m->name);
    else lv_label_set_text(s_midi_status, "Not connected");
    lv_obj_set_style_text_color(s_midi_status, lv_color_hex(m->connected ? 0x45E35F : MUTED), 0);

    lv_obj_clean(s_midi_list);
    if (!m->count) {
        lv_obj_t *l = label(s_midi_list, &lv_font_montserrat_20, MUTED);
        lv_label_set_text(l, "Searching for Bluetooth MIDI devices...");
        return;
    }
    for (int i = 0; i < m->count; i++) {
        bool current = m->devices[i].remembered && m->connected;
        lv_obj_t *btn = lv_button_create(s_midi_list);
        lv_obj_set_size(btn, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current ? 0x1E3A24 : 0x1C1E20), 0);
        lv_obj_set_style_border_width(btn, current ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x45E35F), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_midi_device, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *name = label(btn, &lv_font_montserrat_20, TEXT);
        lv_label_set_text_fmt(name, "%s%s", m->devices[i].name, current ? "  -  connected" : m->devices[i].remembered ? "  -  stored" : "");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *rssi = label(btn, &lv_font_montserrat_14, MUTED);
        lv_label_set_text_fmt(rssi, "%d dBm", m->devices[i].rssi);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void ui_set_midi(const ui_midi_t *midi)
{
    lvgl_port_lock(0);
    s_midi_state = *midi;
    lv_obj_set_style_text_color(s_midi_label, lv_color_hex(midi->connected ? 0x45E35F : MUTED), 0);
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
    s_midi = card(screen, 110, 60, 580, 400);
    lv_obj_set_style_border_color(s_midi, lv_color_hex(0x4A4D50), 0);
    lv_obj_t *title = label(s_midi, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "BLUETOOTH MIDI");
    lv_obj_set_pos(title, 20, 16);
    s_midi_status = label(s_midi, &lv_font_montserrat_14, MUTED);
    lv_obj_align(s_midi_status, LV_ALIGN_TOP_RIGHT, -20, 16);
    lv_obj_t *info = label(s_midi, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(info, 540);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "Tap a device to connect it, e.g. an MC6 with a WIDI adapter. It reconnects by itself.");
    lv_obj_set_pos(info, 20, 40);
    lv_obj_t *map = label(s_midi, &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(map, 540);
    lv_label_set_long_mode(map, LV_LABEL_LONG_WRAP);
    lv_label_set_text(map, "PC 0-63 = presets   CC 37-41 = FX 1-5   CC 1 = reverb mix   CC 50-57 = footswitches 1-8");
    lv_obj_set_pos(map, 20, 292);

    s_midi_list = lv_obj_create(s_midi);
    lv_obj_set_pos(s_midi_list, 10, 82);
    lv_obj_set_size(s_midi_list, 560, 204);
    lv_obj_set_style_bg_opa(s_midi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_midi_list, 0, 0);
    lv_obj_set_style_pad_all(s_midi_list, 6, 0);
    lv_obj_set_style_pad_row(s_midi_list, 6, 0);
    lv_obj_set_flex_flow(s_midi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_midi_list, LV_DIR_VER);

    small_button(s_midi, 20, 336, 180, 46, "FORGET DEVICE", on_midi_button, -1);
    small_button(s_midi, 380, 336, 180, 46, "CLOSE", on_midi_button, 0);
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
                             lv_obj_t **square, lv_obj_t **name, lv_obj_t **slot)
{
    lv_obj_t *c = card(screen, x, 126, 385, 56);
    lv_obj_set_clickable(c, true);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x24272A), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(c, long_cb, LV_EVENT_LONG_PRESSED, NULL);
    *square = icon_square(c, 38);
    lv_obj_set_pos(*square, 10, 9);
    set_icon_square(*square, TEXT, NANO_ICONS[icon], true);
    lv_obj_t *t = label(c, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 58, 6);
    *slot = label(c, &lv_font_montserrat_14, MUTED);
    lv_obj_align(*slot, LV_ALIGN_TOP_RIGHT, -12, 6);
    *name = label(c, &lv_font_montserrat_20, TEXT);
    lv_obj_set_width(*name, 310);
    lv_label_set_long_mode(*name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(*name, 58, 25);
    return c;
}

static void build_tuner(lv_obj_t *screen)
{
    s_tuner = lv_obj_create(screen);
    lv_obj_set_size(s_tuner, 800, 480);
    lv_obj_set_pos(s_tuner, 0, 0);
    lv_obj_set_style_bg_color(s_tuner, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_tuner, 0, 0);
    lv_obj_set_style_radius(s_tuner, 0, 0);
    lv_obj_set_scrollable(s_tuner, false);
    lv_obj_set_clickable(s_tuner, true);
    lv_obj_add_event_cb(s_tuner, on_tuner, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = label(s_tuner, &lv_font_montserrat_20, 0x9A9A9A);
    lv_label_set_text(title, "TUNER  -  tap or footswitch 2 to close");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    // The note in a large TTF font (a scaled bitmap font did not show up on the display).
    extern const uint8_t montserrat_ttf_start[] asm("_binary_Montserrat_Medium_ttf_start");
    extern const uint8_t montserrat_ttf_end[] asm("_binary_Montserrat_Medium_ttf_end");
    const lv_font_t *note_font = lv_tiny_ttf_create_data_ex(montserrat_ttf_start, montserrat_ttf_end - montserrat_ttf_start,
                                                            TUNER_NOTE_SIZE, LV_FONT_KERNING_NORMAL, 16);
    s_tuner_note = label(s_tuner, note_font ? note_font : &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_width(s_tuner_note, 600);
    lv_obj_set_style_text_align(s_tuner_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_tuner_note, LV_ALIGN_CENTER, 0, -76);

    lv_obj_t *track = lv_obj_create(s_tuner);
    lv_obj_set_size(track, TUNER_TRACK_W, 14);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 7, 0);
    lv_obj_set_scrollable(track, false);
    lv_obj_t *center = lv_obj_create(s_tuner);
    lv_obj_set_size(center, 4, 50);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xBDBDBD), 0);
    lv_obj_set_style_border_width(center, 0, 0);

    s_tuner_needle = lv_obj_create(s_tuner);
    lv_obj_set_size(s_tuner_needle, 12, 70);
    lv_obj_set_style_radius(s_tuner_needle, 6, 0);
    lv_obj_set_style_border_width(s_tuner_needle, 0, 0);
    lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, 0, 80);

    s_tuner_cents = label(s_tuner, &lv_font_montserrat_28, 0xBDBDBD);
    lv_obj_align(s_tuner_cents, LV_ALIGN_CENTER, 0, 150);

    lv_obj_set_hidden(s_tuner, true);
}

static void build_editor(lv_obj_t *screen);

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

    // Preset card: status line, preset number and name, amp silhouette behind it
    lv_obj_t *preset = card(screen, 10, 8, 780, 112);
    s_preset_card = preset;
    lv_obj_t *amp = lv_image_create(preset);
    lv_image_set_src(amp, &NANO_AMP_SILHOUETTE);
    lv_image_set_scale(amp, 220);
    lv_obj_set_style_image_recolor(amp, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_image_recolor_opa(amp, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(amp, 70, 0);
    lv_obj_align(amp, LV_ALIGN_CENTER, 0, 2);
    lv_obj_t *prev = button(preset, 8, 10, 70, 90, on_previous);
    lv_obj_t *prev_label = label(prev, &lv_font_montserrat_48, TEXT);
    lv_label_set_text(prev_label, LV_SYMBOL_LEFT);
    lv_obj_center(prev_label);
    lv_obj_t *next = button(preset, 700, 10, 70, 90, on_next);
    lv_obj_t *next_label = label(next, &lv_font_montserrat_48, TEXT);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);

    // Status line between the top buttons: dot, status / preset and bank, EDITED, APP
    lv_obj_t *status_row = lv_obj_create(preset);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 12, 0);
    lv_obj_set_clickable(status_row, false);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, (188 + 504) / 2 - 390, 11);   // centred between VOL and MIDI
    s_status_dot = lv_obj_create(status_row);
    lv_obj_set_size(s_status_dot, 10, 10);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    s_status = label(status_row, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(s_status, 2, 0);
    s_preset_number = label(status_row, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(s_preset_number, 2, 0);
    s_edited = label(status_row, &lv_font_montserrat_14, 0xFF7000);   // unsaved changes: the SAVE button turns orange
    lv_obj_set_hidden(s_edited, true);
    s_app = label(status_row, &lv_font_montserrat_14, 0x3D8BFF);    // editor connected through the controller
    lv_obj_set_style_text_letter_space(s_app, 2, 0);
    lv_label_set_text(s_app, "APP");
    lv_obj_set_hidden(s_app, true);
    lv_obj_t *refresh = button(preset, 86, 4, 44, 30, on_refresh);   // read everything from the Nano again
    lv_obj_t *refresh_label = label(refresh, &lv_font_montserrat_14, MUTED);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_center(refresh_label);
    lv_obj_t *volume = button(preset, 136, 4, 52, 30, open_volume);   // capture volume
    lv_obj_t *volume_label = label(volume, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(volume_label, 1, 0);
    lv_label_set_text(volume_label, "VOL");
    lv_obj_center(volume_label);
    lv_obj_t *midi = button(preset, 504, 4, 52, 30, open_midi);
    s_midi_label = label(midi, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(s_midi_label, 1, 0);
    lv_label_set_text(s_midi_label, "MIDI");
    lv_obj_center(s_midi_label);
    lv_obj_t *usb = button(preset, 560, 4, 52, 30, open_usb);
    lv_obj_t *usb_label = label(usb, &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_letter_space(usb_label, 1, 0);
    lv_label_set_text(usb_label, "USB");
    lv_obj_center(usb_label);
    s_save_button = button(preset, 616, 4, 76, 30, on_save_button);
    s_save_label = label(s_save_button, &lv_font_montserrat_14, TEXT);
    lv_obj_set_style_text_letter_space(s_save_label, 2, 0);
    lv_label_set_text(s_save_label, "SAVE");
    lv_obj_center(s_save_label);
    s_preset_name = label(preset, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_width(s_preset_name, 606);
    lv_obj_set_style_text_align(s_preset_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_preset_name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_preset_name, 86, 40);
    lv_obj_set_clickable(s_preset_name, true);   // long press: rename
    lv_obj_add_event_cb(s_preset_name, open_rename, LV_EVENT_LONG_PRESSED, NULL);

    // Capture and cab cards
    s_source_card[0] = source_card(screen, 10, "CAPTURE", NANO_ICON_CAPTURE, open_capture_picker, open_volume,
                                   &s_capture_square, &s_capture, &s_capture_slot_label);
    s_source_card[1] = source_card(screen, 405, "CAB / IR", NANO_ICON_CAB, open_cab_picker, open_cab_settings,
                                   &s_cab_square, &s_cab, &s_cab_slot_label);

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
    build_mix_editor(screen);
    build_learn(screen);
    build_midi(screen);
    build_toast(screen);

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
    for (int i = 0; i < TILES; i++) {
        place_tile(i);
        fit_name(i);
    }
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
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(connected ? 0x45E35F : 0x555555), 0);
    lv_label_set_text(s_status, connected ? "CONNECTED" : "SEARCHING FOR NANO");
    lv_obj_set_hidden(s_status, false);   // until the first preset is shown
    if (!connected) {
        set_fullscreen(false);   // show the search status
        lv_obj_set_hidden(s_edited, true);
        lv_obj_set_hidden(s_preset_number, true);
        lv_obj_set_hidden(s_usb, true);
        lv_obj_set_hidden(s_volume, true);
        lv_obj_set_hidden(s_cab_settings, true);
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
    lv_obj_set_style_image_recolor(s_tile_pedal[i], lv_color_hex(s_tile_ink[i]), 0);
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
        set_tile(2 + slot, NANO_FX_SLOT_NAMES[slot], empty ? "Empty" : nano_fx_name(st->fx_type[slot]),
                 model ? model->color : 0x6A6A6A, on, empty, model ? NANO_ICONS[model->icon] : NULL, NULL);
        if (!empty) set_tile_pedal(2 + slot, st->fx_type[slot]);
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

    snprintf(text, sizeof(text), "PRESET %d   BANK %d", st->current_preset, view->bank + 1);
    lv_label_set_text(s_preset_number, text);
    lv_obj_set_hidden(s_preset_number, false);
    lv_obj_set_hidden(s_status, true);    // the green dot says "connected"
    s_current_preset = st->current_preset;
    lv_obj_set_style_bg_color(s_save_button, lv_color_hex(st->dirty ? 0xFF7000 : 0x191B1D), 0);
    lv_obj_set_style_bg_grad_dir(s_save_button, st->dirty ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_text_color(s_save_label, lv_color_hex(st->dirty ? 0x000000 : MUTED), 0);
    const char *name = st->preset_names[st->current_preset - 1];
    lv_label_set_text(s_preset_name, st->names_loaded && name[0] ? name : "-");
    memcpy(s_capture_names, st->capture_names, sizeof(s_capture_names));
    memcpy(s_cab_names, st->cab_names, sizeof(s_cab_names));
    s_capture_slot = st->capture_slot;
    memcpy(s_preset_names, st->preset_names, sizeof(s_preset_names));
    s_view = *view;
    s_cab_slot = st->cab_slot;
    lv_label_set_text(s_capture, st->capture_slot ? (st->capture[0] ? st->capture : "-") : "Bypassed");
    lv_obj_set_style_text_color(s_capture, lv_color_hex(st->capture_slot ? TEXT : MUTED), 0);
    set_icon_square(s_capture_square, TEXT, NANO_ICONS[NANO_ICON_CAPTURE], st->capture_slot != 0);
    if (st->capture_slot) snprintf(text, sizeof(text), "BANK %d  -  %d", (st->capture_slot - 1) / 5 + 1, (st->capture_slot - 1) % 5 + 1);
    else snprintf(text, sizeof(text), "BYPASS");
    lv_label_set_text(s_capture_slot_label, text);
    lv_label_set_text(s_cab, st->cab_slot ? (st->cab[0] ? st->cab : "-") : "Bypassed");
    lv_obj_set_style_text_color(s_cab, lv_color_hex(st->cab_slot ? TEXT : MUTED), 0);
    set_icon_square(s_cab_square, TEXT, NANO_ICONS[NANO_ICON_CAB], st->cab_slot != 0);
    if (st->cab_slot) snprintf(text, sizeof(text), "SLOT %d", st->cab_slot);
    else snprintf(text, sizeof(text), "BYPASS");
    lv_label_set_text(s_cab_slot_label, text);
    // Capture volume: not while the slider is held or right after a change on the board (older replies).
    if (lv_obj_is_hidden(s_volume) ||
        (lv_tick_elaps(s_volume_tick) > VOLUME_HOLD_MS && !lv_obj_has_state(s_level_slider[0], LV_STATE_PRESSED))) {
        s_capture_volume = st->capture_volume;
        if (!lv_obj_is_hidden(s_volume)) show_capture_volume();
    }

    if (view->fx_mode) show_fx_tiles(st, view);
    else show_preset_tiles(st, view);

    lvgl_port_unlock();
}

void ui_show_tuner(bool open)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_tuner, !open);
    if (open) {
        lv_label_set_text(s_tuner_note, "-");
        lv_label_set_text(s_tuner_cents, "");
        lv_obj_set_style_bg_color(s_tuner_needle, lv_color_hex(0x555555), 0);
        lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, 0, 80);
    }
    lvgl_port_unlock();
}

void ui_show_tuner_reading(const nano_tuner_reading_t *reading)
{
    char text[24];
    lvgl_port_lock(0);
    if (!reading->valid) {
        lv_label_set_text(s_tuner_note, "-");
        lv_label_set_text(s_tuner_cents, "");
        lv_obj_set_style_bg_color(s_tuner_needle, lv_color_hex(0x555555), 0);
        lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, 0, 80);
    } else {
        float cents = reading->cents < -50 ? -50 : reading->cents > 50 ? 50 : reading->cents;
        bool in_tune = reading->centered || fabsf(cents) <= 2;
        uint32_t color = in_tune ? 0x45E35F : 0xFF7000;
        lv_label_set_text(s_tuner_note, reading->note);
        lv_obj_set_style_text_color(s_tuner_note, lv_color_hex(in_tune ? 0x45E35F : 0xFFFFFF), 0);
        snprintf(text, sizeof(text), "%+.1f ct", (double)cents);
        lv_label_set_text(s_tuner_cents, text);
        lv_obj_set_style_bg_color(s_tuner_needle, lv_color_hex(color), 0);
        lv_obj_align(s_tuner_needle, LV_ALIGN_CENTER, (int)(cents / 50 * (TUNER_TRACK_W / 2)), 80);
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

static void on_model_button(lv_event_t *e)
{
    lv_obj_set_hidden(s_model_list, !lv_obj_is_hidden(s_model_list));
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
    lv_obj_t *back_label = label(back, &lv_font_montserrat_20, 0xF2F2F2);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    s_editor_icon = lv_image_create(s_editor);
    lv_obj_set_pos(s_editor_icon, 142, 14);
    lv_obj_set_style_image_recolor_opa(s_editor_icon, LV_OPA_COVER, 0);
    s_editor_slot = label(s_editor, &lv_font_montserrat_20, 0x9A9A9A);
    lv_obj_set_pos(s_editor_slot, 188, 20);

    lv_obj_t *model = button(s_editor, 300, 8, 300, 48, on_model_button);
    s_editor_model = label(model, &lv_font_montserrat_20, 0xF2F2F2);
    lv_obj_center(s_editor_model);

    s_editor_onoff = button(s_editor, 612, 8, 180, 48, on_editor_onoff);
    s_editor_onoff_label = label(s_editor_onoff, &lv_font_montserrat_20, 0xF2F2F2);
    lv_obj_center(s_editor_onoff_label);

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

// One row per parameter, in the model's display order.
static void build_param_rows(void)
{
    lv_obj_clean(s_editor_body);
    memset(s_param_control, 0, sizeof(s_param_control));
    memset(s_param_value, 0, sizeof(s_param_value));
    s_editor_info = label(s_editor_body, &lv_font_montserrat_20, 0xFF7000);
    lv_obj_set_pos(s_editor_info, 16, 10);

    const nano_fx_model_t *m = s_editor_model_def;
    if (!m) return;
    uint32_t color = m->color;
    for (int row = 0; row < m->param_count; row++) {
        int idx = m->order ? m->order[row] : row;
        if (idx >= m->param_count || idx >= NANO_MAX_PARAMS) continue;
        const nano_param_t *p = &m->params[idx];
        int y = 44 + row * EDITOR_ROW_H;

        lv_obj_t *name = label(s_editor_body, &lv_font_montserrat_20, 0xD8D8D8);
        lv_label_set_text(name, p->name);
        lv_obj_set_width(name, 196);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, 16, y + 20);

        if (p->kind == NANO_PARAM_ENUM) {
            lv_obj_t *dd = lv_dropdown_create(s_editor_body);
            lv_dropdown_set_options_static(dd, p->options);
            lv_obj_set_size(dd, 300, 52);
            lv_obj_set_pos(dd, 220, y + 6);
            lv_obj_set_style_text_font(dd, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &lv_font_montserrat_20, 0);
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
            s_param_value[idx] = label(s_editor_body, &lv_font_montserrat_20, 0xFFFFFF);
            lv_obj_set_width(s_param_value[idx], 130);
            lv_obj_set_style_text_align(s_param_value[idx], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(s_param_value[idx], 656, y + 20);
        }
    }
}

// The models that the Nano allows in this slot.
static void build_model_list(int slot)
{
    lv_obj_clean(s_model_list);
    for (int i = 0; i < NANO_SLOT_MODEL_COUNT[slot]; i++) {
        uint32_t type = NANO_SLOT_MODELS[slot][i];
        const nano_fx_model_t *m = nano_fx_model(type);
        if (!m) continue;
        lv_obj_t *btn = lv_button_create(s_model_list);
        lv_obj_set_size(btn, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(type == s_editor_model_type ? 0x3A3A3A : 0x262626), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_model_choice, LV_EVENT_CLICKED, (void *)(uintptr_t)type);
        if (NANO_ICONS[m->icon]) {
            lv_obj_t *icon = lv_image_create(btn);
            lv_image_set_src(icon, NANO_ICONS[m->icon]);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(m->color), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, -4, 0);
        }
        lv_obj_t *name = label(btn, &lv_font_montserrat_20, m->color);
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
    lv_label_set_text(s_editor_onoff_label, on ? "ON" : "OFF");
    lv_obj_set_style_bg_color(s_editor_onoff, lv_color_hex(on ? color : 0x1C1C1C), 0);
    lv_obj_set_style_text_color(s_editor_onoff_label, lv_color_hex(on ? ink_for(color) : 0xF2F2F2), 0);

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
