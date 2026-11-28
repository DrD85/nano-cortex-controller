#include "ui.h"
#include "ui_fonts.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_lvgl_port.h"
#include "fx_icons.h"
#include "fx_pedals.h"
#include "pedal_assets.h"
#include "fx_focus.h"
#include "fx_info.h"
#include "fx_neural.h"
#include "drum_machine.h"
#include "library.h"
#include "ui_icons.h"
#include "preset_icons.h"
#include "lvgl.h"

// P4 board: 1024 x 600. The main screen sits on four columns of 241 px, 12 px apart (the tiles); the top bar is four
// boxes over those columns (drafts/entwurf-p4-6 and -7, chosen 2026-11-13).
#define SCREEN_W 1024
#define SCREEN_H 600
#define MARGIN 12
#define COL_W 241
#define COL_X(c) (MARGIN + (c) * (COL_W + MARGIN))
// Eight tiles in two rows, one per footswitch (1-4 top, 5-8 bottom), Quad Cortex style.
#define TILES 8
#define TILE_W COL_W
#define TILE_H 166
#define TILE_GAP 12
#define TILE_Y 244
// A tile's note (what holding does, the looper's state ...): its own line under the symbol row, in the smaller font
// when it is too long for the larger one
#define NOTE_FONT (&ui_font_20)
#define NOTE_FONT_SMALL (&ui_font_16)
#define NOTE_FONT_GIG (&ui_font_24)
#define NOTE_Y 50
#define NOTE_Y_GIG 54
#define NOTE_ICON 22          // the symbol in front of a note (pedal, scene), px
// Swipe up: the gig view - a slim status bar (preset, bank, mode) and the tiles over the rest of the screen;
// swipe down: back.
#define GIG_BAR_H 52
#define FULL_TILE_Y (GIG_BAR_H + 12)
#define FULL_TILE_H ((SCREEN_H - FULL_TILE_Y - TILE_GAP - MARGIN) / 2)
// The calm system of the P4 board: one surface, one hairline, one radius; captions small grey capitals, values white;
// colour only where it means something (effect type, bank colour, green = running).
#define SURF 0x121416
#define SURF_ON 0x262A2F      // pressed / chosen
#define HAIRLINE 0x25292D
#define CAPTION 0x8E949B
#define DIMMED 0x5A6067
#define RADIUS 12
#define FILL 0x3A3F45         // fill bars
#define TILE_DIM 0.30f        // share of the colour behind a tile that is off
#define BUTTON_BG 0x1A1D20    // buttons on a panel
#define BUTTON_LINE 0x30353A
#define CENTER_X ((SCREEN_W - 800) / 2)   // dialogs laid out for 800 x 480 sit in the middle of the screen
#define CENTER_Y ((SCREEN_H - 480) / 2)
#define TUNER_TRACK_W 800
#define TUNER_NOTE_SIZE 160   // px

// Look of the desktop editor (1.5): dark gradient cards with a thin border, dialogs as sheets with a title and a
// round close button, filled = on / outlined = off, one set of corner radii (controls 9, panels 12, cards 16,
// dialogs 18), IBM Plex Sans (ui_fonts.c).
#define CARD_TOP 0x17191B
#define CARD_BOTTOM 0x090A0B
#define CARD_BORDER 0x2A2E33
#define TEXT 0xF2F2F2
// Secondary text. On this panel (217 ppi) 14 px is the smallest size that can still be read at arm's length: captions
// are 14 px, hints 16 px (they were 12 and 14), in a grey light enough for the dark panels.
#define MUTED 0xA9ADB3
#define CAPTION_FONT (&ui_font_14)
#define HINT_FONT (&ui_font_16)
#define INK 0x0D0F11          // dark text on light fills
#define TILE_OFF_BG 0x111315
#define PANEL_BG 0x121416
#define GREEN 0x45F862
#define SEL_BLUE 0x4D8DFF     // P4 player: a song selected, not playing yet (the jam tracks' blue)
#define FILL_LINE 0x3A3F45    // the edge of a dialog
#define ORANGE 0xFFB02E       // unsaved changes (the dot next to the name)

static ui_command_cb s_on_command;
static ui_param_cb s_on_param;
static ui_text_cb s_on_text;
static ui_net_t s_net;   // (P4: the Wi-Fi status, taken over 10 times a second in loop_poll)
static void phone_action(ui_phone_action_t a, int value);
static void show_wifi_page(void);   // (P4: Settings > Wi-Fi and the list of networks, see "Wi-Fi" below)
static void netlist_open(void);
static lv_obj_t *s_save_button, *s_ask, *s_ask_text, *s_ask_ok, *s_rename, *s_rename_title, *s_rename_area;
static lv_obj_t *s_rename_ok, *s_rename_keyboard;
static char s_rename_kind = 'N';   // 'N' preset name, '1'-'4' FX preset
static lv_obj_t *s_cap;   // the capture assistant (Settings > Capture)
static void cap_named(const char *text);
static char s_ask_command;
static int s_ask_arg;
static int s_current_preset;
static bool s_fx_mode;
static bool s_looper_mode;   // the tiles are the looper's switches (P4: the tuner's tile or footswitch held)
static bool s_player_mode;   // P4: the tiles are the player's switches (footswitch 1 held, 2026-11-26)

static lv_obj_t *s_main;   // everything of the main screen (hidden under the full-screen editor and tuner)
static lv_obj_t *s_status_dot, *s_status, *s_edited, *s_app, *s_midi_icon, *s_preset_chip, *s_preset_chip_text;
static lv_obj_t *s_gig_bar, *s_gig_dot, *s_gig_number, *s_gig_bank, *s_gig_name, *s_gig_dirty, *s_gig_mode, *s_gig_view;
// Pedal view: in the full-screen view's FX mode the five slots as pedals on a shelf instead of tiles (the "PEDALS"
// field of its bar). A pedal's picture comes from the picture file in the flash memory (pedal_assets.h); a model
// without one - and every model in the published firmware - is drawn: colour, symbol, name.
static lv_obj_t *s_pedals, *s_pv_slot[NANO_FX_SLOTS], *s_pv_image[NANO_FX_SLOTS], *s_pv_drawn[NANO_FX_SLOTS], *s_pv_icon[NANO_FX_SLOTS],
                *s_pv_led[NANO_FX_SLOTS], *s_pv_name[NANO_FX_SLOTS], *s_pv_key[NANO_FX_SLOTS], *s_pv_cap[NANO_FX_SLOTS], *s_pv_num[NANO_FX_SLOTS],
                *s_pv_rp, *s_pv_rp_name, *s_pv_rp_num;   // above the reverb's pedal: the setting the reverb switch is on
static lv_obj_t *s_pv_ab[NANO_FX_SLOTS], *s_pv_ab_icon[NANO_FX_SLOTS], *s_pv_ab_name[NANO_FX_SLOTS];   // above a pedal: the slot's other effect (A / B)
static bool s_pedal_view;     // chosen (kept by the controller)
static bool s_pedals_shown;   // on the screen now: chosen, full screen, FX mode
static lv_obj_t *s_capture_title;
static uint32_t s_tile_accent[TILES];   // colour of the pedal drawing on the tile
static lv_obj_t *s_preset_number, *s_preset_name, *s_capture, *s_cab;
static lv_obj_t *s_prev_box, *s_next_box;   // the arrows beside the preset's name
static lv_obj_t *s_presets;                 // the preset list (a tap on the preset's name)
static lv_obj_t *s_tile[TILES], *s_tile_caption[TILES], *s_tile_name[TILES], *s_tile_number[TILES];
static lv_obj_t *s_tile_square[TILES], *s_tile_pedal[TILES];
static lv_obj_t *s_system;                 // the system page
static lv_obj_t *s_tile_other[TILES];   // second line under the caption: the other effect of an A/B slot
static uint32_t s_tile_ink[TILES];
static uint32_t s_tile_pedal_type[TILES], s_tile_other_type[TILES];   // shown by show_tile_extras
static char s_tile_note_text[TILES][24];    // a note that is not a fixed text (the start scene of a song)
static const char *s_tile_note[TILES];   // a line of text under the caption instead (looper mode, what holding does), NULL = none
static const lv_image_dsc_t *s_tile_note_icon[TILES];   // ... with this symbol in front of it, NULL = none
static lv_obj_t *s_tile_note_image[TILES];
static lv_obj_t *s_tile_badge[TILES];   // beside the symbol: how the footswitch is used for the note's line (HOLD / TAP / PRESS)
static uint32_t s_tile_color[TILES];   // the tile's colour and state as set_tile drew it (for the editor)
static bool s_tile_active[TILES], s_tile_empty[TILES], s_tiles_drawn;
static volatile bool s_tiles_stale;   // P4: the looper's tiles were drawn anew by the screen's own task (loop_poll)
static uint32_t s_tile_head[TILES];   // colour of a tile that is not active, dimmed behind its top row (0 = none)
static lv_obj_t *s_capture_square, *s_capture_slot_label, *s_cab_square, *s_cab_slot_label;
static lv_obj_t *s_picker, *s_picker_title, *s_picker_list, *s_picker_tab[2], *s_picker_chips, *s_picker_chip[LIB_CATEGORY_COUNT];
static lv_obj_t *s_picker_pager, *s_picker_page_label, *s_picker_info, *s_confirm, *s_confirm_text;
static lv_obj_t *s_listen_bar, *s_listen_name, *s_listen_note;
static char s_listen_kind, s_listen_name_text[64];   // what is being listened to ('C' / 'I', 0 = nothing)
static int s_listen_pos;
static bool s_listen_pending;
static lv_obj_t *s_tempo_button, *s_tempo_button_icon, *s_tempo_button_text;
static lv_obj_t *s_metro, *s_metro_bpm, *s_metro_scope[2], *s_metro_switch, *s_metro_vol, *s_metro_vol_text, *s_metro_sig;
static lv_obj_t *s_metro_note[4], *s_metro_sound[2], *s_metro_dots, *s_metro_hint;
static lv_timer_t *s_metro_beat_timer, *s_metro_hide_timer;
static uint32_t s_metro_news_tick;   // when the dialog came up or last got news (see metro_hide)
static ui_tempo_t s_tempo_ui = { .tempo = 120, .muted = true, .time_sig = 2 };
static int s_metro_beat;
static uint32_t s_metro_taps[6];
static int s_metro_tap_count;
static lv_obj_t *s_bank_editor, *s_bank_title, *s_bank_swatch[UI_BANK_COLOR_COUNT], *s_bank_list;
static lv_obj_t *s_toast;
static lv_obj_t *s_usb, *s_usb_slider, *s_usb_value, *s_usb_info, *s_usb_minus, *s_usb_plus, *s_usb_reset;
static float s_usb_db;
static lv_obj_t *s_cab_settings, *s_cab_info, *s_amp, *s_amp_info;
static lv_obj_t *s_rot;   // the wheel while a rotary footswitch of the Nano is turned
// P4: the USB mark, the phone in the top bar and its player, the signal chain under the preset name
#define PRESET_NAME_W 820
#define CHAIN_FIELDS 8        // Pre FX 1-2, capture, cab, Post FX 1-3, output
static lv_obj_t *s_usb_mark, *s_phone_box, *s_phone_play, *s_phone_title, *s_phone_time, *s_phone_track, *s_phone_fill;
static lv_obj_t *s_phone, *s_ph_caption, *s_ph_title, *s_ph_sub, *s_ph_scrub_fill, *s_ph_knob, *s_ph_pos, *s_ph_len;
static lv_obj_t *s_ph_pause, *s_ph_pause_label, *s_ph_vol, *s_ph_vol_value, *s_ph_mute;
static ui_phone_cb s_phone_cb;
static ui_phone_t s_phone_state;
static lv_obj_t *s_chain, *s_chain_dot[CHAIN_FIELDS], *s_chain_cap[CHAIN_FIELDS], *s_chain_name[CHAIN_FIELDS];
static lv_obj_t *s_out_fill, *s_out_value, *s_tempo_line;
static void open_phone(lv_event_t *e);
static lv_obj_t *s_jam;   // P4: the jam player
static int s_source = 1;  // P4: the top bar's player, 0 = the phone, 1 = the jam tracks (first since 2026-11-26)
static void open_source(int source);
static void show_jam_field(void);
static void show_onboard_looper_tiles(void);
static void show_loopbar(void);
static void on_level_open(lv_event_t *e);
static void build_loopbar(lv_obj_t *parent);
static void build_playerbar(lv_obj_t *parent);
static void show_playerbar(void);
static void show_player_tiles(void);
static bool player_pending(void);
static void build_jam(lv_obj_t *screen);
static void source_chooser(lv_obj_t *parent, int x, int y, int chosen);
static void on_phone_toggle(lv_event_t *e);
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h);
static lv_obj_t *seg(lv_obj_t *b, int x, int w, lv_event_cb_t cb);
static void vline(lv_obj_t *b, int x);
static lv_obj_t *fill_bar(lv_obj_t *parent, int x, int y, int w, const char *name, lv_obj_t **value);
static lv_obj_t *calm_switch(lv_obj_t *parent);
static void on_set_phone_volume(lv_event_t *e);
static void on_set_phone_mute(lv_event_t *e);
// Settings screen (1.17.0): topics on the left, the chosen one on the right; MIDI, USB audio and System are its pages
// (P4: Audio = the USB page with the phone; Phone and Display are new)
enum { SET_NANO, SET_USB, SET_PHONE, SET_WIFI, SET_MIDI, SET_SWITCHES, SET_LAYOUT, SET_DISPLAY, SET_SYSTEM, SET_CAPTURE, SET_COUNT };
#define SET_X COL_X(1)                               // the topic's page: columns 2-4
#define SET_W (SCREEN_W - MARGIN - COL_X(1))
static lv_obj_t *s_set_phone_vol, *s_set_phone_vol_value, *s_set_phone_mute;
static lv_obj_t *s_set_bright, *s_set_bright_value;
static void on_set_brightness(lv_event_t *e);
static lv_obj_t *s_settings, *s_set_content, *s_set_item[SET_COUNT], *s_set_item_label[SET_COUNT], *s_set_item_icon[SET_COUNT];
static lv_obj_t *s_set_badge[SET_COUNT], *s_set_page[SET_COUNT], *s_set_nano_text, *s_set_layout_text, *s_set_cloud;
static int s_set_topic = SET_NANO;
static bool s_nano_connected;
static int s_nano_fw_major = -1, s_nano_fw_minor = -1;
static lv_obj_t *s_midi_close, *s_midi_more[2], *s_usb_close, *s_system_close;
static int s_capture_volume = NANO_CAPTURE_VOLUME_0DB;
static int s_amp_values[NANO_AMP_KNOBS];
static uint32_t s_volume_tick, s_amp_tick;
static lv_obj_t *s_mix_editor, *s_mix_model_label, *s_mix_info, *s_mix_slider[2], *s_mix_value[2];
static lv_obj_t *s_mix_tab[3], *s_mix_panel[3], *s_rev_dropdown, *s_rev_edit, *s_mix_free;   // tabs: mix, A / B, two settings
static lv_obj_t *s_rp_chip[2][5];   // per setting: the reverb's FX presets (up to four) and Now (the reverb as it is)
static lv_obj_t *s_rp_name[2], *s_rp_state[2];   // the two settings of the reverb: name (tap = keyboard) and "set" / "not set"
static char s_mix_model[48];
static bool s_mix_touched;                 // Pos 1 / Pos 2 moved: SAVE writes them
static int s_second_pending = -1;           // FX editor of this slot opens with its second effect's list
static bool s_second_for_switch;            // ... and that list was asked for by the reverb switch's dialog: the B chosen is the switch's
static lv_obj_t *s_rev_b_label;             // reverb switch dialog, A / B tab: the reverb's A and B
static int s_mix_tab_shown;                // the tab in front: saved, it is what footswitch 8 does
static uint32_t s_rev_types[16];           // reverb models for the dropdown (index + 1; 0 = none)
static int s_rev_type_count;
static lv_obj_t *s_learn, *s_learn_title, *s_learn_text, *s_learn_first;
// Looper tile dialog (long press on a tile in looper mode): name, colour and symbol of a switch
static lv_obj_t *s_looper_editor, *s_le_title, *s_le_hint, *s_le_name_label, *s_le_swatch[UI_BANK_COLOR_COUNT];
static lv_obj_t *s_le_symbol[UI_LOOPER_SYMBOLS];
static int s_le_tile, s_le_color, s_le_icon;   // tile 0-6 = footswitch 2-8
static char s_le_name[UI_LOOPER_NAME];
// (new ones go at the end: the stored tiles name a symbol by its place)
static const char *const LOOPER_SYMBOLS[UI_LOOPER_SYMBOLS] = {
    NULL, LV_SYMBOL_LOOP, LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE, LV_SYMBOL_STOP, LV_SYMBOL_REFRESH, LV_SYMBOL_AUDIO,
    LV_SYMBOL_PREV, LV_SYMBOL_NEXT, LV_SYMBOL_PLAY LV_SYMBOL_PAUSE,   // for a player: previous, next, play / pause
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
// The reverb switch with two settings: both names as rows on the tile, the one that is on filled (draw_pair).
typedef struct { bool shown, on; int active; char names[2][16]; uint32_t color; } tile_pair_t;   // active: 0 / 1, -1 = neither
static tile_pair_t s_tile_pair[TILES];
// A slot with a second effect (and the reverb switch with two settings): the tile in two halves, A above and B below,
// each with its symbol and name; the one that runs in its colour, the other one dimmed (user, 2026-11-13: "E geteilt")
static bool s_tile_hint[TILES];   // the tile's line is a hint on holding its footswitch (not shown in the gig view)
static char s_tile_caption_text[TILES][24];   // the caption a tile was given (the screen leaves it out; the editor shows it)
// The reverb mix (user, 2026-11-13: "C"): footswitch 8 in two halves, Pos 1 / Pos 2 each with its per cent and a bar;
// the reverb's own tile a slim bar along its bottom of the mix it has now (it follows footswitch 8)
static float s_tile_fill[TILES];          // 0-1, < 0 = no bar
typedef struct {
    bool shown, on;
    int runs;                              // 0 = A, 1 = B
    uint32_t color[2];
    const lv_image_dsc_t *icon[2];
    char names[2][24];
    char letters[2];                       // at the right edge of each half (0: A / B)
    bool fills;                            // each half has a bar of fill[n] (the reverb mix switch)
    float fill[2];
} tile_split_t;
static tile_split_t s_tile_split[TILES];
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
static int s_se_capture;                         // capture slot the edited scene loads, 0 = as it is
static lv_obj_t *s_se_capture_label, *s_scene_cap, *s_scene_cap_list;   // the button and its list
// Set list dialog (tap the preset / bank line at the top): the songs in order on the left, the presets on the right.
static lv_obj_t *s_setlist, *s_sl_songs, *s_sl_presets, *s_sl_tab[2];
static uint8_t s_sl_list[UI_SETLIST_MAX], s_sl_scenes[UI_SETLIST_MAX];   // preset and start scene (0 = none) per song
static int s_sl_count, s_sl_selected = -1;
static bool s_sl_on;
static lv_obj_t *s_sl_scene_chip[1 + UI_SCENES], *s_sl_scene_hint;   // the chosen song's start scene: none, 1-6
static ui_scene_names_cb s_scene_names;
static lv_obj_t *s_preset_card;
static lv_obj_t *s_midi_label, *s_midi, *s_midi_status, *s_midi_list;
static ui_midi_t s_midi_state;
static bool s_fullscreen;
static lv_obj_t *s_layout;   // the tile layout dialog
// Tile layout (ui_set_layout): position p (footswitch / CC 50+p) shows the tile of switch s_layout_order[p]; only the
// first s_layout_count positions are on the screen (6 = 2 x 3, 8 = 2 x 4). Tiles stay indexed by their switch.
static uint8_t s_layout_order[TILES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static int s_layout_count = TILES;
// ... and FX mode's own order (ui_set_layout_fx; main.c has the default: Pre FX on the top row, Post FX below them)
static uint8_t s_layout_fx_order[TILES] = { 3, 4, 7, 2, 5, 6, 8, 1 };
static int s_tile_w = TILE_W;
static uint32_t s_gesture_tick;   // a swipe ends with a click on the tile under the finger: ignore that one
static lv_timer_t *s_toast_timer;

static const nano_library_t *s_library;
static char s_picker_kind = 'C';      // 'C' captures, 'I' cabs
static bool s_picker_library;
static int s_picker_category, s_picker_page, s_confirm_code;
static char s_preset_names[NANO_PRESETS][65];
static ui_view_t s_view;
static int s_be_bank, s_be_tile, s_be_preset, s_be_color, s_be_icon, s_be_preset2;   // (preset2: held, 0 = none)
static lv_obj_t *s_bank_list2;
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
static bool s_spec_eq_dirty;                   // P4: a value changed - the EQ's curve over the spectrum is made anew
static char s_param_text[NANO_MAX_PARAMS][16]; // its text as drawn (number or option)
static int16_t s_param_unit_w[NANO_MAX_PARAMS];
static int32_t s_drag_x;                       // a bar follows the finger sideways (relative, from s_drag_from)
static float s_drag_from;
static lv_obj_t *s_drag_bar;
static int s_drag_param;
static bool s_dragging;
// The FX editor's second page, the pad (see "The pad" below)
static struct {
    bool prefer;                         // the pad is wanted (PAD, or a long press on a pedal) ...
    bool page;                           // ... and shows: the model has corners and its values are known
    bool leave;                          // the editor closes once the waiting values have gone
    const nano_focus_t *focus;
    float base[NANO_MAX_PARAMS];         // the values when the page opened
    float corner[4][NANO_MAX_PARAMS];    // < 0: the corner says nothing about this control
    float sent[NANO_MAX_PARAMS];         // what the Nano has been sent
    uint32_t moved, waiting;             // bit n: the corners move control n / its value still has to go to the Nano
    int next;                            // where the next value to send is looked for
    float x, y;                          // the lens, 0-1
    int near;                            // the zone that shows filled: 0-3 a corner, 4 the centre, -1 none
    bool finger;
} s_padv;
// The FX editor's third page, the info page (P4 board only; see "The info page" below)
#define INFO_MAX_SOUNDS 44   // starting points + players' sounds (16 at most, tools/gen_info.py) + an own build's presets (28, tools/gen_neural.py)
static const nano_info_t *s_info;        // the notes of the editor's model, NULL = none (no button then)
static const nano_info_t *s_info_for;    // ... the page is filled with
static bool s_info_on;                   // the page is wanted
static lv_obj_t *s_info_button, *s_info_button_icon, *s_info_page;
static void info_set(bool on);
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
static void on_pedal_view(lv_event_t *e);
static void build_pedals(lv_obj_t *parent);

static void open_bank_editor(int tile);
static void open_looper_editor(int tile);
static void open_scene_editor(int scene);
static void open_setlist_at(int song);
static void open_mix_editor(void);
static void open_learn(int number);
static int tile_position(int i);

static void on_tile_long(lv_event_t *e)
{
    int tile = (int)(intptr_t)lv_event_get_user_data(e);   // 0-7 = footswitch 1-8
    s_padv.prefer = false;   // (an FX tile opens the editor's bars; a pedal of the pedal view asks for the pad after this)
    // P4: MODE held = the player (jam tracks / phone), TUNER held = looper mode - as holding footswitch 1 / 2.
    // (The small board: MODE held = looper mode, TUNER held = its footswitch is learned - Bluetooth MIDI, not here yet.)
    if (s_player_mode) return;   // player mode: no switch has a held function (8 is out, on its press)
    if (s_looper_mode) {   // the looper on the board: the drums' tile held = their menu, undo held = clear
        if (tile == 0) ui_drum_menu_toggle();
        else if (tile == 4) send('w', UI_SWITCH_HOLD | 5);   // (undo held = clear)
    }
    else if (tile == 0) send('w', UI_SWITCH_HOLD | 1);
    else if (tile == 1) send('w', UI_SWITCH_HOLD | 2);
    else if (s_fx_mode && tile < 2 + NANO_FX_SLOTS) send('O', tile - 2);
    else if (s_fx_mode && tile == 2 + NANO_FX_SLOTS) open_mix_editor();
    else if (!s_fx_mode && tile >= 2 && s_view.scenes) open_scene_editor(tile - 2);
    else if (!s_fx_mode && tile >= 2 && s_view.setlist_on && s_view.setlist_count) open_setlist_at(s_view.setlist_first + tile - 2);
    else if (!s_fx_mode && tile >= 2) open_bank_editor(tile - 2);
}

// A long press on a pedal of the pedal view: the FX editor on its pad page (a long press on an FX tile: its bars).
static void on_pedal_long(lv_event_t *e)
{
    int tile = (int)(intptr_t)lv_event_get_user_data(e);
    on_tile_long(e);
    if (!s_looper_mode && s_fx_mode && tile >= 2 && tile < 2 + NANO_FX_SLOTS) s_padv.prefer = true;   // (the editor opens after this)
}

static lv_obj_t *s_splash, *s_splash_status, *s_splash_version;

static void on_status_tap(lv_event_t *e)
{
    if (!s_splash) open_setlist_at(-1);
}

// Shown on the screen: neither it nor a parent hidden. (MIDI, USB audio and System are pages inside the settings:
// closing the settings hides only the settings, the page that was open stays "not hidden" inside them.)
static bool shown(lv_obj_t *obj)
{
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_is_hidden(obj)) return false;
    }
    return true;
}

static void on_gesture(lv_event_t *e)
{
    s_gesture_tick = lv_tick_get();
    if (s_splash) return;   // start screen
    // Swipes work only on the main screen, not in an open dialog.
    lv_obj_t *const overlays[] = { s_tuner, s_editor, s_picker, s_bank_editor, s_rename, s_ask, s_usb, s_mix_editor, s_learn, s_midi, s_layout,
                                   s_settings, s_cab_settings, s_amp, s_looper_editor, s_scene_editor, s_exp_dialog, s_cal_panel,
                                   s_setlist, s_scene_cap, s_system, s_metro, s_rot, s_phone, s_jam, s_cap, s_presets };
    for (size_t i = 0; i < sizeof(overlays) / sizeof(overlays[0]); i++) {
        if (overlays[i] && shown(overlays[i])) return;
    }
    // A swipe is not a tap: whatever is under the finger gets no click (nor a long press) when it lifts. Before, only
    // the tiles ignored it, and only for 150 ms - a swipe up ended with a button going off.
    lv_indev_wait_release(lv_indev_active());
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
    lv_obj_set_style_bg_color(btn, lv_color_hex(BUTTON_BG), 0);   // flat, one step lighter than the panels
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(SURF_ON), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(BUTTON_LINE), 0);
    lv_obj_set_style_radius(btn, RADIUS, 0);
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
// A top bar button: its symbol above, a small word below (as the tab bars of iOS name their symbols).
static void top_word(lv_obj_t *b, lv_obj_t *img, const char *word)
{
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &ui_font_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9AA0A6), 0);
    lv_label_set_text(l, word);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -3);
}

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
    lv_obj_center(ui_icon(btn, UI_ICON_CLOSE, 20, TEXT));   // a calm button like the others
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
    lv_obj_set_style_bg_color(c, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_radius(c, RADIUS, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollable(c, false);
    return c;
}

// Dialog sheet over the screen.
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    bool on_screen = parent == lv_screen_active();   // a dialog over the screen: in its middle
    lv_obj_t *c = card(parent, x + (on_screen ? CENTER_X : 0), y + (on_screen ? CENTER_Y : 0), w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(SURF), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(FILL_LINE), 0);   // flat, a slightly lighter edge
    lv_obj_set_style_radius(c, RADIUS, 0);
    return c;
}

// P4: a dialog that takes the screen under the top bar (as the phone player): the top bar stays in sight.
#define DLG_W (SCREEN_W - 2 * MARGIN)
#define DLG_H (SCREEN_H - 72 - MARGIN)
// (kept to be moved up in the gig view: under its slimmer bar the tiles' top edges showed above the dialog)
static lv_obj_t *s_full_dialogs[24];
static int s_full_dialog_count;

static lv_obj_t *dialog_full(lv_obj_t *screen)
{
    lv_obj_t *c = panel(screen, 0, 0, DLG_W, DLG_H);
    lv_obj_set_pos(c, MARGIN, 72);
    if (s_full_dialog_count < (int)(sizeof(s_full_dialogs) / sizeof(s_full_dialogs[0]))) s_full_dialogs[s_full_dialog_count++] = c;
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

// Where switch i's tile sits (0 = first position), or -1 when the layout leaves it out.
// (Looper mode: the eight looper switches in their own fixed places - drums, A, B, pause / back, rec, play, undo -
// whatever order the home screen has.)
static int tile_position(int i)
{
    if (s_looper_mode || s_player_mode) return i >= 0 && i < TILES ? i : -1;
    const uint8_t *order = s_fx_mode ? s_layout_fx_order : s_layout_order;   // (FX mode: its own order)
    for (int p = 0; p < s_layout_count; p++) if (order[p] == i + 1) return p;
    return -1;
}

static void place_tile(int i)
{
    int h = tile_height(), p = tile_position(i), columns = (s_looper_mode || s_player_mode ? TILES : s_layout_count) / 2;
    s_tile_w = (SCREEN_W - 2 * MARGIN - (columns - 1) * TILE_GAP) / columns;   // 241 with four columns, 325 with three
    lv_obj_set_hidden(s_tile[i], p < 0 || s_pedals_shown);
    if (p < 0) return;
    lv_obj_set_size(s_tile[i], s_tile_w, h);
    lv_obj_set_pos(s_tile[i], MARGIN + (p % columns) * (s_tile_w + TILE_GAP), (s_fullscreen ? FULL_TILE_Y : TILE_Y) + (p / columns) * (h + TILE_GAP));
    if (!s_tile_name[i]) return;   // (still being built)
    lv_obj_set_width(s_tile_other[i], s_tile_w - 28);
    lv_obj_set_width(s_tile_name[i], s_tile_w - 24);
    lv_label_set_text_fmt(s_tile_number[i], "%d", p + 1);   // the footswitch that presses it
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
    static const lv_font_t *const gig[] = { &ui_font_title_46, &ui_font_title_40, &ui_font_title_34, &ui_font_title_30, &ui_font_title_26 };
    static const lv_font_t *const normal[] = { &ui_font_title_30, &ui_font_title_26, &ui_font_24, &ui_font_20, &ui_font_16 };
    enum { SIZES = 5 };
    // Measuring takes a while and the names rarely change: only when a name or the view (gig / normal) changed.
    static char measured[TILES][65];
    static int measured_view = -1, measured_w;
    static bool measured_note[TILES];
    bool same = measured_view == s_fullscreen && measured_w == s_tile_w;
    for (int i = 0; i < TILES && same; i++)
        same = !strcmp(measured[i], lv_label_get_text(s_tile_name[i])) && measured_note[i] == !lv_obj_is_hidden(s_tile_other[i]);
    if (same) return;
    measured_view = s_fullscreen;
    measured_w = s_tile_w;
    for (int i = 0; i < TILES; i++) {
        strlcpy(measured[i], lv_label_get_text(s_tile_name[i]), sizeof(measured[i]));
        measured_note[i] = !lv_obj_is_hidden(s_tile_other[i]);
    }
    const lv_font_t *const *fonts = s_fullscreen ? gig : normal;
    int room = tile_height() - (s_fullscreen ? 66 : 58);
    // a tile with a note: the name stays below the note's line
    int note_room = tile_height() - 10 - (s_fullscreen ? NOTE_Y_GIG + lv_font_get_line_height(NOTE_FONT_GIG) : NOTE_Y + lv_font_get_line_height(NOTE_FONT)) - 2;
    const lv_font_t *font = fonts[SIZES - 1];
    for (int f = 0; f < SIZES; f++) {
        int line_space = -lv_font_get_line_height(fonts[f]) / 6;
        bool fits = true;
        for (int i = 0; i < TILES && fits; i++) {
            const char *name = lv_label_get_text(s_tile_name[i]);
            if (!name[0]) continue;
            lv_point_t size;
            lv_text_get_size(&size, name, fonts[f], 0, line_space, s_tile_w - 24, LV_TEXT_FLAG_NONE);
            fits = size.y <= (measured_note[i] ? note_room : room) && words_fit(name, fonts[f], s_tile_w - 24);
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
        const int size = 20, pitch = 27, top = 12;   // (14 px marks could not be told apart from standing height)
        lv_area_t a = { tile->x1 + 14 + slot * pitch, tile->y1 + top, tile->x1 + 14 + slot * pitch + size, tile->y1 + top + size };
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = 6;
        if (!model) {
            a = (lv_area_t){ a.x1 + 7, a.y1 + 7, a.x2 - 7, a.y2 - 7 };
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
            lv_area_t bar = { a.x1 + 1, a.y2 + 4, a.x2 - 1, a.y2 + 7 };
            lv_draw_rect_dsc_init(&r);
            r.radius = 1;
            r.bg_color = lv_color_hex(color);
            r.bg_opa = on ? LV_OPA_COVER : filled ? LV_OPA_50 : 150;
            lv_draw_rect(layer, &r, &bar);
        }
    }
}

// The two settings of the reverb as two rows - the one that is on filled, the other outlined (as every pair of
// states here: filled = on). With the reverb off the rows are dimmed, and the one that would be on stays marked.
static void draw_pair(lv_layer_t *layer, const lv_area_t *tile, int i)
{
    const int h = 33, gap = 6, pad = 10;
    for (int n = 0; n < 2; n++) {
        int y2 = tile->y2 - pad - (1 - n) * (h + gap);
        lv_area_t a = { tile->x1 + pad, y2 - h, tile->x2 - pad, y2 };
        bool active = s_tile_pair[i].active == n;
        uint32_t ink = s_tile_ink[i];
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = 9;
        if (active) {
            r.bg_color = lv_color_hex(s_tile_pair[i].on ? ink : blend(s_tile_pair[i].color, TILE_OFF_BG, 0.45f));
        } else {
            r.bg_opa = LV_OPA_TRANSP;
            r.border_width = 2;
            r.border_color = lv_color_hex(s_tile_pair[i].on ? ink : 0x5D6267);
            r.border_opa = s_tile_pair[i].on ? 110 : LV_OPA_COVER;
        }
        lv_draw_rect(layer, &r, &a);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = &ui_font_title_22;
        d.text = s_tile_pair[i].names[n];
        d.align = LV_TEXT_ALIGN_LEFT;
        d.color = lv_color_hex(active ? (s_tile_pair[i].on ? s_tile_pair[i].color : 0xFFFFFF) : s_tile_pair[i].on ? ink : 0xB9BDC2);
        int lh = lv_font_get_line_height(d.font);
        lv_area_t t = { a.x1 + 12, a.y1 + (h - lh) / 2 + 1, a.x2 - 8, a.y1 + (h - lh) / 2 + 1 + lh };
        lv_draw_label(layer, &d, &t);
    }
}

// A slot with a second effect as a tile in two halves: A above, B below, each with its symbol and name; the one that
// runs in its colour (bright when the slot is on, dimmed when off), the other one barely tinted.
static void draw_split(lv_layer_t *layer, const lv_area_t *tile, int i)
{
    const tile_split_t *sp = &s_tile_split[i];
    const int pad = 4, gap = 4;
    int mid = (tile->y1 + tile->y2) / 2;
    for (int n = 0; n < 2; n++) {
        lv_area_t a = { tile->x1 + pad, n ? mid + gap / 2 : tile->y1 + pad, tile->x2 - pad, n ? tile->y2 - pad : mid - gap / 2 };
        bool runs = sp->runs == n;
        uint32_t c = sp->color[n];
        uint32_t lit = blend(c, SURF, 0.80f);   // (as the other tiles when on)
        uint32_t bg = runs ? (sp->on ? lit : blend(c, SURF, TILE_DIM + 0.1f)) : blend(c, SURF, 0.10f);
        uint32_t ink = runs ? (sp->on ? ink_for(lit) : TEXT) : 0x8E949B;
        uint32_t mark = runs ? (sp->on ? ink : blend(c, 0xFFFFFF, 0.7f)) : blend(c, SURF, 0.45f);
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = 9;
        r.bg_color = lv_color_hex(bg);
        if (!runs) {
            r.border_width = 1;
            r.border_color = lv_color_hex(blend(c, SURF, 0.22f));
        }
        lv_draw_rect(layer, &r, &a);
        int letter_y2 = a.y2 - 4;   // (the letter: at the half's bottom right, beside a bar)
        if (sp->fills) {   // the half's mix bar along its bottom; the rest of the half for symbol and name
            const int bh = 6;
            lv_area_t track = { a.x1 + 10, a.y2 - 8 - bh, a.x2 - 26, a.y2 - 8 };
            lv_draw_rect_dsc_t br;
            lv_draw_rect_dsc_init(&br);
            br.radius = bh / 2;
            br.bg_color = lv_color_hex(ink);
            br.bg_opa = 60;
            lv_draw_rect(layer, &br, &track);
            float f = sp->fill[n] < 0 ? 0 : sp->fill[n] > 1 ? 1 : sp->fill[n];
            lv_area_t fill = track;
            fill.x2 = track.x1 + (int)lroundf(f * lv_area_get_width(&track));
            if (fill.x2 > fill.x1 + bh) {
                br.bg_opa = LV_OPA_COVER;
                lv_draw_rect(layer, &br, &fill);
            }
            a.y2 -= 8 + bh;   // (symbol and name above the bar)
        }
        int h = lv_area_get_height(&a), x = a.x1 + 8;
        if (sp->icon[n]) {   // the symbol at 28 px (scaled from 36)
            const int size = h >= 80 ? 36 : 28, w = sp->icon[n]->header.w, ih = sp->icon[n]->header.h;
            int cx = x + size / 2, cy = a.y1 + h / 2;
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = sp->icon[n];
            d.recolor = lv_color_hex(mark);
            d.recolor_opa = LV_OPA_COVER;
            d.scale_x = d.scale_y = (int32_t)(256 * size / w);
            d.pivot.x = w / 2;
            d.pivot.y = ih / 2;
            lv_area_t ia = { cx - w / 2, cy - ih / 2, cx - w / 2 + w - 1, cy - ih / 2 + ih - 1 };
            lv_draw_image(layer, &d, &ia);
            x += size + 6;
        }
        // The letter at the right edge (A's half: under the number)
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.font = &ui_font_16;
        char letter[2] = { sp->letters[n] ? sp->letters[n] : n ? 'B' : 'A', 0 };
        l.text = letter;
        l.text_local = true;
        l.color = lv_color_hex(runs && sp->on ? ink : 0xB9BDC2);
        l.align = LV_TEXT_ALIGN_RIGHT;
        int llh = lv_font_get_line_height(l.font);
        lv_area_t la = { a.x2 - 30, letter_y2 - llh, a.x2 - 9, letter_y2 };
        lv_draw_label(layer, &l, &la);
        // The name in up to two lines: the largest font it fits in
        static const lv_font_t *const FONTS[] = { &ui_font_title_30, &ui_font_title_26, &ui_font_title_22, &ui_font_20, &ui_font_16 };
        int room = a.x2 - 24 - x;   // (the letter)
        int first = h >= 90 ? 0 : h >= 70 ? 1 : 2;
        const lv_font_t *font = FONTS[4];
        lv_point_t size;
        for (int f = first; f < 5; f++) {
            int ls = -lv_font_get_line_height(FONTS[f]) / 6;
            lv_text_get_size(&size, sp->names[n], FONTS[f], 0, ls, room, LV_TEXT_FLAG_NONE);
            if (size.y <= h - 8 && words_fit(sp->names[n], FONTS[f], room)) {
                font = FONTS[f];
                break;
            }
        }
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = font;
        d.text = sp->names[n];
        d.color = lv_color_hex(ink);
        d.line_space = -lv_font_get_line_height(font) / 6;
        lv_text_get_size(&size, sp->names[n], font, 0, d.line_space, room, LV_TEXT_FLAG_NONE);
        lv_area_t t = { x, a.y1 + (h - size.y) / 2, x + room, a.y1 + (h - size.y) / 2 + size.y };
        lv_draw_label(layer, &d, &t);
    }
}

// A tile that is not active: its top row (symbol, caption, number) lies on the dimmed colour.
// The reverb's mix as a slim bar along the bottom of its tile: a track and the fill up to the mix.
static void draw_fill(lv_layer_t *layer, const lv_area_t *tile, int i)
{
    uint32_t ink = s_tile_active[i] ? s_tile_ink[i] : blend(s_tile_color[i], 0xFFFFFF, 0.7f);
    const int pad = 10, h = 4;
    lv_area_t track = { tile->x1 + pad, tile->y2 - 5 - h, tile->x2 - pad, tile->y2 - 5 };
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = h / 2;
    r.bg_color = lv_color_hex(ink);
    r.bg_opa = 60;
    lv_draw_rect(layer, &r, &track);
    float f = s_tile_fill[i] > 1 ? 1 : s_tile_fill[i];
    lv_area_t fill = track;
    fill.x2 = track.x1 + (int)lroundf(f * lv_area_get_width(&track));
    if (fill.x2 > fill.x1 + h) {
        r.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &r, &fill);
    }
}

static void on_tile_draw(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_tile_fill[i] >= 0) {
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
        if (s_tile_split[i].shown) draw_split(lv_event_get_layer(e), &a, i);
        draw_fill(lv_event_get_layer(e), &a, i);
        return;
    }
    if (s_tile_split[i].shown) {
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
        draw_split(lv_event_get_layer(e), &a, i);
        return;
    }
    if (!s_tile_head[i] && !s_tile_marks[i] && !s_tile_pair[i].shown) return;
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
    lv_area_t in = { a.x1 + 2, a.y1 + 2, a.x2 - 2, a.y2 - 2 };   // inside the border
    if (s_tile_head[i]) draw_top(lv_event_get_layer(e), &in, 14, 50, blend(s_tile_head[i], TILE_OFF_BG, 0.24f));
    if (s_tile_marks[i]) draw_scene_marks(lv_event_get_layer(e), &a, i);
    if (s_tile_pair[i].shown) draw_pair(lv_event_get_layer(e), &a, i);
}

// ---- pedal view ----

#define PV_SLOT_W ((SCREEN_W - 2 * MARGIN) / 5)
#define PV_H (SCREEN_H - FULL_TILE_Y - MARGIN)
#define PV_BASE 84            // from the shelf's lower edge up to the line the pedals stand on
// The fields above a pedal (the slot's other effect, the reverb switch's setting). NANO_PV_SMALL: as on the small
// board's layout (34 px, 16 px text) - the P4's shelf has room above the pedals for larger ones.
#ifdef NANO_PV_SMALL
#define PV_FIELD_H 34
#define PV_FIELD_Y 36
#define PV_FIELD_Y2 76        // (below the reverb's setting)
#define PV_AB_ICON 150        // scale of the 36 px symbol
#define PV_AB_TEXT_X 34
#define PV_AB_FONTS { &ui_font_16, &ui_font_14 }
#else
#define PV_FIELD_H 60
#define PV_FIELD_Y 14
#define PV_FIELD_Y2 82
#define PV_AB_ICON 230        // the symbol at 32 px
#define PV_AB_TEXT_X 46
#define PV_AB_FONTS { &ui_font_title_22, &ui_font_20, &ui_font_16 }
#endif

static void on_pedal_view(lv_event_t *e)
{
    (void)e;
    s_pedal_view = !s_pedal_view;
    send('_', s_pedal_view);   // the controller keeps it and shows the state again
}

// The pedals instead of the tiles, or back: only what is shown changes hands, both are built and kept up to date.
static void pedals_show(bool shown)
{
    if (shown == s_pedals_shown) return;
    s_pedals_shown = shown;
    lv_obj_set_hidden(s_pedals, !shown);
    for (int i = 0; i < TILES; i++) place_tile(i);
}

// A tap on the field above a pedal: the slot's other effect (as A | B in the FX editor).
static void on_pedal_swap(lv_event_t *e)
{
    if (lv_tick_elaps(s_gesture_tick) < 150) return;   // end of a swipe, not a tap
    send('[', (int)(intptr_t)lv_event_get_user_data(e));
}

static void build_pedals(lv_obj_t *parent)
{
    s_pedals = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pedals);
    lv_obj_set_pos(s_pedals, MARGIN, FULL_TILE_Y);
    lv_obj_set_size(s_pedals, SCREEN_W - 2 * MARGIN, PV_H);
    lv_obj_set_style_radius(s_pedals, 16, 0);
    lv_obj_set_style_bg_color(s_pedals, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(s_pedals, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pedals, 1, 0);
    lv_obj_set_style_border_color(s_pedals, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_scrollable(s_pedals, false);
    // Between Pre FX 2 and Post FX 1: where the capture and the cab are in the chain
    lv_obj_t *amp_line = lv_obj_create(s_pedals);
    lv_obj_remove_style_all(amp_line);
    lv_obj_set_size(amp_line, 1, PV_H - PV_BASE - 36);
    lv_obj_set_pos(amp_line, 2 * PV_SLOT_W, 30);
    lv_obj_set_style_bg_color(amp_line, lv_color_hex(0x33383E), 0);
    lv_obj_set_style_bg_opa(amp_line, LV_OPA_COVER, 0);
    lv_obj_t *amp = label(s_pedals, &ui_font_12, 0x969AA0);
    lv_label_set_text(amp, "AMP");
    lv_obj_set_style_text_letter_space(amp, 1, 0);
    lv_obj_set_style_pad_hor(amp, 7, 0);
    lv_obj_set_style_pad_ver(amp, 2, 0);
    lv_obj_set_style_radius(amp, 6, 0);
    lv_obj_set_style_bg_color(amp, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(amp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(amp, 1, 0);
    lv_obj_set_style_border_color(amp, lv_color_hex(0x33383E), 0);
    // (on the line below the fields above the pedals: at the top, the field of Pre FX 2's second effect covered it)
    lv_obj_align(amp, LV_ALIGN_TOP_LEFT, 2 * PV_SLOT_W - 22, PV_FIELD_Y + PV_FIELD_H + 8);
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        lv_obj_t *box = lv_obj_create(s_pedals);
        s_pv_slot[slot] = box;
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, slot * PV_SLOT_W, 0);
        lv_obj_set_size(box, PV_SLOT_W, PV_H);
        lv_obj_set_scrollable(box, false);
        lv_obj_set_clickable(box, true);
        lv_obj_set_style_opa(box, 190, LV_STATE_PRESSED);
        lv_obj_add_event_cb(box, on_tile, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)(2 + slot));
        lv_obj_add_event_cb(box, on_pedal_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(2 + slot));

        s_pv_image[slot] = lv_image_create(box);
        lv_obj_align(s_pv_image[slot], LV_ALIGN_BOTTOM_MID, 0, -PV_BASE);
        lv_obj_set_hidden(s_pv_image[slot], true);

        // A pedal without a picture: an enclosure in the effect's colour with its symbol and a footswitch
        lv_obj_t *drawn = lv_obj_create(box);
        s_pv_drawn[slot] = drawn;
        lv_obj_remove_style_all(drawn);
        lv_obj_set_size(drawn, 126, 220);
        lv_obj_align(drawn, LV_ALIGN_BOTTOM_MID, 0, -PV_BASE);
        lv_obj_set_style_radius(drawn, 14, 0);
        lv_obj_set_style_bg_color(drawn, lv_color_hex(0x2A2E33), 0);
        lv_obj_set_style_bg_grad_color(drawn, lv_color_hex(0x15171A), 0);
        lv_obj_set_style_bg_grad_dir(drawn, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(drawn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(drawn, 2, 0);
        lv_obj_set_scrollable(drawn, false);
        lv_obj_set_clickable(drawn, false);
        s_pv_icon[slot] = lv_image_create(drawn);
        lv_image_set_scale(s_pv_icon[slot], 512);   // the 36 px symbol at twice its size
        lv_obj_set_style_image_recolor_opa(s_pv_icon[slot], LV_OPA_COVER, 0);
        lv_obj_align(s_pv_icon[slot], LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_t *stomp = lv_obj_create(drawn);
        lv_obj_remove_style_all(stomp);
        lv_obj_set_size(stomp, 38, 38);
        lv_obj_set_style_radius(stomp, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(stomp, 3, 0);
        lv_obj_set_style_border_color(stomp, lv_color_hex(0x6B7076), 0);
        lv_obj_set_style_bg_color(stomp, lv_color_hex(0x0D0F11), 0);
        lv_obj_set_style_bg_opa(stomp, LV_OPA_COVER, 0);
        lv_obj_align(stomp, LV_ALIGN_BOTTOM_MID, 0, -22);
        lv_obj_set_hidden(drawn, true);

        // Under the pedal: its light and name, then the footswitch - filled while the effect is on
        s_pv_led[slot] = lv_obj_create(box);
        lv_obj_remove_style_all(s_pv_led[slot]);
        lv_obj_set_size(s_pv_led[slot], 10, 10);
        lv_obj_set_style_radius(s_pv_led[slot], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_pv_led[slot], 2, 0);
        lv_obj_align(s_pv_led[slot], LV_ALIGN_BOTTOM_LEFT, 12, -(PV_BASE - 20));
        s_pv_name[slot] = label(box, &ui_font_16, TEXT);
        lv_obj_set_size(s_pv_name[slot], PV_SLOT_W - 32, lv_font_get_line_height(&ui_font_16));
        lv_label_set_long_mode(s_pv_name[slot], LV_LABEL_LONG_DOT);
        lv_obj_align(s_pv_name[slot], LV_ALIGN_BOTTOM_LEFT, 28, -(PV_BASE - 26));
        s_pv_key[slot] = lv_obj_create(box);
        lv_obj_remove_style_all(s_pv_key[slot]);
        lv_obj_set_size(s_pv_key[slot], PV_SLOT_W - 20, 34);
        lv_obj_align(s_pv_key[slot], LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_radius(s_pv_key[slot], 9, 0);
        lv_obj_set_style_border_width(s_pv_key[slot], 2, 0);
        lv_obj_set_clickable(s_pv_key[slot], false);
        static const char *const SHORT[NANO_FX_SLOTS] = { "PRE 1", "PRE 2", "POST 1", "POST 2", "POST 3" };
        s_pv_cap[slot] = label(s_pv_key[slot], &ui_font_14, TEXT);
        lv_label_set_text(s_pv_cap[slot], SHORT[slot]);
        lv_obj_set_style_text_letter_space(s_pv_cap[slot], 1, 0);
        lv_obj_align(s_pv_cap[slot], LV_ALIGN_LEFT_MID, 10, 0);
        s_pv_num[slot] = label(s_pv_key[slot], &ui_font_24, TEXT);
        lv_obj_align(s_pv_num[slot], LV_ALIGN_RIGHT_MID, -10, 0);
    }
    // A slot with a second effect: the one that waits, above the pedal of the one that runs - a field like the
    // footswitches', outlined in the waiting effect's colour, with its symbol and name. A tap swaps the two.
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        lv_obj_t *field = lv_obj_create(s_pedals);
        s_pv_ab[slot] = field;
        lv_obj_remove_style_all(field);
        lv_obj_set_size(field, PV_SLOT_W - 20, PV_FIELD_H);
        lv_obj_set_style_radius(field, 10, 0);
        lv_obj_set_style_border_width(field, 2, 0);
        lv_obj_set_scrollable(field, false);
        lv_obj_set_clickable(field, true);
        lv_obj_set_ext_click_area(field, 8);
        lv_obj_set_style_bg_opa(field, 190, LV_STATE_PRESSED);
        lv_obj_add_event_cb(field, on_pedal_swap, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)slot);
        lv_obj_add_event_cb(field, on_tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(2 + slot));
        s_pv_ab_icon[slot] = lv_image_create(field);
        lv_image_set_scale(s_pv_ab_icon[slot], PV_AB_ICON);
        lv_obj_set_style_image_recolor_opa(s_pv_ab_icon[slot], LV_OPA_COVER, 0);
        lv_obj_align(s_pv_ab_icon[slot], LV_ALIGN_LEFT_MID, PV_AB_TEXT_X / 2 - 18, 0);
        static const lv_font_t *const fonts[] = PV_AB_FONTS;
        s_pv_ab_name[slot] = label(field, fonts[0], TEXT);
#ifdef NANO_PV_SMALL
        // (the small board's 34 px field: one line high, so a name too long for the smallest size ends in dots)
        lv_obj_set_size(s_pv_ab_name[slot], PV_SLOT_W - 20 - PV_AB_TEXT_X - 8, lv_font_get_line_height(fonts[0]));
#else
        // (P4: no height on purpose - the 60 px field has room for two lines, and a name too long for the smallest
        // size goes on in the second one and stays whole: "Solid State / Comp (ST)". The small board's one-line rule
        // would cut it: shot 70-pedal-view-long-name.)
        lv_obj_set_width(s_pv_ab_name[slot], PV_SLOT_W - 20 - PV_AB_TEXT_X - 8);
#endif
        lv_label_set_long_mode(s_pv_ab_name[slot], LV_LABEL_LONG_DOT);
        lv_obj_align(s_pv_ab_name[slot], LV_ALIGN_LEFT_MID, PV_AB_TEXT_X, 0);
        lv_obj_set_hidden(field, true);
    }
    // The reverb switch with two settings: the one that is on, above the reverb's pedal - a field like the pedals'
    // footswitches (filled while the reverb is on), with the switch's number. A tap is a press of that switch.
    s_pv_rp = lv_obj_create(s_pedals);
    lv_obj_remove_style_all(s_pv_rp);
    lv_obj_set_size(s_pv_rp, PV_SLOT_W - 20, PV_FIELD_H);
    lv_obj_set_style_radius(s_pv_rp, 10, 0);
    lv_obj_set_style_border_width(s_pv_rp, 2, 0);
    lv_obj_set_scrollable(s_pv_rp, false);
    lv_obj_set_clickable(s_pv_rp, true);
    lv_obj_set_ext_click_area(s_pv_rp, 8);
    lv_obj_set_style_bg_opa(s_pv_rp, 190, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_pv_rp, on_tile, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)7);
    lv_obj_add_event_cb(s_pv_rp, on_tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)7);
    s_pv_rp_name = label(s_pv_rp, &ui_font_title_22, TEXT);
    lv_obj_set_size(s_pv_rp_name, PV_SLOT_W - 20 - 44, lv_font_get_line_height(&ui_font_title_22));
    lv_label_set_long_mode(s_pv_rp_name, LV_LABEL_LONG_DOT);
    lv_obj_align(s_pv_rp_name, LV_ALIGN_LEFT_MID, 10, 0);
    s_pv_rp_num = label(s_pv_rp, &ui_font_24, TEXT);
    lv_obj_align(s_pv_rp_num, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_hidden(s_pv_rp, true);
    lv_obj_set_hidden(s_pedals, true);
}

// The five slots as they are now (called with every state while the pedals are shown, and when they come up).
static void show_pedals(const nano_state_t *st, const ui_view_t *view)
{
    char text[12];
    // The reverb's setting (see s_pv_rp)
    int rslot = view->mix_slot;
    bool rp = view->rp_used && rslot >= 0 && rslot < NANO_FX_SLOTS && nano_fx_model(st->fx_known ? st->fx_type[rslot] : 0);
    set_hidden(s_pv_rp, !rp);
    if (rp) {
        const nano_fx_model_t *model = nano_fx_model(st->fx_type[rslot]);
        bool on = st->fx_on[rslot], neither = view->rp_active > 1;
        int n = view->rp_active == 1 ? 1 : 0;
        uint32_t ink = on ? ink_for(model->color) : 0xC8CCD1;
        lv_obj_set_pos(s_pv_rp, rslot * PV_SLOT_W + 10, PV_FIELD_Y);
        set_color_prop(s_pv_rp, LV_STYLE_BG_COLOR, model->color);
        set_num_prop(s_pv_rp, LV_STYLE_BG_OPA, on && !neither ? LV_OPA_COVER : LV_OPA_TRANSP);
        set_color_prop(s_pv_rp, LV_STYLE_BORDER_COLOR, on ? model->color : blend(model->color, PANEL_BG, 0.45f));
        if (neither) ink = on ? model->color : 0xC8CCD1;   // set to neither: outlined, the name of the one the next press brings
        set_text(s_pv_rp_name, view->rp_names[n][0] ? view->rp_names[n] : n ? "Setting 2" : "Setting 1");
        set_color_prop(s_pv_rp_name, LV_STYLE_TEXT_COLOR, ink);
        int p = tile_position(7);
        snprintf(text, sizeof(text), "%d", p + 1);
        set_text(s_pv_rp_num, p >= 0 ? text : "");
        set_color_prop(s_pv_rp_num, LV_STYLE_TEXT_COLOR, ink);
    }
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        uint32_t type = st->fx_known ? st->fx_type[slot] : 0;
        const nano_fx_model_t *model = nano_fx_model(type);
        // (a model this firmware does not know yet - a newer NanOS - is a pedal too: grey, by the name nano_fx_name gives)
        bool on = type && st->fx_on[slot];
        uint32_t color = model ? model->color : type ? 0x8E9297 : 0x5D6267;
        const lv_image_dsc_t *picture = type ? pedal_asset(type) : NULL;
        set_hidden(s_pv_image[slot], picture == NULL);
        set_hidden(s_pv_drawn[slot], picture != NULL || !type);
        if (picture) {
            set_image(s_pv_image[slot], picture);
            set_num_prop(s_pv_image[slot], LV_STYLE_IMAGE_OPA, on ? LV_OPA_COVER : 82);   // off: in the dark
        } else if (type) {
            set_hidden(s_pv_icon[slot], model == NULL);
            if (model) set_image(s_pv_icon[slot], NANO_ICONS[model->icon]);
            set_color_prop(s_pv_icon[slot], LV_STYLE_IMAGE_RECOLOR, color);
            set_color_prop(s_pv_drawn[slot], LV_STYLE_BORDER_COLOR, color);
            set_num_prop(s_pv_drawn[slot], LV_STYLE_OPA, on ? LV_OPA_COVER : 100);
        }
        set_color_prop(s_pv_led[slot], LV_STYLE_BG_COLOR, color);
        set_num_prop(s_pv_led[slot], LV_STYLE_BG_OPA, on ? LV_OPA_COVER : LV_OPA_TRANSP);
        set_color_prop(s_pv_led[slot], LV_STYLE_BORDER_COLOR, on ? color : 0x5D6267);
        const char *name = type ? nano_fx_name(type) : "Empty";
        lv_point_t size;   // a long name in the smaller size rather than cut off
        lv_text_get_size(&size, name, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        set_ptr_prop(s_pv_name[slot], LV_STYLE_TEXT_FONT, size.x > PV_SLOT_W - 34 ? &ui_font_14 : &ui_font_16);
        set_text(s_pv_name[slot], name);
        set_color_prop(s_pv_name[slot], LV_STYLE_TEXT_COLOR, on ? TEXT : 0x969AA0);
        uint32_t ink = on ? ink_for(color) : 0xC8CCD1;
        set_color_prop(s_pv_key[slot], LV_STYLE_BG_COLOR, color);
        set_num_prop(s_pv_key[slot], LV_STYLE_BG_OPA, on ? LV_OPA_COVER : LV_OPA_TRANSP);
        set_color_prop(s_pv_key[slot], LV_STYLE_BORDER_COLOR, on ? color : blend(color, PANEL_BG, 0.45f));
        set_color_prop(s_pv_cap[slot], LV_STYLE_TEXT_COLOR, ink);
        set_color_prop(s_pv_num[slot], LV_STYLE_TEXT_COLOR, ink);
        int p = tile_position(2 + slot);   // the footswitch that presses it (none: the slot is not on the six-field layout)
        snprintf(text, sizeof(text), "%d", p + 1);
        set_text(s_pv_num[slot], p >= 0 ? text : "");
        // The slot's other effect (see s_pv_ab): the pedal is the one that runs, the field names the one that waits
        bool has_b = type && view->ab_b_type[slot], b_runs = has_b && ((view->ab_active >> slot) & 1);
        set_hidden(s_pv_ab[slot], !has_b);
        if (has_b) {
            uint32_t a_type = b_runs ? view->ab_a_type[slot] : type, b_type = view->ab_b_type[slot];
            uint32_t other = b_runs ? a_type : b_type;   // (A's type is 0 until it has been read)
            const nano_fx_model_t *waiting = nano_fx_model(other);
            uint32_t other_color = waiting ? waiting->color : 0x8E9297;
            const char *other_name = a_type == b_type ? (b_runs ? "1st setting" : "2nd setting") : other ? nano_fx_name(other) : "A";
            // (the reverb's setting keeps the first row, see above)
            lv_obj_set_pos(s_pv_ab[slot], slot * PV_SLOT_W + 10, rp && slot == rslot ? PV_FIELD_Y2 : PV_FIELD_Y);
            set_color_prop(s_pv_ab[slot], LV_STYLE_BORDER_COLOR, blend(other_color, PANEL_BG, on ? 0.60f : 0.40f));
            set_hidden(s_pv_ab_icon[slot], waiting == NULL);
            if (waiting) set_image(s_pv_ab_icon[slot], NANO_ICONS[waiting->icon]);
            set_color_prop(s_pv_ab_icon[slot], LV_STYLE_IMAGE_RECOLOR, on ? other_color : blend(other_color, PANEL_BG, 0.6f));
            // the largest size the name fits in (the smallest one ends in dots)
            static const lv_font_t *const fonts[] = PV_AB_FONTS;
            const int nf = sizeof(fonts) / sizeof(fonts[0]);
            int f = 0;
            for (; f < nf - 1; f++) {
                lv_text_get_size(&size, other_name, fonts[f], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (size.x <= PV_SLOT_W - 20 - PV_AB_TEXT_X - 8) break;
            }
            set_ptr_prop(s_pv_ab_name[slot], LV_STYLE_TEXT_FONT, fonts[f]);
            set_text(s_pv_ab_name[slot], other_name);
            set_color_prop(s_pv_ab_name[slot], LV_STYLE_TEXT_COLOR, on ? 0xC8CCD1 : 0x969AA0);
        }
    }
}

void ui_set_pedal_view(bool on)
{
    lvgl_port_lock(0);
    s_pedal_view = on;
    lvgl_port_unlock();
}

static void build_tile(lv_obj_t *parent, int i)
{
    lv_obj_t *tile = lv_obj_create(parent);
    s_tile[i] = tile;
    place_tile(i);
    lv_obj_set_style_radius(tile, RADIUS, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    // pressed: its background a little through (2026-11-27; was the whole tile's opacity, which LVGL draws through a
    // layer of the tile's size - slower, and a suspect for the short glitch in a pressed tile the user saw)
    lv_obj_set_style_bg_opa(tile, 190, LV_STATE_PRESSED);
    // the flash's outline from the start, invisible (2026-11-27: set at the first press it re-measured the tile's
    // drawing area while the flash began - the user saw a glitch at the first press)
    lv_obj_set_style_outline_width(tile, 4, 0);
    lv_obj_set_style_outline_pad(tile, 2, 0);
    lv_obj_set_style_outline_color(tile, lv_color_white(), 0);
    lv_obj_set_style_outline_opa(tile, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(tile, on_tile, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tile, on_tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tile, on_tile_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);

    // Line drawing of the pedal at the right edge, behind everything else (FX tiles only)
    s_tile_pedal[i] = lv_image_create(tile);
    lv_obj_set_style_image_recolor_opa(s_tile_pedal[i], LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(s_tile_pedal[i], 72, 0);   // 28 %: there, but the name on top of it stays easy to read
    lv_obj_align(s_tile_pedal[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_hidden(s_tile_pedal[i], true);

    // The symbol without a square (as on the editor's signal chain), in the effect colour or the text colour.
    s_tile_square[i] = icon_square(tile, 40);
    lv_obj_set_pos(s_tile_square[i], 8, 8);
    lv_obj_set_style_bg_opa(s_tile_square[i], LV_OPA_TRANSP, 0);


    // (14 px and light grey: the 12 px captions and notes could not be read from standing height)
    s_tile_caption[i] = label(tile, &ui_font_14, TEXT);
    lv_obj_set_style_text_letter_space(s_tile_caption[i], 1, 0);
    lv_obj_set_width(s_tile_caption[i], s_tile_w - 96);
    lv_label_set_long_mode(s_tile_caption[i], LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_tile_caption[i], 56, 10);

    // The note (what holding does, the looper's state ...): a line of its own under the symbol row, 20 px (24 px in the
    // gig view) - beside the symbol at 14 px it could not be read from standing height (2026-11-28, draft A)
    s_tile_other[i] = label(tile, NOTE_FONT, TEXT);
    lv_obj_set_size(s_tile_other[i], s_tile_w - 28, lv_font_get_line_height(NOTE_FONT));   // one line
    lv_label_set_long_mode(s_tile_other[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_tile_other[i], 14, NOTE_Y);
    lv_obj_set_hidden(s_tile_other[i], true);
    s_tile_note_image[i] = lv_image_create(tile);   // a small symbol in front of that line
    lv_image_set_scale(s_tile_note_image[i], 256 * NOTE_ICON / 36);
    lv_obj_set_size(s_tile_note_image[i], NOTE_ICON, NOTE_ICON);
    lv_image_set_inner_align(s_tile_note_image[i], LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_image_recolor_opa(s_tile_note_image[i], LV_OPA_COVER, 0);
    lv_obj_set_pos(s_tile_note_image[i], 12, NOTE_Y);
    lv_obj_set_hidden(s_tile_note_image[i], true);
    // (draft B) the key of a note "Hold: ...": a small outlined key beside the symbol, the function large below it
    s_tile_badge[i] = label(tile, &ui_font_title_14, TEXT);
    lv_obj_set_style_text_letter_space(s_tile_badge[i], 1, 0);
    lv_obj_set_style_pad_hor(s_tile_badge[i], 8, 0);
    lv_obj_set_style_pad_ver(s_tile_badge[i], 2, 0);
    lv_obj_set_style_radius(s_tile_badge[i], 6, 0);
    lv_obj_set_style_border_width(s_tile_badge[i], 2, 0);
    lv_obj_set_pos(s_tile_badge[i], 56, 15);
    lv_obj_set_hidden(s_tile_badge[i], true);

    s_tile_number[i] = label(tile, &ui_font_title_18, TEXT);
    lv_label_set_text_fmt(s_tile_number[i], "%d", i + 1);
    lv_obj_align(s_tile_number[i], LV_ALIGN_TOP_RIGHT, -12, 6);

    s_tile_name[i] = label(tile, &ui_font_title_26, TEXT);
    lv_obj_set_width(s_tile_name[i], s_tile_w - 24);
    lv_label_set_long_mode(s_tile_name[i], LV_LABEL_LONG_WRAP);
    lv_obj_align(s_tile_name[i], LV_ALIGN_BOTTOM_LEFT, 12, -10);
    place_tile(i);   // number and widths by the layout
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
    lv_obj_t *t = s_pedals_shown && i >= 2 && i < 2 + NANO_FX_SLOTS ? s_pv_key[i - 2]
                  : s_pedals_shown && i == 7 && !lv_obj_is_hidden(s_pv_rp) ? s_pv_rp : s_tile[i];
    // (2026-11-27: a flash still running on this tile is stopped first - LVGL 9 runs two side by side, and their two
    // opacities alternated frame by frame: a flicker in the tile when it was pressed twice, or a press came twice.
    // The outline is set only when it differs: every style set redraws and re-measures the tile.)
    lv_anim_delete(t, flash_cb);
    set_num_prop(t, LV_STYLE_OUTLINE_WIDTH, 4);
    set_num_prop(t, LV_STYLE_OUTLINE_PAD, 2);
    set_color_prop(t, LV_STYLE_OUTLINE_COLOR, 0xFFFFFF);
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
    // (user, 2026-11-13, from the P4: "aus gedimmt farbig hinterlegt, an hell farbig hinterlegt - live gut lesbar")
    uint32_t lit = blend(color, SURF, 0.80f);   // on: the colour a little less glaring (calm tiles, 2026-11-13)
    uint32_t bg = empty ? 0x0D0E10 : active ? lit : blend(color, SURF, TILE_DIM);
    uint32_t border = empty ? 0x24272B : active ? lit : blend(color, SURF, TILE_DIM + 0.14f);
    uint32_t ink = empty ? 0x5D6267 : active ? ink_for(lit) : TEXT;
    uint32_t mark = empty ? 0x3A3F45 : active ? ink : blend(color, 0xFFFFFF, 0.7f);   // symbol colour
    s_tile_ink[i] = ink;
    s_tile_accent[i] = mark;
    s_tile_color[i] = color;
    s_tile_active[i] = active;
    s_tile_empty[i] = empty;
    s_tiles_drawn = true;
    s_tile_pedal_type[i] = 0;   // set again by set_tile_pedal / set_tile_other, shown by show_tile_extras
    s_tile_other_type[i] = 0;
    s_tile_hint[i] = false;
    s_tile_fill[i] = -1;
    s_tile_note[i] = NULL;
    s_tile_note_icon[i] = NULL;
    s_tile_marks[i] = 0;        // set again by show_scene_tiles
    memset(&s_tile_pair[i], 0, sizeof(s_tile_pair[i]));   // set again for the reverb switch with two settings
    memset(&s_tile_split[i], 0, sizeof(s_tile_split[i]));
    lv_obj_t *t = s_tile[i];
    uint32_t head = 0;   // no coloured head band: the colour is in the fill, the symbol and the border
    if (head != s_tile_head[i]) {
        s_tile_head[i] = head;
        lv_obj_invalidate(t);
    }
    set_color_prop(t, LV_STYLE_BG_COLOR, bg);
    set_num_prop(t, LV_STYLE_BG_GRAD_DIR, LV_GRAD_DIR_NONE);
    set_color_prop(t, LV_STYLE_BORDER_COLOR, border);
    set_num_prop(t, LV_STYLE_SHADOW_WIDTH, 0);   // (calm: no glow)
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
    set_num_prop(s_tile_caption[i], LV_STYLE_Y, 10);
    set_num_prop(s_tile_caption[i], LV_STYLE_WIDTH, (icon || symbol) ? s_tile_w - 98 : s_tile_w - 56);
    set_num_prop(s_tile_caption[i], LV_STYLE_TEXT_ALIGN, LV_TEXT_ALIGN_LEFT);
    set_align_if(s_tile_number[i], LV_ALIGN_TOP_RIGHT, -12, 6);
    set_num_prop(sq, LV_STYLE_BG_OPA, LV_OPA_TRANSP);
    set_num_prop(sq, LV_STYLE_BORDER_WIDTH, 0);
    set_num_prop(img, LV_STYLE_IMAGE_OPA, LV_OPA_COVER);

    char upper[40];   // captions in small capitals, as the editor's labels
    size_t n = 0;
    for (; caption[n] && n < sizeof(upper) - 1; n++) upper[n] = (char)((caption[n] >= 'a' && caption[n] <= 'z') ? caption[n] - 32 : caption[n]);
    upper[n] = 0;
    set_text(s_tile_caption[i], upper);
    strlcpy(s_tile_caption_text[i], upper, sizeof(s_tile_caption_text[i]));
    set_color_prop(s_tile_caption[i], LV_STYLE_TEXT_COLOR, active ? ink : empty ? MUTED : 0xB9BDC2);
    set_num_prop(s_tile_caption[i], LV_STYLE_TEXT_OPA, active ? 230 : LV_OPA_COVER);
    set_color_prop(s_tile_number[i], LV_STYLE_TEXT_COLOR, active ? ink : empty ? 0x5D6267 : 0x9AA0A6);
    set_num_prop(s_tile_number[i], LV_STYLE_TEXT_OPA, active ? 200 : LV_OPA_COVER);

    set_text(s_tile_name[i], name);
    set_color_prop(s_tile_name[i], LV_STYLE_TEXT_COLOR, ink);
    // Calm tiles (2026-11-13): symbol, big name, small number - no caption (the colour and the symbol say what it is)
    set_ptr_prop(s_tile_number[i], LV_STYLE_TEXT_FONT, &ui_font_20);
    if (!empty) set_text(s_tile_caption[i], "");
}

// A note that starts with how the footswitch is used ("Hold: Tuner", LV_SYMBOL_PLAY " Hold: Player"): that word as
// the key's badge (HOLD) and the rest, with its first letter large, as the line (draft B). Else the note as it is.
static bool split_note(const char *note, char *key, size_t key_n, char *rest, size_t rest_n)
{
    static const char *const KEYS[] = { "Hold: ", "Tap: ", "Press: " };
    char symbol[8] = "";
    if ((uint8_t)note[0] == 0xEF && note[1] && note[2] && note[3] == ' ') {   // a leading LV_SYMBOL_* (3 bytes) stays with the rest
        snprintf(symbol, sizeof(symbol), "%.4s", note);
        note += 4;
    }
    for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
        size_t n = strlen(KEYS[k]);
        if (strncmp(note, KEYS[k], n)) continue;
        size_t j = 0;
        for (; j < n - 2 && j < key_n - 1; j++) key[j] = (char)(KEYS[k][j] >= 'a' && KEYS[k][j] <= 'z' ? KEYS[k][j] - 32 : KEYS[k][j]);
        key[j] = 0;
        snprintf(rest, rest_n, "%s%s", symbol, note + n);
        char *first = rest + strlen(symbol);
        if (*first >= 'a' && *first <= 'z') *first -= 32;
        return true;
    }
    snprintf(rest, rest_n, "%s%s", symbol, note);
    key[0] = 0;
    return false;
}

// After the tiles are set: the pedal drawings (FX tiles), the A/B lines and the marks of the scenes.
static void show_tile_extras(void)
{
    char text[48], key[8];
    static int drawn_marks[TILES];
    static uint32_t drawn_types[NANO_FX_SLOTS];   // the marks have the colours of these effects
    bool types_changed = memcmp(drawn_types, s_slot_type, sizeof(drawn_types)) != 0;
    memcpy(drawn_types, s_slot_type, sizeof(drawn_types));
    // The notes: one size for all (as the names) - the larger one unless a note is too long for it
    const lv_font_t *font = s_fullscreen ? NOTE_FONT_GIG : NOTE_FONT;
    for (int i = 0; i < TILES; i++) {
        if (s_fullscreen && s_tile_hint[i]) s_tile_note[i] = NULL;   // the gig view: no hints on holding
        if (lv_label_get_text(s_tile_caption[i])[0]) s_tile_note[i] = NULL;   // (an empty tile's caption: "UNDO / REDO", no "Hold: clear")
        if (!s_tile_note[i] || tile_position(i) < 0) continue;
        split_note(s_tile_note[i], key, sizeof(key), text, sizeof(text));
        lv_point_t size;
        lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > s_tile_w - (s_tile_note_icon[i] ? 12 + NOTE_ICON + 6 : 14) - 14) font = s_fullscreen ? NOTE_FONT : NOTE_FONT_SMALL;
    }
    for (int i = 0; i < TILES; i++) {
        static tile_pair_t drawn_pair[TILES];
        static tile_split_t drawn_split[TILES];
        static float drawn_fill[TILES];
        if (s_tile_marks[i] != drawn_marks[i] || (types_changed && s_tile_marks[i]) || memcmp(&s_tile_pair[i], &drawn_pair[i], sizeof(drawn_pair[i]))
            || memcmp(&s_tile_split[i], &drawn_split[i], sizeof(drawn_split[i])) || drawn_fill[i] != s_tile_fill[i]) {
            drawn_fill[i] = s_tile_fill[i];
            drawn_marks[i] = s_tile_marks[i];
            drawn_pair[i] = s_tile_pair[i];
            drawn_split[i] = s_tile_split[i];
            lv_obj_invalidate(s_tile[i]);
        }
        const lv_image_dsc_t *pedal = NULL;   // (calm tiles: no pedal drawing behind the name)
        set_hidden(s_tile_pedal[i], pedal == NULL);
        if (pedal) {
            set_image(s_tile_pedal[i], pedal);
            set_color_prop(s_tile_pedal[i], LV_STYLE_IMAGE_RECOLOR, s_tile_accent[i]);
        }
        uint32_t other = s_tile_other_type[i];
        const lv_image_dsc_t *note_icon = s_tile_note[i] ? s_tile_note_icon[i] : NULL;
        bool line = other || s_tile_note[i];
        int32_t y = s_fullscreen ? NOTE_Y_GIG : NOTE_Y, h = lv_font_get_line_height(s_fullscreen ? NOTE_FONT_GIG : NOTE_FONT);
        set_hidden(s_tile_other[i], !line);
        set_hidden(s_tile_note_image[i], note_icon == NULL);
        set_num_prop(s_tile_note_image[i], LV_STYLE_X, 12);
        set_num_prop(s_tile_note_image[i], LV_STYLE_Y, y + (h - NOTE_ICON) / 2);
        bool keyed = false;
        if (line) {
            if (s_tile_note[i]) keyed = split_note(s_tile_note[i], key, sizeof(key), text, sizeof(text));
            else snprintf(text, sizeof(text), LV_SYMBOL_SHUFFLE " %.40s", nano_fx_name(other));
            set_text(s_tile_other[i], text);
            uint32_t color = s_tile_ink[i] == TEXT ? 0xC8CCD1 : s_tile_ink[i];   // a little lighter than the caption: it is read
            set_color_prop(s_tile_other[i], LV_STYLE_TEXT_COLOR, color);
            set_num_prop(s_tile_other[i], LV_STYLE_TEXT_OPA, 230);
            if (keyed) {   // the key beside the symbol: outlined (a label, not a state), in the caption's grey
                uint32_t kc = s_tile_ink[i] == TEXT ? 0xB9BDC2 : s_tile_ink[i];
                set_text(s_tile_badge[i], key);
                set_color_prop(s_tile_badge[i], LV_STYLE_TEXT_COLOR, kc);
                set_color_prop(s_tile_badge[i], LV_STYLE_BORDER_COLOR, kc);
                set_num_prop(s_tile_badge[i], LV_STYLE_BORDER_OPA, s_tile_ink[i] == TEXT ? 150 : 120);
                set_num_prop(s_tile_badge[i], LV_STYLE_X, lv_obj_is_hidden(s_tile_square[i]) ? 12 : 56);
            }
            int32_t x = note_icon ? 12 + NOTE_ICON + 6 : 14;
            int32_t fh = lv_font_get_line_height(font);
            set_ptr_prop(s_tile_other[i], LV_STYLE_TEXT_FONT, font);
            set_num_prop(s_tile_other[i], LV_STYLE_X, x);
            set_num_prop(s_tile_other[i], LV_STYLE_Y, y + (h - fh) / 2);   // (its own line under the symbol row: the whole width)
            set_num_prop(s_tile_other[i], LV_STYLE_WIDTH, s_tile_w - x - 14);
            set_num_prop(s_tile_other[i], LV_STYLE_HEIGHT, fh);
            if (note_icon) {
                set_image(s_tile_note_image[i], note_icon);
                set_color_prop(s_tile_note_image[i], LV_STYLE_IMAGE_RECOLOR, color);
            }
        }
        set_hidden(s_tile_badge[i], !keyed);
    }
}

// ---- capture / cab picker: slots of the Nano or its library ----

static void on_picker_close(lv_event_t *e)
{
    lv_obj_set_hidden(s_picker, true);
    if (s_listen_kind) send('.', -1);   // as the app: leaving the library ends the listening
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
    lv_obj_t *l = label(s_picker_list, CAPTION_FONT, MUTED);
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

static void on_listen_play(lv_event_t *e)
{
    int position = (int)(intptr_t)lv_event_get_user_data(e);
    bool ir = s_picker_kind == 'I';
    if (s_listen_kind == (ir ? 'I' : 'C') && s_listen_pos == position) send('.', -1);
    else send('.', (ir ? 1 << 16 : 0) | position);
}

// A library row: the name (with USER), a round play button on the right; green while it is listened to.
static lv_obj_t *library_row(const lib_item_t *item, int position)
{
    bool playing = s_listen_kind == (s_picker_kind == 'I' ? 'I' : 'C') && s_listen_pos == position;
    lv_obj_t *btn = picker_entry(item->name, NULL, false);
    lv_obj_t *name = lv_obj_get_child(btn, 0);
    lv_obj_set_width(name, item->user ? 400 : 470);
    if (item->user) {
        lv_obj_t *t = label(btn, &ui_font_14, GREEN);
        lv_label_set_text(t, "USER");
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, -54, 0);
    }
    if (playing) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x16301C), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_t *play = lv_button_create(btn);
    lv_obj_set_size(play, 40, 40);
    lv_obj_align(play, LV_ALIGN_RIGHT_MID, 6, 0);
    lv_obj_set_ext_click_area(play, 6);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(play, 0, 0);
    lv_obj_set_style_border_width(play, playing ? 0 : 2, 0);
    lv_obj_set_style_border_color(play, lv_color_hex(0x4A4E53), 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(playing ? GREEN : 0x1A1D20), 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(playing ? 0x7FF593 : 0x34383C), LV_STATE_PRESSED);
    lv_obj_set_style_opa(play, s_listen_pending && playing ? LV_OPA_60 : LV_OPA_COVER, 0);
    lv_obj_center(ui_icon(play, playing ? UI_ICON_STOP : UI_ICON_PLAY, playing ? 16 : 18, playing ? INK : TEXT));
    lv_obj_add_event_cb(play, on_listen_play, LV_EVENT_CLICKED, (void *)(intptr_t)position);
    lv_obj_add_event_cb(btn, on_library_entry, LV_EVENT_CLICKED, (void *)(intptr_t)position);
    return btn;
}

// The bar at the bottom of the library while something is listened to (in place of Prev / Next)
static void update_listen_bar(void)
{
    bool show = s_listen_kind && s_picker_library && s_listen_kind == (s_picker_kind == 'I' ? 'I' : 'C');
    lv_obj_set_hidden(s_listen_bar, !show);
    lv_obj_set_hidden(s_picker_pager, !s_picker_library || show);
    if (!show) return;
    lv_label_set_text(s_listen_name, s_listen_name_text);
    lv_label_set_text(s_listen_note, s_listen_pending ? "Starting ..." : "Listening - slot and preset stay");
}

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
    update_listen_bar();
    lv_obj_set_hidden(s_picker_info, !s_picker_library);
    int top = s_picker_library ? (cab ? 92 : 136) : 60;
    lv_obj_set_pos(s_picker_list, 10, top);
    lv_obj_set_size(s_picker_list, DLG_W - 20, (s_picker_library ? DLG_H - 66 : DLG_H - 18) - top);

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
        send('Y', 0);   // (it is read by itself only in a quiet moment: now it is wanted)
        lv_label_set_text(s_picker_page_label, "");
        return;
    }
    int matches = 0, shown = 0, first = s_picker_page * LIB_PAGE;
    for (int i = 0; i < list->count; i++) {
        if (!cab && s_picker_category != LIB_ALL && list->items[i].category != s_picker_category) continue;
        if (matches >= first && shown < LIB_PAGE) {
            library_row(&list->items[i], i);
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

static void on_listen_stop(lv_event_t *e) { send('.', -1); }

static void on_listen_load(lv_event_t *e)
{
    if (!s_listen_kind) return;
    char text[120];
    snprintf(text, sizeof(text), "Load \"%.60s\" into this slot?", s_listen_name_text);
    int slot = s_listen_kind == 'I' ? s_cab_slot : s_capture_slot;
    if (!slot) {
        ui_show_message("Choose a slot first - the library loads into the active slot.");
        return;
    }
    lv_label_set_text(s_confirm_text, text);
    s_confirm_code = (s_listen_kind == 'I' ? 1 << 16 : 0) | s_listen_pos;
    lv_obj_set_hidden(s_confirm, false);
    lv_obj_move_foreground(s_confirm);
}

void ui_set_listening(char kind, int position, const char *name, bool pending)
{
    lvgl_port_lock(0);
    s_listen_kind = kind;
    s_listen_pos = position;
    s_listen_pending = pending;
    strlcpy(s_listen_name_text, name ? name : "", sizeof(s_listen_name_text));
    if (!lv_obj_is_hidden(s_picker) && s_picker_library) {
        lv_coord_t y = lv_obj_get_scroll_y(s_picker_list);   // the list stays where it is
        fill_picker();
        lv_obj_scroll_to_y(s_picker_list, y, LV_ANIM_OFF);
    } else if (s_listen_bar) {
        update_listen_bar();
    }
    lvgl_port_unlock();
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
    s_picker = dialog_full(screen);   // P4: under the top bar, the whole width
    s_picker_title = dialog_title(s_picker, "");
    s_picker_tab[0] = small_button(s_picker, DLG_W - 340, 12, 120, 44, "Slots", on_picker_tab, 0);
    s_picker_tab[1] = small_button(s_picker, DLG_W - 212, 12, 120, 44, "Library", on_picker_tab, 1);
    close_button(s_picker, DLG_W - 60, 12, on_picker_close, 0);

    s_picker_info = label(s_picker, HINT_FONT, MUTED);
    lv_obj_set_pos(s_picker_info, 20, 64);

    s_picker_chips = lv_obj_create(s_picker);
    lv_obj_set_pos(s_picker_chips, 10, 86);
    lv_obj_set_size(s_picker_chips, DLG_W - 20, 46);
    lv_obj_set_style_bg_opa(s_picker_chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_chips, 0, 0);
    lv_obj_set_style_pad_all(s_picker_chips, 0, 0);
    lv_obj_set_scrollable(s_picker_chips, false);
    for (int c = 0; c < LIB_CATEGORY_COUNT; c++) {
        s_picker_chip[c] = small_button(s_picker_chips, c * ((DLG_W - 20) / LIB_CATEGORY_COUNT), 2, (DLG_W - 20) / LIB_CATEGORY_COUNT - 6, 40, LIB_CATEGORY_NAMES[c], on_picker_chip, c);
    }

    s_picker_list = lv_obj_create(s_picker);
    lv_obj_set_style_bg_opa(s_picker_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_list, 0, 0);
    lv_obj_set_style_pad_all(s_picker_list, 4, 0);
    lv_obj_set_style_pad_row(s_picker_list, 6, 0);
    lv_obj_set_flex_flow(s_picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_picker_list, LV_DIR_VER);

    s_picker_pager = lv_obj_create(s_picker);
    lv_obj_set_pos(s_picker_pager, 10, DLG_H - 60);
    lv_obj_set_size(s_picker_pager, DLG_W - 20, 50);
    lv_obj_set_style_bg_opa(s_picker_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_pager, 0, 0);
    lv_obj_set_style_pad_all(s_picker_pager, 0, 0);
    lv_obj_set_scrollable(s_picker_pager, false);
    small_button(s_picker_pager, 0, 4, 120, 42, LV_SYMBOL_LEFT " Prev", on_picker_page, -1);
    small_button(s_picker_pager, DLG_W - 20 - 120, 4, 120, 42, "Next " LV_SYMBOL_RIGHT, on_picker_page, 1);
    s_picker_page_label = label(s_picker_pager, &ui_font_20, MUTED);
    lv_obj_align(s_picker_page_label, LV_ALIGN_CENTER, 0, 0);

    // Listening: what plays, Stop and Load (in place of Prev / Next)
    s_listen_bar = card(s_picker, 10, DLG_H - 66, DLG_W - 20, 58);
    lv_obj_set_style_bg_color(s_listen_bar, lv_color_hex(SURF_ON), 0);
    lv_obj_set_style_bg_grad_dir(s_listen_bar, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(s_listen_bar, 1, 0);
    lv_obj_set_style_border_color(s_listen_bar, lv_color_hex(GREEN), 0);
    lv_obj_t *stop = lv_button_create(s_listen_bar);
    lv_obj_set_size(stop, 40, 40);
    lv_obj_set_pos(stop, 7, 7);
    lv_obj_set_style_radius(stop, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(stop, 0, 0);
    lv_obj_set_style_bg_color(stop, lv_color_hex(GREEN), 0);
    lv_obj_center(ui_icon(stop, UI_ICON_STOP, 16, INK));
    lv_obj_add_event_cb(stop, on_listen_stop, LV_EVENT_CLICKED, NULL);
    s_listen_name = label(s_listen_bar, &ui_font_20, TEXT);
    lv_obj_set_width(s_listen_name, DLG_W - 20 - 300);
    lv_label_set_long_mode(s_listen_name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_listen_name, 58, 4);
    s_listen_note = label(s_listen_bar, &ui_font_12, MUTED);
    lv_obj_set_pos(s_listen_note, 58, 32);
    small_button(s_listen_bar, DLG_W - 20 - 226, 6, 100, 42, "Stop", on_listen_stop, 0);
    primary(small_button(s_listen_bar, DLG_W - 20 - 118, 6, 106, 42, "Load", on_listen_load, 0));
    lv_obj_set_hidden(s_listen_bar, true);

    // Confirmation before a library item replaces the slot content
    s_confirm = card(s_picker, 10, DLG_H - 120, DLG_W - 20, 110);
    lv_obj_set_style_bg_color(s_confirm, lv_color_hex(0x1A1D20), 0);
    lv_obj_set_style_bg_grad_dir(s_confirm, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(s_confirm, lv_color_hex(0x3A3F45), 0);
    s_confirm_text = label(s_confirm, &ui_font_20, TEXT);
    lv_obj_set_width(s_confirm_text, DLG_W - 50);
    lv_label_set_long_mode(s_confirm_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_confirm_text, 14, 12);
    small_button(s_confirm, DLG_W - 20 - 290, 54, 130, 44, "Cancel", on_confirm, 0);
    primary(small_button(s_confirm, DLG_W - 20 - 150, 54, 130, 44, "Load", on_confirm, 1));
    lv_obj_set_hidden(s_confirm, true);

    lv_obj_set_hidden(s_picker, true);
}

// ---- tempo and metronome (NanOS 2.3.0): the button in the top bar, the dialog ----

#define METRO_SIGS NANO_TIME_SIGS   // (nano_state.c: the app's list)

static void metro(int what, int value) { send('\'', (what << 24) | (value & 0xFFFFFF)); }

// The dialog goes by itself, as a volume display does: 5 s after it came up, after the last touch on the screen and
// after the last news from the Nano. The controller is told with 10 (it leaves the Nano's own tempo screen alone).
#define METRO_HIDE_MS 5000
static void metro_keep(void) { s_metro_news_tick = lv_tick_get(); }
static void metro_hide(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_is_hidden(s_metro)) return;
    if (!lv_obj_is_hidden(s_rename) && s_rename_kind == 'b') return;   // the tempo is being typed: the dialog waits
    uint32_t idle = lv_tick_elaps(s_metro_news_tick), touch = lv_display_get_inactive_time(NULL);
    if ((touch < idle ? touch : idle) < METRO_HIDE_MS) return;
    lv_obj_set_hidden(s_metro, true);
    s_tempo_ui.open = false;
    metro(10, 0);
}

static void on_tempo_button(lv_event_t *e)
{
    metro(0, 0);
    metro_keep();
    s_tempo_ui.open = true;
    lv_obj_set_hidden(s_metro, false);   // at once; main confirms with ui_set_tempo
    lv_obj_move_foreground(s_metro);
}
static void on_metro_close(lv_event_t *e)
{
    lv_obj_set_hidden(s_metro, true);
    s_tempo_ui.open = false;
    metro(1, 0);
}
static void on_metro_step(lv_event_t *e) { metro(3, (int)(intptr_t)lv_event_get_user_data(e)); }

static void show_rename(char kind, const char *title, const char *ok, const char *text, int max_length);
static void close_rename(void);

// A tap on the BPM value: the tempo is typed on the number pad (40-240, one decimal: "92", "120.5").
static void on_metro_bpm(lv_event_t *e)
{
    (void)e;
    metro_keep();
    show_rename('b', "Tempo in BPM", "Set", "", 5);
}

// From the keyboard. What is no tempo is said and the keyboard stays; a tempo goes to the Nano as TAP's does.
static void metro_typed(const char *text)
{
    char number[8];
    size_t n = 0;
    for (const char *c = text; *c && n + 1 < sizeof(number); c++) number[n++] = *c == ',' ? '.' : *c;
    number[n] = 0;
    char *end = number;
    float bpm = strtof(number, &end);
    if (end == number || *end || !(bpm >= 40 && bpm <= 240)) {
        ui_show_message("The tempo goes from 40 to 240 BPM.");
        return;
    }
    close_rename();
    metro_keep();
    metro(0, 0);   // (should the dialog have gone meanwhile: the tempo is the controller's again)
    metro(2, (int)lroundf(bpm * 10));
}
static void on_metro_scope(lv_event_t *e) { metro(9, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_metro_switch(lv_event_t *e) { metro(4, 0); }
static void on_metro_note(lv_event_t *e) { metro(7, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_metro_sound(lv_event_t *e) { metro(8, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_metro_sig(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    metro(6, (s_tempo_ui.time_sig + step + NANO_TIME_SIG_COUNT) % NANO_TIME_SIG_COUNT);
}

// TAP: the mean of the last intervals (40-240 BPM as the app; a pause of 2 s starts again)
static void on_metro_tap(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    if (s_metro_tap_count && now - s_metro_taps[s_metro_tap_count - 1] > 2000) s_metro_tap_count = 0;
    if (s_metro_tap_count == 6) {
        memmove(s_metro_taps, s_metro_taps + 1, 5 * sizeof(s_metro_taps[0]));
        s_metro_tap_count = 5;
    }
    s_metro_taps[s_metro_tap_count++] = now;
    if (s_metro_tap_count < 2) return;
    float ms = (float)(s_metro_taps[s_metro_tap_count - 1] - s_metro_taps[0]) / (float)(s_metro_tap_count - 1);
    float bpm = 60000.0f / ms;
    if (bpm < 40 || bpm > 240) return;
    metro(2, (int)lroundf(bpm * 10));
}

static void metro_vol_text(int half_db)
{
    lv_label_set_text_fmt(s_metro_vol_text, "%s%d.%d dB", half_db < 0 ? "\xE2\x88\x92" : "", abs(half_db) / 2, abs(half_db) % 2 ? 5 : 0);
}
static void on_metro_vol(lv_event_t *e)
{
    int v = lv_slider_get_value(s_metro_vol);
    metro_vol_text(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) metro(5, v);
}

// the dots: the beats of a bar, moving with the tempo while the metronome is on (a picture only)
static int metro_beats(void)
{
    int beats = atoi(METRO_SIGS[s_tempo_ui.time_sig]);
    return beats < 1 ? 1 : beats > 8 ? 8 : beats;
}
static void metro_beat(lv_timer_t *t)
{
    int beats = metro_beats();
    s_metro_beat = (s_metro_beat + 1) % beats;
    for (int i = 0; i < 8; i++) {
        lv_obj_t *dot = lv_obj_get_child(s_metro_dots, i);
        lv_obj_set_style_bg_opa(dot, i == s_metro_beat ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void metro_dots_update(void)
{
    int beats = metro_beats();
    for (int i = 0; i < 8; i++) {
        lv_obj_t *dot = lv_obj_get_child(s_metro_dots, i);
        lv_obj_set_hidden(dot, i >= beats);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    }
    bool run = !s_tempo_ui.muted && !s_tempo_ui.ext_clock && s_tempo_ui.open;
    if (run) {
        int denominator = atoi(strchr(METRO_SIGS[s_tempo_ui.time_sig], '/') + 1);   // x/8 counts eighths
        float per_minute = s_tempo_ui.tempo * (float)denominator / 4;
        lv_timer_set_period(s_metro_beat_timer, (uint32_t)(60000.0f / (per_minute > 1 ? per_minute : 1)));
        lv_timer_resume(s_metro_beat_timer);
    } else {
        lv_timer_pause(s_metro_beat_timer);
    }
}

void ui_set_tempo(const ui_tempo_t *t)
{
    lvgl_port_lock(0);
    // (news: the dialog comes up, or something in it changed - the state alone arrives every few seconds)
    if (lv_obj_is_hidden(s_metro) || t->tempo != s_tempo_ui.tempo || t->volume_db != s_tempo_ui.volume_db || t->muted != s_tempo_ui.muted ||
        t->global != s_tempo_ui.global || t->ext_clock != s_tempo_ui.ext_clock || t->time_sig != s_tempo_ui.time_sig ||
        t->note_len != s_tempo_ui.note_len || t->sound != s_tempo_ui.sound) metro_keep();
    s_tempo_ui = *t;
    // the button: symbol and tempo, green while the metronome is on (filled = on)
    // (only what differs: this comes with every state reply, and a style or text set draws the button again)
    char bpm[12];
    snprintf(bpm, sizeof(bpm), "%d", (int)lroundf(t->tempo));
    set_hidden(s_tempo_button, !t->supported);
    set_hidden(s_tempo_line, !t->supported);
    set_text(s_tempo_button_text, bpm);
    set_color_prop(s_tempo_button_icon, LV_STYLE_IMAGE_RECOLOR, t->muted ? TEXT : GREEN);
    set_color_prop(s_tempo_button_text, LV_STYLE_TEXT_COLOR, t->muted ? CAPTION : TEXT);
    // the dialog
    if (t->open && t->supported) {
        lv_obj_set_hidden(s_metro, false);
        lv_obj_move_foreground(s_metro);
    } else {
        lv_obj_set_hidden(s_metro, true);
    }
    lv_label_set_text_fmt(s_metro_bpm, "%d.%d", (int)(t->tempo * 10 + 0.5f) / 10, (int)(t->tempo * 10 + 0.5f) % 10);
    style_tab(s_metro_scope[0], t->global);
    style_tab(s_metro_scope[1], !t->global);
    if (t->muted) lv_obj_remove_state(s_metro_switch, LV_STATE_CHECKED);
    else lv_obj_add_state(s_metro_switch, LV_STATE_CHECKED);
    int half_db = (int)lroundf(t->volume_db * 2);
    if (!lv_obj_has_state(s_metro_vol, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_metro_vol, half_db, LV_ANIM_OFF);
        metro_vol_text(half_db);
    }
    lv_label_set_text(s_metro_sig, METRO_SIGS[t->time_sig < NANO_TIME_SIG_COUNT ? t->time_sig : 2]);
    for (int i = 0; i < 4; i++) style_tab(s_metro_note[i], i == t->note_len);
    for (int i = 0; i < 2; i++) style_tab(s_metro_sound[i], i == t->sound);
    lv_label_set_text(s_metro_hint, t->ext_clock ? "The tempo comes from an external MIDI clock." : "Plays on the Nano's outputs.");
    metro_dots_update();
    lvgl_port_unlock();
}

static lv_obj_t *metro_caption(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *l = label(parent, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void build_metro(lv_obj_t *screen)
{
    s_metro = dialog_full(screen);
    dialog_title(s_metro, "Tempo");
    s_metro_scope[0] = small_button(s_metro, DLG_W - 60 - 268, 12, 120, 44, "Global", on_metro_scope, 1);
    s_metro_scope[1] = small_button(s_metro, DLG_W - 60 - 140, 12, 120, 44, "Preset", on_metro_scope, 0);
    close_button(s_metro, DLG_W - 60, 12, on_metro_close, 0);

    // left: - tempo +, the beat, TAP
    lv_obj_t *minus = small_button(s_metro, 20, 82, 72, 64, "\xE2\x88\x92", on_metro_step, -10);
    lv_obj_set_style_text_font(lv_obj_get_child(minus, 0), &ui_font_28, 0);
    lv_obj_t *plus = small_button(s_metro, DLG_W / 2 - 92, 82, 72, 64, "+", on_metro_step, 10);
    lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), &ui_font_28, 0);
    s_metro_bpm = label(s_metro, &ui_font_title_40, 0xFFFFFF);
    lv_obj_set_width(s_metro_bpm, DLG_W / 2 - 200);
    lv_obj_set_style_text_align(s_metro_bpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_metro_bpm, 100, 84);
    lv_obj_set_clickable(s_metro_bpm, true);   // a tap: type the tempo
    lv_obj_set_ext_click_area(s_metro_bpm, 6);
    lv_obj_add_event_cb(s_metro_bpm, on_metro_bpm, LV_EVENT_SHORT_CLICKED, NULL);
    // - and + held: they go on, a step at a time
    lv_obj_add_event_cb(minus, on_metro_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-10);
    lv_obj_add_event_cb(plus, on_metro_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)10);
    lv_obj_t *bpm = metro_caption(s_metro, 100, 140, "BPM");
    lv_obj_set_width(bpm, DLG_W / 2 - 200);
    lv_obj_set_style_text_align(bpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(bpm, 82);
    s_metro_dots = lv_obj_create(s_metro);
    lv_obj_remove_style_all(s_metro_dots);
    lv_obj_set_size(s_metro_dots, DLG_W / 2 - 40, 16);
    lv_obj_set_pos(s_metro_dots, 20, 166);
    lv_obj_set_flex_flow(s_metro_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_metro_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_metro_dots, 10, 0);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *dot = lv_obj_create(s_metro_dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, i == 0 ? 14 : 12, i == 0 ? 14 : 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0x4A4E53), 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(GREEN), 0);
    }
    s_metro_beat_timer = lv_timer_create(metro_beat, 500, NULL);
    s_metro_hide_timer = lv_timer_create(metro_hide, 250, NULL);
    lv_timer_pause(s_metro_beat_timer);
    lv_obj_t *tap = button(s_metro, 20, 198, DLG_W / 2 - 40, 230, on_metro_tap);
    lv_obj_remove_event_cb(tap, on_metro_tap);
    lv_obj_add_event_cb(tap, on_metro_tap, LV_EVENT_PRESSED, NULL);   // the moment of the touch, not of the release
    lv_obj_set_style_radius(tap, 14, 0);
    lv_obj_t *tap_label = label(tap, &ui_font_24, TEXT);
    lv_obj_set_style_text_letter_space(tap_label, 6, 0);
    lv_label_set_text(tap_label, "TAP");
    lv_obj_center(tap_label);
    s_metro_hint = label(s_metro, &ui_font_12, MUTED);
    lv_obj_set_pos(s_metro_hint, 20, 446);

    // right: metronome, volume, time signature, note, sound
    metro_caption(s_metro, DLG_W / 2 + 10, 86, "METRONOME");
    s_metro_switch = lv_switch_create(s_metro);
    lv_obj_set_size(s_metro_switch, 72, 38);
    lv_obj_set_pos(s_metro_switch, DLG_W - 2 - 18 - 72, 76);
    lv_obj_set_ext_click_area(s_metro_switch, 8);
    lv_obj_set_style_bg_color(s_metro_switch, lv_color_hex(0x2A2D30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_metro_switch, lv_color_hex(GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_metro_switch, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_metro_switch, lv_color_hex(INK), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(s_metro_switch, -3, LV_PART_KNOB);
    lv_obj_add_event_cb(s_metro_switch, on_metro_switch, LV_EVENT_VALUE_CHANGED, NULL);

    s_metro_vol = lv_slider_create(s_metro);   // a fill bar: drag anywhere on it
    lv_obj_set_size(s_metro_vol, DLG_W / 2 - 40, 56);
    lv_obj_set_pos(s_metro_vol, DLG_W / 2 + 10, 124);
    lv_slider_set_range(s_metro_vol, -120, 18);   // dB x 2
    lv_obj_set_style_radius(s_metro_vol, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(s_metro_vol, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_metro_vol, lv_color_hex(0x0B0C0D), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_metro_vol, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_metro_vol, lv_color_hex(0x3A3D40), LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_metro_vol, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_metro_vol, lv_color_hex(0x1E4A28), LV_PART_INDICATOR);
    lv_obj_set_style_border_side(s_metro_vol, LV_BORDER_SIDE_RIGHT, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_metro_vol, 4, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_metro_vol, lv_color_hex(GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_metro_vol, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_metro_vol, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_metro_vol, on_metro_vol, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_metro_vol, on_metro_vol, LV_EVENT_RELEASED, NULL);
    lv_obj_t *vol_label = label(s_metro_vol, &ui_font_16, TEXT);
    lv_label_set_text(vol_label, "Volume");
    lv_obj_align(vol_label, LV_ALIGN_LEFT_MID, 14, 0);
    s_metro_vol_text = label(s_metro_vol, &ui_font_16, TEXT);
    lv_obj_align(s_metro_vol_text, LV_ALIGN_RIGHT_MID, -14, 0);

    metro_caption(s_metro, DLG_W / 2 + 10, 190, "TIME SIGNATURE");
    small_button(s_metro, DLG_W / 2 + 10, 210, 64, 50, LV_SYMBOL_LEFT, on_metro_sig, -1);
    small_button(s_metro, DLG_W - 2 - 18 - 64, 210, 64, 50, LV_SYMBOL_RIGHT, on_metro_sig, 1);
    s_metro_sig = label(s_metro, &ui_font_20, TEXT);
    lv_obj_set_width(s_metro_sig, DLG_W / 2 - 40 - 140);
    lv_obj_set_style_text_align(s_metro_sig, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_metro_sig, DLG_W / 2 + 10 + 70, 222);

    metro_caption(s_metro, DLG_W / 2 + 10, 280, "NOTE");
    static const char *const NOTES[4] = { "1/4", "1/8", "1/8T", "1/16" };
    for (int i = 0; i < 4; i++) s_metro_note[i] = small_button(s_metro, DLG_W / 2 + 10 + i * ((DLG_W / 2 - 40) / 4), 300, (DLG_W / 2 - 40) / 4 - 8, 50, NOTES[i], on_metro_note, i);
    metro_caption(s_metro, DLG_W / 2 + 10, 370, "SOUND");
    s_metro_sound[0] = small_button(s_metro, DLG_W / 2 + 10, 390, (DLG_W / 2 - 40) / 2 - 4, 50, "Blip", on_metro_sound, 0);
    s_metro_sound[1] = small_button(s_metro, DLG_W / 2 + 10 + (DLG_W / 2 - 40) / 2 + 4, 390, (DLG_W / 2 - 40) / 2 - 4, 50, "Digital", on_metro_sound, 1);
    lv_obj_set_hidden(s_metro, true);
}

// ---- the wheel while a rotary footswitch of the Nano is turned (1.17.0) ----
static lv_obj_t *s_rot_square, *s_rot_title, *s_rot_where, *s_rot_dots[5], *s_rot_prev, *s_rot_now, *s_rot_name, *s_rot_next;
static lv_obj_t *s_rot_back, *s_rot_back_label;
static lv_timer_t *s_rot_timer;
static int s_rot_back_code;

static void rot_close(lv_timer_t *t) { lv_obj_set_hidden(s_rot, true); lv_timer_pause(s_rot_timer); }
static void on_rot_back(lv_event_t *e)
{
    lv_obj_set_hidden(s_rot, true);
    lv_timer_pause(s_rot_timer);
    send('"', s_rot_back_code);
}

void ui_show_rotate(const ui_rotate_t *r)
{
    lvgl_port_lock(0);
    set_icon_square(s_rot_square, TEXT, NANO_ICONS[r->cab ? NANO_ICON_CAB : NANO_ICON_CAPTURE], true);
    lv_label_set_text(s_rot_title, r->cab ? "CAB / IR  \xC2\xB7  FOOTSWITCH II" : "CAPTURE  \xC2\xB7  FOOTSWITCH I");
    int position = r->cab ? r->slot : (r->slot ? (r->slot - 1) % 5 + 1 : 0);
    if (!r->slot && !r->cab && r->bank) lv_label_set_text_fmt(s_rot_where, "Bank %d  \xC2\xB7  bypass", r->bank);
    else if (!r->slot) lv_label_set_text(s_rot_where, "Bypass");
    else if (r->cab) lv_label_set_text_fmt(s_rot_where, "Slot %d of 5", r->slot);
    else lv_label_set_text_fmt(s_rot_where, "Bank %d  \xC2\xB7  slot %d", (r->slot - 1) / 5 + 1, position);
    for (int i = 0; i < 5; i++) {
        bool on = i + 1 == position;
        lv_obj_set_style_bg_opa(s_rot_dots[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_rot_dots[i], lv_color_hex(on ? GREEN : 0x4A4E53), 0);
    }
    lv_label_set_text(s_rot_prev, r->prev);
    lv_label_set_text(s_rot_name, r->name);
    lv_label_set_text(s_rot_next, r->next);
    bool back = r->from != r->slot;
    lv_obj_set_hidden(s_rot_back, !back);
    if (back) lv_label_set_text_fmt(s_rot_back_label, "Back to %.40s", r->from_name);
    s_rot_back_code = (r->cab ? 1 << 8 : 0) | (r->from & 0xFF);
    lv_obj_set_hidden(s_rot, false);
    lv_obj_move_foreground(s_rot);
    lv_timer_reset(s_rot_timer);
    lv_timer_resume(s_rot_timer);
    lvgl_port_unlock();
}

static void build_rotate(lv_obj_t *screen)
{
    s_rot = panel(screen, 110, 70, 580, 340);
    s_rot_square = icon_square(s_rot, 44);
    lv_obj_set_pos(s_rot_square, 22, 20);
    s_rot_title = label(s_rot, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(s_rot_title, 2, 0);
    lv_obj_set_pos(s_rot_title, 78, 20);
    s_rot_where = label(s_rot, &ui_font_20, TEXT);
    lv_obj_set_pos(s_rot_where, 78, 40);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *dot = lv_obj_create(s_rot);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_pos(dot, 470 + i * 20, 32);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(GREEN), 0);
        s_rot_dots[i] = dot;
    }
    s_rot_prev = label(s_rot, &ui_font_20, 0x5E6369);
    lv_obj_set_width(s_rot_prev, 536);
    lv_obj_set_style_text_align(s_rot_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_rot_prev, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_rot_prev, 22, 96);
    s_rot_now = lv_obj_create(s_rot);   // the one now: large, on green
    lv_obj_remove_style_all(s_rot_now);
    lv_obj_set_size(s_rot_now, 536, 74);
    lv_obj_set_pos(s_rot_now, 22, 132);
    lv_obj_set_style_radius(s_rot_now, 14, 0);
    lv_obj_set_style_bg_color(s_rot_now, lv_color_hex(0x122417), 0);
    lv_obj_set_style_bg_opa(s_rot_now, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rot_now, 2, 0);
    lv_obj_set_style_border_color(s_rot_now, lv_color_hex(GREEN), 0);
    s_rot_name = label(s_rot_now, &ui_font_title_30, 0xFFFFFF);
    lv_obj_set_width(s_rot_name, 510);
    lv_obj_set_style_text_align(s_rot_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_rot_name, LV_LABEL_LONG_DOT);
    lv_obj_center(s_rot_name);
    s_rot_next = label(s_rot, &ui_font_20, 0x5E6369);
    lv_obj_set_width(s_rot_next, 536);
    lv_obj_set_style_text_align(s_rot_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_rot_next, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_rot_next, 22, 218);
    lv_obj_t *hint = label(s_rot, &ui_font_12, 0x7C8187);
    lv_label_set_text(hint, "closes by itself after 2 s");
    lv_obj_set_pos(hint, 22, 296);
    s_rot_back = small_button(s_rot, 300, 278, 258, 46, "Back", on_rot_back, 0);
    s_rot_back_label = lv_obj_get_child(s_rot_back, 0);
    lv_obj_set_width(s_rot_back_label, 240);
    lv_label_set_long_mode(s_rot_back_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_rot_back_label, LV_TEXT_ALIGN_CENTER, 0);
    s_rot_timer = lv_timer_create(rot_close, 2000, NULL);
    lv_timer_pause(s_rot_timer);
    lv_obj_set_hidden(s_rot, true);
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
static void on_bank_preset2(lv_event_t *e)
{
    s_be_preset2 = (int)(intptr_t)lv_event_get_user_data(e);
    fill_bank_list();
}

static void fill_bank_list(void)
{
    char text[80];
    for (int k = 0; k < 2; k++) {
    lv_obj_t *list = k ? s_bank_list2 : s_bank_list, *chosen = NULL;
    lv_obj_clean(list);
    for (int preset = k ? 0 : 1; preset <= NANO_PRESETS; preset++) {   // (held: "None" first)
        bool selected = preset == (k ? s_be_preset2 : s_be_preset);
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 46);
        lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x2A2E33 : 0x1A1D20), 0);
        lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(BANK_COLORS[s_be_color]), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 9, 0);
        lv_obj_add_event_cb(btn, k ? on_bank_preset2 : on_bank_preset, LV_EVENT_CLICKED, (void *)(intptr_t)preset);
        lv_obj_t *l = label(btn, &ui_font_20, selected ? 0xFFFFFF : 0xD8D8D8);
        if (!preset) snprintf(text, sizeof(text), "None");
        else snprintf(text, sizeof(text), "%2d   %.60s", preset, s_preset_names[preset - 1][0] ? s_preset_names[preset - 1] : "-");
        lv_label_set_text(l, text);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        if (selected) chosen = btn;
    }
    if (chosen) {   // the chosen preset in the middle of the list: the ones before and after it in sight, none cut at the top
        lv_obj_update_layout(list);
        lv_area_t row, box;
        lv_obj_get_coords(chosen, &row);
        lv_obj_get_coords(list, &box);
        lv_obj_scroll_by_bounded(list, 0, (box.y1 + box.y2) / 2 - (row.y1 + row.y2) / 2, LV_ANIM_OFF);
    }
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
    s_be_preset2 = s_view.bank_presets2[tile] <= NANO_PRESETS ? s_view.bank_presets2[tile] : 0;
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
        open_learn(tile_position(s_be_tile + 2) + 1);   // the footswitch at the tile's position
        return;
    }
    lv_obj_set_hidden(s_bank_editor, true);
    int base = s_be_bank << 14 | s_be_tile << 11;
    if (action == 1 && s_be_preset) send('B', s_be_preset2 << 23 | s_be_icon << 18 | base | s_be_color << 7 | s_be_preset);
    else if (action == 2) send('B', base);
}

static void build_bank_editor(lv_obj_t *screen)
{
    s_bank_editor = dialog_full(screen);   // P4: under the top bar, the whole width
    s_bank_title = dialog_title(s_bank_editor, "");
    lv_obj_t *hint = label(s_bank_editor, HINT_FONT, MUTED);
    lv_label_set_text(hint, "Colour, symbol and preset for this switch - stored on the controller");
    lv_obj_set_pos(hint, 20, 46);
    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_bank_editor);
        s_bank_swatch[i] = sw;
        // (48 px colours and symbols, 2026-11-28 draft: the preset lists below get a third whole row)
        lv_obj_set_size(sw, 48, 48);
        lv_obj_set_pos(sw, 22 + i * 96, 76);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int i = 0; i <= PRESET_ICON_COUNT; i++) {   // "none" and the symbols, ten per row
        lv_obj_t *sq = icon_square(s_bank_editor, 48);
        s_bank_symbol[i] = sq;
        lv_obj_set_pos(sq, 22 + (i % 10) * 96, 134 + (i / 10) * 56);
        lv_obj_set_clickable(sq, true);
        lv_obj_add_event_cb(sq, on_symbol, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    // the presets: pressed (left) | held, on the release (right; "None" first)
    int lw = (DLG_W - 22 - 16) / 2;
    static const char *const LISTS[2] = { "PRESSED", "HELD \xC2\xB7 ON THE RELEASE" };
    for (int k = 0; k < 2; k++) {
        lv_obj_t *c = label(s_bank_editor, CAPTION_FONT, MUTED);
        lv_label_set_text(c, LISTS[k]);
        lv_obj_set_style_text_letter_space(c, 2, 0);
        lv_obj_set_pos(c, 16 + k * (lw + 16), 250);
    }
    s_bank_list = lv_obj_create(s_bank_editor);
    lv_obj_set_pos(s_bank_list, 10, 270);
    lv_obj_set_size(s_bank_list, lw, 172);
    lv_obj_set_style_bg_opa(s_bank_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bank_list, 0, 0);
    lv_obj_set_style_pad_all(s_bank_list, 4, 0);
    lv_obj_set_style_pad_row(s_bank_list, 6, 0);
    lv_obj_set_flex_flow(s_bank_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bank_list, LV_DIR_VER);
    s_bank_list2 = lv_obj_create(s_bank_editor);
    lv_obj_set_pos(s_bank_list2, 10 + lw + 16, 270);
    lv_obj_set_size(s_bank_list2, lw, 172);
    lv_obj_set_style_bg_opa(s_bank_list2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bank_list2, 0, 0);
    lv_obj_set_style_pad_all(s_bank_list2, 4, 0);
    lv_obj_set_style_pad_row(s_bank_list2, 6, 0);
    lv_obj_set_flex_flow(s_bank_list2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bank_list2, LV_DIR_VER);
    small_button(s_bank_editor, 18, 452, 150, 46, "Default", on_bank_button, 2);
    small_button(s_bank_editor, 182, 452, 180, 46, "Learn switch", on_bank_button, 3);
    small_button(s_bank_editor, DLG_W - 2 - 18 - 330, 452, 160, 46, "Cancel", on_bank_button, 0);
    primary(small_button(s_bank_editor, DLG_W - 2 - 18 - 160, 452, 160, 46, "Save", on_bank_button, 1));
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
        lv_obj_set_style_text_font(sym, i && LOOPER_SYMBOLS[i][3] ? &ui_font_20 : &ui_font_28, 0);   // two symbols in one square: smaller
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

__attribute__((unused)) static void open_looper_editor(int tile)
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
    s_looper_editor = dialog_full(screen);
    s_le_title = dialog_title(s_looper_editor, "");
    s_le_hint = label(s_looper_editor, HINT_FONT, MUTED);
    lv_obj_set_pos(s_le_hint, 20, 46);

    lv_obj_t *name = button(s_looper_editor, 18, 78, DLG_W - 2 - 36, 60, on_looper_name);   // tap: the keyboard
    s_le_name_label = label(name, &ui_font_title_22, TEXT);
    lv_obj_align(s_le_name_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *hint = label(name, HINT_FONT, MUTED);
    lv_label_set_text(hint, "Rename");
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -4, 0);

    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_looper_editor);
        s_le_swatch[i] = sw;
        lv_obj_set_size(sw, 64, 64);
        lv_obj_set_pos(sw, 18 + i * 96, 160);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_looper_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int i = 0; i < UI_LOOPER_SYMBOLS; i++) {   // "none" and the symbols
        lv_obj_t *sq = icon_square(s_looper_editor, 64);
        s_le_symbol[i] = sq;
        lv_obj_set_pos(sq, 18 + i * 96, 248);
        lv_obj_set_clickable(sq, true);
        lv_obj_add_event_cb(sq, on_looper_symbol, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    small_button(s_looper_editor, 18, 446, 160, 52, "Default", on_looper_button, 2);
    small_button(s_looper_editor, DLG_W - 2 - 18 - 330, 446, 160, 52, "Cancel", on_looper_button, 0);
    primary(small_button(s_looper_editor, DLG_W - 2 - 18 - 160, 446, 160, 52, "Save", on_looper_button, 1));
    lv_obj_set_hidden(s_looper_editor, true);
}

// ---- footswitch learn: the next pressed footswitch (or switch over MIDI) takes this switch's place ----

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
    lv_label_set_text_fmt(s_learn_text, "Press the footswitch - or the switch on the MIDI controller - that should be switch %d now. Waiting 15 seconds.", number);
    lv_obj_set_hidden(s_learn_first, number == 1);   // tile 1 held is the looper mode: its switch is learned from here
    lv_obj_set_hidden(s_learn, false);
    lv_obj_move_foreground(s_learn);
    send('D', number);
}

// All switches, one after the other (Settings > Footswitches): the same window, which says where it is. Each press
// swaps two switches, so after the last one every switch is where it was pressed.
static void show_learn_all(int number)
{
    lv_label_set_text_fmt(s_learn_title, "Learn footswitch %d of %d", number, TILES);
    if (number > 1) lv_label_set_text_fmt(s_learn_text, "Switch %d learned. Now press the one that should be switch %d. Waiting 15 seconds.", number - 1, number);
    else lv_label_set_text_fmt(s_learn_text, "Press the footswitch - or the switch on the MIDI controller - that should be switch 1 now. Waiting 15 seconds.");
    lv_obj_set_hidden(s_learn_first, true);
    lv_obj_set_hidden(s_learn, false);
    lv_obj_move_foreground(s_learn);
}

void ui_learn_next(int number, const char *message)
{
    (void)message;   // (what was swapped is in the log; the window says which switch is next)
    lvgl_port_lock(0);
    show_learn_all(number);
    lvgl_port_unlock();
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

static void preset_new_named(const char *text);
static void metro_typed(const char *text);

static void submit_rename(void)
{
    const char *text = lv_textarea_get_text(s_rename_area);
    while (*text == ' ') text++;
    if (s_rename_kind == 'b') {   // the tempo, typed (a tap on the BPM value of the tempo dialog); stays open if it is no tempo
        metro_typed(text);
        return;
    }
    if (s_rename_kind == 'N' && strlen(text) < 4) {
        ui_show_message("Preset names need at least 4 characters.");
        return;
    }
    if (s_rename_kind == 'W') {   // P4: the Wi-Fi network's name - then its password (empty: the board's own network)
        if (s_on_text) s_on_text('W', text);
        if (text[0]) {
            char title[64];
            snprintf(title, sizeof(title), "Password for \xE2\x80\x9C%.32s\xE2\x80\x9D", text);
            show_rename('K', title, "Join", "", 64);
        } else {
            if (s_on_text) s_on_text('K', "");
            close_rename();
        }
        return;
    }
    if (s_rename_kind == 'c') {   // the capture's name (sent with Save)
        if (strlen(text) < 4) {
            ui_show_message("Capture names need at least 4 characters.");
            return;
        }
        cap_named(text);
        close_rename();
        return;
    }
    if (s_rename_kind == 'p') {   // the new preset's name, into its sheet (sent with Create)
        if (strlen(text) < 4) {
            ui_show_message("Preset names need at least 4 characters.");
            return;
        }
        preset_new_named(text);
        close_rename();
        return;
    }
    if (s_rename_kind == 'L') looper_editor_named(text);   // into the looper tile's dialog, sent with its Save
    else if (s_rename_kind == 'S') scene_editor_named(text);   // the same for a scene
    else if (s_rename_kind == 'R' || s_rename_kind == 'r') {   // a reverb setting: "<1 / 2>name"
        char named[20];
        snprintf(named, sizeof(named), "%c%.15s", s_rename_kind == 'R' ? '1' : '2', text);
        if (s_on_text) s_on_text('R', named);
    }
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
    lv_textarea_set_placeholder_text(s_rename_area, kind == 'N' || kind == 'L' || kind == 'S' || kind == 'p' || kind == 'c' ? ""
                                     : kind == 'W' ? "Network name (empty = the board's own network)" : kind == 'K' ? "Password"
                                     : kind == 'b' ? "40 - 240" : "Name (empty name = delete)");
    lv_textarea_set_password_mode(s_rename_area, kind == 'K');
    // The tempo is typed on the number pad; every other text gets the letters back
    if (kind == 'b') lv_keyboard_set_mode(s_rename_keyboard, LV_KEYBOARD_MODE_NUMBER);
    else if (lv_keyboard_get_mode(s_rename_keyboard) == LV_KEYBOARD_MODE_NUMBER) lv_keyboard_set_mode(s_rename_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
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
    lv_obj_set_size(s_rename, SCREEN_W, SCREEN_H);
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

    lv_obj_t *kb = s_rename_keyboard = lv_keyboard_create(s_rename);
    lv_obj_set_size(kb, SCREEN_W, 380);
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
    // over the keyboard's page: under the text field (at the top it covered the title and Cancel / OK)
    lv_obj_set_y(s_toast, lv_obj_is_hidden(s_rename) ? 8 : 150);
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
    s_usb_info = label(s_usb, HINT_FONT, MUTED);
    lv_obj_set_pos(s_usb_info, 20, 46);
    s_usb_value = label(s_usb, &ui_font_28, 0xFFFFFF);
    lv_obj_set_width(s_usb_value, 160);
    lv_obj_set_style_text_align(s_usb_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_usb_value, LV_ALIGN_TOP_RIGHT, -76, 16);
    s_usb_close = close_button(s_usb, 460, 12, on_usb_button, 0);

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

// The capture volume: the first row of the capture dialog (1.17.0; it had a dialog and a top bar button of its own)
static void show_capture_volume(void)
{
    lv_slider_set_value(s_level_slider[0], s_capture_volume, LV_ANIM_OFF);
    show_level_value(0);
}

static void on_volume_zero(lv_event_t *e) { level_changed(0, NANO_CAPTURE_VOLUME_0DB, true); }

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
    s_cab_settings = dialog_full(screen);
    dialog_title(s_cab_settings, "Cab / IR");
    s_cab_info = label(s_cab_settings, HINT_FONT, MUTED);
    lv_obj_set_width(s_cab_info, DLG_W - 120);
    lv_label_set_long_mode(s_cab_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_cab_info, 20, 46);
    close_button(s_cab_settings, DLG_W - 60, 12, on_cab_settings_close, 0);
    for (int i = 0; i < NANO_CAB_SETTINGS; i++) {
        int row = 1 + i, y = 76 + i * 98;
        lv_obj_t *name = label(s_cab_settings, CAPTION_FONT, MUTED);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, NAMES[i]);
        lv_obj_set_pos(name, 20, y);
        s_level_value[row] = label(s_cab_settings, &ui_font_20, 0xFFFFFF);
        lv_obj_set_width(s_level_value[row], 200);
        lv_obj_set_style_text_align(s_level_value[row], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_level_value[row], LV_ALIGN_TOP_RIGHT, -20, y - 3);
        build_level_row(s_cab_settings, row, 20, y + 24, DLG_W - 42);
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
    show_capture_volume();
    show_amp_values();
    lv_obj_set_hidden(s_amp, false);
    lv_obj_move_foreground(s_amp);
}

static void build_amp(lv_obj_t *screen)
{
    static const char *const NAMES[AMP_ROWS] = { "GAIN", "BASS", "MID", "TREBLE" };
    s_amp = dialog_full(screen);   // P4: under the top bar, the whole width
    dialog_title(s_amp, "Capture");
    s_amp_info = label(s_amp, HINT_FONT, MUTED);
    lv_obj_set_width(s_amp_info, DLG_W - 240);
    lv_label_set_long_mode(s_amp_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_amp_info, 20, 46);
    close_button(s_amp, DLG_W - 60, 12, on_amp_close, 0);
    small_button(s_amp, DLG_W - 60 - 124, 18, 100, 40, "0 dB", on_volume_zero, 0);
    // the volume first, on green (it is what is reached for most)
    lv_obj_t *vol_bg = lv_obj_create(s_amp);
    lv_obj_remove_style_all(vol_bg);
    lv_obj_set_size(vol_bg, DLG_W - 18, 88);
    lv_obj_set_pos(vol_bg, 8, 70);
    lv_obj_set_style_radius(vol_bg, 12, 0);
    lv_obj_set_style_bg_color(vol_bg, lv_color_hex(0x16301C), 0);
    lv_obj_set_style_bg_opa(vol_bg, LV_OPA_COVER, 0);
    lv_obj_t *vol_name = label(s_amp, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(vol_name, 2, 0);
    lv_label_set_text(vol_name, "VOLUME");
    lv_obj_set_pos(vol_name, 20, 76);
    s_level_value[0] = label(s_amp, &ui_font_20, 0xFFFFFF);
    lv_obj_set_width(s_level_value[0], 200);
    lv_obj_set_style_text_align(s_level_value[0], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_level_value[0], LV_ALIGN_TOP_RIGHT, -20, 73);
    build_level_row(s_amp, 0, 20, 98, DLG_W - 42);
    style_slider(s_level_slider[0], GREEN);
    for (int i = 0; i < AMP_ROWS; i++) {
        int row = AMP_ROW + i, y = 164 + i * 73;
        lv_obj_t *name = label(s_amp, CAPTION_FONT, MUTED);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_label_set_text(name, NAMES[i]);
        lv_obj_set_pos(name, 20, y);
        s_level_value[row] = label(s_amp, &ui_font_20, 0xFFFFFF);
        lv_obj_set_width(s_level_value[row], 200);
        lv_obj_set_style_text_align(s_level_value[row], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_level_value[row], LV_ALIGN_TOP_RIGHT, -20, y - 3);
        build_level_row(s_amp, row, 20, y + 20, DLG_W - 42);
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
    for (int i = 0; i < 3; i++) {
        style_tab(s_mix_tab[i], i == tab);
        lv_obj_set_hidden(s_mix_panel[i], i != tab);
    }
    lv_obj_set_hidden(s_mix_free, tab != 0 || !s_view.mix_exp);
}

// The two settings of the reverb in the dialog: their names, and whether they are set.
// A choice button: filled light while it is the one (as the editor's chosen buttons), else as the others.
static void choice_on(lv_obj_t *btn, bool on)
{
    set_color_prop(btn, LV_STYLE_BG_COLOR, on ? 0xECECEC : BUTTON_BG);
    set_color_prop(btn, LV_STYLE_BORDER_COLOR, on ? 0xECECEC : BUTTON_LINE);
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) set_color_prop(l, LV_STYLE_TEXT_COLOR, on ? INK : TEXT);
}

static void show_rp_rows(void)
{
    for (int n = 0; n < 2; n++) {
        bool set = (s_view.rp_set >> n) & 1;
        char text[24];
        if (s_view.rp_names[n][0]) snprintf(text, sizeof(text), "%s", s_view.rp_names[n]);
        else snprintf(text, sizeof(text), "Setting %d", n + 1);
        lv_label_set_text(s_rp_name[n], text);
        lv_label_set_text(s_rp_state[n], set ? LV_SYMBOL_OK : "");
        lv_obj_set_style_text_color(s_rp_state[n], lv_color_hex(GREEN), 0);
        // The reverb's FX presets: the one this setting was taken from filled (the same name)
        for (int k = 0; k < 4; k++) {
            bool shown = k < s_view.rp_fxp_count;
            set_hidden(s_rp_chip[n][k], !shown);
            if (!shown) continue;
            lv_label_set_text(lv_obj_get_child(s_rp_chip[n][k], 0), s_view.rp_fxp_names[k]);
            choice_on(s_rp_chip[n][k], set && !strcmp(s_view.rp_fxp_names[k], s_view.rp_names[n]));
        }
    }
}

// A chip of a setting's row: an FX preset of the reverb (0-3) or Now (4, the reverb as it is set now).
static void on_rp_chip(lv_event_t *e)
{
    int data = (int)(intptr_t)lv_event_get_user_data(e), n = data >> 4, k = data & 0xF;
    if (k == 4) send('-', 1 + n);
    else send('-', 0x100 | n << 4 | k);
}

static void on_rp_name(lv_event_t *e)
{
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    char title[40], name[16];
    snprintf(title, sizeof(title), "Name of reverb setting %d", n + 1);
    if (s_view.rp_names[n][0]) strlcpy(name, s_view.rp_names[n], sizeof(name));
    else snprintf(name, sizeof(name), n ? "Atmos" : "Room");
    show_rename(n ? 'r' : 'R', title, "OK", name, 15);
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

// "Same reverb": B is the reverb that is in the slot - a second setting of it (set up with Edit B).
// A / B tab: the reverb's FX editor, with its second effect's list open
static void on_rev_choose(lv_event_t *e)
{
    (void)e;
    if (s_view.mix_slot < 0) return;
    lv_obj_set_hidden(s_mix_editor, true);
    send('K', 0);
    s_second_pending = s_view.mix_slot;
    send('O', s_view.mix_slot);
}

// 0 cancel, 1 save, 2 edit reverb B, 3 save and take the reverb off the Nano's expression pedal.
// The tab in front decides what footswitch 8 does from now on: saved on the mix tab it switches the mix, on the
// Reverb A / B tab it swaps the two reverbs, on the 2 settings tab it changes between them. The reverb's own tile
// only switches the reverb on and off (unless a second reverb was given to that tile in its FX editor).
static void on_mix_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    bool settings = s_mix_tab_shown == 2;
    if (action == 1 && settings && s_view.rp_set != 3) {   // (the dialog stays open)
        ui_show_message("Set both settings first: tap an FX preset or Now in each row.");
        return;
    }
    lv_obj_set_hidden(s_mix_editor, true);
    bool decide = action == 1 || action == 2;   // (Free pedal leaves the switch as it is)
    bool mix = s_mix_tab_shown == 0;
    // Pos 1 / Pos 2: when moved - and the ones shown, if the preset has none yet and the switch is to use them
    if (action && (s_mix_touched || (decide && mix && !s_view.mix_known))) {
        send('W', (int)lv_slider_get_value(s_mix_slider[0]) << 8 | (int)lv_slider_get_value(s_mix_slider[1]));
    }
    if (decide) {
        uint32_t chosen = chosen_rev_b();
        bool parked = s_mix_tab_shown != 1 && chosen, was_parked = s_view.rev_b_stored && !s_view.rev_b_type;
        if (chosen != s_view.rev_b_stored || parked != was_parked) send('A', (parked ? 2 : 0) << 24 | (int)chosen);
        send('-', settings ? 17 : 16);   // the two settings are what the switch does - or not
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
        ? "The reverb's mix, Pos 1 / Pos 2. The Nano's preset still has this reverb on the expression pedal - Free pedal ends that."
        : "The reverb's mix, Pos 1 or Pos 2. Moving a slider plays that mix.");
    {   // A / B tab: the reverb's two, as its tile has them (chosen in its FX editor)
        uint32_t a = s_view.mix_slot >= 0 ? s_slot_type[s_view.mix_slot] : 0, b = s_view.rev_b_stored;
        if (s_view.rev_active && s_view.mix_slot >= 0 && s_view.ab_a_type[s_view.mix_slot]) a = s_view.ab_a_type[s_view.mix_slot];
        if (b) lv_label_set_text_fmt(s_rev_b_label, "A  %s\nB  %s", a ? nano_fx_name(a) : "-", b == a ? "the same reverb, 2nd setting" : nano_fx_name(b));
        else lv_label_set_text(s_rev_b_label, "No B yet - choose one in the reverb's editor.");
    }
    int selected = 0;
    for (int i = 0; i < s_rev_type_count; i++) if (s_rev_types[i] == s_view.rev_b_stored) selected = i + 1;
    lv_dropdown_set_selected(s_rev_dropdown, (uint32_t)selected);
    on_rev_dropdown(NULL);
    // The tab of what footswitch 8 does now is in front and carries a tick.
    int used = s_view.rp_used ? 2 : s_view.rev_b_type ? 1 : 0;
    lv_label_set_text(lv_obj_get_child(s_mix_tab[0], 0), used == 0 ? LV_SYMBOL_OK " Mix" : "Mix");
    lv_label_set_text(lv_obj_get_child(s_mix_tab[1], 0), used == 1 ? LV_SYMBOL_OK " Reverb A / B" : "Reverb A / B");
    lv_label_set_text(lv_obj_get_child(s_mix_tab[2], 0), used == 2 ? LV_SYMBOL_OK " 2 settings" : "2 settings");
    show_rp_rows();
    show_mix_tab(used);
    lv_obj_set_hidden(s_mix_editor, false);
    lv_obj_move_foreground(s_mix_editor);
}

static lv_obj_t *mix_panel(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, y);
    lv_obj_set_size(p, DLG_W - 2, h);   // the reverb dialog's width
    lv_obj_set_scrollable(p, false);
    return p;
}

static void build_mix_editor(lv_obj_t *screen)
{
    s_mix_editor = dialog_full(screen);
    dialog_title(s_mix_editor, "Footswitch 8 switches");
    s_mix_model_label = label(s_mix_editor, HINT_FONT, MUTED);
    lv_obj_align(s_mix_model_label, LV_ALIGN_TOP_RIGHT, -20, 20);
    static const char *const tabs[3] = { "Mix", "Reverb A / B", "2 settings" };
    static const int tab_x[3] = { 20, 136, 348 }, tab_w[3] = { 108, 204, 192 };
    for (int i = 0; i < 3; i++) {
        s_mix_tab[i] = small_button(s_mix_editor, tab_x[i], 54, tab_w[i], 40, tabs[i], on_mix_tab, i);
        s_mix_panel[i] = mix_panel(s_mix_editor, 106, 188);
    }

    // Tab 3: two named settings of the reverb, every parameter - the switch changes between them
    lv_obj_t *two = s_mix_panel[2];
    lv_obj_t *two_info = label(two, HINT_FONT, MUTED);
    lv_obj_set_width(two_info, DLG_W - 42);
    lv_label_set_long_mode(two_info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(two_info, "Tap an FX preset of the reverb, or Now for the reverb as it is. Tap a name to rename it.");
    lv_obj_set_pos(two_info, 20, 0);
    for (int n = 0; n < 2; n++) {   // (user, 2026-11-13: draft C - the FX presets as buttons beside each name)
        int y = 50 + n * 62;
        lv_obj_t *name = button(two, 20, y, 150, 52, on_rp_name);
        lv_obj_remove_event_cb(name, on_rp_name);
        lv_obj_add_event_cb(name, on_rp_name, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        s_rp_name[n] = label(name, &ui_font_20, TEXT);
        lv_obj_set_width(s_rp_name[n], 96);
        lv_label_set_long_mode(s_rp_name[n], LV_LABEL_LONG_DOT);
        lv_obj_align(s_rp_name[n], LV_ALIGN_LEFT_MID, 0, 0);
        s_rp_state[n] = label(name, HINT_FONT, GREEN);
        lv_obj_align(s_rp_state[n], LV_ALIGN_RIGHT_MID, 0, 0);
        for (int k = 0; k < 5; k++) {
            lv_obj_t *chip = small_button(two, 178 + k * 74, y + 4, k < 4 ? 68 : 66, 44, k < 4 ? "" : "Now", on_rp_chip, n << 4 | k);
            lv_obj_t *l = lv_obj_get_child(chip, 0);
            lv_obj_set_style_text_font(l, &ui_font_14, 0);
            lv_obj_set_width(l, 62);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(l);
            s_rp_chip[n][k] = chip;
        }
    }

    // Tab 1: Pos 1 / Pos 2 of the expression assignment, played while moving
    lv_obj_t *mix = s_mix_panel[0];
    s_mix_info = label(mix, HINT_FONT, MUTED);
    lv_obj_set_width(s_mix_info, DLG_W - 42);
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
        lv_obj_set_size(s_mix_slider[i], DLG_W - 2 - 118 - 140, 16);
        lv_obj_set_pos(s_mix_slider[i], 118, y + 16);
        lv_obj_set_ext_click_area(s_mix_slider[i], 20);
        style_slider(s_mix_slider[i], 0x00FFDD);   // reverb colour
        lv_obj_add_event_cb(s_mix_slider[i], on_mix_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        s_mix_value[i] = label(mix, &ui_font_28, 0xFFFFFF);
        lv_obj_set_width(s_mix_value[i], 90);
        lv_obj_set_style_text_align(s_mix_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_mix_value[i], DLG_W - 2 - 20 - 90, y + 6);
    }

    // Tab 2: a second reverb; footswitch 8 swaps the preset's reverb (A) and this one (B)
    lv_obj_t *rev = s_mix_panel[1];
    lv_obj_t *info = label(rev, HINT_FONT, MUTED);
    lv_obj_set_width(info, DLG_W - 42);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "Footswitch 8 swaps the reverb's A and B; the reverb's own switch only switches it on and "
                            "off. B is chosen and set up in the reverb's editor.");
    lv_obj_set_pos(info, 20, 0);
    s_rev_b_label = label(rev, &ui_font_20, TEXT);
    lv_obj_set_width(s_rev_b_label, DLG_W - 42);
    lv_obj_set_style_text_line_space(s_rev_b_label, 8, 0);
    lv_obj_set_pos(s_rev_b_label, 20, 58);
    small_button(rev, 20, 130, 320, 46, "Choose B in the reverb's editor", on_rev_choose, 0);
    static char options[512];
    size_t n = (size_t)snprintf(options, sizeof(options), "None (footswitch 8 = mix)");
    for (unsigned i = 0; i < NANO_SLOT_MODEL_COUNT[4] && s_rev_type_count < 16; i++) {
        const nano_fx_model_t *m = nano_fx_model(NANO_SLOT_MODELS[4][i]);
        if (!m || m->icon != NANO_ICON_REVERB || n + strlen(nano_fx_name(m->type)) + 2 > sizeof(options)) continue;
        s_rev_types[s_rev_type_count++] = m->type;
        n += (size_t)snprintf(options + n, sizeof(options) - n, "\n%s", nano_fx_name(m->type));
    }
    s_rev_dropdown = lv_dropdown_create(rev);
    lv_dropdown_set_options_static(s_rev_dropdown, options);
    lv_obj_set_size(s_rev_dropdown, 320, 52);
    lv_obj_set_pos(s_rev_dropdown, 20, 56);
    lv_obj_set_style_text_font(s_rev_dropdown, &ui_font_20, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_rev_dropdown), &ui_font_20, 0);
    lv_obj_add_event_cb(s_rev_dropdown, on_rev_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    s_rev_edit = small_button(rev, 360, 56, 180, 52, "Edit B", on_mix_button, 2);
    lv_obj_set_hidden(s_rev_dropdown, true);   // (B is chosen in the reverb's editor; the list stays for chosen_rev_b())
    lv_obj_set_hidden(s_rev_edit, true);

    s_mix_free = small_button(s_mix_editor, 20, 452, 160, 46, "Free pedal", on_mix_button, 3);
    small_button(s_mix_editor, DLG_W - 2 - 18 - 330, 452, 160, 46, "Cancel", on_mix_button, 0);
    primary(small_button(s_mix_editor, DLG_W - 2 - 18 - 160, 452, 160, 46, "Save", on_mix_button, 1));
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
#define MIDI_INFO "Tap a device to connect it. It reconnects by itself."
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
        lv_obj_set_size(btn, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current ? SURF_ON : SURF), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(current ? GREEN : HAIRLINE), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 9, 0);
        lv_obj_add_event_cb(btn, on_midi_device, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *name = label(btn, &ui_font_20, TEXT);
        lv_label_set_text_fmt(name, "%s%s", m->devices[i].name, current ? "  -  connected" : m->devices[i].remembered ? "  -  stored" : "");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
#ifndef NANO_WEB
        lv_obj_t *rssi = label(btn, HINT_FONT, MUTED);
        lv_label_set_text_fmt(rssi, "%d dBm", m->devices[i].rssi);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
#endif
    }
}

void ui_set_midi(const ui_midi_t *midi)
{
    lvgl_port_lock(0);
    s_midi_state = *midi;
    if (s_midi_icon) lv_obj_set_style_image_recolor(s_midi_icon, lv_color_hex(midi->connected ? GREEN : 0xC8CCD1), 0);
    if (s_set_badge[SET_MIDI]) lv_label_set_text(s_set_badge[SET_MIDI], midi->connected ? "ON" : "");
    if (shown(s_midi)) fill_midi();
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

// ---- tile layout: 6 or 8 fields, in any order ----

static lv_obj_t *s_layout_tab[2], *s_layout_cell[TILES], *s_layout_cell_number[TILES], *s_layout_cell_name[TILES];
static lv_obj_t *s_layout_mode_tab[2];   // which order is edited: presets (and scenes) | FX mode
static lv_obj_t *s_layout_hidden_label, *s_layout_switch1;
static int s_switch1, s_lay_switch1;   // what footswitch 1 changes between (UI_SWITCH1_*): stored / being edited
static const char *const SWITCH1_NAMES[UI_SWITCH1_COUNT] = {
    "Switch 1: FX / presets", "Switch 1: FX / scenes", "Switch 1: presets / scenes",
    "Switch 1: presets, scenes, FX",
};
// Being edited: position -> switch 1-8 (all eight; the last ones are not shown), [0] presets and scenes, [1] FX mode.
static uint8_t s_lay_orders[2][TILES];
static int s_lay_count, s_lay_selected = -1, s_lay_fx;
// FX mode's order when it is not set (as main.c's LAYOUT_FX_DEFAULT): [0] 6 fields, [1] 8 fields
static const uint8_t LAY_FX_DEFAULT[2][TILES] = { { 3, 4, 1, 5, 6, 7, 2, 8 }, { 3, 4, 7, 2, 5, 6, 8, 1 } };

static const char *const LAYOUT_NAMES[TILES] = {
    "Mode", "Tuner", "Pre FX 1\npreset / scene 1", "Pre FX 2\npreset / scene 2", "Post FX 1\npreset / scene 3",
    "Post FX 2\npreset / scene 4", "Post FX 3\npreset / scene 5", "Reverb\npreset / scene 6",
};

#define LAYOUT_GRID_Y 98
#define LAYOUT_CELL_H 100

static void fill_layout(void)
{
    style_tab(s_layout_tab[0], s_lay_count == 6);
    style_tab(s_layout_tab[1], s_lay_count == 8);
    style_tab(s_layout_mode_tab[0], !s_lay_fx);
    style_tab(s_layout_mode_tab[1], s_lay_fx);
    int columns = s_lay_count / 2, w = (DLG_W - 2 - 40 - (columns - 1) * 10) / columns;
    for (int p = 0; p < TILES; p++) {
        lv_obj_t *c = s_layout_cell[p];
        bool shown = p < s_lay_count;
        if (shown) {
            lv_obj_set_pos(c, 20 + (p % columns) * (w + 10), LAYOUT_GRID_Y + (p / columns) * (LAYOUT_CELL_H + 10));
            lv_obj_set_size(c, w, LAYOUT_CELL_H);
            lv_label_set_text_fmt(s_layout_cell_number[p], "%d", p + 1);
        } else {   // below "Not on the screen"
            lv_obj_set_pos(c, 20 + (p - s_lay_count) * 240, 356);
            lv_obj_set_size(c, 230, 56);
            lv_label_set_text(s_layout_cell_number[p], "");
        }
        int sw = s_lay_orders[s_lay_fx][p] - 1;
        if (shown && !s_lay_fx) {
            lv_label_set_text(s_layout_cell_name[p], LAYOUT_NAMES[sw]);
        } else {   // one line: the name only (FX mode: what the switch is there)
            char first[24];
            strlcpy(first, LAYOUT_NAMES[sw], sizeof(first));
            first[strcspn(first, "\n")] = 0;
            lv_label_set_text(s_layout_cell_name[p], first);
        }
        lv_obj_set_width(s_layout_cell_name[p], (shown ? w : 210) - 44);
        bool sel = p == s_lay_selected;
        lv_obj_set_style_border_width(c, sel ? 3 : 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(sel ? 0xFFFFFF : 0x3A3D40), 0);
        lv_obj_set_hidden(c, false);
    }
    lv_obj_set_hidden(s_layout_hidden_label, s_lay_count == TILES);
    lv_label_set_text(lv_obj_get_child(s_layout_switch1, 0), SWITCH1_NAMES[s_lay_switch1]);
}

static void on_layout_cell(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_lay_selected < 0) {
        s_lay_selected = p;
    } else {   // the second tap: the two swap places
        uint8_t t = s_lay_orders[s_lay_fx][p];
        s_lay_orders[s_lay_fx][p] = s_lay_orders[s_lay_fx][s_lay_selected];
        s_lay_orders[s_lay_fx][s_lay_selected] = t;
        s_lay_selected = -1;
    }
    fill_layout();
}

static void on_layout_button(lv_event_t *e)
{
    // 0 close, 6 / 8 fields, -1 default, 1 save, 2 switch 1, 10 / 11 the presets' / FX mode's order
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    s_lay_selected = -1;
    if (action == 2) {
        s_lay_switch1 = (s_lay_switch1 + 1) % UI_SWITCH1_COUNT;
    } else if (action == 10 || action == 11) {
        s_lay_fx = action == 11;
    } else if (action == 6 || action == 8) {
        // (FX mode's order as it came: the one for the other number of fields)
        if (!memcmp(s_lay_orders[1], LAY_FX_DEFAULT[s_lay_count == TILES], TILES))
            memcpy(s_lay_orders[1], LAY_FX_DEFAULT[action == TILES], TILES);
        s_lay_count = action;
    } else if (action == -1) {   // the order shown back as it came
        if (s_lay_fx) {
            memcpy(s_lay_orders[1], LAY_FX_DEFAULT[s_lay_count == TILES], TILES);
        } else {
            for (int p = 0; p < TILES; p++) s_lay_orders[0][p] = (uint8_t)(p + 1);
            if (!memcmp(s_lay_orders[1], LAY_FX_DEFAULT[s_lay_count == TILES], TILES)) memcpy(s_lay_orders[1], LAY_FX_DEFAULT[1], TILES);
            s_lay_count = TILES;
            s_lay_switch1 = UI_SWITCH1_FX_PRESETS;
        }
    } else {
        lv_obj_set_hidden(s_layout, true);
        if (action == 1) {   // the presets' order (with the count and switch 1), then FX mode's
            for (int fx = 0; fx < 2; fx++) {
                int packed = fx << 30 | s_lay_switch1 << 28 | s_lay_count << 24;
                for (int p = 0; p < TILES; p++) packed |= (s_lay_orders[fx][p] - 1) << (3 * p);
                send(']', packed);
            }
        }
        return;
    }
    fill_layout();
}

static void open_layout(lv_event_t *e)
{
    memcpy(s_lay_orders[0], s_layout_order, TILES);
    memcpy(s_lay_orders[1], s_layout_fx_order, TILES);
    s_lay_fx = s_fx_mode;   // (the order of the mode the board is in)
    s_lay_count = s_layout_count;
    s_lay_switch1 = s_switch1;
    s_lay_selected = -1;
    fill_layout();
    lv_obj_set_hidden(s_layout, false);
    lv_obj_move_foreground(s_layout);
}

static void build_layout(lv_obj_t *screen)
{
    s_layout = dialog_full(screen);   // P4: under the top bar, the whole width
    dialog_title(s_layout, "Tile layout");
    s_layout_tab[0] = small_button(s_layout, DLG_W - 60 - 268, 12, 120, 44, "6 fields", on_layout_button, 6);
    s_layout_tab[1] = small_button(s_layout, DLG_W - 60 - 140, 12, 120, 44, "8 fields", on_layout_button, 8);
    s_layout_mode_tab[0] = small_button(s_layout, DLG_W - 60 - 268 - 24 - 2 * 140, 12, 136, 44, "Presets", on_layout_button, 10);
    s_layout_mode_tab[1] = small_button(s_layout, DLG_W - 60 - 268 - 24 - 140, 12, 136, 44, "FX mode", on_layout_button, 11);
    close_button(s_layout, DLG_W - 60, 12, on_layout_button, 0);
    lv_obj_t *info = label(s_layout, HINT_FONT, MUTED);
    lv_obj_set_width(info, DLG_W - 40);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "Tap two fields to swap them. Field n is footswitch n and MIDI CC 49 + n. FX mode has its own order.");
    lv_obj_set_pos(info, 20, 68);
    s_layout_hidden_label = label(s_layout, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(s_layout_hidden_label, 2, 0);
    lv_label_set_text(s_layout_hidden_label, "NOT ON THE SCREEN");
    lv_obj_set_pos(s_layout_hidden_label, 20, 332);
    for (int p = 0; p < TILES; p++) {
        lv_obj_t *c = button(s_layout, 0, 0, 100, 50, on_layout_cell);
        lv_obj_remove_event_cb(c, on_layout_cell);
        lv_obj_add_event_cb(c, on_layout_cell, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        s_layout_cell[p] = c;
        s_layout_cell_number[p] = label(c, &ui_font_20, TEXT);
        lv_obj_align(s_layout_cell_number[p], LV_ALIGN_TOP_RIGHT, -4, -2);
        s_layout_cell_name[p] = label(c, &ui_font_16, TEXT);
        lv_label_set_long_mode(s_layout_cell_name[p], LV_LABEL_LONG_WRAP);
        lv_obj_align(s_layout_cell_name[p], LV_ALIGN_LEFT_MID, 0, 0);
    }
    small_button(s_layout, 20, 452, 150, 46, "Default", on_layout_button, -1);
    // Between them: what footswitch 1 changes between (a tap takes the next choice)
    s_layout_switch1 = small_button(s_layout, 180, 452, 400, 46, SWITCH1_NAMES[0], on_layout_button, 2);
    primary(small_button(s_layout, DLG_W - 2 - 18 - 160, 452, 160, 46, "Save", on_layout_button, 1));
    lv_obj_set_hidden(s_layout, true);
}

// A scrolling list of entries as the bank editor's presets (set list, capture of a scene).
static lv_obj_t *list_entry(lv_obj_t *list, const char *text, bool selected, uint32_t frame, lv_event_cb_t cb, int data)
{
    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_size(btn, LV_PCT(100), 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x2A2E33 : 0x1A1D20), 0);
    lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(frame), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 9, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t *l = label(btn, &ui_font_20, selected ? 0xFFFFFF : 0xD8D8D8);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return btn;
}

static lv_obj_t *scroll_list(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_pos(list, x, y);
    lv_obj_set_size(list, w, h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

// ---- set list: the songs in the order of the gig (tap the preset / bank line at the top) ----

static void fill_setlist(void);
static lv_obj_t *frame_button(lv_obj_t *parent);
static void style_frame_button(lv_obj_t *b, uint32_t color, bool active, bool empty);

static void on_setlist_song(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_sl_selected = i == s_sl_selected ? -1 : i;
    fill_setlist();
}

// A preset tapped on the right: a new song after the chosen one (or at the end).
static void on_setlist_preset(lv_event_t *e)
{
    int preset = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_sl_count >= UI_SETLIST_MAX) {
        ui_show_message("The set list is full (36 songs).");
        return;
    }
    int at = s_sl_selected >= 0 ? s_sl_selected + 1 : s_sl_count;
    memmove(s_sl_list + at + 1, s_sl_list + at, (size_t)(s_sl_count - at));
    memmove(s_sl_scenes + at + 1, s_sl_scenes + at, (size_t)(s_sl_count - at));
    s_sl_list[at] = (uint8_t)preset;
    s_sl_scenes[at] = 0;
    s_sl_count++;
    s_sl_selected = at;
    fill_setlist();
}

static void on_setlist_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 close, 1 save, 2 off, 3 on, 4 up, 5 down, 6 remove, 7 clear
    int i = s_sl_selected;
    if (action == 2 || action == 3) s_sl_on = action == 3;
    else if ((action == 4 && i > 0) || (action == 5 && i >= 0 && i + 1 < s_sl_count)) {
        int j = action == 4 ? i - 1 : i + 1;
        uint8_t t = s_sl_list[i], sc = s_sl_scenes[i];
        s_sl_list[i] = s_sl_list[j];
        s_sl_scenes[i] = s_sl_scenes[j];
        s_sl_list[j] = t;
        s_sl_scenes[j] = sc;
        s_sl_selected = j;
    } else if (action == 6 && i >= 0) {
        memmove(s_sl_list + i, s_sl_list + i + 1, (size_t)(s_sl_count - i - 1));
        memmove(s_sl_scenes + i, s_sl_scenes + i + 1, (size_t)(s_sl_count - i - 1));
        s_sl_count--;
        s_sl_selected = i < s_sl_count ? i : s_sl_count - 1;
    } else if (action == 7) {
        s_sl_count = 0;
        s_sl_selected = -1;
    } else if (action == 0 || action == 1) {
        lv_obj_set_hidden(s_setlist, true);
        if (action == 1 && s_on_text) {
            char text[2 + 3 * UI_SETLIST_MAX + 1];
            int n = snprintf(text, sizeof(text), "%c", s_sl_on ? '1' : '0');
            for (int k = 0; k < s_sl_count; k++) n += snprintf(text + n, sizeof(text) - (size_t)n, "%02X%c", s_sl_list[k], '0' + s_sl_scenes[k]);
            s_on_text('T', text);
        }
        return;
    } else {
        return;
    }
    fill_setlist();
}

static void scene_names_of(int preset, char names[UI_SCENES][UI_SCENE_NAME])
{
    memset(names, 0, UI_SCENES * UI_SCENE_NAME);
    if (s_scene_names) s_scene_names(preset, names);
}

static void on_setlist_scene(lv_event_t *e)
{
    int scene = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_sl_selected < 0) return;
    char names[UI_SCENES][UI_SCENE_NAME];
    scene_names_of(s_sl_list[s_sl_selected], names);
    if (scene && !names[scene - 1][0]) {
        ui_show_message("This scene of the preset is empty - set it up in scene mode first.");
        return;
    }
    s_sl_scenes[s_sl_selected] = (uint8_t)scene;
    fill_setlist();
}

// The row of start scenes, for the chosen song: None and the preset's six scenes (empty ones grey).
static void style_setlist_scenes(void)
{
    char names[UI_SCENES][UI_SCENE_NAME];
    bool song = s_sl_selected >= 0 && s_sl_selected < s_sl_count;
    if (song) scene_names_of(s_sl_list[s_sl_selected], names);
    lv_label_set_text(s_sl_scene_hint, song ? "START SCENE OF THE CHOSEN SONG" : "START SCENE: CHOOSE A SONG FIRST");
    for (int i = 0; i <= UI_SCENES; i++) {
        lv_obj_t *chip = s_sl_scene_chip[i];
        bool filled = song && (i == 0 || names[i - 1][0]);
        bool chosen = song && s_sl_scenes[s_sl_selected] == i;
        char text[UI_SCENE_NAME + 4];
        if (i == 0) snprintf(text, sizeof(text), "None");
        else if (song && names[i - 1][0]) snprintf(text, sizeof(text), "%s", names[i - 1]);
        else snprintf(text, sizeof(text), "%d", i);
        lv_obj_t *l = lv_obj_get_child(chip, 0);
        lv_label_set_text(l, text);
        style_frame_button(chip, 0xECECEC, chosen, !filled);
        lv_obj_set_style_text_color(l, lv_color_hex(chosen ? INK : filled ? TEXT : 0x5D6267), 0);
    }
}

static void fill_setlist(void)
{
    char text[80];
    style_tab(s_sl_tab[0], !s_sl_on);
    style_tab(s_sl_tab[1], s_sl_on);
    style_setlist_scenes();
    lv_obj_clean(s_sl_songs);
    lv_obj_t *selected = NULL;
    for (int i = 0; i < s_sl_count; i++) {
        int preset = s_sl_list[i];
        int n = snprintf(text, sizeof(text), "%2d   %.40s", i + 1, s_preset_names[preset - 1][0] ? s_preset_names[preset - 1] : "-");
        if (s_sl_scenes[i] && n > 0 && n < (int)sizeof(text)) {   // " · scene"
            char names[UI_SCENES][UI_SCENE_NAME];
            scene_names_of(preset, names);
            const char *scene = names[s_sl_scenes[i] - 1];
            if (scene[0]) snprintf(text + n, sizeof(text) - (size_t)n, "  \xC2\xB7  %.15s", scene);
            else snprintf(text + n, sizeof(text) - (size_t)n, "  \xC2\xB7  scene %d", s_sl_scenes[i]);
        }
        lv_obj_t *entry = list_entry(s_sl_songs, text, i == s_sl_selected, 0xFFFFFF, on_setlist_song, i);
        if (i == s_sl_selected) selected = entry;
    }
    if (!s_sl_count) {
        lv_obj_t *l = label(s_sl_songs, &ui_font_16, MUTED);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, "No songs yet - tap presets on the right to add them in the order of the gig.");
    }
    lv_obj_update_layout(s_sl_songs);
    if (selected) lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
    if (lv_obj_get_child_count(s_sl_presets)) return;   // the presets stay built while the dialog is open
    for (int preset = 1; preset <= NANO_PRESETS; preset++) {
        snprintf(text, sizeof(text), "%2d   %.40s", preset, s_preset_names[preset - 1][0] ? s_preset_names[preset - 1] : "-");
        list_entry(s_sl_presets, text, false, 0, on_setlist_preset, preset);
    }
}

// song: the song to choose in the list (a long press on its tile), -1 = none.
static void open_setlist_at(int song)
{
    s_sl_count = s_view.setlist_count <= UI_SETLIST_MAX ? s_view.setlist_count : 0;
    memcpy(s_sl_list, s_view.setlist_presets, sizeof(s_sl_list));
    memcpy(s_sl_scenes, s_view.setlist_scenes, sizeof(s_sl_scenes));
    s_sl_on = s_view.setlist_on;
    s_sl_selected = song >= 0 && song < s_sl_count ? song : -1;
    lv_obj_clean(s_sl_presets);   // names may have changed
    fill_setlist();
    lv_obj_set_hidden(s_setlist, false);
    lv_obj_move_foreground(s_setlist);
}

static void build_setlist(lv_obj_t *screen)
{
    s_setlist = dialog_full(screen);   // P4: under the top bar, the whole width
    dialog_title(s_setlist, "Set list");
    s_sl_tab[0] = small_button(s_setlist, DLG_W - 318, 12, 110, 44, "Off", on_setlist_button, 2);
    s_sl_tab[1] = small_button(s_setlist, DLG_W - 200, 12, 110, 44, "On", on_setlist_button, 3);
    close_button(s_setlist, DLG_W - 60, 12, on_setlist_button, 0);
    lv_obj_t *hint = label(s_setlist, HINT_FONT, MUTED);
    lv_obj_set_width(hint, DLG_W - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "On: footswitches 3-8 are the songs. Tap a preset to add it after the chosen one.");
    lv_obj_set_pos(hint, 20, 62);
    lv_obj_t *songs = label(s_setlist, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(songs, 2, 0);
    lv_label_set_text(songs, "SONGS");
    lv_obj_set_pos(songs, 20, 106);
    lv_obj_t *presets = label(s_setlist, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(presets, 2, 0);
    lv_label_set_text(presets, "ADD A PRESET");
    lv_obj_set_pos(presets, 520, 106);
    s_sl_songs = scroll_list(s_setlist, 10, 124, 490, 250);
    s_sl_presets = scroll_list(s_setlist, 510, 124, DLG_W - 2 - 520, 250);
    s_sl_scene_hint = label(s_setlist, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(s_sl_scene_hint, 2, 0);
    lv_obj_set_pos(s_sl_scene_hint, 20, 384);
    for (int i = 0; i <= UI_SCENES; i++) {
        lv_obj_t *chip = frame_button(s_setlist);
        lv_obj_set_pos(chip, 18 + i * 137, 404);
        lv_obj_set_size(chip, 127, 48);
        lv_obj_add_event_cb(chip, on_setlist_scene, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = label(chip, &ui_font_14, TEXT);
        lv_obj_set_width(l, 122);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_center(l);
        s_sl_scene_chip[i] = chip;
    }
    small_button(s_setlist, 18, 460, 64, 46, LV_SYMBOL_UP, on_setlist_button, 4);
    small_button(s_setlist, 90, 460, 64, 46, LV_SYMBOL_DOWN, on_setlist_button, 5);
    small_button(s_setlist, 162, 460, 110, 46, "Remove", on_setlist_button, 6);
    small_button(s_setlist, 280, 460, 100, 46, "Clear", on_setlist_button, 7);
    small_button(s_setlist, DLG_W - 20 - 290, 460, 130, 46, "Cancel", on_setlist_button, 0);
    primary(small_button(s_setlist, DLG_W - 20 - 150, 460, 140, 46, "Save", on_setlist_button, 1));
    lv_obj_set_hidden(s_setlist, true);
}

void ui_set_scene_names_source(ui_scene_names_cb cb)
{
    s_scene_names = cb;
}

// ---- preset list: the Nano's 64 presets, a page of sixteen at a time (a tap on the preset's name) ----
// Fields instead of a scrolling list: a page is drawn once and nothing moves under the finger. "New preset" asks for
// the place first (a tap on a field), then for what it starts from and its name; the controller does the rest (text
// 'P': "<place, two hex digits><0 = the standard board / 1 = a copy of the preset that is on>name", see preset_new
// in main.c). A tap on a preset sends 'g' with its number.
// (From the 4.3" board, 1.25.0; here laid out under the top bar over the whole width - build_presets and the PL_*
// sizes are P4-only.)
#define PL_PAGES 4
#define PL_CELLS 16
#define PL_X 16
#define PL_Y 68
#define PL_GAP 8
#define PL_CELL_W ((DLG_W - 2 * PL_X - 3 * PL_GAP) / 4)   // 236
#define PL_CELL_H 88
#define PL_SHEET_W 720
#define PL_SHEET_H 372
static lv_obj_t *s_pl_title, *s_pl_tab[PL_PAGES], *s_pl_cell[PL_CELLS], *s_pl_num[PL_CELLS], *s_pl_name[PL_CELLS];
static lv_obj_t *s_pl_new, *s_pl_hint, *s_pl_sheet, *s_pl_sheet_title, *s_pl_sheet_note, *s_pl_start[2], *s_pl_start_note[2], *s_pl_name_text;
static int s_pl_page, s_pl_place, s_pl_start_mode;
static bool s_pl_choosing;         // "New preset": the next tap on a field chooses its place
static char s_pl_new_name[33];

static void fill_presets(void)
{
    char text[12];
    lv_label_set_text(s_pl_title, s_pl_choosing ? "New preset" : "Presets");
    for (int i = 0; i < PL_PAGES; i++) style_tab(s_pl_tab[i], i == s_pl_page);
    for (int i = 0; i < PL_CELLS; i++) {
        int preset = s_pl_page * PL_CELLS + i + 1;
        bool current = preset == s_current_preset;
        const char *name = s_preset_names[preset - 1];
        snprintf(text, sizeof(text), "%d", preset);
        set_text(s_pl_num[i], text);
        set_text(s_pl_name[i], name[0] ? name : "-");
        // The preset that is on: filled light (as a chosen tab); the others as buttons
        set_color_prop(s_pl_cell[i], LV_STYLE_BG_COLOR, current ? 0xECECEC : BUTTON_BG);
        set_color_prop(s_pl_cell[i], LV_STYLE_BORDER_COLOR, current ? 0xECECEC : BUTTON_LINE);
        set_color_prop(s_pl_num[i], LV_STYLE_TEXT_COLOR, current ? 0x4A4E53 : MUTED);
        set_color_prop(s_pl_name[i], LV_STYLE_TEXT_COLOR, current ? INK : TEXT);
    }
    lv_label_set_text(lv_obj_get_child(s_pl_new, 0), s_pl_choosing ? "Cancel" : "New preset");
    lv_label_set_text(s_pl_hint, s_pl_choosing ? "Tap the place the new preset goes into - what is there is replaced."
                                               : "Tap a preset to load it. The one that is on is filled.");
    set_hidden(s_pl_sheet, true);
}

static void close_presets(void)
{
    lv_obj_set_hidden(s_presets, true);
}

// The sheet of a new preset: its place, what it starts from, its name.
static void show_new_sheet(void)
{
    char text[120];
    const char *old = s_preset_names[s_pl_place - 1];
    snprintf(text, sizeof(text), "New preset in place %d", s_pl_place);
    lv_label_set_text(s_pl_sheet_title, text);
    snprintf(text, sizeof(text), "It replaces \"%.60s\" on the Nano.", old[0] ? old : "-");
    lv_label_set_text(s_pl_sheet_note, text);
    bool copy_possible = s_pl_place != s_current_preset;
    if (!copy_possible) s_pl_start_mode = 0;
    for (int i = 0; i < 2; i++) {
        bool on = i == s_pl_start_mode;
        choice_on(s_pl_start[i], on);
        set_color_prop(s_pl_start_note[i], LV_STYLE_TEXT_COLOR, on ? 0x4A4E53 : MUTED);
    }
    if (copy_possible) lv_obj_remove_state(s_pl_start[1], LV_STATE_DISABLED);
    else lv_obj_add_state(s_pl_start[1], LV_STATE_DISABLED);
    const char *now = s_current_preset >= 1 ? s_preset_names[s_current_preset - 1] : "";
    snprintf(text, sizeof(text), "\"%.40s\" as it plays now", now[0] ? now : "-");
    lv_label_set_text(s_pl_start_note[1], copy_possible ? text : "The place is the preset that is on");
    lv_label_set_text(s_pl_name_text, s_pl_new_name);
    set_hidden(s_pl_sheet, false);
}

static void on_presets_cell(lv_event_t *e)
{
    int preset = s_pl_page * PL_CELLS + (int)(intptr_t)lv_event_get_user_data(e) + 1;
    if (!lv_obj_is_hidden(s_pl_sheet)) return;
    if (s_pl_choosing) {
        s_pl_place = preset;
        s_pl_start_mode = 0;
        // A name that is not taken yet: "New preset", then with the place's number
        snprintf(s_pl_new_name, sizeof(s_pl_new_name), "New preset");
        for (int i = 0; i < NANO_PRESETS; i++) {
            if (i != preset - 1 && !strcasecmp(s_preset_names[i], s_pl_new_name)) {
                snprintf(s_pl_new_name, sizeof(s_pl_new_name), "New preset %d", preset);
                break;
            }
        }
        show_new_sheet();
        return;
    }
    close_presets();
    if (preset != s_current_preset) send('g', preset);
}

static void on_presets_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 close, 1 new preset / cancel, 10-13 page, 20 / 21 start from, 30 name, 40 sheet cancel, 41 create
    if (action == 0) {
        close_presets();
    } else if (action == 1) {
        if (!s_linked) {
            ui_show_message("The Nano is not connected.");
            return;
        }
        s_pl_choosing = !s_pl_choosing;
        fill_presets();
    } else if (action >= 10 && action < 10 + PL_PAGES) {
        if (!lv_obj_is_hidden(s_pl_sheet)) return;
        s_pl_page = action - 10;
        fill_presets();
    } else if (action == 20 || action == 21) {
        s_pl_start_mode = action - 20;
        show_new_sheet();
    } else if (action == 30) {
        char title[40];
        snprintf(title, sizeof(title), "Name of the new preset (place %d)", s_pl_place);
        show_rename('p', title, "OK", s_pl_new_name, 32);
    } else if (action == 40) {
        set_hidden(s_pl_sheet, true);
    } else if (action == 41) {
        size_t len = strlen(s_pl_new_name);
        if (len < 4 || len > 32) {
            ui_show_message("Preset names need 4 to 32 characters.");
            return;
        }
        for (int i = 0; i < NANO_PRESETS; i++) {
            if (i != s_pl_place - 1 && !strcasecmp(s_preset_names[i], s_pl_new_name)) {
                ui_show_message("This preset name already exists - tap the name to change it.");
                return;
            }
        }
        char text[40];
        snprintf(text, sizeof(text), "%02X%c%s", s_pl_place, s_pl_start_mode ? '1' : '0', s_pl_new_name);
        s_pl_choosing = false;
        close_presets();
        if (s_on_text) s_on_text('P', text);
    }
}

// From the keyboard: the new preset's name (kind 'p').
static void preset_new_named(const char *text)
{
    strlcpy(s_pl_new_name, text, sizeof(s_pl_new_name));
    if (s_presets && !lv_obj_is_hidden(s_presets) && s_pl_place) show_new_sheet();
}

static void open_presets(lv_event_t *e)
{
    (void)e;
    if (s_current_preset < 1) return;   // (no preset shown yet)
    s_pl_choosing = false;
    s_pl_place = 0;
    s_pl_page = (s_current_preset - 1) / PL_CELLS;
    fill_presets();
    lv_obj_set_hidden(s_presets, false);
    lv_obj_move_foreground(s_presets);
}

static void build_presets(lv_obj_t *screen)
{
    s_presets = dialog_full(screen);   // P4: under the top bar, the whole width
    s_pl_title = dialog_title(s_presets, "Presets");
    for (int i = 0; i < PL_PAGES; i++) {   // the four pages, at the right before the close button (as the set list's Off / On)
        char text[12];
        snprintf(text, sizeof(text), "%d-%d", i * PL_CELLS + 1, (i + 1) * PL_CELLS);
        s_pl_tab[i] = small_button(s_presets, DLG_W - 84 - (PL_PAGES - i) * 132, 12, 124, 44, text, on_presets_button, 10 + i);
    }
    close_button(s_presets, DLG_W - 60, 12, on_presets_button, 0);
    for (int i = 0; i < PL_CELLS; i++) {
        lv_obj_t *cell = button(s_presets, PL_X + (i % 4) * (PL_CELL_W + PL_GAP), PL_Y + (i / 4) * (PL_CELL_H + PL_GAP), PL_CELL_W, PL_CELL_H,
                                on_presets_cell);
        lv_obj_remove_event_cb(cell, on_presets_cell);
        lv_obj_add_event_cb(cell, on_presets_cell, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        s_pl_num[i] = label(cell, &ui_font_14, MUTED);
        lv_obj_set_pos(s_pl_num[i], 12, 7);
        s_pl_name[i] = label(cell, &ui_font_20, TEXT);
        lv_obj_set_pos(s_pl_name[i], 12, 28);
        lv_obj_set_size(s_pl_name[i], PL_CELL_W - 22, 50);   // two lines, then dots
        lv_label_set_long_mode(s_pl_name[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_line_space(s_pl_name[i], -2, 0);
        s_pl_cell[i] = cell;
    }
    int foot_y = PL_Y + 4 * (PL_CELL_H + PL_GAP) + 4;   // 456
    s_pl_new = small_button(s_presets, PL_X, foot_y, 180, 48, "New preset", on_presets_button, 1);
    s_pl_hint = label(s_presets, HINT_FONT, MUTED);
    lv_obj_set_width(s_pl_hint, DLG_W - PL_X - 200 - 20);
    lv_label_set_long_mode(s_pl_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_pl_hint, PL_X + 200, foot_y + 14);

    // The sheet of a new preset, over the fields
    s_pl_sheet = card(s_presets, (DLG_W - PL_SHEET_W) / 2, PL_Y + 2, PL_SHEET_W, PL_SHEET_H);
    lv_obj_set_style_bg_color(s_pl_sheet, lv_color_hex(BUTTON_BG), 0);
    lv_obj_set_style_border_color(s_pl_sheet, lv_color_hex(FILL_LINE), 0);
    lv_obj_set_clickable(s_pl_sheet, true);   // (nothing below it is touched through it)
    s_pl_sheet_title = dialog_title(s_pl_sheet, "");
    lv_obj_set_pos(s_pl_sheet_title, 20, 16);
    s_pl_sheet_note = label(s_pl_sheet, HINT_FONT, MUTED);
    lv_obj_set_width(s_pl_sheet_note, PL_SHEET_W - 40);
    lv_label_set_long_mode(s_pl_sheet_note, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_pl_sheet_note, 20, 50);
    lv_obj_t *caption = label(s_pl_sheet, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(caption, 2, 0);
    lv_label_set_text(caption, "START FROM");
    lv_obj_set_pos(caption, 20, 88);
    static const char *const STARTS[2] = { "Standard board, all off", "Copy of this preset" };
    int start_w = (PL_SHEET_W - 40 - 12) / 2;   // 334
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = button(s_pl_sheet, 20 + i * (start_w + 12), 110, start_w, 84, on_presets_button);
        lv_obj_remove_event_cb(b, on_presets_button);
        lv_obj_add_event_cb(b, on_presets_button, LV_EVENT_CLICKED, (void *)(intptr_t)(20 + i));
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_t *l = label(b, &ui_font_20, TEXT);   // (child 0: choice_on colours it)
        lv_label_set_text(l, STARTS[i]);
        lv_obj_set_pos(l, 16, 13);
        s_pl_start_note[i] = label(b, &ui_font_14, MUTED);
        lv_obj_set_width(s_pl_start_note[i], start_w - 30);
        lv_label_set_long_mode(s_pl_start_note[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_pl_start_note[i], 16, 48);
        s_pl_start[i] = b;
    }
    lv_label_set_text(s_pl_start_note[0], "Comp, drive, chorus, delay, reverb");
    caption = label(s_pl_sheet, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(caption, 2, 0);
    lv_label_set_text(caption, "NAME");
    lv_obj_set_pos(caption, 20, 210);
    lv_obj_t *name = button(s_pl_sheet, 20, 232, PL_SHEET_W - 40, 56, on_presets_button);
    lv_obj_remove_event_cb(name, on_presets_button);
    lv_obj_add_event_cb(name, on_presets_button, LV_EVENT_CLICKED, (void *)(intptr_t)30);
    s_pl_name_text = label(name, &ui_font_20, TEXT);
    lv_obj_set_width(s_pl_name_text, PL_SHEET_W - 80);
    lv_label_set_long_mode(s_pl_name_text, LV_LABEL_LONG_DOT);
    lv_obj_align(s_pl_name_text, LV_ALIGN_LEFT_MID, 0, 0);
    small_button(s_pl_sheet, PL_SHEET_W - 20 - 150 - 10 - 140, PL_SHEET_H - 18 - 48, 140, 48, "Cancel", on_presets_button, 40);
    primary(small_button(s_pl_sheet, PL_SHEET_W - 20 - 150, PL_SHEET_H - 18 - 48, 150, 48, "Create", on_presets_button, 41));
    lv_obj_set_hidden(s_pl_sheet, true);
    lv_obj_set_hidden(s_presets, true);
}

// ---- system page: is everything well (the System button of the MIDI dialog) ----

static void on_system_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 1 open / refresh, 0 close
    if (action) send(')', 0);
    else lv_obj_set_hidden(s_system, true);
}

// The system page as rows - what it is on the left, its value on the right, in groups (a blank line in the text).
// A line that begins with '!' is something to look at: its value is orange.
static lv_obj_t *s_system_rows;
// A memory bar under a system card's line: the card takes the whole width; orange from 85 %.
static void system_bar(lv_obj_t *card, int full)
{
    lv_obj_set_width(card, SET_W);
    lv_obj_t *track = lv_obj_create(card);
    lv_obj_remove_style_all(track);
    lv_obj_set_style_bg_color(track, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_size(track, LV_PCT(100), 6);
    lv_obj_set_style_margin_top(track, 6, 0);
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_style_bg_color(fill, lv_color_hex(full >= 85 ? ORANGE : TEXT), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, 3, 0);
    lv_obj_set_size(fill, LV_PCT(full < 0 ? 0 : full > 100 ? 100 : full), 6);
}

static void system_rows(const char *text)
{
    // P4: each group of the text is a card - its first row as caption (small grey capitals) and large value, the
    // others as small lines; a value with "% full" gets a bar (orange from 85 %). Cards are one column wide, two
    // side by side where they fit; a card with a bar takes the whole width.
    lv_obj_clean(s_system_rows);
    lv_obj_set_flex_flow(s_system_rows, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_system_rows, MARGIN, 0);
    lv_obj_set_style_pad_column(s_system_rows, MARGIN, 0);
    static char buf[768];   // (called with the screen's lock held; not on the caller's stack)
    snprintf(buf, sizeof(buf), "%s", text);
    lv_obj_t *card = NULL;
    int rows = 0;
    for (char *line = buf, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!*line) { card = NULL; continue; }
        bool warn = *line == '!';
        if (warn) line++;
        char *value = strstr(line, ": ");
        if (value) { *value = 0; value += 2; }
        if (!card) {
            card = lv_obj_create(s_system_rows);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, (SET_W - MARGIN) / 2, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(card, RADIUS, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(SURF), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(HAIRLINE), 0);
            lv_obj_set_style_pad_hor(card, 16, 0);
            lv_obj_set_style_pad_ver(card, 14, 0);
            lv_obj_set_style_pad_row(card, 4, 0);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            rows = 0;
            char cap[48];
            size_t n = 0;
            for (; line[n] && n < sizeof(cap) - 1; n++) cap[n] = (char)(line[n] >= 'a' && line[n] <= 'z' ? line[n] - 32 : line[n]);
            cap[n] = 0;
            lv_obj_t *c = label(card, &ui_font_12, CAPTION);
            lv_obj_set_style_text_letter_space(c, 1, 0);
            lv_label_set_text(c, cap);
            lv_obj_t *v = label(card, &ui_font_title_22, warn ? ORANGE : TEXT);
            lv_label_set_text(v, value ? value : "");
            const char *pct = value ? strstr(value, "% full") : NULL;
            if (pct) system_bar(card, atoi(value));
        } else {
            lv_obj_t *l = label(card, &ui_font_14, warn ? ORANGE : CAPTION);
            lv_label_set_text_fmt(l, "%s%s%s", line, value ? "  \xC2\xB7  " : "", value ? value : "");
            if (rows++ == 0) lv_obj_set_style_margin_top(l, 2, 0);
            if (value && strstr(value, "% full")) system_bar(card, atoi(value));
        }
    }
}

static void settings_select(int topic, bool ask);

void ui_show_system(const char *text)
{
    lvgl_port_lock(0);
    system_rows(text);
    if (lv_obj_is_hidden(s_settings) || s_set_topic != SET_SYSTEM) {   // (the console's ')' opens it from anywhere)
        lv_obj_set_hidden(s_settings, false);
        lv_obj_move_foreground(s_settings);
        settings_select(SET_SYSTEM, false);
    }
    lvgl_port_unlock();
}

static void build_system(lv_obj_t *screen)
{
    s_system = panel(screen, 70, 40, 660, 400);
    dialog_title(s_system, "System");
    s_system_close = close_button(s_system, 600, 12, on_system_button, 0);
    s_system_rows = lv_obj_create(s_system);
    lv_obj_remove_style_all(s_system_rows);
    lv_obj_set_size(s_system_rows, 620, 276);
    lv_obj_set_pos(s_system_rows, 20, 60);
    lv_obj_set_flex_flow(s_system_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_system_rows, 8, 0);
    lv_obj_set_scrollable(s_system_rows, false);
    small_button(s_system, 20, 338, 150, 46, "Refresh", on_system_button, 1);
    lv_obj_set_hidden(s_system, true);
}

void ui_set_layout(int count, const uint8_t order[TILES], int switch1)
{
    lvgl_port_lock(0);
    s_switch1 = switch1;
    s_layout_count = count;
    memcpy(s_layout_order, order, sizeof(s_layout_order));
    for (int i = 0; i < TILES; i++) place_tile(i);
    fit_names();
    lvgl_port_unlock();
}

void ui_set_layout_fx(const uint8_t order[TILES])
{
    lvgl_port_lock(0);
    memcpy(s_layout_fx_order, order, sizeof(s_layout_fx_order));
    for (int i = 0; i < TILES; i++) place_tile(i);
    lvgl_port_unlock();
}

static void build_midi(lv_obj_t *screen)
{
    s_midi = panel(screen, 110, 60, 580, 400);
    dialog_title(s_midi, MIDI_TITLE);
    s_midi_status = label(s_midi, HINT_FONT, MUTED);
    lv_obj_align(s_midi_status, LV_ALIGN_TOP_RIGHT, 0, 12);   // (P4: on the settings page)
    s_midi_close = close_button(s_midi, 520, 12, on_midi_button, 0);
    lv_obj_t *info = label(s_midi, HINT_FONT, MUTED);
    lv_obj_set_width(info, 540);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, MIDI_INFO);
    lv_obj_set_pos(info, 0, 40);
    lv_obj_t *map = label(s_midi, &ui_font_14, MUTED);   // (a line to look up, in one row)
    lv_obj_set_width(map, 540);
    lv_label_set_long_mode(map, LV_LABEL_LONG_WRAP);
    lv_label_set_text(map, "PC 0-63 = presets   CC 37-41 = FX 1-5   CC 1 = reverb mix   CC 50-57 = fields 1-8");
    lv_obj_set_pos(map, 0, 340);

    s_midi_list = lv_obj_create(s_midi);
    lv_obj_set_pos(s_midi_list, 0, 80);
    lv_obj_set_size(s_midi_list, SET_W, 250);
    lv_obj_set_style_bg_opa(s_midi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_midi_list, 0, 0);
    lv_obj_set_style_pad_all(s_midi_list, 6, 0);
    lv_obj_set_style_pad_row(s_midi_list, 6, 0);
    lv_obj_set_flex_flow(s_midi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_midi_list, LV_DIR_VER);

    small_button(s_midi, 0, 376, 220, 52, "Forget device", on_midi_button, -1);
    s_midi_more[0] = small_button(s_midi, 210, 336, 180, 46, "Tile layout", open_layout, 0);
    s_midi_more[1] = small_button(s_midi, 400, 336, 160, 46, "System", on_system_button, 1);
    lv_obj_set_hidden(s_midi, true);
}

// ---- settings screen (1.17.0): one place for what was spread over the top bar's dialogs ----
// Topics on the left with their state, the chosen one on the right. MIDI, USB audio and System are the dialogs of
// before, placed into the page (their close buttons hidden); tile layout and the footswitch learning open their
// dialogs from a page. Back to playing with the arrow at the top left.

static bool s_link_usb = true;   // P4: the link goes over USB (else Bluetooth)

static void settings_nano_text(void)
{
    char text[160];
#ifdef NANO_P4
    const char *connected = s_link_usb ? "Connected over USB" : "Connected over Bluetooth";
#else
    const char *connected = "Connected";
#endif
    if (s_nano_connected && s_nano_fw_major >= 0) {
        snprintf(text, sizeof(text), "%s\nNanOS %d.%d", connected, s_nano_fw_major, s_nano_fw_minor);
    } else {
        snprintf(text, sizeof(text), "%s", s_nano_connected ? connected : "Searching for the Nano ...");
    }
    lv_label_set_text(s_set_nano_text, text);
    lv_label_set_text(s_set_badge[SET_NANO], s_nano_connected ? "ON" : "");
}

void ui_set_nano_firmware(int major, int minor)
{
    lvgl_port_lock(0);
    s_nano_fw_major = major;
    s_nano_fw_minor = minor;
    if (s_set_nano_text) settings_nano_text();
    lvgl_port_unlock();
}

// ask: start what the page needs from the controller (MIDI scan, USB volume, system values)
static void settings_select(int topic, bool ask)
{
    if (s_set_topic == SET_MIDI && topic != SET_MIDI && !lv_obj_is_hidden(s_settings)) send('X', 0);   // MIDI scan off
    s_set_topic = topic;
    for (int i = 0; i < SET_COUNT; i++) {
        bool on = i == topic;
        lv_obj_set_style_bg_color(s_set_item[i], lv_color_hex(on ? SURF_ON : SURF), 0);
        lv_obj_set_style_text_color(s_set_item_label[i], lv_color_hex(on ? TEXT : 0xC4C8CD), 0);
        set_color_prop(s_set_item_icon[i], LV_STYLE_IMAGE_RECOLOR, on ? TEXT : CAPTION);
        set_color_prop(s_set_item_icon[i], LV_STYLE_TEXT_COLOR, on ? TEXT : CAPTION);
        lv_obj_set_hidden(s_set_page[i], !on);
    }
    switch (topic) {
    case SET_NANO: settings_nano_text(); break;
    case SET_WIFI: show_wifi_page(); break;
    case SET_MIDI:
        fill_midi();
        if (ask) send('X', 1);
        break;
    case SET_USB:
        if (ask) {   // the value can also be changed in the app: read it each time
            set_usb_enabled(false);
            lv_label_set_text(s_usb_value, "");
            lv_label_set_text(s_usb_info, "Reading the Nano's settings...");
            send('V', 0);
        }
        break;
    case SET_LAYOUT:
        lv_label_set_text_fmt(s_set_layout_text, "%d fields on the screen\n%s", s_layout_count,
                              SWITCH1_NAMES[s_switch1 >= 0 && s_switch1 < UI_SWITCH1_COUNT ? s_switch1 : 0]);
        break;
    case SET_SYSTEM:
        if (ask) send(')', 0);
        break;
    default: break;
    }
}

static void on_settings_item(lv_event_t *e) { settings_select((int)(intptr_t)lv_event_get_user_data(e), true); }

static void open_settings(lv_event_t *e)
{
    lv_obj_set_hidden(s_settings, false);
    lv_obj_move_foreground(s_settings);
    settings_select(s_set_topic, true);
}

static void close_settings(lv_event_t *e)
{
    if (s_set_topic == SET_MIDI) send('X', 0);
    lv_obj_set_hidden(s_settings, true);
}

static void on_settings_layout(lv_event_t *e) { open_layout(NULL); lv_obj_move_foreground(s_layout); }
static void on_settings_learn(lv_event_t *e)
{
    show_learn_all(1);
    send('D', 1 | UI_LEARN_ALL);
}
static void on_settings_reload(lv_event_t *e) { send('r', 0); }
static void on_settings_cloud(lv_event_t *e) { send('\\', lv_obj_has_state(s_set_cloud, LV_STATE_CHECKED) ? 1 : 0); }

void ui_set_cloud(bool on)
{
    lvgl_port_lock(0);
    if (s_set_cloud) {
        if (on) lv_obj_add_state(s_set_cloud, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_set_cloud, LV_STATE_CHECKED);
    }
    lvgl_port_unlock();
}

// A dialog of before becomes a page: no frame, no close button
static void settings_embed(int topic, lv_obj_t *dialog, lv_obj_t *close)
{
    lv_obj_set_parent(dialog, s_set_content);
    lv_obj_set_pos(dialog, 0, 0);
    lv_obj_set_size(dialog, SET_W, SCREEN_H);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dialog, 0, 0);
    lv_obj_set_style_shadow_width(dialog, 0, 0);
    lv_obj_set_style_radius(dialog, 0, 0);
    if (close) lv_obj_set_hidden(close, true);
    s_set_page[topic] = dialog;
}

// A page of its own: title, text, buttons
static lv_obj_t *settings_page(int topic, const char *title, const char *hint)
{
    lv_obj_t *p = lv_obj_create(s_set_content);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SET_W, SCREEN_H);
    lv_obj_set_scrollable(p, false);
    lv_obj_t *t = label(p, &ui_font_title_26, TEXT);   // title and line in the top band, beside "Settings"
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 0, 4);
    lv_obj_t *h = label(p, HINT_FONT, CAPTION);
    lv_obj_set_width(h, SET_W);
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    lv_label_set_text(h, hint);
    lv_obj_set_pos(h, 0, 40);
    s_set_page[topic] = p;
    return p;
}

// ---- Neural Capture V1 assistant (Settings > Capture) ----
// Steps as approved in drafts/entwurf-capture-v1-schritte.png: Connect, Levels, Capture, Compare & save, Done. The
// texts follow Neural DSP's guide to capturing on the Nano. The controller side (main.c) moves the steps on what the
// Nano reports; Connect <-> Levels is the assistant's own.
#define CAP_W (SCREEN_W - 2 * MARGIN)   // P4: the sheet over the whole screen
#define CAP_H (SCREEN_H - 2 * MARGIN)
#define CAP_X 24                  // the left edge inside it
#define CAP_R (CAP_W - CAP_X)     // the right edge
#define CAP_IW (CAP_R - CAP_X)
#define CAP_PAGE_Y 132            // the pages under the title and the steps
#define CAP_BTN_Y (CAP_H - CAP_PAGE_Y - 24 - 64)   // the bottom row of buttons (64 px high), in a page
#define CAP_METER_W (CAP_R - 144 - 16 - 200 - 4)  // the level bars' inner width
#define CAP_ORANGE 0xFFB02E
#define CAP_BLUE 0x4AA3FF
#define CAP_RED 0xFF5A4E
enum { CAP_TYPE_AMP_CAB, CAP_TYPE_HEAD, CAP_TYPE_PEDAL, CAP_TYPE_COUNT };
enum { CAP_SAVE_AMP, CAP_SAVE_AMP_CAB, CAP_SAVE_CAB, CAP_SAVE_PEDAL, CAP_SAVE_OTHER, CAP_SAVE_COUNT };
static const char *const CAP_SAVE_NAMES[CAP_SAVE_COUNT] = { "Amp", "Amp + cab", "Cab", "Pedal", "Other" };
static const char *const CAP_SAVE_TYPES[CAP_SAVE_COUNT] = { "amp_head", "amp_and_cab", "cab", "pedal", "default" };
static const char *const CAP_STEP_NAMES[4] = { "Connect", "Levels", "Capture", "Compare & save" };
static const char *const CAP_CONNECT[CAP_TYPE_COUNT] = {
    "1   Guitar into INPUT 1.\n"
    "2   OUTPUT 2R (CAPTURE OUT) to the amp's input.\n"
    "3   A mic in front of the speaker into CAPTURE INPUT: about 1 inch (2-3 cm) from the grill, at the centre of "
    "the cone, straight on. Dynamic mics plug in directly, condenser mics need a preamp.\n"
    "4   Headphones into HP OUTPUT, or a monitor on OUTPUT 1L.",
    "1   Guitar into INPUT 1.\n"
    "2   OUTPUT 2R (CAPTURE OUT) to the amp's input.\n"
    "3   Reactive load box: the amp's speaker output into the load box, its line output into CAPTURE INPUT. "
    "Or the amp's D.I. out into CAPTURE INPUT - with a cabinet still connected to the amp.\n"
    "4   Headphones into HP OUTPUT, or a monitor on OUTPUT 1L.",
    "1   Guitar into INPUT 1.\n"
    "2   OUTPUT 2R (CAPTURE OUT) to the pedal's input.\n"
    "3   The pedal's output into CAPTURE INPUT.\n"
    "4   Headphones into HP OUTPUT, or a monitor on OUTPUT 1L.",
};

static lv_obj_t *s_cap_panel, *s_cap_page[UI_CAP_DONE + 1], *s_cap_step_box[4], *s_cap_step_label[4], *s_cap_step_check[4];
static lv_obj_t *s_cap_type_btn[CAP_TYPE_COUNT], *s_cap_connect_text;
static lv_obj_t *s_cap_value[2], *s_cap_fill[2], *s_cap_peak[2], *s_cap_cab_btn[2][NANO_CAB_SLOTS + 1];
static lv_obj_t *s_cap_msg[UI_CAP_DONE + 1];
static lv_obj_t *s_cap_progress_title, *s_cap_progress_bar, *s_cap_progress_left, *s_cap_stop;
static lv_obj_t *s_cap_listen_btn[2], *s_cap_auto_btn[4], *s_cap_name, *s_cap_save_btn[CAP_SAVE_COUNT];
static lv_obj_t *s_cap_lib_button, *s_cap_slot_button, *s_cap_discard;
static lv_obj_t *s_cap_slots, *s_cap_slot_btn[NANO_CAPTURE_SLOTS], *s_cap_slot_ok;
static lv_obj_t *s_cap_done_title, *s_cap_done_text;
static lv_obj_t *s_set_cap_count;
static lv_timer_t *s_cap_auto_timer, *s_cap_stop_timer;
static struct {
    int step, type, save_type, cab, auto_index, slot, user_captures;
    bool source, busy, stop_armed;
    int peak[2];
    uint32_t peak_tick[2];
    char name[33];
} s_capui = { .user_captures = -1 };

static void cap_command(int action, int value) { send('0', action << 16 | (value & 0xFFFF)); }   // (CAPC_* in main.c)

static void cap_choose(lv_obj_t *btn, bool on)   // filled = chosen (a segment of a row)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(on ? 0xECECEC : BUTTON_BG), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(on ? 0xECECEC : BUTTON_LINE), 0);
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? INK : 0xC8CCD1), 0);
}

static void cap_go(lv_obj_t *btn)   // the green main action (Start capture, Save to slot)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(GREEN), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x7BFF92), LV_STATE_PRESSED);
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(INK), 0);
}

static void cap_outline(lv_obj_t *btn)   // Cancel / Back: only the frame
{
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
}

static lv_obj_t *cap_text(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = label(parent, font, color);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *cap_dot(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 12, 12);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_style_radius(d, 6, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

static lv_obj_t *cap_page(void)
{
    lv_obj_t *p = lv_obj_create(s_cap_panel);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, CAP_PAGE_Y);
    lv_obj_set_size(p, CAP_W, CAP_H - CAP_PAGE_Y);
    lv_obj_set_scrollable(p, false);
    return p;
}

static lv_obj_t *cap_message_label(lv_obj_t *page, int x, int y, int w)
{
    lv_obj_t *m = cap_text(page, x, y, w, &ui_font_14, CAP_ORANGE, "");
    return m;
}

static void cap_show_step(void)
{
    int step = s_capui.step;
    for (int i = 0; i <= UI_CAP_DONE; i++) if (s_cap_page[i]) lv_obj_set_hidden(s_cap_page[i], i != step);
    for (int i = 0; i < 4; i++) {
        int number = i + 1;   // UI_CAP_CONNECT .. UI_CAP_COMPARE
        bool on = number == step, done = number < step;
        lv_obj_set_style_bg_color(s_cap_step_box[i], lv_color_hex(on ? 0xECECEC : SURF), 0);
        lv_obj_set_style_border_color(s_cap_step_box[i], lv_color_hex(on ? 0xECECEC : BUTTON_LINE), 0);
        lv_obj_set_style_text_color(s_cap_step_label[i], lv_color_hex(on ? INK : done ? TEXT : MUTED), 0);
        if (done) lv_label_set_text(s_cap_step_label[i], CAP_STEP_NAMES[i]);
        else lv_label_set_text_fmt(s_cap_step_label[i], "%d  %s", number, CAP_STEP_NAMES[i]);
        lv_obj_set_hidden(s_cap_step_check[i], !done);
        lv_obj_align(s_cap_step_label[i], LV_ALIGN_CENTER, done ? 10 : 0, 0);
        lv_obj_align_to(s_cap_step_check[i], s_cap_step_label[i], LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
    lv_obj_set_hidden(s_cap_slots, true);
    if (step != UI_CAP_COMPARE && s_cap_auto_timer) lv_timer_pause(s_cap_auto_timer);
}

static void cap_type_show(void)
{
    for (int i = 0; i < CAP_TYPE_COUNT; i++) cap_choose(s_cap_type_btn[i], i == s_capui.type);
    lv_label_set_text(s_cap_connect_text, CAP_CONNECT[s_capui.type]);
}

static void cap_save_type_show(void)
{
    for (int i = 0; i < CAP_SAVE_COUNT; i++) cap_choose(s_cap_save_btn[i], i == s_capui.save_type);
}

static void cap_values_show(int gain, int level)
{
    char text[24];
    snprintf(text, sizeof(text), "%.1f dB", (double)nano_capture_db(0, gain));
    lv_label_set_text(s_cap_value[0], text);
    float out = nano_capture_db(1, level);
    snprintf(text, sizeof(text), "%s%.1f dB", out < 0 ? "\xE2\x88\x92" : "", (double)fabsf(out));
    lv_label_set_text(s_cap_value[1], text);
}

static void cap_cab_show(void)
{
    for (int r = 0; r < 2; r++)
        for (int i = 0; i <= NANO_CAB_SLOTS; i++) cap_choose(s_cap_cab_btn[r][i], i == s_capui.cab);
}

static void cap_listen_show(void)
{
    cap_choose(s_cap_listen_btn[0], !s_capui.source);
    cap_choose(s_cap_listen_btn[1], s_capui.source);
    for (int i = 0; i < 4; i++) cap_choose(s_cap_auto_btn[i], i == s_capui.auto_index);
}

static void cap_name_show(void)
{
    lv_obj_t *l = lv_obj_get_child(s_cap_name, 0);
    lv_label_set_text(l, s_capui.name[0] ? s_capui.name : "Tap to name it (at least 4 characters)");
    lv_obj_set_style_text_color(l, lv_color_hex(s_capui.name[0] ? TEXT : MUTED), 0);
    bool ok = strlen(s_capui.name) >= 4 && !s_capui.busy;
    if (ok) {
        lv_obj_remove_state(s_cap_lib_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_cap_slot_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_cap_lib_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_cap_slot_button, LV_STATE_DISABLED);
    }
}

static void cap_close_assistant(void)
{
    lv_obj_set_hidden(s_cap, true);
    if (s_cap_auto_timer) lv_timer_pause(s_cap_auto_timer);
}

// ---- events ----

static void on_cap_close(lv_event_t *e)
{
    cap_close_assistant();
    cap_command(2, 0);   // CAPC_EXIT: out of capture mode (the controller goes on until the Nano is back in play mode)
}

static void on_cap_type(lv_event_t *e)
{
    s_capui.type = (int)(intptr_t)lv_event_get_user_data(e);
    s_capui.save_type = s_capui.type == CAP_TYPE_HEAD ? CAP_SAVE_AMP : s_capui.type == CAP_TYPE_PEDAL ? CAP_SAVE_PEDAL : CAP_SAVE_AMP_CAB;
    cap_type_show();
    cap_save_type_show();
}

static void on_cap_next(lv_event_t *e)
{
    s_capui.step = (int)(intptr_t)lv_event_get_user_data(e);
    cap_command(12, s_capui.step);   // CAPC_STEP
    cap_show_step();
}

static int s_cap_raw[2];

static void on_cap_step_value(lv_event_t *e)   // - / + (held: repeats)
{
    int data = (int)(intptr_t)lv_event_get_user_data(e), which = data >> 1, dir = data & 1 ? 1 : -1;
    int v = s_cap_raw[which] + dir * 3;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (v == s_cap_raw[which]) return;
    s_cap_raw[which] = v;
    cap_values_show(s_cap_raw[0], s_cap_raw[1]);
    cap_command(which ? 4 : 3, v);   // CAPC_LEVEL / CAPC_GAIN
}

static void on_cap_cab(lv_event_t *e)
{
    s_capui.cab = (int)(intptr_t)lv_event_get_user_data(e);
    cap_cab_show();
    cap_command(5, s_capui.cab);   // CAPC_CAB
}

static void on_cap_start(lv_event_t *e) { cap_command(6, 0); }   // CAPC_TRAIN

static void cap_stop_disarm(lv_timer_t *t)
{
    s_capui.stop_armed = false;
    lv_label_set_text(lv_obj_get_child(s_cap_stop, 0), "Stop capture");
    lv_timer_pause(t);
}

static void on_cap_stop(lv_event_t *e)   // twice: the capture so far is lost
{
    if (!s_capui.stop_armed) {
        s_capui.stop_armed = true;
        lv_label_set_text(lv_obj_get_child(s_cap_stop, 0), "Tap again to stop");
        lv_timer_reset(s_cap_stop_timer);
        lv_timer_resume(s_cap_stop_timer);
        return;
    }
    cap_stop_disarm(s_cap_stop_timer);
    cap_command(7, 0);   // CAPC_STOP
}

static void cap_listen(bool source)
{
    s_capui.source = source;
    cap_listen_show();
    cap_command(8, source);   // CAPC_LISTEN
}

static void on_cap_listen(lv_event_t *e)
{
    s_capui.auto_index = 0;
    if (s_cap_auto_timer) lv_timer_pause(s_cap_auto_timer);
    cap_listen((int)(intptr_t)lv_event_get_user_data(e) != 0);
}

static void cap_auto_cb(lv_timer_t *t) { cap_listen(!s_capui.source); }

static void on_cap_auto(lv_event_t *e)   // switch between capture and original every 2 / 4 / 8 s
{
    static const int SECONDS[4] = { 0, 2, 4, 8 };
    s_capui.auto_index = (int)(intptr_t)lv_event_get_user_data(e);
    cap_listen_show();
    if (!s_capui.auto_index) {
        lv_timer_pause(s_cap_auto_timer);
        return;
    }
    lv_timer_set_period(s_cap_auto_timer, (uint32_t)SECONDS[s_capui.auto_index] * 1000);
    lv_timer_reset(s_cap_auto_timer);
    lv_timer_resume(s_cap_auto_timer);
}

static void on_cap_name(lv_event_t *e) { show_rename('c', "Name of the new capture", "OK", s_capui.name, 32); }

static void cap_named(const char *text)   // from the keyboard
{
    snprintf(s_capui.name, sizeof(s_capui.name), "%.32s", text);
    cap_name_show();
}

static void on_cap_save_type(lv_event_t *e)
{
    s_capui.save_type = (int)(intptr_t)lv_event_get_user_data(e);
    cap_save_type_show();
}

static void cap_save(int slot)
{
    char text[64];
    snprintf(text, sizeof(text), "%d|%s|%s", slot, CAP_SAVE_TYPES[s_capui.save_type], s_capui.name);
    if (s_on_text) s_on_text('c', text);
}

static void on_cap_save_library(lv_event_t *e) { cap_save(0); }

static void cap_slots_show(void)
{
    for (int i = 0; i < NANO_CAPTURE_SLOTS; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%d-%d  %.40s", i / 5 + 1, i % 5 + 1, s_capture_names[i][0] ? s_capture_names[i] : "-");
        lv_label_set_text(lv_obj_get_child(s_cap_slot_btn[i], 0), text);
        cap_choose(s_cap_slot_btn[i], i + 1 == s_capui.slot);
    }
    if (s_capui.slot) {
        lv_label_set_text_fmt(lv_obj_get_child(s_cap_slot_ok, 0), "Save into %d-%d", (s_capui.slot - 1) / 5 + 1, (s_capui.slot - 1) % 5 + 1);
        lv_obj_remove_state(s_cap_slot_ok, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(lv_obj_get_child(s_cap_slot_ok, 0), "Choose a slot");
        lv_obj_add_state(s_cap_slot_ok, LV_STATE_DISABLED);
    }
}

static void on_cap_save_slot(lv_event_t *e)   // first the slot (the capture in it stays in the library)
{
    s_capui.slot = 0;
    cap_slots_show();
    lv_obj_set_hidden(s_cap_slots, false);
    lv_obj_move_foreground(s_cap_slots);
}

static void on_cap_slot(lv_event_t *e)
{
    s_capui.slot = (int)(intptr_t)lv_event_get_user_data(e);
    cap_slots_show();
}

static void on_cap_slot_button(lv_event_t *e)
{
    lv_obj_set_hidden(s_cap_slots, true);
    if ((int)(intptr_t)lv_event_get_user_data(e) && s_capui.slot) cap_save(s_capui.slot);
}

static void on_cap_discard(lv_event_t *e) { cap_command(11, 0); }   // CAPC_DISCARD: back to the levels
static void on_cap_again(lv_event_t *e) { cap_command(9, 0); }      // CAPC_AGAIN

static void on_cap_done(lv_event_t *e)
{
    cap_command(2, 0);   // CAPC_EXIT
    cap_close_assistant();
}

static void on_settings_capture(lv_event_t *e)
{
    s_capui.name[0] = 0;
    s_capui.step = UI_CAP_CONNECT;
    s_capui.cab = 0;
    s_capui.source = false;
    s_capui.auto_index = 0;
    cap_type_show();
    cap_save_type_show();
    cap_cab_show();
    cap_listen_show();
    cap_name_show();
    cap_show_step();
    for (int i = 0; i <= UI_CAP_DONE; i++) if (s_cap_msg[i]) lv_label_set_text(s_cap_msg[i], "");
    lv_obj_set_hidden(s_cap, false);
    lv_obj_move_foreground(s_cap);
    cap_command(1, 0);   // CAPC_OPEN: into capture mode
}

// ---- from the controller ----

void ui_capture_show(const ui_capture_t *c)
{
    lvgl_port_lock(0);
    s_capui.user_captures = c->user_captures;
    if (s_set_cap_count) {
        if (c->user_captures >= 0) lv_label_set_text_fmt(s_set_cap_count, "The new capture goes to My Neural Captures - %d of 256 used.", c->user_captures);
        else lv_label_set_text(s_set_cap_count, "The new capture goes to My Neural Captures (up to 256).");
    }
    if (c->step == UI_CAP_CLOSED) {
        s_capui.step = UI_CAP_CLOSED;
        cap_close_assistant();
        lvgl_port_unlock();
        return;
    }
    bool opened = lv_obj_is_hidden(s_cap);
    if (opened) {   // the Nano went into capture mode by itself
        s_capui.name[0] = 0;
        cap_type_show();
        cap_save_type_show();
    }
    s_capui.step = c->step;
    s_capui.busy = c->busy;
    s_capui.cab = c->cab;
    if (s_capui.auto_index == 0) s_capui.source = c->source;
    s_cap_raw[0] = c->gain;
    s_cap_raw[1] = c->level;
    cap_values_show(c->gain, c->level);
    cap_cab_show();
    cap_listen_show();
    cap_name_show();
    lv_label_set_text_fmt(s_cap_progress_title, "Capturing \xC2\xB7 %d %%", c->progress);
    lv_bar_set_value(s_cap_progress_bar, c->progress, LV_ANIM_ON);
    int left = (100 - c->progress) * 23 / 10;   // (on the Nano: about 3.5 minutes from the start to done)
    if (c->progress <= 0) lv_label_set_text(s_cap_progress_left, "Starting ...");
    else if (left >= 90) lv_label_set_text_fmt(s_cap_progress_left, "About %d minutes left.", (left + 30) / 60);
    else lv_label_set_text(s_cap_progress_left, "Almost done.");
    if (c->saved && c->saved[0]) {
        const char *nl = strchr(c->saved, '\n');
        lv_label_set_text_fmt(s_cap_done_title, "%.*s", nl ? (int)(nl - c->saved) : (int)strlen(c->saved), c->saved);
        lv_label_set_text(s_cap_done_text, nl ? nl + 1 : "");
    }
    for (int i = 0; i <= UI_CAP_DONE; i++) {
        if (!s_cap_msg[i]) continue;
        if (i == c->step && c->message) lv_label_set_text(s_cap_msg[i], c->message);
        else if (i != c->step) lv_label_set_text(s_cap_msg[i], "");
    }
    if (c->step == UI_CAP_COMPARE && c->busy) lv_label_set_text(s_cap_msg[UI_CAP_COMPARE], "Saving ...");
    cap_show_step();
    if (opened || !lv_obj_is_hidden(s_cap)) {
        lv_obj_set_hidden(s_cap, false);
        lv_obj_move_foreground(s_cap);
        if (!lv_obj_is_hidden(s_rename)) lv_obj_move_foreground(s_rename);
    }
    lvgl_port_unlock();
}

// The meters: the Nano's raw value, shown on 0-255 (its scale is still to be confirmed on the Nano) with a hold.
void ui_capture_meter(int which, int value)
{
    if (which < 0 || which > 1) return;
    lvgl_port_lock(0);
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    int w = CAP_METER_W * value / 255;
    lv_obj_set_width(s_cap_fill[which], w > 0 ? w : 1);
    uint32_t now = lv_tick_get();
    if (value >= s_capui.peak[which] || now - s_capui.peak_tick[which] > 1500) {
        s_capui.peak[which] = value;
        s_capui.peak_tick[which] = now;
        lv_obj_set_x(s_cap_peak[which], CAP_METER_W * value / 255);
    }
    uint32_t color = value > 240 ? CAP_RED : GREEN;
    lv_obj_set_style_bg_color(s_cap_fill[which], lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_cap_peak[which], lv_color_hex(color), 0);
    lvgl_port_unlock();
}

// ---- building ----

static lv_obj_t *cap_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, lv_event_cb_t cb, int data)
{
    lv_obj_t *b = small_button(parent, x, y, w, h, text, cb, data);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &ui_font_20, 0);   // P4: the larger sheet, larger words
    return b;
}

static void cap_meter_row(lv_obj_t *page, int which, int y, const char *caption)
{
    cap_text(page, CAP_X, y, 0, &ui_font_14, MUTED, caption);
    s_cap_value[which] = cap_text(page, CAP_X, y + 20, 0, &ui_font_28, TEXT, "");
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 200, y + 4);
    lv_obj_set_size(bar, CAP_METER_W + 4, 56);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B0C0D), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_style_radius(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_t *zone = lv_obj_create(bar);   // where it should be (the Nano's green)
    lv_obj_remove_style_all(zone);
    lv_obj_set_pos(zone, CAP_METER_W * 230 / 420, 0);
    lv_obj_set_size(zone, CAP_METER_W * 140 / 420, 54);
    lv_obj_set_style_bg_color(zone, lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_10, 0);
    lv_obj_set_style_border_width(zone, 1, 0);
    lv_obj_set_style_border_side(zone, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(zone, lv_color_hex(0x5B6168), 0);
    s_cap_fill[which] = lv_obj_create(bar);
    lv_obj_remove_style_all(s_cap_fill[which]);
    lv_obj_set_pos(s_cap_fill[which], 1, 6);
    lv_obj_set_size(s_cap_fill[which], 1, 42);
    lv_obj_set_style_radius(s_cap_fill[which], 4, 0);
    lv_obj_set_style_bg_color(s_cap_fill[which], lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_opa(s_cap_fill[which], LV_OPA_COVER, 0);
    s_cap_peak[which] = lv_obj_create(bar);
    lv_obj_remove_style_all(s_cap_peak[which]);
    lv_obj_set_pos(s_cap_peak[which], 0, 3);
    lv_obj_set_size(s_cap_peak[which], 4, 48);
    lv_obj_set_style_bg_color(s_cap_peak[which], lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_opa(s_cap_peak[which], LV_OPA_COVER, 0);
    for (int d = 0; d < 2; d++) {
        lv_obj_t *b = cap_button(page, CAP_R - 144 + d * 80, y, 64, 64, d ? "+" : "\xE2\x88\x92", on_cap_step_value, which << 1 | d);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &ui_font_28, 0);
        lv_obj_add_event_cb(b, on_cap_step_value, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(which << 1 | d));
    }
}

static void cap_cab_row(lv_obj_t *page, int row, int x, int y)   // Off + 1-5: 372 px wide
{
    for (int i = 0; i <= NANO_CAB_SLOTS; i++) {
        char text[4];
        snprintf(text, sizeof(text), "%d", i);
        s_cap_cab_btn[row][i] = cap_button(page, x + (i ? 80 + (i - 1) * 60 : 0), y, i ? 52 : 72, 56, i ? text : "Off", on_cap_cab, i);
    }
}

static void build_capture(lv_obj_t *screen)
{
    s_cap = lv_obj_create(screen);   // dark over the whole screen, the sheet on it
    lv_obj_remove_style_all(s_cap);
    lv_obj_set_size(s_cap, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_cap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cap, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_cap, false);
    lv_obj_set_clickable(s_cap, true);   // (taps do not reach the screen below)
    s_cap_panel = card(s_cap, MARGIN, MARGIN, CAP_W, CAP_H);   // P4: the sheet over the whole screen (capture mode is a mode of its own)
    lv_obj_set_style_bg_color(s_cap_panel, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_border_color(s_cap_panel, lv_color_hex(FILL_LINE), 0);
    lv_obj_set_style_radius(s_cap_panel, RADIUS, 0);
    lv_obj_t *title = dialog_title(s_cap_panel, "New Neural Capture");
    lv_obj_set_style_text_font(title, &ui_font_title_26, 0);
    lv_obj_set_pos(title, CAP_X, 18);
    close_button(s_cap_panel, CAP_W - 64, 12, on_cap_close, 0);
    for (int i = 0; i < 4; i++) {   // the steps
        lv_obj_t *box = lv_obj_create(s_cap_panel);
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, CAP_X + i * 240, 72);
        lv_obj_set_size(box, 232, 44);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        s_cap_step_box[i] = box;
        s_cap_step_label[i] = label(box, &ui_font_16, MUTED);
        s_cap_step_check[i] = label(box, &lv_font_montserrat_20, GREEN);
        lv_label_set_text(s_cap_step_check[i], LV_SYMBOL_OK);
    }

    // 1 Connect
    lv_obj_t *p = s_cap_page[UI_CAP_CONNECT] = cap_page();
    static const char *const TYPES[CAP_TYPE_COUNT] = { "Amp + cab (mic)", "Amp head", "Pedal" };
    static const int TYPE_W[CAP_TYPE_COUNT] = { 230, 170, 130 };
    for (int i = 0, x = CAP_X; i < CAP_TYPE_COUNT; x += TYPE_W[i] + 8, i++) s_cap_type_btn[i] = cap_button(p, x, 0, TYPE_W[i], 56, TYPES[i], on_cap_type, i);
    s_cap_connect_text = cap_text(p, CAP_X, 78, CAP_IW, &ui_font_20, TEXT, "");
    lv_obj_set_style_text_line_space(s_cap_connect_text, 8, 0);
    lv_obj_t *warn = card(p, CAP_X, CAP_BTN_Y, 530, 64);
    lv_obj_set_style_bg_color(warn, lv_color_hex(0x1D1609), 0);
    lv_obj_set_style_border_color(warn, lv_color_hex(0x6B4A12), 0);
    cap_text(warn, 12, 10, 506, &ui_font_16, 0xFFD38A,
             "Tube amps: never connect the speaker output to the Nano without a reactive load box - it can damage both.");
    cap_outline(cap_button(p, CAP_R - 382, CAP_BTN_Y, 170, 64, "Cancel", on_cap_close, 0));
    primary(cap_button(p, CAP_R - 200, CAP_BTN_Y, 200, 64, "Next", on_cap_next, UI_CAP_LEVELS));
    s_cap_msg[UI_CAP_CONNECT] = cap_message_label(p, CAP_X, CAP_BTN_Y - 36, CAP_IW);

    // 2 Levels
    p = s_cap_page[UI_CAP_LEVELS] = cap_page();
    cap_meter_row(p, 0, 0, "CAPTURE IN");
    cap_meter_row(p, 1, 84, "OUTPUT LEVEL");
    cap_dot(p, CAP_X, 178, GREEN);
    cap_text(p, CAP_X + 24, 172, 530, &ui_font_16, TEXT, "Strum hard. Green = good.");
    cap_dot(p, CAP_X, 208, CAP_BLUE);
    cap_text(p, CAP_X + 24, 202, 530, &ui_font_16, TEXT, "Too low: raise CAPTURE IN. Still low at +24 dB? Set it to 0 and switch BOOST on at the Nano (+26 dB).");
    cap_dot(p, CAP_X, 262, CAP_RED);
    cap_text(p, CAP_X + 24, 256, 530, &ui_font_16, TEXT, "Clipping: BOOST off, or turn the amp's / pedal's output down.");
    cap_text(p, CAP_R - 372, 172, 372, &ui_font_16, MUTED, "Output level is only what you hear (headphones, OUTPUT 1L).");
    cap_text(p, CAP_R - 372, 222, 0, &ui_font_16, MUTED, "Listen through cab");
    cap_cab_row(p, 0, CAP_R - 372, 250);
    s_cap_msg[UI_CAP_LEVELS] = cap_message_label(p, CAP_X, CAP_BTN_Y - 36, CAP_IW);
    cap_outline(cap_button(p, CAP_R - 382, CAP_BTN_Y, 170, 64, "Back", on_cap_next, UI_CAP_CONNECT));
    cap_go(cap_button(p, CAP_R - 200, CAP_BTN_Y, 200, 64, "Start capture", on_cap_start, 0));

    // 3 Capturing
    p = s_cap_page[UI_CAP_TRAINING] = cap_page();
    s_cap_progress_title = cap_text(p, CAP_X, 0, 0, &ui_font_title_34, TEXT, "Capturing");
    s_cap_progress_bar = lv_bar_create(p);
    lv_obj_set_pos(s_cap_progress_bar, CAP_X, 60);
    lv_obj_set_size(s_cap_progress_bar, CAP_IW, 24);
    lv_bar_set_range(s_cap_progress_bar, 0, 100);
    lv_obj_set_style_bg_color(s_cap_progress_bar, lv_color_hex(0x0B0C0D), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cap_progress_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_cap_progress_bar, lv_color_hex(CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cap_progress_bar, lv_color_hex(GREEN), LV_PART_INDICATOR);
    s_cap_progress_left = cap_text(p, CAP_X, 96, 0, &ui_font_16, MUTED, "");
    lv_obj_t *tips = card(p, CAP_X, 136, CAP_IW, 150);
    static const char *const TIPS[3] = { "The Nano plays test tones through your amp - keep the volume where it is.",
                                         "Don't play, don't touch the amp, the mic or the cables until it is done.",
                                         "Keep the room quiet - the mic hears everything." };
    for (int i = 0; i < 3; i++) {
        cap_dot(tips, 18, 26 + i * 44, CAP_ORANGE);
        cap_text(tips, 44, 18 + i * 44, CAP_IW - 64, &ui_font_20, TEXT, TIPS[i]);
    }
    s_cap_msg[UI_CAP_TRAINING] = cap_message_label(p, CAP_X, CAP_BTN_Y - 36, CAP_IW);
    s_cap_stop = cap_button(p, CAP_R - 240, CAP_BTN_Y, 240, 64, "Stop capture", on_cap_stop, 0);
    cap_outline(s_cap_stop);
    s_cap_stop_timer = lv_timer_create(cap_stop_disarm, 3000, NULL);
    lv_timer_pause(s_cap_stop_timer);

    // 4 Compare & save
    p = s_cap_page[UI_CAP_COMPARE] = cap_page();
    cap_text(p, CAP_X, 16, 0, &ui_font_16, MUTED, "Listen");
    s_cap_listen_btn[0] = cap_button(p, 120, 0, 140, 56, "Capture", on_cap_listen, 0);
    s_cap_listen_btn[1] = cap_button(p, 268, 0, 140, 56, "Original", on_cap_listen, 1);
    cap_text(p, 450, 16, 0, &ui_font_16, MUTED, "Auto");
    static const char *const AUTO[4] = { "Off", "2 s", "4 s", "8 s" };
    for (int i = 0; i < 4; i++) s_cap_auto_btn[i] = cap_button(p, 510 + i * 76, 0, 70, 56, AUTO[i], on_cap_auto, i);
    cap_text(p, CAP_X, 86, 0, &ui_font_16, MUTED, "Cab");
    cap_cab_row(p, 1, 120, 70);
    cap_text(p, 520, 74, CAP_R - 520, &ui_font_16, MUTED, "Captured a cab too? Listen without a cab (Off) to compare fairly.");
    cap_text(p, CAP_X, 160, 0, &ui_font_16, MUTED, "Name");
    s_cap_name = cap_button(p, 120, 142, CAP_R - 120, 60, "", on_cap_name, 0);
    lv_obj_set_style_border_color(s_cap_name, lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_color(s_cap_name, lv_color_hex(0x0D0F11), 0);
    lv_obj_align(lv_obj_get_child(s_cap_name, 0), LV_ALIGN_LEFT_MID, 8, 0);
    cap_text(p, CAP_X, 234, 0, &ui_font_16, MUTED, "Type");
    for (int i = 0; i < CAP_SAVE_COUNT; i++) s_cap_save_btn[i] = cap_button(p, 120 + i * 150, 218, 142, 56, CAP_SAVE_NAMES[i], on_cap_save_type, i);
    s_cap_msg[UI_CAP_COMPARE] = cap_message_label(p, CAP_X, CAP_BTN_Y - 36, CAP_IW);
    s_cap_discard = cap_button(p, CAP_X, CAP_BTN_Y, 170, 64, "Discard", on_cap_discard, 0);
    cap_outline(s_cap_discard);
    s_cap_lib_button = cap_button(p, CAP_R - 412, CAP_BTN_Y, 200, 64, "Save to library", on_cap_save_library, 0);
    s_cap_slot_button = cap_button(p, CAP_R - 200, CAP_BTN_Y, 200, 64, "Save to slot ...", on_cap_save_slot, 0);
    cap_go(s_cap_slot_button);
    s_cap_auto_timer = lv_timer_create(cap_auto_cb, 2000, NULL);
    lv_timer_pause(s_cap_auto_timer);

    // the slot for "Save to slot": 5 banks x 5 (what is in it now stays in the library)
    s_cap_slots = lv_obj_create(s_cap_panel);
    lv_obj_remove_style_all(s_cap_slots);
    lv_obj_set_pos(s_cap_slots, 0, 64);
    lv_obj_set_size(s_cap_slots, CAP_W, CAP_H - 64);
    lv_obj_set_style_bg_color(s_cap_slots, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(s_cap_slots, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_cap_slots, true);
    lv_obj_set_scrollable(s_cap_slots, false);
    cap_text(s_cap_slots, CAP_X, 8, CAP_IW, &ui_font_16, MUTED, "Which capture slot? (bank - slot) The capture in it now stays in My Neural Captures.");
    for (int i = 0; i < NANO_CAPTURE_SLOTS; i++) {
        s_cap_slot_btn[i] = cap_button(s_cap_slots, CAP_X + (i % 5) * 192, 44 + (i / 5) * 72, 184, 64, "", on_cap_slot, i + 1);
        lv_obj_t *l = lv_obj_get_child(s_cap_slot_btn[i], 0);
        lv_obj_set_style_text_font(l, &ui_font_16, 0);
        lv_obj_set_size(l, 168, 22);   // one line, "..." when longer
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_center(l);
    }
    cap_outline(cap_button(s_cap_slots, CAP_R - 412, CAP_H - 64 - 24 - 64, 200, 64, "Cancel", on_cap_slot_button, 0));
    s_cap_slot_ok = cap_button(s_cap_slots, CAP_R - 200, CAP_H - 64 - 24 - 64, 200, 64, "", on_cap_slot_button, 1);
    cap_go(s_cap_slot_ok);
    lv_obj_set_hidden(s_cap_slots, true);

    // Done
    p = s_cap_page[UI_CAP_DONE] = cap_page();
    lv_obj_t *ok = lv_obj_create(p);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, 96, 96);
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_radius(ok, 48, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_t *mark = label(ok, &lv_font_montserrat_48, INK);
    lv_label_set_text(mark, LV_SYMBOL_OK);
    lv_obj_center(mark);
    s_cap_done_title = label(p, &ui_font_title_30, TEXT);
    lv_obj_set_width(s_cap_done_title, CAP_IW);
    lv_obj_set_style_text_align(s_cap_done_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_cap_done_title, CAP_X, 150);
    s_cap_done_text = label(p, &ui_font_20, MUTED);
    lv_obj_set_width(s_cap_done_text, CAP_IW);
    lv_obj_set_style_text_align(s_cap_done_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_cap_done_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_cap_done_text, CAP_X, 200);
    s_cap_msg[UI_CAP_DONE] = NULL;
    cap_button(p, CAP_R - 452, CAP_BTN_Y, 240, 64, "Another capture", on_cap_again, 0);
    primary(cap_button(p, CAP_R - 200, CAP_BTN_Y, 200, 64, "Done", on_cap_done, 0));

    s_capui.type = CAP_TYPE_AMP_CAB;
    s_capui.save_type = CAP_SAVE_AMP_CAB;
    cap_type_show();
    cap_save_type_show();
    cap_values_show(0, 0);
    lv_obj_set_hidden(s_cap, true);
}

// Settings > Capture
static void build_settings_capture(void)
{
    lv_obj_t *page = settings_page(SET_CAPTURE, "Neural Capture",
                                   "Capture an amp, a cab or a pedal with the Nano (Neural Capture V1). The Nano learns on its own - "
                                   "no computer, no internet.");
    lv_obj_t *box = card(page, 20, 112, 546, 150);
    cap_dot(box, 14, 16, GREEN);
    cap_text(box, 36, 11, 496, &ui_font_14, TEXT, "Takes about 5 minutes.");
    cap_dot(box, 14, 44, GREEN);
    cap_text(box, 36, 39, 496, &ui_font_14, TEXT, "You need: the amp or pedal, a dynamic mic or a load box / D.I. out, two instrument cables, headphones.");
    cap_dot(box, 14, 92, GREEN);
    s_set_cap_count = cap_text(box, 36, 87, 496, &ui_font_14, TEXT, "The new capture goes to My Neural Captures (up to 256).");
    cap_text(page, 20, 280, 546, &ui_font_14, MUTED, "The board stays in the capture assistant until you close it. Footswitches, "
                                                    "presets and scenes do nothing meanwhile.");
    cap_go(small_button(page, 386, 400, 180, 50, "New capture", on_settings_capture, 0));
}

// ---- Wi-Fi: Settings > Wi-Fi and the list of the networks around (P4) ----
// Wi-Fi is for tracks from the Mac (Jam Player: send to the board; or the page at nano-board.local). The list comes
// from the Wi-Fi part looking around (UI_NET_SCAN, about 2 s); tap a network and only its password is asked for.
static lv_obj_t *s_wifi_state, *s_wifi_addr, *s_wifi_switch;
static lv_obj_t *s_netlist, *s_netlist_list, *s_netlist_status;
static char s_netlist_ssid[16][33];
static bool s_netlist_secure[16];
static uint32_t s_netlist_sig;

static const char *signal_word(int rssi)
{
    return rssi >= -55 ? "Strong" : rssi >= -67 ? "Good" : rssi >= -78 ? "Fair" : "Weak";
}

static void show_rename(char kind, const char *title, const char *ok, const char *text, int max_length);

static void netlist_close(lv_event_t *e) { (void)e; lv_obj_set_hidden(s_netlist, true); }

static void netlist_choose(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_netlist, true);
    if (i == -1) {   // another network: name and password on the keyboard
        show_rename('W', "Wi-Fi network for tracks from the Mac", "Next", "", 32);
        return;
    }
    if (!s_on_text) return;
    if (i == -2) {   // the board's own network
        s_on_text('W', "");
        s_on_text('K', "");
    } else if (s_netlist_secure[i]) {
        s_on_text('W', s_netlist_ssid[i]);
        char title[64];
        snprintf(title, sizeof(title), "Password for \xE2\x80\x9C%.32s\xE2\x80\x9D", s_netlist_ssid[i]);
        show_rename('K', title, "Join", "", 64);
    } else {   // an open network: joined at once
        s_on_text('W', s_netlist_ssid[i]);
        s_on_text('K', "");
    }
    if (s_net.state == UI_NET_OFF) phone_action(UI_NET_ON, 1);   // (chosen to be used: Wi-Fi goes on)
}

static lv_obj_t *netlist_row(const char *name, const char *right, bool joined, int data)
{
    lv_obj_t *btn = lv_button_create(s_netlist_list);
    lv_obj_set_size(btn, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(btn, lv_color_hex(joined ? 0x2A2E33 : 0x1A1D20), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(SURF_ON), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, joined ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(GREEN), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, netlist_choose, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t *l = label(btn, &ui_font_20, TEXT);
    lv_label_set_text(l, name);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, DLG_W - 360);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *r = label(btn, &ui_font_16, joined ? GREEN : MUTED);
    lv_label_set_text(r, right);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -8, 0);
    return btn;
}

// The rows anew when what the last look found has changed (called 10 times a second while the list shows).
static void netlist_fill(bool force)
{
    const ui_net_t *n = &s_net;
    uint32_t sig = (uint32_t)n->scan_count * 2654435761u ^ (uint32_t)n->scanning ^ (uint32_t)n->state << 8;
    for (int i = 0; i < n->scan_count; i++)
        for (const char *c = n->scan[i].ssid; *c; c++) sig = sig * 31 + (uint8_t)*c;
    if (!force && sig == s_netlist_sig) return;
    s_netlist_sig = sig;
    set_text(s_netlist_status, n->scanning ? "Looking for networks \xE2\x80\xA6" : n->scan_count ? "" : "No network found \xE2\x80\x93 look again, or enter one below.");
    lv_obj_clean(s_netlist_list);
    char right[48];
    for (int i = 0; i < n->scan_count; i++) {
        strlcpy(s_netlist_ssid[i], n->scan[i].ssid, sizeof(s_netlist_ssid[i]));
        s_netlist_secure[i] = n->scan[i].secure;
        bool joined = n->state == UI_NET_STATION && !strcmp(n->ssid, n->scan[i].ssid);
        snprintf(right, sizeof(right), "%s%s \xC2\xB7 %s", joined ? "Joined \xC2\xB7 " : "", signal_word(n->scan[i].rssi),
                 n->scan[i].secure ? "password" : "open");
        netlist_row(n->scan[i].ssid, right, joined, i);
    }
    netlist_row("Other network \xE2\x80\xA6", "Enter its name", false, -1);
    netlist_row("The board's own network", "\xE2\x80\x9CNano Board\xE2\x80\x9D \xC2\xB7 the Mac joins it", n->state == UI_NET_AP, -2);
}

static void netlist_rescan(lv_event_t *e)
{
    (void)e;
    phone_action(UI_NET_SCAN, 0);
    set_text(s_netlist_status, "Looking for networks \xE2\x80\xA6");
}

static void netlist_open(void)
{
    lv_obj_set_hidden(s_netlist, false);
    lv_obj_move_foreground(s_netlist);
    netlist_fill(true);
    netlist_rescan(NULL);
}

static void on_wifi_choose(lv_event_t *e) { (void)e; netlist_open(); }
static void on_wifi_switch(lv_event_t *e) { (void)e; phone_action(UI_NET_ON, lv_obj_has_state(s_wifi_switch, LV_STATE_CHECKED)); }

static void build_netlist(lv_obj_t *screen)
{
    s_netlist = dialog_full(screen);
    dialog_title(s_netlist, "Wi-Fi network");
    lv_obj_t *hint = label(s_netlist, HINT_FONT, MUTED);
    lv_label_set_text(hint, "For tracks from the Mac. Tap a network - the board asks only for its password.");
    lv_obj_set_pos(hint, 20, 48);
    close_button(s_netlist, DLG_W - 60, 16, netlist_close, 0);
    small_button(s_netlist, DLG_W - 236, 12, 160, 50, "Look again", netlist_rescan, 0);
    s_netlist_status = label(s_netlist, &ui_font_16, CAPTION);
    lv_obj_set_pos(s_netlist_status, 20, 78);
    s_netlist_list = scroll_list(s_netlist, 16, 104, DLG_W - 32, DLG_H - 116);
    lv_obj_set_hidden(s_netlist, true);
}

static void show_wifi_page(void)
{
    const ui_net_t *n = &s_net;
    char text[96], addr[96];
    addr[0] = 0;
    switch (n->state) {
    case UI_NET_STARTING: snprintf(text, sizeof(text), "Starting \xE2\x80\xA6"); break;
    case UI_NET_JOINING: snprintf(text, sizeof(text), "Joining %.32s \xE2\x80\xA6", n->ssid); break;
    case UI_NET_STATION:
        snprintf(text, sizeof(text), "Joined %.32s", n->ssid);
        snprintf(addr, sizeof(addr), "http://nano-board.local  \xC2\xB7  %.40s", n->addr);
        break;
    case UI_NET_AP:
        snprintf(text, sizeof(text), "Own network \xE2\x80\x9CNano Board\xE2\x80\x9D");
        snprintf(addr, sizeof(addr), "Password %.11s  \xC2\xB7  http://192.168.4.1", n->ap_pass);
        break;
    case UI_NET_FAILED: snprintf(text, sizeof(text), "The Wi-Fi part does not answer"); break;
    default: snprintf(text, sizeof(text), "Off"); break;
    }
    set_text(s_wifi_state, text);
    set_color_prop(s_wifi_state, LV_STYLE_TEXT_COLOR, n->state == UI_NET_OFF ? CAPTION : n->state == UI_NET_FAILED ? 0xFF4B4B : TEXT);
    set_text(s_wifi_addr, addr);
    bool on = n->state != UI_NET_OFF;
    if (!lv_obj_has_state(s_wifi_switch, LV_STATE_PRESSED) && on != lv_obj_has_state(s_wifi_switch, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_wifi_switch, LV_STATE_CHECKED);
    }
    lv_label_set_text(s_set_badge[SET_WIFI], on ? "ON" : "");
}

static void build_wifi_page(void)
{
    lv_obj_t *page = settings_page(SET_WIFI, "Wi-Fi", "Tracks from the Mac onto the board's card. Off after every start.");
    lv_obj_t *row = box(page, 0, 84, SET_W, 64);
    lv_obj_t *t = label(row, &ui_font_20, TEXT);
    lv_label_set_text(t, "Wi-Fi");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 16, 0);
    s_wifi_switch = calm_switch(row);
    lv_obj_align(s_wifi_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_event_cb(s_wifi_switch, on_wifi_switch, LV_EVENT_VALUE_CHANGED, NULL);
    s_wifi_state = label(page, &ui_font_24, TEXT);
    lv_obj_set_pos(s_wifi_state, 20, 170);
    s_wifi_addr = label(page, HINT_FONT, MUTED);
    lv_obj_set_pos(s_wifi_addr, 20, 206);
    small_button(page, 20, 246, 260, 50, "Choose network \xE2\x80\xA6", on_wifi_choose, 0);
    lv_obj_t *hint = label(page, HINT_FONT, MUTED);
    lv_obj_set_width(hint, SET_W - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "On the Mac: Jam Player, Send to the board - or http://nano-board.local in the browser. "
                            "Without a network of your own the board opens its own network \xE2\x80\x9CNano Board\xE2\x80\x9D.");
    lv_obj_set_pos(hint, 20, 320);
}

static void build_settings(lv_obj_t *screen)
{
    s_settings = lv_obj_create(screen);
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_size(s_settings, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_settings, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_settings, false);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_CLICKABLE);   // (taps do not reach the screen below)

    // P4: "< Settings" in the first top box, the topics as one box of rows in column 1, the page in columns 2-4
    lv_obj_t *hb = box(s_settings, COL_X(0), 10, COL_W, 52);
    lv_obj_t *back = seg(hb, 0, COL_W - 2, close_settings);   // back to playing
    lv_obj_t *back_icon = ui_icon(back, UI_ICON_BACK, 22, TEXT);
    lv_obj_align(back_icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *back_label = label(back, &ui_font_title_22, TEXT);
    lv_label_set_text(back_label, "Settings");
    lv_obj_align(back_label, LV_ALIGN_LEFT_MID, 42, 0);

    static const struct { int icon; const char *name; } ITEMS[SET_COUNT] = {   // (icon -1: LVGL's Wi-Fi symbol)
        { UI_ICON_NANO, "Nano" }, { UI_ICON_VOLUME, "Audio" }, { UI_ICON_PLAY, "Phone" }, { -1, "Wi-Fi" }, { UI_ICON_MIDI, "MIDI" },
        { UI_ICON_FOOTSWITCH, "Footswitches" }, { UI_ICON_GRID, "Tile layout" }, { UI_ICON_GEAR, "Display" },
        { UI_ICON_INFO, "System" }, { UI_ICON_CAP_AMP_CAB, "Capture" } };
    const int row_h = 50;   // ten topics under the top bar (57 fitted nine, 64 eight)
    lv_obj_t *side = box(s_settings, COL_X(0), 72, COL_W, SET_COUNT * row_h + (SET_COUNT - 1) + 2);
    for (int i = 0; i < SET_COUNT; i++) {
        int y = i * (row_h + 1);
        if (i) {
            lv_obj_t *l = lv_obj_create(side);
            lv_obj_remove_style_all(l);
            lv_obj_set_style_bg_color(l, lv_color_hex(HAIRLINE), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            lv_obj_set_pos(l, 0, y - 1);
            lv_obj_set_size(l, COL_W - 2, 1);
        }
        lv_obj_t *it = seg(side, 0, COL_W - 2, on_settings_item);
        lv_obj_set_pos(it, 0, y);
        lv_obj_set_height(it, row_h);
        lv_obj_set_style_radius(it, i == 0 || i == SET_COUNT - 1 ? RADIUS - 1 : 0, 0);
        lv_obj_remove_event_cb(it, on_settings_item);
        lv_obj_add_event_cb(it, on_settings_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_set_item[i] = it;
        if (ITEMS[i].icon >= 0) {
            s_set_item_icon[i] = ui_icon(it, ITEMS[i].icon, 20, CAPTION);
        } else {   // a symbol of LVGL's own font, recoloured like the pictures (see settings_select)
            s_set_item_icon[i] = label(it, &lv_font_montserrat_20, CAPTION);
            lv_label_set_text(s_set_item_icon[i], LV_SYMBOL_WIFI);
        }
        lv_obj_align(s_set_item_icon[i], LV_ALIGN_LEFT_MID, 16, 0);
        s_set_item_label[i] = label(it, &ui_font_title_16, 0xC4C8CD);
        lv_label_set_text(s_set_item_label[i], ITEMS[i].name);
        lv_obj_align(s_set_item_label[i], LV_ALIGN_LEFT_MID, 48, 0);
        s_set_badge[i] = label(it, &ui_font_12, CAPTION);
        lv_obj_set_style_text_letter_space(s_set_badge[i], 1, 0);
        lv_obj_align(s_set_badge[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }

    s_set_content = lv_obj_create(s_settings);
    lv_obj_remove_style_all(s_set_content);
    lv_obj_set_size(s_set_content, SET_W, SCREEN_H);
    lv_obj_set_pos(s_set_content, SET_X, 10);
    lv_obj_set_scrollable(s_set_content, false);

    build_wifi_page();

    lv_obj_t *nano = settings_page(SET_NANO, "Nano", "The connection to the Nano Cortex.");
    s_set_nano_text = label(nano, &ui_font_24, TEXT);
    lv_obj_set_pos(s_set_nano_text, 20, 96);
    lv_obj_t *nano_hint = label(nano, HINT_FONT, MUTED);
    lv_obj_set_width(nano_hint, 540);
    lv_label_set_long_mode(nano_hint, LV_LABEL_LONG_WRAP);
#if NANO_P4
    lv_label_set_text(nano_hint, "Not found? Its USB cable goes into the USB-A socket - without the cable press EXIT + CAPTURE on the Nano (Bluetooth).");
#else
    lv_label_set_text(nano_hint, "Not found? Press EXIT + CAPTURE on the Nano: it can be found for a few seconds then.");
#endif
    lv_obj_set_pos(nano_hint, 20, 170);
    small_button(nano, 20, 220, 260, 50, "Read everything again", on_settings_reload, 0);
    // Cortex Cloud on the phone through the controller (the board offers itself with the Nano's identity)
    lv_obj_t *cloud = label(nano, &ui_font_20, TEXT);
    lv_label_set_text(cloud, "Cortex Cloud through the board");
    lv_obj_set_pos(cloud, 20, 306);
    lv_obj_t *cloud_hint = label(nano, &ui_font_14, MUTED);
    lv_obj_set_width(cloud_hint, 440);
    lv_label_set_long_mode(cloud_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(cloud_hint, "The app on the phone finds your Nano through the board - load captures without "
                                  "switching the board off. Meanwhile the phone's Bluetooth MIDI (looper) is not offered. "
                                  "No firmware updates this way.");
    lv_obj_set_pos(cloud_hint, 20, 336);
    s_set_cloud = lv_switch_create(nano);
    lv_obj_set_size(s_set_cloud, 72, 38);
    lv_obj_set_pos(s_set_cloud, 494, 300);
    lv_obj_set_ext_click_area(s_set_cloud, 8);
    lv_obj_set_style_bg_color(s_set_cloud, lv_color_hex(0x2A2D30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_set_cloud, lv_color_hex(GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_set_cloud, lv_color_hex(0xF2F2F2), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_set_cloud, lv_color_hex(INK), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(s_set_cloud, -3, LV_PART_KNOB);
    lv_obj_add_event_cb(s_set_cloud, on_settings_cloud, LV_EVENT_VALUE_CHANGED, NULL);
    // P4 board: the Nano is on the cable, its own Bluetooth stays free - the phone's app can go to the Nano directly
    // (to check on the hardware: USB control and a Bluetooth app at the same time). Through the board: stage f.
    lv_label_set_text(cloud, "Cortex Cloud on the phone");
    lv_label_set_text(cloud_hint, "The board talks to the Nano over the cable, so the app on the phone connects to the "
                                  "Nano itself, as without a controller.");
    lv_obj_set_hidden(s_set_cloud, true);

    settings_embed(SET_MIDI, s_midi, s_midi_close);
    lv_obj_set_style_text_font(lv_obj_get_child(s_midi, 0), &ui_font_title_26, 0);
    lv_obj_set_pos(lv_obj_get_child(s_midi, 0), 0, 4);
    for (int i = 0; i < 2; i++) lv_obj_set_hidden(s_midi_more[i], true);   // (the list on the left has them)
    lv_obj_align(s_midi_status, LV_ALIGN_TOP_RIGHT, -20, 22);
    {
        lv_obj_t *audio = settings_page(SET_USB, "Audio", "What the Nano plays besides your guitar");
        lv_obj_t *c = label(audio, &ui_font_12, CAPTION);
        lv_obj_set_style_text_letter_space(c, 1, 0);
        lv_label_set_text(c, "USB AUDIO \xC2\xB7 FROM THE COMPUTER");
        lv_obj_set_pos(c, 0, 76);
        // The USB dialog's slider (it carries the logic), value and line move here with the fill bar's look; its
        // - / + / -6 dB buttons stay in the hidden dialog.
        lv_obj_t *bar = s_usb_slider;
        lv_obj_set_parent(bar, audio);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 0, 98);
        lv_obj_set_size(bar, SET_W, 60);
        lv_obj_set_ext_click_area(bar, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(SURF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_hex(HAIRLINE), LV_PART_MAIN);
        lv_obj_set_style_radius(bar, RADIUS, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(FILL), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, RADIUS - 1, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, lv_color_hex(TEXT), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_pad_hor(bar, -29, LV_PART_KNOB);
        lv_obj_set_style_pad_ver(bar, -1, LV_PART_KNOB);
        lv_obj_set_style_opa(bar, LV_OPA_50, LV_STATE_DISABLED);
        lv_obj_t *name = label(bar, &ui_font_title_18, TEXT);
        lv_label_set_text(name, "Volume");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_set_parent(s_usb_value, bar);
        lv_obj_set_style_text_font(s_usb_value, &ui_font_title_18, 0);
        lv_obj_align(s_usb_value, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_parent(s_usb_info, audio);
        lv_obj_set_style_text_color(s_usb_info, lv_color_hex(CAPTION), 0);
        lv_obj_align(s_usb_info, LV_ALIGN_TOP_RIGHT, 0, 76);
        lv_obj_set_style_text_font(s_usb_info, &ui_font_14, 0);
        c = label(audio, &ui_font_12, CAPTION);
        lv_obj_set_style_text_letter_space(c, 1, 0);
        lv_label_set_text(c, "PHONE \xC2\xB7 BLUETOOTH");
        lv_obj_set_pos(c, 0, 176);
        s_set_phone_vol = fill_bar(audio, 0, 198, SET_W, "Volume", &s_set_phone_vol_value);
        lv_slider_set_range(s_set_phone_vol, -600, 60);
        lv_obj_add_event_cb(s_set_phone_vol, on_set_phone_volume, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_t *rows = box(audio, 0, 276, SET_W, 2 * 60 + 3);
        lv_obj_t *l = label(rows, &ui_font_16, TEXT);
        lv_label_set_text(l, "Mute the phone while the tuner is on");
        lv_obj_set_pos(l, 16, 19);
        s_set_phone_mute = calm_switch(rows);
        lv_obj_set_pos(s_set_phone_mute, SET_W - 2 - 16 - 52, 15);
        lv_obj_add_event_cb(s_set_phone_mute, on_set_phone_mute, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_t *hl = lv_obj_create(rows);
        lv_obj_remove_style_all(hl);
        lv_obj_set_style_bg_color(hl, lv_color_hex(HAIRLINE), 0);
        lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
        lv_obj_set_pos(hl, 0, 60);
        lv_obj_set_size(hl, SET_W - 2, 1);
        l = label(rows, &ui_font_16, TEXT);
        lv_label_set_text(l, "Play the phone on");
        lv_obj_set_pos(l, 16, 80);
        l = label(rows, &ui_font_16, CAPTION);
        lv_label_set_text(l, "USB 1 / 2");
        lv_obj_set_pos(l, SET_W - 2 - 16 - 80, 80);
        lv_obj_set_hidden(s_usb, true);
    }
    {
        lv_obj_t *ph = settings_page(SET_PHONE, "Phone", "Bluetooth audio from the phone, through the board into the Nano");
        lv_obj_t *l = label(ph, &ui_font_16, CAPTION);
        lv_obj_set_width(l, SET_W);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, "Pairing, the paired phone and what it plays come with the Bluetooth audio part (stage e).");
        lv_obj_set_pos(l, 0, 84);
        lv_obj_t *dp = settings_page(SET_DISPLAY, "Display", "Brightness of the screen");
        s_set_bright = fill_bar(dp, 0, 84, SET_W, "Brightness", &s_set_bright_value);
        lv_slider_set_range(s_set_bright, 10, 100);
        lv_slider_set_value(s_set_bright, 100, LV_ANIM_OFF);
        lv_label_set_text(s_set_bright_value, "100 %");
        lv_obj_add_event_cb(s_set_bright, on_set_brightness, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_set_bright, on_set_brightness, LV_EVENT_RELEASED, NULL);
    }

    lv_obj_t *layout = settings_page(SET_LAYOUT, "Tile layout", "6 or 8 fields on the screen, in any order, and what footswitch 1 does.");
    s_set_layout_text = label(layout, &ui_font_20, TEXT);
    lv_obj_set_pos(s_set_layout_text, 20, 110);
    primary(small_button(layout, 20, 190, 260, 50, "Change the layout ...", on_settings_layout, 0));

    lv_obj_t *switches = settings_page(SET_SWITCHES, "Footswitches",
                                       "After the wiring changed: teach the board which footswitch is which, one after the other.");
    primary(small_button(switches, 20, 130, 260, 50, "Learn the order ...", on_settings_learn, 0));

    settings_embed(SET_SYSTEM, s_system, s_system_close);
    lv_obj_t *st = lv_obj_get_child(s_system, 0);   // its dialog title
    lv_obj_set_style_text_font(st, &ui_font_title_26, 0);
    lv_obj_set_pos(st, 0, 4);
    lv_obj_t *sh = label(s_system, HINT_FONT, CAPTION);
    lv_label_set_text(sh, "What is connected, and how the board is doing");
    lv_obj_set_pos(sh, 0, 40);
    lv_obj_set_hidden(lv_obj_get_child(s_system, -2), true);   // the Refresh button: the page reads again when chosen
    lv_obj_set_pos(s_system_rows, 0, 74);
    lv_obj_set_size(s_system_rows, SET_W, SCREEN_H - 74 - 10 - MARGIN);
    build_settings_capture();

    lv_obj_set_hidden(s_settings, true);
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
__attribute__((unused)) static lv_obj_t *source_card(lv_obj_t *screen, int x, const char *title, int icon, lv_event_cb_t cb, lv_event_cb_t long_cb,
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
    lv_obj_t *t = label(c, &ui_font_14, 0xA8ADB3);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 56, 5);
    if (title_label) *title_label = t;
    *slot = label(c, &ui_font_14, 0xA8ADB3);
    lv_obj_align(*slot, LV_ALIGN_TOP_RIGHT, -12, 5);
    *name = label(c, &ui_font_20, TEXT);
    lv_obj_set_width(*name, 310);
    lv_label_set_long_mode(*name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(*name, 56, 24);
    return c;
}


// ---- P4: the calm system's pieces ----

// A box: the one surface, a hairline border, the one radius.
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_radius(b, RADIUS, 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_clickable(b, false);
    return b;
}

// A field of a box that can be tapped: the box's full height, x / w inside its border.
static lv_obj_t *seg(lv_obj_t *b, int x, int w, lv_event_cb_t cb)
{
    lv_obj_t *f = lv_button_create(b);
    lv_obj_remove_style_all(f);
    lv_obj_set_pos(f, x, 0);
    lv_obj_set_size(f, w, lv_obj_get_style_height(b, 0) - 2);
    lv_obj_set_style_bg_color(f, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(SURF_ON), LV_STATE_PRESSED);
    lv_obj_set_style_radius(f, RADIUS - 1, 0);
    lv_obj_set_style_opa(f, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_scrollable(f, false);
    if (cb) lv_obj_add_event_cb(f, cb, LV_EVENT_CLICKED, NULL);
    else lv_obj_set_clickable(f, false);
    return f;
}

// The hairline between two fields of a box.
static void vline(lv_obj_t *b, int x)
{
    lv_obj_t *l = lv_obj_create(b);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_bg_color(l, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_pos(l, x, 0);
    lv_obj_set_size(l, 1, lv_obj_get_style_height(b, 0) - 2);
}

// A small round mark: filled in `color` = on, a hollow ring = off / empty.
static void set_dot(lv_obj_t *d, bool on, uint32_t color)
{
    set_color_prop(d, LV_STYLE_BG_COLOR, color);
    set_num_prop(d, LV_STYLE_BG_OPA, on ? LV_OPA_COVER : LV_OPA_TRANSP);
    set_num_prop(d, LV_STYLE_BORDER_WIDTH, on ? 0 : 1);
}

// ---- P4: the signal chain - one box, eight equal fields split by hairlines ----

static void on_chain_fx(lv_event_t *e)
{
    send('O', (int)(intptr_t)lv_event_get_user_data(e));   // the FX editor of the slot
}

static void chain_field(lv_obj_t *b, int n, int x, int w, const char *caption, lv_event_cb_t cb, lv_event_cb_t long_cb, void *arg)
{
    lv_obj_t *f = seg(b, x, w, NULL);
    if (cb) {
        lv_obj_set_clickable(f, true);
        lv_obj_add_event_cb(f, cb, LV_EVENT_SHORT_CLICKED, arg);
    }
    if (long_cb) lv_obj_add_event_cb(f, long_cb, LV_EVENT_LONG_PRESSED, arg);
    lv_obj_t *d = lv_obj_create(f);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 7, 7);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(d, lv_color_hex(DIMMED), 0);
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_pos(d, 12, 13);
    s_chain_dot[n] = d;
    lv_obj_t *c = label(f, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, caption);
    lv_obj_set_pos(c, 24, 8);
    s_chain_cap[n] = c;
    lv_obj_t *name = label(f, &ui_font_title_14, TEXT);
    lv_obj_set_size(name, w - 20, 2 * lv_font_get_line_height(&ui_font_title_14));   // two lines at most
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, 12, 28);
    s_chain_name[n] = name;
}

static void build_chain(lv_obj_t *parent)
{
    lv_obj_t *b = box(parent, MARGIN, 162, SCREEN_W - 2 * MARGIN, 72);
    s_chain = b;
    static const char *const CAPS[CHAIN_FIELDS] = { "PRE 1", "PRE 2", "CAPTURE", "CAB", "POST 1", "POST 2", "POST 3", "OUT" };
    static const int SLOT[CHAIN_FIELDS] = { 0, 1, -1, -2, 2, 3, 4, -3 };   // FX slot, -1 capture, -2 cab, -3 output
    int inner = SCREEN_W - 2 * MARGIN - 2;
    for (int n = 0; n < CHAIN_FIELDS; n++) {
        int x = n * inner / CHAIN_FIELDS, w = (n + 1) * inner / CHAIN_FIELDS - x;
        if (n) vline(b, x), x++, w--;
        if (SLOT[n] >= 0) chain_field(b, n, x, w, CAPS[n], on_chain_fx, NULL, (void *)(intptr_t)SLOT[n]);
        else if (SLOT[n] == -1) chain_field(b, n, x, w, CAPS[n], open_capture_picker, open_amp, NULL);
        else if (SLOT[n] == -2) chain_field(b, n, x, w, CAPS[n], open_cab_picker, open_cab_settings, NULL);
        else chain_field(b, n, x, w, CAPS[n], on_level_open, NULL, NULL);   // (OUT: the Level sheet)
        lv_obj_t *f = lv_obj_get_parent(s_chain_name[n]);
        if (SLOT[n] == -1 || SLOT[n] == -2) {   // the capture's / cab's labels as the old cards had them
            lv_obj_t *slot = label(f, &ui_font_12, CAPTION);
            lv_obj_align(slot, LV_ALIGN_TOP_RIGHT, -10, 8);
            lv_obj_t *sq = icon_square(f, 1);   // (kept for the state code; not shown)
            lv_obj_set_hidden(sq, true);
            if (SLOT[n] == -1) s_capture = s_chain_name[n], s_capture_title = s_chain_cap[n], s_capture_slot_label = slot, s_capture_square = sq;
            else s_cab = s_chain_name[n], s_cab_slot_label = slot, s_cab_square = sq;
        }
        if (SLOT[n] == -3) {   // the Nano's output: a level bar and the loudness (USB capture, later)
            lv_obj_set_hidden(s_chain_dot[n], true);
            lv_obj_set_pos(s_chain_cap[n], 12, 8);
            lv_obj_t *track = lv_obj_create(f);
            lv_obj_remove_style_all(track);
            lv_obj_set_style_bg_color(track, lv_color_hex(HAIRLINE), 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(track, 3, 0);
            lv_obj_set_size(track, w - 24, 5);
            lv_obj_set_pos(track, 12, 30);
            s_out_fill = lv_obj_create(track);
            lv_obj_remove_style_all(s_out_fill);
            lv_obj_set_style_bg_color(s_out_fill, lv_color_hex(TEXT), 0);
            lv_obj_set_style_bg_opa(s_out_fill, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_out_fill, 3, 0);
            lv_obj_set_size(s_out_fill, 0, 5);
            s_out_value = s_chain_name[n];
            lv_obj_set_pos(s_out_value, 12, 42);
            lv_label_set_text(s_out_value, "\xE2\x80\x93");
            lv_obj_set_style_text_color(s_out_value, lv_color_hex(CAPTION), 0);
        }
    }
}

// The chain's FX fields from the state: model name, the dot in the effect's colour (filled = on, ring = off),
// grey while off. Capture and cab: dot filled while they are not bypassed.
static void show_chain(const nano_state_t *st)
{
    static const int SLOT[CHAIN_FIELDS] = { 0, 1, -1, -2, 2, 3, 4, -3 };
    for (int n = 0; n < CHAIN_FIELDS; n++) {
        int slot = SLOT[n];
        bool on;
        uint32_t color;
        if (slot >= 0) {
            const nano_fx_model_t *model = nano_fx_model(st->fx_type[slot]);
            bool empty = !st->fx_known || !st->fx_type[slot];
            on = !empty && st->fx_on[slot];
            color = model ? model->color : DIMMED;
            set_text(s_chain_name[n], empty ? "Empty" : nano_fx_name(st->fx_type[slot]));
        } else if (slot == -1) {
            on = st->capture_slot != 0;
            color = TEXT;
        } else if (slot == -2) {
            on = st->cab_slot != 0;
            color = 0xB38CFF;
        } else {
            continue;
        }
        set_dot(s_chain_dot[n], on, on ? color : DIMMED);
        set_color_prop(s_chain_name[n], LV_STYLE_TEXT_COLOR, on ? TEXT : DIMMED);
        set_color_prop(s_chain_cap[n], LV_STYLE_TEXT_COLOR, on ? CAPTION : DIMMED);
    }
}

void ui_set_output_level(bool known, float lufs, float level)
{
    lvgl_port_lock(0);
    int w = lv_obj_get_style_width(lv_obj_get_parent(s_out_fill), 0);
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    set_num_prop(s_out_fill, LV_STYLE_WIDTH, known ? (int)(w * level) : 0);
    char text[16];
    if (known) snprintf(text, sizeof(text), "%s%.0f LUFS", lufs < -0.5f ? "\xE2\x88\x92" : "", fabsf(lufs));
    else snprintf(text, sizeof(text), "\xE2\x80\x93");
    set_text(s_out_value, text);
    set_color_prop(s_out_value, LV_STYLE_TEXT_COLOR, known ? TEXT : CAPTION);
    lvgl_port_unlock();
}

// ---- P4: the phone (Bluetooth audio through the ESP32): the top bar's field and the player ----

static void phone_action(ui_phone_action_t a, int value)
{
    if (s_phone_cb) s_phone_cb(a, value);
}

static void on_phone_toggle(lv_event_t *e) { phone_action(s_source ? UI_JAM_TOGGLE : UI_PHONE_TOGGLE, 0); }

// ---- the players' footswitch row (2026-11-26, draft C): Back | Play | Pause | Next as four tiles along the bottom of
//      both players, drawn as the main screen's tiles - symbol top left, the footswitch's number top right, the name
//      bottom left; Play lit green while it plays, Pause lit while it is paused. The footswitches are the bottom row of
//      the tile layout (positions 5-8; with 6 fields 4-6: Back, Play / Pause, Next); footswitch 1 closes the player
//      (the Close tile at the top right says so). ----
typedef struct { lv_obj_t *tile[4], *sym[4], *num[4], *name[4]; } transport_t;
static transport_t s_jam_tr, s_ph_tr;
static const char *const TRANSPORT_SYM[4] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, LV_SYMBOL_CLOSE };
static const char *const TRANSPORT_NAME[4] = { "Back", "Play", "Next", "Close" };
#define PLAYER_ROW_H 96
#define PLAYER_ROW_Y (SCREEN_H - 72 - MARGIN - 2 - 12 - PLAYER_ROW_H)   // (in the player's box)

// The footswitch of field k (0-3), 0 = none
// (only in player mode: there switches 5-8 are these, in fixed places)
static int transport_switch(int k) { return s_player_mode ? 5 + k : 0; }
static void on_player_row_out(lv_event_t *e);


static lv_obj_t *player_tile(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb, lv_obj_t **sym, lv_obj_t **num,
                             lv_obj_t **name, const char *symbol, const char *text)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, RADIUS, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_scrollable(t, false);
    lv_obj_set_clickable(t, true);
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
    *sym = label(t, &ui_font_24, TEXT);
    lv_label_set_text(*sym, symbol);
    lv_obj_set_pos(*sym, 14, 10);
    *num = label(t, &ui_font_title_16, CAPTION);
    lv_obj_align(*num, LV_ALIGN_TOP_RIGHT, -14, 10);
    *name = label(t, &ui_font_title_22, TEXT);
    lv_label_set_text(*name, text);
    lv_obj_align(*name, LV_ALIGN_BOTTOM_LEFT, 14, -10);
    return t;
}

static void player_tile_look(lv_obj_t *t, lv_obj_t *sym, lv_obj_t *num, lv_obj_t *name, uint32_t color, bool on)
{
    uint32_t lit = blend(color, SURF, 0.80f), ink = on ? ink_for(lit) : TEXT;
    set_color_prop(t, LV_STYLE_BG_COLOR, on ? lit : blend(color, SURF, TILE_DIM));
    set_color_prop(t, LV_STYLE_BORDER_COLOR, on ? lit : blend(color, SURF, TILE_DIM + 0.14f));
    set_color_prop(sym, LV_STYLE_TEXT_COLOR, on ? ink : blend(color, 0xFFFFFF, 0.7f));
    set_color_prop(num, LV_STYLE_TEXT_COLOR, on ? ink : CAPTION);
    set_color_prop(name, LV_STYLE_TEXT_COLOR, ink);
}

// The row along the bottom of a player, four tiles over the width
static void build_transport(transport_t *t, lv_obj_t *parent, const lv_event_cb_t cb[4])
{
    int inner = SCREEN_W - 2 * MARGIN - 2, w = (inner - 5 * 12) / 4;
    for (int k = 0; k < 4; k++)
        t->tile[k] = player_tile(parent, 12 + k * (w + 12), PLAYER_ROW_Y, w, PLAYER_ROW_H, cb[k], &t->sym[k], &t->num[k],
                                 &t->name[k], TRANSPORT_SYM[k], TRANSPORT_NAME[k]);
}

static void show_transport(transport_t *t, bool playing)
{
    // (2026-11-26: play / pause on one switch, 8 back out - as the player mode's tiles; 2026-11-28, player draft: in
    // player mode the jam page's row is the footswitches' selection - previous / next song, 6 plays the selected one)
    bool select = s_player_mode && t == &s_jam_tr, pending = select && player_pending();
    set_text(t->sym[0], select ? LV_SYMBOL_UP : LV_SYMBOL_PREV);
    set_text(t->name[0], select ? "Previous" : "Back");
    set_text(t->sym[2], select ? LV_SYMBOL_DOWN : LV_SYMBOL_NEXT);
    set_text(t->sym[1], playing && !pending ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    set_text(t->name[1], playing && !pending ? "Pause" : "Play");
    set_text(t->name[3], s_player_mode ? "Presets" : "Close");
    for (int k = 0; k < 4; k++) {
        bool on = k == 1 && playing && !pending;
        player_tile_look(t->tile[k], t->sym[k], t->num[k], t->name[k], k == 1 ? (pending ? SEL_BLUE : 0x45F862) : 0x9AA0A6, on);
        char n[4] = "";
        int sw = transport_switch(k);
        if (sw >= 1 && sw <= 8) snprintf(n, sizeof(n), "%d", sw);
        set_text(t->num[k], n);
    }
}

// The row's fourth tile: in player mode back out to the presets (as switch 8), else the page closes
static void on_player_row_out(lv_event_t *e)
{
    (void)e;
    if (s_player_mode) send('w', 8);
    else { lv_obj_set_hidden(s_phone, true); lv_obj_set_hidden(s_jam, true); }
}

// Close, at the top right of both players (by touch)
static void player_close(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *sym, *num, *name;
    int w = 150, h = 44;   // (as high as the Jam tracks | Phone switch beside it, on its line)
    lv_obj_t *t = player_tile(parent, SCREEN_W - 2 * MARGIN - 2 - 24 - w, 14, w, h, cb, &sym, &num, &name, "", "Close");
    lv_obj_t *x = ui_icon(t, UI_ICON_CLOSE, 18, TEXT);
    lv_obj_align(x, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    lv_obj_set_style_text_font(name, &ui_font_title_18, 0);
    lv_label_set_text(num, "");
    lv_obj_align(num, LV_ALIGN_RIGHT_MID, -14, 0);
    player_tile_look(t, sym, num, name, 0x9AA0A6, false);
}
static void on_phone_prev(lv_event_t *e) { phone_action(UI_PHONE_PREV, 0); }
static void on_phone_next(lv_event_t *e) { phone_action(UI_PHONE_NEXT, 0); }
static bool s_ph_playing;
static void on_phone_play(lv_event_t *e) { if (!s_ph_playing) phone_action(UI_PHONE_TOGGLE, 0); }
static void on_phone_pause(lv_event_t *e) { if (s_ph_playing) phone_action(UI_PHONE_TOGGLE, 0); }
static void close_phone(lv_event_t *e) { lv_obj_set_hidden(s_phone, true); }

static void open_phone(lv_event_t *e)
{
    open_source(s_source);   // the player of what the field shows
}

static void on_phone_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(s_ph_vol);   // dB x 10
    char text[16];
    snprintf(text, sizeof(text), "%s%.1f dB", v < 0 ? "\xE2\x88\x92" : "", fabsf(v / 10.0f));
    lv_label_set_text(s_ph_vol_value, text);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        phone_action(UI_PHONE_VOLUME, v);
}

static void on_phone_mute(lv_event_t *e)
{
    phone_action(UI_PHONE_MUTE_TUNER, lv_obj_has_state(s_ph_mute, LV_STATE_CHECKED));
}

static lv_obj_t *calm_switch(lv_obj_t *parent)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 52, 30);
    lv_obj_set_style_bg_color(sw, lv_color_hex(FILL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
    return sw;
}

// A fill bar as the board's sliders: the fill in the neutral colour with a white edge, name left, value right.
static lv_obj_t *fill_bar(lv_obj_t *parent, int x, int y, int w, const char *name, lv_obj_t **value)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_pos(sl, x, y);
    lv_obj_set_size(sl, w, 60);
    lv_obj_set_style_bg_color(sl, lv_color_hex(SURF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sl, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sl, lv_color_hex(HAIRLINE), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sl, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(FILL), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, RADIUS - 1, LV_PART_INDICATOR);
    // the white edge at the end of the fill: the knob, 2 px wide and the bar's height
    lv_obj_set_style_bg_color(sl, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(sl, -29, LV_PART_KNOB);
    lv_obj_set_style_pad_ver(sl, -1, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_t *l = label(sl, &ui_font_title_18, TEXT);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
    *value = label(sl, &ui_font_title_18, TEXT);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -16, 0);
    return sl;
}

static void on_set_brightness(lv_event_t *e)
{
    int v = lv_slider_get_value(s_set_bright);
    char text[8];
    snprintf(text, sizeof(text), "%d %%", v);
    lv_label_set_text(s_set_bright_value, text);
    phone_action(lv_event_get_code(e) == LV_EVENT_RELEASED ? UI_BRIGHTNESS_KEEP : UI_BRIGHTNESS, v);
}

void ui_set_brightness(int percent)
{
    lvgl_port_lock(0);
    if (s_set_bright) {
        char text[8];
        snprintf(text, sizeof(text), "%d %%", percent);
        lv_slider_set_value(s_set_bright, percent, LV_ANIM_OFF);
        lv_label_set_text(s_set_bright_value, text);
    }
    lvgl_port_unlock();
}

static void on_set_phone_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(s_set_phone_vol);
    char text[16];
    snprintf(text, sizeof(text), "%s%.1f dB", v < 0 ? "\xE2\x88\x92" : "", fabsf(v / 10.0f));
    lv_label_set_text(s_set_phone_vol_value, text);
    phone_action(UI_PHONE_VOLUME, v);
}

static void on_set_phone_mute(lv_event_t *e)
{
    phone_action(UI_PHONE_MUTE_TUNER, lv_obj_has_state(s_set_phone_mute, LV_STATE_CHECKED));
}

static void build_phone(lv_obj_t *screen)
{
    // Opens under the top bar (its phone field stays in sight), opaque: no scrim on the board.
    s_phone = box(screen, MARGIN, 72, SCREEN_W - 2 * MARGIN, SCREEN_H - 72 - MARGIN);
    lv_obj_set_style_bg_color(s_phone, lv_color_black(), 0);
    lv_obj_set_style_border_color(s_phone, lv_color_hex(FILL), 0);
    lv_obj_set_clickable(s_phone, true);   // (taps stay here)
    const int X = 24, W = SCREEN_W - 2 * MARGIN - 2 * X - 2;
    s_ph_caption = label(s_phone, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_ph_caption, 1, 0);
    lv_obj_set_pos(s_ph_caption, X + 2 * 124 + 3 + 20, 30);
    source_chooser(s_phone, X, 14, 0);
    player_close(s_phone, close_phone);
    s_ph_title = label(s_phone, &ui_font_title_46, TEXT);
    lv_obj_set_width(s_ph_title, W - 80);
    lv_label_set_long_mode(s_ph_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ph_title, X, 60);
    s_ph_sub = label(s_phone, &ui_font_20, CAPTION);
    lv_obj_set_width(s_ph_sub, W);
    lv_label_set_long_mode(s_ph_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ph_sub, X, 124);
    lv_obj_t *track = lv_obj_create(s_phone);   // the scrubber with the times at its ends
    lv_obj_remove_style_all(track);
    lv_obj_set_style_bg_color(track, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_set_size(track, W, 4);
    lv_obj_set_pos(track, X, 168);
    s_ph_scrub_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_ph_scrub_fill);
    lv_obj_set_style_bg_color(s_ph_scrub_fill, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_ph_scrub_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ph_scrub_fill, 2, 0);
    lv_obj_set_size(s_ph_scrub_fill, 0, 4);
    s_ph_knob = lv_obj_create(s_phone);
    lv_obj_remove_style_all(s_ph_knob);
    lv_obj_set_style_bg_color(s_ph_knob, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_ph_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ph_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(s_ph_knob, 16, 16);
    lv_obj_set_pos(s_ph_knob, X - 8, 162);
    s_ph_pos = label(s_phone, &ui_font_12, CAPTION);
    lv_obj_set_pos(s_ph_pos, X, 184);
    s_ph_len = label(s_phone, &ui_font_12, CAPTION);
    lv_obj_align(s_ph_len, LV_ALIGN_TOP_RIGHT, -X, 184);
    // volume into the Nano (columns 1-2) | the tuner mute (columns 3-4); the footswitch row along the bottom
    int half = (W - MARGIN) / 2;
    lv_obj_t *l;
    lv_obj_t *vc = label(s_phone, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(vc, 1, 0);
    lv_label_set_text(vc, "VOLUME INTO THE NANO");
    lv_obj_set_pos(vc, X, 222);
    s_ph_vol = fill_bar(s_phone, X, 242, half, "Phone", &s_ph_vol_value);
    lv_slider_set_range(s_ph_vol, -600, 60);   // dB x 10
    lv_obj_add_event_cb(s_ph_vol, on_phone_volume, LV_EVENT_VALUE_CHANGED, NULL);
    static const lv_event_cb_t PH_CB[4] = { on_phone_prev, on_phone_toggle, on_phone_next, on_player_row_out };
    build_transport(&s_ph_tr, s_phone, PH_CB);
    lv_obj_t *mc = label(s_phone, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(mc, 1, 0);
    lv_label_set_text(mc, "WHILE TUNING");
    lv_obj_set_pos(mc, X + half + MARGIN, 222);
    lv_obj_t *mb = box(s_phone, X + half + MARGIN, 242, W - half - MARGIN, 60);
    l = label(mb, &ui_font_16, TEXT);
    lv_label_set_text(l, "Mute the phone while the tuner is on");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
    s_ph_mute = calm_switch(mb);
    lv_obj_align(s_ph_mute, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_add_event_cb(s_ph_mute, on_phone_mute, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_hidden(s_phone, true);
}

static void fmt_time(char *out, size_t n, int s)
{
    if (s < 0) s = 0;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

void ui_set_phone_handler(ui_phone_cb cb)
{
    s_phone_cb = cb;
}

void ui_set_phone(const ui_phone_t *p)
{
    lvgl_port_lock(0);
    s_phone_state = *p;
    char text[96], pos[12], len[12];
    fmt_time(pos, sizeof(pos), p->pos_s);
    fmt_time(len, sizeof(len), p->len_s);
    float k = p->len_s > 0 ? (float)p->pos_s / p->len_s : 0;
    if (k > 1) k = 1;
    // top bar (while the phone is its source)
    if (s_source == 0) {
        set_text(s_phone_title, p->connected ? (p->title[0] ? p->title : "Phone") : "Phone");
        set_color_prop(s_phone_title, LV_STYLE_TEXT_COLOR, p->connected ? TEXT : CAPTION);
        set_text(s_phone_time, p->connected ? (p->title[0] ? pos : "") : "not connected");
        set_text(s_phone_play, p->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        set_color_prop(s_phone_play, LV_STYLE_TEXT_COLOR, p->connected ? TEXT : DIMMED);
        set_hidden(s_phone_track, !p->connected || !p->len_s);
        set_num_prop(s_phone_fill, LV_STYLE_WIDTH, (int)(164 * k));
    }
    // player
    snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", p->device[0] ? p->device : "Phone",
             !p->connected ? "not connected" : p->playing ? "playing" : "paused");
    for (char *c = text; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    set_text(s_ph_caption, text);
    set_text(s_ph_title, p->connected && p->title[0] ? p->title : p->connected ? "Nothing playing" : "No phone");
    set_text(s_ph_sub, p->connected ? p->artist : "Pair the phone with the board \xE2\x80\x93 Settings \xE2\x80\xBA Phone.");
    int w = lv_obj_get_style_width(lv_obj_get_parent(s_ph_scrub_fill), 0);
    set_num_prop(s_ph_scrub_fill, LV_STYLE_WIDTH, (int)(w * k));
    set_num_prop(s_ph_knob, LV_STYLE_X, 24 - 8 + (int)(w * k));
    set_text(s_ph_pos, pos);
    set_text(s_ph_len, len);
    show_transport(&s_ph_tr, p->playing);
    s_ph_playing = p->playing;
    if (!lv_obj_has_state(s_ph_vol, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_ph_vol, (int)lroundf(p->volume_db * 10), LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%s%.1f dB", p->volume_db < 0 ? "\xE2\x88\x92" : "", fabsf(p->volume_db));
        set_text(s_ph_vol_value, text);
    }
    lv_obj_t *const mutes[] = { s_ph_mute, s_set_phone_mute };
    for (int i = 0; i < 2; i++) {
        if (!mutes[i]) continue;
        if (p->mute_on_tuner) lv_obj_add_state(mutes[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(mutes[i], LV_STATE_CHECKED);
    }
    if (s_set_phone_vol && !lv_obj_has_state(s_set_phone_vol, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_set_phone_vol, (int)lroundf(p->volume_db * 10), LV_ANIM_OFF);
        set_text(s_set_phone_vol_value, lv_label_get_text(s_ph_vol_value));
    }
    lvgl_port_unlock();
}


// ---- P4: the looper on the board (the loop strip in place of the chain, its tiles) and the jam tracks (player) ----
// Design: drafts/entwurf-p4-9 A (user, 2026-11-13: "Looper A, Jam A"). Colour only with meaning: red = recording,
// amber = overdub, green = the loop plays, blue = jam tracks; tiles off = dimmed, on = bright.

#define LOOP_RED 0xFF4B4B
#define LOOP_AMBER 0xFFB020
#define LOOP_GREEN 0x45F862
#define LOOP_GREY 0x9AA0A6
#define JAM_BLUE 0x4D8DFF
#define LOOP_SEGS 8            // bar fields at most (longer loops: one field for several bars)
#define STRIP_X 200
#define STRIP_W 640
#define JAM_LIST_W 466

static ui_loop_source_cb s_loop_source;
static ui_jam_name_cb s_jam_names;
static ui_jam_note_cb s_jam_notes;
static ui_net_t s_net;
static lv_obj_t *s_net_caption, *s_net_text, *s_net_switch;
static ui_loop_t s_loop;
static ui_jam_t s_jam_st;
static uint32_t s_loop_sig;
static lv_obj_t *s_loopbar, *s_lb_dot, *s_lb_state, *s_lb_time, *s_lb_seg[LOOP_SEGS], *s_lb_fill[LOOP_SEGS], *s_lb_head;
static lv_obj_t *s_lb_left, *s_lb_right, *s_lb_bars_value, *s_rec_mark;
static lv_obj_t *s_jam_list, *s_jam_caption, *s_jam_title, *s_jam_scrub_fill, *s_jam_knob, *s_jam_pos, *s_jam_len;
static lv_obj_t *s_jam_pause_label, *s_jam_vol, *s_jam_vol_value, *s_jam_level_value, *s_jam_foot;
static int s_jam_list_count = -1, s_jam_list_current = -2, s_jam_list_playing = -1;
static lv_obj_t *s_gr_row;              // the FX editor's gain reduction row (a compressor), see build_gr_row
static void show_gr_row(void);
static uint32_t s_jam_list_sig;          // the names (and the album in view) the list was filled with
static lv_obj_t *s_jam_albums, *s_jam_album_name, *s_jam_album_pos;
static int s_jam_view = -1;              // the album in view: its first track; follows the playing one
static int s_jam_view_current = -2;      // (the current track the view last followed)

// ---- jam tracks in albums (2026-11-27: the Jam Player's folders become albums on the card, p4_jam.c) ----
// A track's name is "Album/Title"; a loose one (in the jam folder itself) just "Title" - those form the album
// "Jam tracks". The list is sorted by album, so an album's tracks stand together. The jam page shows one album at a
// time with arrows to the others (draft C, chosen by the user 2026-11-27: drafts/entwurf-p4-14-alben-*).
static const char *jam_name(int i) { return s_jam_names && i >= 0 && i < s_jam_st.count ? s_jam_names(i) : ""; }
static const char *jam_title(const char *name) { const char *s = strchr(name, '/'); return s ? s + 1 : name; }
static int jam_album_len(const char *name) { const char *s = strchr(name, '/'); return s ? (int)(s - name) : 0; }
static bool jam_same_album(int a, int b)
{
    const char *x = jam_name(a), *y = jam_name(b);
    int n = jam_album_len(x);
    return n == jam_album_len(y) && !strncmp(x, y, (size_t)n);
}
static void jam_album_range(int i, int *first, int *last)   // the album of track i: its first and last track
{
    int a = i, b = i;
    while (a > 0 && jam_same_album(a - 1, i)) a--;
    while (b + 1 < s_jam_st.count && jam_same_album(b + 1, i)) b++;
    *first = a;
    *last = b;
}
static void jam_album_name(int i, char *out, size_t n, bool caption)   // caption: in capitals, as captions are
{
    const char *name = jam_name(i);
    int len = jam_album_len(name);
    if (len) snprintf(out, n, "%.*s", len, name);
    else snprintf(out, n, "Jam tracks");
    if (caption)
        for (char *p = out; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
}
static int jam_album_count(void)
{
    int n = 0;
    for (int i = 0; i < s_jam_st.count; i++) n += i == 0 || !jam_same_album(i - 1, i);
    return n;
}
static uint32_t jam_names_sig(void)   // (a list read again can hold other names with the same count: moved tracks)
{
    uint32_t h = 2166136261u ^ (uint32_t)s_jam_st.count;
    for (int i = 0; i < s_jam_st.count; i++)
        for (const char *p = jam_name(i); *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static uint32_t loop_color(int state)
{
    return state == UI_LOOP_REC ? LOOP_RED : state == UI_LOOP_DUB ? LOOP_AMBER : state == UI_LOOP_PLAY ? LOOP_GREEN : LOOP_GREY;
}

static void fmt_tenths(char *out, size_t n, float s)
{
    int t = s > 0 ? (int)(s * 10) : 0;
    snprintf(out, n, "%d:%02d.%d", t / 600, (t / 10) % 60, t % 10);
}

static void on_loop_bars(lv_event_t *e) { phone_action(UI_LOOP_BARS, !s_loop.bar_lines); }

static void build_loopbar(lv_obj_t *parent)
{
    s_loopbar = box(parent, MARGIN, 162, SCREEN_W - 2 * MARGIN, 72);
    s_lb_dot = lv_obj_create(s_loopbar);
    lv_obj_remove_style_all(s_lb_dot);
    lv_obj_set_size(s_lb_dot, 9, 9);
    lv_obj_set_style_radius(s_lb_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_lb_dot, lv_color_hex(DIMMED), 0);
    lv_obj_set_pos(s_lb_dot, 16, 13);
    s_lb_state = label(s_loopbar, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_lb_state, 1, 0);
    lv_obj_set_pos(s_lb_state, 32, 9);
    s_lb_time = label(s_loopbar, &ui_font_title_18, TEXT);
    lv_obj_set_pos(s_lb_time, 16, 34);
    for (int k = 0; k < LOOP_SEGS; k++) {
        lv_obj_t *sg = lv_obj_create(s_loopbar);
        lv_obj_remove_style_all(sg);
        lv_obj_set_style_bg_color(sg, lv_color_hex(SURF_ON), 0);
        lv_obj_set_style_bg_opa(sg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sg, 7, 0);
        lv_obj_set_style_clip_corner(sg, true, 0);
        lv_obj_set_size(sg, 10, 28);
        lv_obj_set_pos(sg, STRIP_X, 14);
        s_lb_seg[k] = sg;
        lv_obj_t *f = lv_obj_create(sg);
        lv_obj_remove_style_all(f);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_size(f, 0, 28);
        s_lb_fill[k] = f;
    }
    s_lb_head = lv_obj_create(s_loopbar);
    lv_obj_remove_style_all(s_lb_head);
    lv_obj_set_style_bg_color(s_lb_head, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_lb_head, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_lb_head, 1, 0);
    lv_obj_set_size(s_lb_head, 2, 40);
    lv_obj_set_pos(s_lb_head, STRIP_X, 8);
    s_lb_left = label(s_loopbar, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_lb_left, 1, 0);
    lv_obj_set_pos(s_lb_left, STRIP_X, 48);
    s_lb_right = label(s_loopbar, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_lb_right, 1, 0);
    lv_obj_set_width(s_lb_right, STRIP_W);
    lv_obj_set_style_text_align(s_lb_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_lb_right, STRIP_X, 48);
    // the bar lines: a tap switches them on / off
    int fx = STRIP_X + STRIP_W + 16, fw = SCREEN_W - 2 * MARGIN - 2 - fx;
    vline(s_loopbar, fx);
    lv_obj_t *f = seg(s_loopbar, fx + 1, fw - 1, on_loop_bars);
    lv_obj_t *c = label(f, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, "BAR LINES");
    lv_obj_set_pos(c, 14, 8);
    s_lb_bars_value = label(f, &ui_font_title_16, TEXT);
    lv_obj_set_pos(s_lb_bars_value, 14, 32);
    lv_obj_set_hidden(s_loopbar, true);
}

static void show_loopbar(void)
{
    const ui_loop_t *l = &s_loop;
    static const char *const NAMES[] = { "LOOPER", "RECORDING", "PLAYING", "OVERDUB", "STOPPED" };
    int st = l->state >= 0 && l->state <= UI_LOOP_STOP ? l->state : 0;
    uint32_t color = loop_color(st);
    char text[64], a[16], b[16];
    if (!l->ready) snprintf(text, sizeof(text), "LOOPER");
    else snprintf(text, sizeof(text), "PART %c \xC2\xB7 %s%s", 'A' + (l->part & 1), l->paused ? "PAUSED" : NAMES[st],
                  l->next_part >= 0 ? " > " : "");
    if (l->ready && l->next_part >= 0) snprintf(text + strlen(text), sizeof(text) - strlen(text), "%c", 'A' + (l->next_part & 1));
    set_text(s_lb_state, text);
    set_color_prop(s_lb_state, LV_STYLE_TEXT_COLOR, st == UI_LOOP_EMPTY ? CAPTION : color);
    set_dot(s_lb_dot, st != UI_LOOP_EMPTY, color);
    fmt_tenths(a, sizeof(a), l->pos_s);
    fmt_tenths(b, sizeof(b), l->len_s);
    if (!l->ready) snprintf(text, sizeof(text), "No memory");
    else if (st == UI_LOOP_EMPTY) snprintf(text, sizeof(text), "Ready");
    else if (st == UI_LOOP_REC) snprintf(text, sizeof(text), "%s", a);
    else snprintf(text, sizeof(text), "%s  /  %s", a, b);
    set_text(s_lb_time, text);

    // the strip: one field per bar (or a few bars), the part played filled in the state's colour
    int n = l->bars > 0 && st != UI_LOOP_REC ? l->bars : 1, per = (n + LOOP_SEGS - 1) / LOOP_SEGS, fields = (n + per - 1) / per;
    float frac = st == UI_LOOP_EMPTY ? 0 : st == UI_LOOP_REC ? (l->max_s > 0 ? l->pos_s / l->max_s : 0)
               : (l->len_s > 0 ? l->pos_s / l->len_s : 0);
    if (frac > 1) frac = 1;
    if (frac < 0) frac = 0;
    for (int k = 0; k < LOOP_SEGS; k++) {
        set_hidden(s_lb_seg[k], k >= fields);
        if (k >= fields) continue;
        int x0 = k * STRIP_W / fields, w = (k + 1) * STRIP_W / fields - x0 - 4;
        set_num_prop(s_lb_seg[k], LV_STYLE_X, STRIP_X + x0 + 2);
        set_num_prop(s_lb_seg[k], LV_STYLE_WIDTH, w);
        float part = frac * fields - k;
        if (part < 0) part = 0;
        if (part > 1) part = 1;
        set_num_prop(s_lb_fill[k], LV_STYLE_WIDTH, (int)(part * w));
        set_color_prop(s_lb_fill[k], LV_STYLE_BG_COLOR, blend(color, SURF, 0.38f));
    }
    set_hidden(s_lb_head, st == UI_LOOP_EMPTY || st == UI_LOOP_REC || !l->ready);
    set_num_prop(s_lb_head, LV_STYLE_X, STRIP_X + (int)(frac * STRIP_W) - 1);

    if (!l->ready) snprintf(text, sizeof(text), "NOT ENOUGH MEMORY FOR THE LOOPER");
    else if (l->bars > 0 && st != UI_LOOP_REC) snprintf(text, sizeof(text), "%d BAR%s \xC2\xB7 %d BPM", l->bars, l->bars == 1 ? "" : "S", (int)lroundf(l->bpm));
    else if (st == UI_LOOP_EMPTY) snprintf(text, sizeof(text), "FOOTSWITCH 2 RECORDS");
    else if (st == UI_LOOP_REC && l->to_bar_line) snprintf(text, sizeof(text), "GOING ON TO THE BAR LINE");
    else if (st == UI_LOOP_REC && l->bar_lines && l->bpm > 0) snprintf(text, sizeof(text), "ROUNDED TO BARS AT %d BPM", (int)lroundf(l->bpm));
    else snprintf(text, sizeof(text), "FREE LENGTH");
    set_text(s_lb_left, text);
    if (!l->ready || st == UI_LOOP_EMPTY) snprintf(text, sizeof(text), "MAX %d S", (int)l->max_s);
    else if (st == UI_LOOP_REC) snprintf(text, sizeof(text), "%d S LEFT", (int)(l->max_s - l->pos_s));
    else snprintf(text, sizeof(text), "PASS %d \xC2\xB7 MAX %d S", l->passes + 1, (int)l->max_s);
    set_text(s_lb_right, text);
    if (l->bar_lines && l->bpm > 0) snprintf(text, sizeof(text), "On \xC2\xB7 %d BPM", (int)lroundf(l->bpm));
    else snprintf(text, sizeof(text), "%s", l->bar_lines ? "On \xC2\xB7 no tempo" : "Off");
    set_text(s_lb_bars_value, text);
}


// Looper mode on the P4 (after the user, 2026-11-19): top row 1 drums (held: their menu), 2 part A, 3 part B, 4 pause
// (drums and loop); bottom row 5 back, 6 record / overdub, 7 play / stop, 8 undo / redo (held: clear).
// Part A and B are two loops of their own; a tap on the other one switches at the end of the loop that plays.
#define DRUM_ORANGE 0xFF8A3D
#define PART_TEAL 0x2EC4B6

// ---- the player mode (P4, 2026-11-26, after the looper mode): footswitch 1 held - the tiles become the player's
//      switches in fixed places and the signal chain becomes the player's stage. ----
// The player's stage (2026-11-27, user: "take the preset out, put the title and the player in the centre"): in player
// mode the preset row and the bar give their place (y 72-234) to one box - the cover at the left (the record with the
// track's number while there is none), what plays and its time beside it.
// Player draft (2026-11-28, user: "choose a song without playing it" and "the albums on the footswitches"): at the
// right a wheel, drawn as the capture wheel of footswitch I - the album in view with its place as dots, the song the
// switches point at in a frame between the one before and the one after. Green = it plays, blue = selected and not
// playing yet; the song that plays stays at the left the whole time. Nothing here moves by itself: the stage changes
// on a press and once a second (the time line, a few pixels).
#define PB_Y 72
#define PB_H 162
#define PB_STRIP_X 170
#define PB_STRIP_W 404
#define PW_X 614                                        // the wheel
#define PW_W (SCREEN_W - 2 * MARGIN - 2 - PW_X - 16)
#define PW_PIPS 10                                      // album dots at most (more albums: "3 / 14" in their place)
static lv_obj_t *s_playerbar, *s_pb_dot, *s_pb_state, *s_pb_album, *s_pb_title, *s_pb_track, *s_pb_fill, *s_pb_knob;
static lv_obj_t *s_pb_left, *s_pb_right, *s_pb_head, *s_pb_disc, *s_pb_disc_label, *s_pb_disc_text, *s_pb_cover;
static lv_obj_t *s_pw_head, *s_pw_pos, *s_pw_pip[PW_PIPS], *s_pw_row[3], *s_pw_num[3], *s_pw_title[3];
static uint32_t s_player_sig;
// the cover of the jam track that plays (p4_cover.c, through ui_set_cover_source)
static ui_cover_cb s_cover_get;
static uint16_t *s_cover_px;
static lv_image_dsc_t s_cover_dsc;
static bool s_cover_has;
static uint32_t s_cover_seq = UINT32_MAX;

// ---- the song the switches point at (draft B): selected with 5 / 7 (inside its album) and 3 (the next album, round),
//      played with 6; it stays until it plays (or 8 leaves the mode). -1 = none: they point at the song that plays
//      (before any played: the first). ----
static int s_sel = -1;
static char s_sel_note[48];      // tile 6's note: the song it plays

static bool player_listed(void) { return s_source == 1 && s_jam_st.card && s_jam_st.count > 0 && s_jam_names; }
static int player_sel(void)
{
    const ui_jam_t *j = &s_jam_st;
    if (s_sel >= 0 && s_sel < j->count) return s_sel;
    if (j->current >= 0 && j->current < j->count) return j->current;
    return j->count > 0 ? 0 : -1;
}
static bool player_pending(void)   // a song is selected that does not play (yet)
{
    return player_listed() && player_sel() >= 0 && player_sel() != s_jam_st.current;
}
static int jam_album_index(int i)   // the place of track i's album, 0-based
{
    int k = -1;
    for (int t = 0; t <= i && t < s_jam_st.count; t++) k += t == 0 || !jam_same_album(t - 1, t);
    return k;
}
// The album before / after the selection's: its first song - or the song that plays, back in the album it plays in.
static int player_album_target(int step)
{
    const int n = s_jam_st.count, cur = s_jam_st.current;
    int sel = player_sel(), first, last;
    if (n <= 0 || sel < 0) return -1;
    jam_album_range(sel, &first, &last);
    int to;
    if (step > 0) to = last + 1 < n ? last + 1 : 0;   // (round: after the last album the first)
    else {
        jam_album_range(first > 0 ? first - 1 : n - 1, &first, &last);
        to = first;
    }
    if (cur >= 0 && cur < n && jam_same_album(to, cur)) to = cur;
    return to;
}
static void player_select(int i)
{
    if (i < 0) return;
    s_sel = i == s_jam_st.current ? -1 : i;
    s_jam_view = i;   // (the jam page shows the selection's album)
}
static void player_song_step(int step)
{
    int sel = player_sel(), first, last;
    if (sel < 0) return;
    jam_album_range(sel, &first, &last);
    if (sel + step >= first && sel + step <= last) player_select(sel + step);   // (inside the album: at its ends it stays)
}
// The selection after a look at the player: gone when it plays now or the list changed - else it stays (draft B: no
// time limit; a song that ends and the album's next that follows do not move it)
static void player_check_sel(void)
{
    if (s_sel < 0) return;
    if (s_sel == s_jam_st.current || s_sel >= s_jam_st.count || s_source != 1) s_sel = -1;
}
static uint32_t player_sig(void)   // what the player's tiles are drawn from
{
    bool pl = s_source == 1 ? s_jam_st.playing : s_phone_state.playing;
    return ((uint32_t)s_source | (uint32_t)pl << 1 | (uint32_t)player_pending() << 2 | (uint32_t)s_jam_st.card << 3
            | (uint32_t)(player_sel() + 2) << 4) ^ (uint32_t)(s_jam_st.current + 2) * 2654435761u ^ (uint32_t)s_jam_st.count * 0x85EBCA6Bu;
}

static lv_obj_t *pb_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *pb_ring(lv_obj_t *parent, int cx, int cy, int d, uint32_t fill, uint32_t line)
{
    lv_obj_t *o = pb_plain(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (line) {
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(line), 0);
    }
    return o;
}

static void build_playerbar(lv_obj_t *parent)
{
    s_playerbar = box(parent, MARGIN, PB_Y, SCREEN_W - 2 * MARGIN, PB_H);
    // the record while there is no cover: black with three grooves, the label in the source tile's teal
    s_pb_disc = pb_plain(s_playerbar);
    lv_obj_set_size(s_pb_disc, 130, 130);
    lv_obj_set_pos(s_pb_disc, 15, PB_H / 2 - 66);
    pb_ring(s_pb_disc, 65, 65, 130, 0x08090A, 0x2A2F35);
    pb_ring(s_pb_disc, 65, 65, 108, 0x08090A, 0x24282D);
    pb_ring(s_pb_disc, 65, 65, 86, 0x08090A, 0x24282D);
    s_pb_disc_label = pb_ring(s_pb_disc, 65, 65, 58, 0x2EC4B6, 0);
    s_pb_disc_text = label(s_pb_disc_label, &ui_font_title_22, INK);
    lv_obj_align(s_pb_disc_text, LV_ALIGN_CENTER, 0, -6);
    pb_ring(s_pb_disc_label, 29, 43, 5, 0x08090A, 0);   // (the spindle hole, below the number)
    // the cover, square with round corners, where the record is (drawn from its own pixels: never scaled)
    s_cover_px = lv_malloc(UI_COVER_PX * UI_COVER_PX * 2);
    s_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cover_dsc.header.w = UI_COVER_PX;
    s_cover_dsc.header.h = UI_COVER_PX;
    s_cover_dsc.header.stride = UI_COVER_PX * 2;
    s_cover_dsc.data_size = UI_COVER_PX * UI_COVER_PX * 2;
    s_cover_dsc.data = (const uint8_t *)s_cover_px;
    s_pb_cover = lv_image_create(s_playerbar);
    lv_obj_set_pos(s_pb_cover, 16, (PB_H - 2 - UI_COVER_PX) / 2);
    lv_obj_set_size(s_pb_cover, UI_COVER_PX, UI_COVER_PX);
    lv_obj_set_style_radius(s_pb_cover, 10, 0);
    lv_obj_set_style_clip_corner(s_pb_cover, true, 0);
    if (s_cover_px) lv_image_set_src(s_pb_cover, &s_cover_dsc);
    lv_obj_set_hidden(s_pb_cover, true);
    // the state line: a dot, NOW PLAYING / PAUSED in the state's colour, the album and the place in it in grey
    s_pb_head = pb_plain(s_playerbar);
    lv_obj_set_size(s_pb_head, PB_STRIP_W, 20);
    lv_obj_set_flex_flow(s_pb_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pb_head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_pb_head, 8, 0);
    lv_obj_set_pos(s_pb_head, PB_STRIP_X, 14);
    s_pb_dot = pb_plain(s_pb_head);
    lv_obj_set_size(s_pb_dot, 9, 9);
    lv_obj_set_style_radius(s_pb_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_pb_dot, lv_color_hex(DIMMED), 0);
    s_pb_state = label(s_pb_head, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_pb_state, 1, 0);
    s_pb_album = label(s_pb_head, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_pb_album, 1, 0);
    lv_label_set_long_mode(s_pb_album, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_pb_album, 1);
    // the title large; the time as a thin line with a knob at the place, the times below its ends
    s_pb_title = label(s_playerbar, &ui_font_title_40, TEXT);
    lv_label_set_long_mode(s_pb_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_pb_title, PB_STRIP_W);
    lv_obj_set_pos(s_pb_title, PB_STRIP_X, 38);
    s_pb_track = pb_plain(s_playerbar);
    lv_obj_set_style_bg_color(s_pb_track, lv_color_hex(SURF_ON), 0);
    lv_obj_set_style_bg_opa(s_pb_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pb_track, 2, 0);
    lv_obj_set_style_clip_corner(s_pb_track, true, 0);
    lv_obj_set_size(s_pb_track, PB_STRIP_W, 4);
    lv_obj_set_pos(s_pb_track, PB_STRIP_X, 106);
    s_pb_fill = pb_plain(s_pb_track);
    lv_obj_set_style_bg_opa(s_pb_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pb_fill, 2, 0);
    lv_obj_set_size(s_pb_fill, 0, 4);
    s_pb_knob = pb_plain(s_playerbar);
    lv_obj_set_size(s_pb_knob, 12, 12);
    lv_obj_set_style_radius(s_pb_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pb_knob, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_pb_knob, PB_STRIP_X - 6, 102);
    s_pb_left = label(s_playerbar, &ui_font_14, CAPTION);
    lv_obj_set_pos(s_pb_left, PB_STRIP_X, 120);
    s_pb_right = label(s_playerbar, &ui_font_14, CAPTION);
    lv_obj_set_width(s_pb_right, PB_STRIP_W);
    lv_obj_set_style_text_align(s_pb_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_pb_right, PB_STRIP_X, 120);
    // the wheel at the right: the album in view and its dots, the song before, the one pointed at, the one after
    vline(s_playerbar, PW_X - 20);
    s_pw_head = label(s_playerbar, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_pw_head, 1, 0);
    lv_obj_set_size(s_pw_head, PW_W - 28 - PW_PIPS * 14, lv_font_get_line_height(&ui_font_12));
    lv_label_set_long_mode(s_pw_head, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_pw_head, PW_X + 14, 16);
    s_pw_pos = label(s_playerbar, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_pw_pos, 1, 0);
    lv_obj_set_width(s_pw_pos, 120);
    lv_obj_set_style_text_align(s_pw_pos, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_pw_pos, PW_X + PW_W - 14 - 120, 16);
    for (int k = 0; k < PW_PIPS; k++) {
        s_pw_pip[k] = pb_plain(s_playerbar);
        lv_obj_set_size(s_pw_pip[k], 8, 8);
        lv_obj_set_style_radius(s_pw_pip[k], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_pw_pip[k], 1, 0);
        lv_obj_set_hidden(s_pw_pip[k], true);
    }
    static const int RY[3] = { 38, 68, 116 }, RH[3] = { 28, 46, 28 };
    for (int r = 0; r < 3; r++) {
        lv_obj_t *row = s_pw_row[r] = pb_plain(s_playerbar);
        lv_obj_set_pos(row, PW_X, RY[r]);
        lv_obj_set_size(row, PW_W, RH[r]);
        lv_obj_set_style_radius(row, 10, 0);
        if (r == 1) {
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        s_pw_num[r] = label(row, r == 1 ? &ui_font_title_16 : &ui_font_12, CAPTION);
        lv_obj_set_width(s_pw_num[r], 24);
        lv_obj_align(s_pw_num[r], LV_ALIGN_LEFT_MID, 14, 0);
        s_pw_title[r] = label(row, r == 1 ? &ui_font_title_22 : &ui_font_16, r == 1 ? TEXT : CAPTION);
        lv_obj_set_width(s_pw_title[r], PW_W - 46 - 14);
        lv_label_set_long_mode(s_pw_title[r], LV_LABEL_LONG_DOT);
        lv_obj_align(s_pw_title[r], LV_ALIGN_LEFT_MID, 46 - (r == 1 ? 2 : 0), 0);
    }
    lv_obj_set_hidden(s_playerbar, true);
}

// A new cover (its number moved): copied into the screen's own picture, which is then drawn again.
static void cover_refresh(uint32_t seq)
{
    if (seq == s_cover_seq) return;
    s_cover_seq = seq;
    s_cover_has = s_cover_px && s_cover_get && s_cover_get(s_cover_px, NULL);
    if (s_pb_cover) lv_obj_invalidate(s_pb_cover);   // (no image cache here: CONFIG_LV_CACHE_DEF_SIZE=0, drawn from the data)
}

void ui_set_cover_source(ui_cover_cb get)
{
    lvgl_port_lock(0);
    s_cover_get = get;
    s_cover_seq = UINT32_MAX;   // (read it at the next look)
    lvgl_port_unlock();
}

// The wheel: the songs around the one the switches point at, inside its album. The frame is green while that song
// is the one that plays (6 pauses it), blue while it is only selected (6 plays it).
static void show_wheel(void)
{
    const ui_jam_t *j = &s_jam_st;
    const ui_phone_t *ph = &s_phone_state;
    char text[96];
    int sel = player_listed() ? player_sel() : -1;
    bool pending = sel >= 0 && player_pending();
    uint32_t accent = sel < 0 ? DIMMED : pending ? SEL_BLUE : GREEN;
    set_color_prop(s_pw_row[1], LV_STYLE_BORDER_COLOR, sel < 0 ? HAIRLINE : blend(accent, SURF, 0.85f));
    set_color_prop(s_pw_row[1], LV_STYLE_BG_COLOR, sel < 0 ? SURF : blend(accent, SURF, 0.10f));
    if (sel < 0) {   // the phone (it has no list), or no tracks on the card
        bool jam = s_source == 1;
        snprintf(text, sizeof(text), "%s", jam ? "JAM TRACKS" : "PHONE");
        set_text(s_pw_head, text);
        set_color_prop(s_pw_head, LV_STYLE_TEXT_COLOR, CAPTION);
        set_hidden(s_pw_pos, true);
        for (int k = 0; k < PW_PIPS; k++) set_hidden(s_pw_pip[k], true);
        for (int r = 0; r < 3; r++) {
            set_text(s_pw_num[r], r == 1 && !jam ? LV_SYMBOL_BLUETOOTH : "");
            set_text(s_pw_title[r], "");
        }
        set_text(s_pw_title[1], jam ? (!j->card ? "No card" : "No tracks on the card")
                                    : ph->connected ? (ph->artist[0] ? ph->artist : ph->device) : "Not connected");
        set_color_prop(s_pw_title[1], LV_STYLE_TEXT_COLOR, CAPTION);
        return;
    }
    int first, last;
    jam_album_range(sel, &first, &last);
    char album[72];
    jam_album_name(sel, album, sizeof(album), true);
    if (pending) snprintf(text, sizeof(text), "SELECTED \xC2\xB7 %s", album);
    else snprintf(text, sizeof(text), "%s", album);
    set_text(s_pw_head, text);
    set_color_prop(s_pw_head, LV_STYLE_TEXT_COLOR, pending ? SEL_BLUE : CAPTION);
    // the albums as dots at the right: the one in view filled, the one that plays ringed in green
    int albums = jam_album_count(), in_view = jam_album_index(sel), playing_in = j->current >= 0 ? jam_album_index(j->current) : -1;
    bool dots = albums > 1 && albums <= PW_PIPS;
    for (int k = 0; k < PW_PIPS; k++) {
        set_hidden(s_pw_pip[k], !dots || k >= albums);
        if (!dots || k >= albums) continue;
        set_num_prop(s_pw_pip[k], LV_STYLE_X, PW_X + PW_W - 14 - (albums - k) * 14 + 6);
        set_num_prop(s_pw_pip[k], LV_STYLE_Y, 18);
        bool on = k == in_view;
        uint32_t c = on ? (pending ? SEL_BLUE : TEXT) : k == playing_in ? GREEN : DIMMED;
        set_color_prop(s_pw_pip[k], LV_STYLE_BORDER_COLOR, c);
        set_color_prop(s_pw_pip[k], LV_STYLE_BG_COLOR, c);
        set_num_prop(s_pw_pip[k], LV_STYLE_BG_OPA, on ? LV_OPA_COVER : LV_OPA_TRANSP);
    }
    set_hidden(s_pw_pos, albums <= PW_PIPS);
    if (albums > PW_PIPS) {
        snprintf(text, sizeof(text), "%d / %d", in_view + 1, albums);
        set_text(s_pw_pos, text);
    }
    for (int r = 0; r < 3; r++) {
        int i = sel - 1 + r;
        bool in = i >= first && i <= last, plays = in && i == j->current;
        if (plays) snprintf(text, sizeof(text), "%s", j->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        else if (in) snprintf(text, sizeof(text), "%d", i - first + 1);
        else text[0] = 0;
        set_text(s_pw_num[r], text);
        set_color_prop(s_pw_num[r], LV_STYLE_TEXT_COLOR, plays ? GREEN : r == 1 && pending ? SEL_BLUE : CAPTION);
        set_text(s_pw_title[r], in ? jam_title(jam_name(i)) : r == 2 ? "End of the album" : "");
        set_color_prop(s_pw_title[r], LV_STYLE_TEXT_COLOR, r == 1 ? TEXT : in ? CAPTION : DIMMED);
    }
}

static void show_playerbar(void)
{
    bool jam = s_source == 1;
    const ui_jam_t *j = &s_jam_st;
    const ui_phone_t *ph = &s_phone_state;
    bool playing = jam ? j->playing : ph->playing;
    int pos = jam ? j->pos_s : ph->pos_s, len = jam ? j->len_s : ph->len_s;
    char text[96], a[12], b[12];
    bool listed = jam && j->card && j->count && j->current >= 0 && j->current < j->count && s_jam_names;
    const char *state = jam ? (!j->card ? (j->unreadable ? "CARD NOT READABLE" : "NO CARD") : !listed ? "NOT PLAYING" : playing ? "NOW PLAYING" : "PAUSED")
                            : !ph->connected ? "NOT CONNECTED" : playing ? "NOW PLAYING" : "PAUSED";
    set_text(s_pb_state, state);
    if (listed) {
        int first, last;
        char album[72];
        jam_album_range(j->current, &first, &last);
        jam_album_name(j->current, album, sizeof(album), true);
        snprintf(text, sizeof(text), "\xC2\xB7  %.28s  \xC2\xB7  %d / %d", album, j->current - first + 1, last - first + 1);
    } else if (jam) snprintf(text, sizeof(text), j->card ? "\xC2\xB7  %d TRACKS" : "", j->count);
    else snprintf(text, sizeof(text), "\xC2\xB7  PHONE");
    set_text(s_pb_album, text);
    if (jam) set_text(s_pb_title, listed ? jam_title(jam_name(j->current)) : j->card && j->count ? "Nothing playing" : "No track");
    else set_text(s_pb_title, ph->connected && ph->title[0] ? ph->title : ph->connected ? (ph->device[0] ? ph->device : "Phone") : "Connect the phone");
    set_color_prop(s_pb_title, LV_STYLE_TEXT_COLOR, jam && !listed ? CAPTION : TEXT);
    uint32_t color = playing ? GREEN : 0x9AA0A6;
    set_color_prop(s_pb_state, LV_STYLE_TEXT_COLOR, playing ? color : CAPTION);
    set_dot(s_pb_dot, playing, color);
    float k = len > 0 ? (float)pos / len : 0;
    if (k > 1) k = 1;
    set_num_prop(s_pb_fill, LV_STYLE_WIDTH, (int)(k * PB_STRIP_W));
    set_color_prop(s_pb_fill, LV_STYLE_BG_COLOR, playing ? GREEN : DIMMED);
    set_hidden(s_pb_knob, len <= 0);
    set_num_prop(s_pb_knob, LV_STYLE_X, PB_STRIP_X - 6 + (int)(k * PB_STRIP_W));
    set_color_prop(s_pb_knob, LV_STYLE_BG_COLOR, playing ? GREEN : 0x9AA0A6);
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), len);
    set_text(s_pb_left, len > 0 ? a : "");
    set_text(s_pb_right, len > 0 ? b : "");
    // the cover (dimmed while paused), else the record: its label teal while playing, grey while paused, with the
    // track's number or the phone's mark
    bool cover = listed && s_cover_has;
    set_hidden(s_pb_cover, !cover);
    set_hidden(s_pb_disc, cover);
    if (cover) set_num_prop(s_pb_cover, LV_STYLE_IMAGE_OPA, playing ? LV_OPA_COVER : 110);
    else {
        set_color_prop(s_pb_disc_label, LV_STYLE_BG_COLOR, !playing ? 0x3A3F45 : 0x2EC4B6);
        int first = 0, last = 0;   // (its number in the album, as the state line and the wheel count)
        if (listed) jam_album_range(j->current, &first, &last);
        if (listed) snprintf(text, sizeof(text), "%d", j->current - first + 1);
        else snprintf(text, sizeof(text), "%s", jam ? "\xE2\x80\x93" : LV_SYMBOL_BLUETOOTH);
        set_text(s_pb_disc_text, text);
        set_color_prop(s_pb_disc_text, LV_STYLE_TEXT_COLOR, playing ? INK : TEXT);
    }
    show_wheel();
}

// The player's switches (draft B): top row 1 jam tracks / phone, 2 tuner, 3 the next album (round), 4 the looper (as
// before); bottom row 5 / 7 the song before / after (selected, not played; the phone: skip at once), 6 plays the
// selected song - or pauses / plays the one that plays -, 8 back out.
static void show_player_tiles(void)
{
    bool jam = s_source == 1, listed = player_listed();
    bool playing = jam ? s_jam_st.playing : s_phone_state.playing;
    bool pending = player_pending();
    set_tile(0, "SOURCE", jam ? "Jam tracks" : "Phone", 0x2EC4B6, true, false, NULL, jam ? LV_SYMBOL_SD_CARD : LV_SYMBOL_BLUETOOTH);
    s_tile_note[0] = jam ? "Tap: phone" : "Tap: jam tracks";
    set_tile(1, "LIST", jam ? "Songs" : "Phone", 0x9AA0A6, false, false, NULL, LV_SYMBOL_LIST);   // (2026-11-28: the list, not the tuner)
    static char album_note[48];
    bool albums = listed && jam_album_count() > 1;
    set_tile(2, "ALBUM", "Album", 0x9AA0A6, false, !albums, NULL, LV_SYMBOL_RIGHT);
    if (albums) {   // (the note: the album a press goes to - after the last one the first)
        jam_album_name(player_album_target(1), album_note, sizeof(album_note), false);
        s_tile_note[2] = album_note;
    }
    set_tile(3, "LOOPER", "Looper", 0x9AA0A6, false, false, NULL, LV_SYMBOL_LOOP);
    s_tile_note[3] = "Loop over it";
    set_tile(4, jam ? "SONG" : "BACK", jam ? "Previous" : "Back", 0x9AA0A6, false, false, NULL, jam ? LV_SYMBOL_UP : LV_SYMBOL_PREV);
    if (pending) {   // 6 plays the selected song: blue, not lit (it does not play yet), its title as the note
        snprintf(s_sel_note, sizeof(s_sel_note), "%s", jam_title(jam_name(player_sel())));
        set_tile(5, "PLAY", "Play", SEL_BLUE, false, false, NULL, LV_SYMBOL_PLAY);
        s_tile_note[5] = s_sel_note;
    } else {
        set_tile(5, "PLAY / PAUSE", playing ? "Pause" : "Play", 0x45F862, playing, false, NULL, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    set_tile(6, jam ? "SONG" : "NEXT", "Next", 0x9AA0A6, false, false, NULL, jam ? LV_SYMBOL_DOWN : LV_SYMBOL_NEXT);
    set_tile(7, "MODE", "Player", 0x8A9099, false, false, NULL, LV_SYMBOL_AUDIO);
    s_tile_note[7] = "Press: back";
}

static void show_onboard_looper_tiles(void)
{
    const ui_loop_t *l = &s_loop;
    static char drum_cap[32], part_note[2][24];
    if (l->bpm > 0) snprintf(drum_cap, sizeof(drum_cap), "DRUMS \xC2\xB7 %d BPM", (int)lroundf(l->bpm));
    else snprintf(drum_cap, sizeof(drum_cap), "DRUMS");
    const char *groove = l->drums_ready && l->groove >= 0 && l->groove < DRUM_GROOVE_COUNT ? DRUM_GROOVES[l->groove].name : "Drums";
    set_tile(0, drum_cap, !l->drums_ready ? "Making the kit" : l->drums_on ? groove : "Drums off", DRUM_ORANGE, l->drums_on,
             !l->drums_ready, NULL, LV_SYMBOL_AUDIO);
    s_tile_note[0] = l->drums_on ? "Hold: drums" : groove;
    for (int part = 0; part < 2; part++) {
        bool active = l->part == part, waiting = l->next_part == part, empty = l->part_state[part] == UI_LOOP_EMPTY;
        bool taking = l->part_state[part] == UI_LOOP_REC;   // its first take is being recorded
        if (waiting) snprintf(part_note[part], sizeof(part_note[part]), "At the loop's end");
        else if (taking) snprintf(part_note[part], sizeof(part_note[part]), "First take");
        else if (empty) snprintf(part_note[part], sizeof(part_note[part]), active ? "Record it" : "Tap: record %c", 'A' + part);
        else snprintf(part_note[part], sizeof(part_note[part]), "%.1f s", l->part_len_s[part]);
        set_tile(1 + part, part ? "PART B" : "PART A", empty ? "Empty" : taking ? "Recording" : part ? "Loop B" : "Loop A", PART_TEAL, active && !empty,
                 !l->ready, NULL, part ? "B" : "A");
        s_tile_note[1 + part] = part_note[part];
    }
    set_tile(3, "PAUSE", l->paused ? "Go on" : "Pause", LOOP_GREY, l->paused, !l->ready, NULL, l->paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    s_tile_note[3] = "Drums + loop";

    set_tile(7, "MODE", "Looper", LOOP_GREY, false, false, NULL, LV_SYMBOL_LOOP);
    s_tile_note[7] = "Press: back";
    bool rec = l->state == UI_LOOP_REC, dub = l->state == UI_LOOP_DUB, first = l->state == UI_LOOP_EMPTY || rec;
    set_tile(5, "REC / DUB", !l->ready ? "No memory" : first ? (rec ? "Recording" : "Record") : "Overdub",
             first ? LOOP_RED : LOOP_AMBER, rec || dub, !l->ready, NULL, " ");
    if (rec && l->to_bar_line) s_tile_note[5] = "To the bar line";
    if (l->counting) {
        set_tile(5, "REC / DUB", "Counting in", LOOP_RED, true, false, NULL, " ");
        s_tile_note[5] = l->count_in ? "One bar" : "To the bar line";
    } else if (l->armed) {
        set_tile(5, "REC / DUB", "Armed", LOOP_RED, false, false, NULL, " ");
        s_tile_note[5] = "Play to start";
    } else if (first && l->arm) {
        s_tile_note[5] = "Arms: the first note";
    }
    if (!s_rec_mark) {   // the record symbol: a ring (filled while it records)
        s_rec_mark = lv_obj_create(s_tile_square[5]);
        lv_obj_remove_style_all(s_rec_mark);
        lv_obj_set_size(s_rec_mark, 20, 20);
        lv_obj_set_style_radius(s_rec_mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_rec_mark, 3, 0);
        lv_obj_center(s_rec_mark);
    }
    set_hidden(s_rec_mark, false);
    set_color_prop(s_rec_mark, LV_STYLE_BORDER_COLOR, s_tile_accent[5]);
    set_color_prop(s_rec_mark, LV_STYLE_BG_COLOR, s_tile_accent[5]);
    set_num_prop(s_rec_mark, LV_STYLE_BG_OPA, rec || dub ? LV_OPA_COVER : LV_OPA_TRANSP);

    // (while the first take records, play / stop fixes its length and plays: "Play")
    bool loop = l->state != UI_LOOP_EMPTY, stopped = l->state == UI_LOOP_STOP || l->state == UI_LOOP_REC || !loop;
    set_tile(6, "PLAY / STOP", stopped ? "Play" : "Stop", LOOP_GREEN, l->state == UI_LOOP_PLAY, !loop, NULL,
             stopped ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP);
    set_tile(4, "UNDO / REDO", l->can_undo || !l->can_redo ? "Undo" : "Redo", LOOP_GREY, false, !loop, NULL, LV_SYMBOL_REFRESH);
    s_tile_note[4] = "Hold: clear";
}

// ---- Level (a tap on OUT in the signal chain): the Nano's loudness and auto level, after the QC board's sheet and the
//      desktop editor's Auto level - play 4 s, the preset's capture volume is set so that it lands on the target of its
//      kind: Clean at the base, Edge +1.5, Drive / Fuzz +3, Lead +3 over Drive, minus the profile's correction; peaks
//      at or under -1 dBFS. The sound on USB 3/4 follows the Nano's LEVEL knob: it is set to 10 for the measurement
//      (asked first when lower), as the editor does. ----
#define LV_KINDS 5
static const char *const LV_KIND_NAME[LV_KINDS] = { "Clean", "Edge", "Drive", "Fuzz", "Lead" };
static const float LV_STEP[LV_KINDS] = { 0, 1.5f, 3, 3, 6 };
static const float LV_CORR[2][LV_KINDS] = { { 0, -0.5f, -0.5f, -0.5f, -0.5f }, { 0, -0.5f, -2, -1, -2 } };   // rehearsal, quiet
#define LV_CHIP 0xE8EAED
static lv_obj_t *s_lvs, *s_lv_sub, *s_lv_short, *s_lv_mom, *s_lv_peak, *s_lv_target, *s_lv_bar, *s_lv_mark;
static lv_obj_t *s_lv_kind[LV_KINDS], *s_lv_prof[2], *s_lv_base, *s_lv_cap_name, *s_lv_cap_val, *s_lv_lvl_val, *s_lv_lvl_note;
static lv_obj_t *s_lv_go, *s_lv_prog, *s_lv_result, *s_lv_undo, *s_lv_count;
static uint32_t s_lv_count_until;      // the countdown before a measurement ends then (lv_tick), 0 = none
#define LV_COUNTDOWN_MS 5000          // time to stand up and start playing (2026-11-27: 4 s straight away was too soon)
#define LV_MEASURE_BLOCKS 80          // 100 ms blocks of played sound (8 s) - p4_audio.c LV_MEASURE
static int s_lv_shown_key = -1;          // kind / profile / base as last drawn
static bool s_lv_waiting;                // a measurement runs, its result is to be applied
static unsigned s_lv_seen_seq;
static bool s_lv_level_asked;            // the LEVEL knob's question was shown once
static int s_lv_undo_cap = -1, s_lv_undo_lvl = -1;
#define LV_BAR_W 288

static float lv_target(void)
{
    int k = s_loop.lv_kind >= 0 && s_loop.lv_kind < LV_KINDS ? s_loop.lv_kind : 0, p = s_loop.lv_profile ? 1 : 0;
    return s_loop.lv_base_x10 / 10.0f + LV_STEP[k] + LV_CORR[p][k];
}
static void lv_send_settings(void)
{
    phone_action(UI_LEVEL_SET, s_loop.lv_kind | s_loop.lv_profile << 4 | (s_loop.lv_base_x10 + 1000) << 8);
}
static void lv_chip_look(lv_obj_t *b, bool on)
{
    set_color_prop(b, LV_STYLE_BG_COLOR, on ? LV_CHIP : TILE_OFF_BG);
    set_color_prop(b, LV_STYLE_BORDER_COLOR, on ? LV_CHIP : 0x2E3237);
    set_color_prop(lv_obj_get_child(b, 0), LV_STYLE_TEXT_COLOR, on ? INK : TEXT);
}
static void lv_number(lv_obj_t *l, float v, const char *unit)
{
    char t[24];
    if (v < -70) snprintf(t, sizeof(t), "\xE2\x80\x93");
    else snprintf(t, sizeof(t), "%s%.1f%s", v < -0.05f ? "\xE2\x88\x92" : "", fabsf(v), unit);
    set_text(l, t);
}

static void lv_apply_result(void)
{
    const ui_loop_t *l = &s_loop;
    if (!l->lv_result_ok) { set_text(s_lv_result, "Nothing measured - play a little louder."); return; }
    float target = lv_target(), delta = target - l->lv_result_lufs;
    char note[96] = "", text[320];
    if (l->lv_result_peak_db + delta > -1) {
        delta = -1 - l->lv_result_peak_db;
        snprintf(note, sizeof(note), " Kept lower so that the peaks stay under -1 dBFS.");
    }
    float now = nano_capture_volume_db(s_capture_volume), want = now + delta, next = want < -24 ? -24 : want > 12 ? 12 : want;
    if (next != want) strlcat(note, next == 12 ? " The capture volume is at its maximum." : " The capture volume is at its minimum.", sizeof(note));
    if (fabsf(delta) < 0.25f) {
        snprintf(text, sizeof(text), "Measured %.1f LUFS (peaks %.1f dBFS), target %.1f: it fits already.", l->lv_result_lufs, l->lv_result_peak_db, target);
    } else {
        int raw = nano_capture_volume_raw(next);
        if (s_lv_undo_cap < 0) s_lv_undo_cap = s_capture_volume;
        s_capture_volume = raw;
        s_volume_tick = lv_tick_get();
        send('v', 0 << 16 | raw);
        snprintf(text, sizeof(text), "Measured %.1f LUFS (peaks %.1f dBFS), target %.1f: capture volume %+.1f " LV_SYMBOL_RIGHT
                 " %+.1f dB.%s Save the preset to keep it.", l->lv_result_lufs, l->lv_result_peak_db, target, now, next, note);
    }
    set_text(s_lv_result, text);
    set_hidden(s_lv_undo, s_lv_undo_cap < 0 && s_lv_undo_lvl < 0);
}

// 10 times a second while it shows (loop_poll)
static void show_level_sheet(void)
{
    const ui_loop_t *l = &s_loop;
    lv_number(s_lv_short, l->lv_short, "");
    lv_number(s_lv_mom, l->lv_mom, "");
    lv_number(s_lv_peak, l->lv_peak_db, " dBFS");
    float k = l->lv_short < -60 ? 0 : l->lv_short > 0 ? 1 : (l->lv_short + 60) / 60;
    set_num_prop(s_lv_bar, LV_STYLE_WIDTH, (int)(k * LV_BAR_W));
    int key = l->lv_kind | l->lv_profile << 4 | (l->lv_base_x10 + 1000) << 8;
    if (key != s_lv_shown_key) {
        s_lv_shown_key = key;
        for (int i = 0; i < LV_KINDS; i++) lv_chip_look(s_lv_kind[i], l->lv_kind == i);
        for (int i = 0; i < 2; i++) lv_chip_look(s_lv_prof[i], l->lv_profile == i);
        char t[24];
        snprintf(t, sizeof(t), "%s%.0f LUFS", l->lv_base_x10 < 0 ? "\xE2\x88\x92" : "", fabsf(l->lv_base_x10 / 10.0f));
        set_text(s_lv_base, t);
        float target = lv_target();
        lv_number(s_lv_target, target, "");
        float tk = (target + 60) / 60;
        set_num_prop(s_lv_mark, LV_STYLE_X, 16 + (int)(tk * LV_BAR_W));
    }
    char t[64];
    const char *name = s_current_preset >= 1 && s_current_preset <= NANO_PRESETS ? s_preset_names[s_current_preset - 1] : "";
    snprintf(t, sizeof(t), "%d \xC2\xB7 %s", s_current_preset, name[0] ? name : "-");
    set_text(s_lv_sub, t);
    set_text(s_lv_cap_name, s_capture ? lv_label_get_text(s_capture) : "");
    snprintf(t, sizeof(t), "%+.1f dB", nano_capture_volume_db(s_capture_volume));
    set_text(s_lv_cap_val, t);
    float lvl = s_amp_values[NANO_AMP_LEVEL] / 25.5f;
    bool full = s_amp_values[NANO_AMP_LEVEL] >= 250;
    snprintf(t, sizeof(t), "%.1f", lvl);
    set_text(s_lv_lvl_val, t);
    set_color_prop(s_lv_lvl_val, LV_STYLE_TEXT_COLOR, full ? TEXT : 0xFFB020);
    set_text(s_lv_lvl_note, full ? "The sound on USB follows it" : "Below 10 - Measure sets it to 10 first");
    int m = l->lv_measuring;
    if (s_lv_count_until) {   // the countdown: the seconds large, then the measurement starts by itself
        int32_t left = (int32_t)(s_lv_count_until - lv_tick_get());
        if (left <= 0) {
            s_lv_count_until = 0;
            s_lv_waiting = true;
            s_lv_seen_seq = l->lv_result_seq;
            phone_action(UI_LEVEL_MEASURE, 1);
            set_text(s_lv_result, "Play now - as you would on stage.");
        } else {
            char c[16];
            snprintf(c, sizeof(c), "%d", (int)((left + 999) / 1000));
            set_text(s_lv_count, c);
            set_color_prop(s_lv_count, LV_STYLE_TEXT_COLOR, 0xFFB020);
        }
    }
    if (s_lv_waiting && m >= 0) {   // measuring: the seconds of played sound still wanted
        char c[16];
        snprintf(c, sizeof(c), "%d", (LV_MEASURE_BLOCKS - m + 9) / 10);
        set_text(s_lv_count, c);
        set_color_prop(s_lv_count, LV_STYLE_TEXT_COLOR, GREEN);
    }
    set_hidden(s_lv_count, !s_lv_count_until && !s_lv_waiting);
    set_text(lv_obj_get_child(s_lv_go, 0), s_lv_count_until || s_lv_waiting ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Measure");
    set_num_prop(s_lv_prog, LV_STYLE_WIDTH, m < 0 ? (s_lv_waiting ? 0 : lv_obj_get_style_width(s_lv_prog, 0)) : m * 300 / LV_MEASURE_BLOCKS);
    if (s_lv_waiting && m < 0 && l->lv_result_seq != s_lv_seen_seq) {
        s_lv_waiting = false;
        s_lv_seen_seq = l->lv_result_seq;
        set_num_prop(s_lv_prog, LV_STYLE_WIDTH, 300);
        lv_apply_result();
    }
}

static void on_lv_kind(lv_event_t *e) { s_loop.lv_kind = (int)(intptr_t)lv_event_get_user_data(e); lv_send_settings(); show_level_sheet(); }
static void on_lv_prof(lv_event_t *e) { s_loop.lv_profile = (int)(intptr_t)lv_event_get_user_data(e); lv_send_settings(); show_level_sheet(); }
static void on_lv_base(lv_event_t *e)
{
    int b = s_loop.lv_base_x10 + ((int)(intptr_t)lv_event_get_user_data(e) ? 10 : -10);
    if (b < -400 || b > -80) return;
    s_loop.lv_base_x10 = b;
    lv_send_settings();
    show_level_sheet();
}
static void on_lv_go(lv_event_t *e)
{
    if (s_lv_waiting || s_lv_count_until) {   // (pressed again while it counts down or measures: stop)
        if (s_lv_waiting) phone_action(UI_LEVEL_MEASURE, 0);
        s_lv_waiting = false;
        s_lv_count_until = 0;
        set_text(s_lv_result, "Stopped.");
        show_level_sheet();
        return;
    }
    if (s_loop.lv_short < -90 && s_loop.lv_mom < -90) {
        set_text(s_lv_result, "No sound from the Nano - it has to be on the USB-A socket.");
        return;
    }
    if (s_amp_values[NANO_AMP_LEVEL] < 250) {
        if (!s_lv_level_asked) {
            s_lv_level_asked = true;
            char t[200];
            snprintf(t, sizeof(t), "The Nano's LEVEL knob is at %.1f. The sound on USB follows it, so the board sets it to 10 to "
                     "measure - the Nano gets louder. Tap Measure again.", s_amp_values[NANO_AMP_LEVEL] / 25.5f);
            set_text(s_lv_result, t);
            return;
        }
        if (s_lv_undo_lvl < 0) s_lv_undo_lvl = s_amp_values[NANO_AMP_LEVEL];
        s_amp_values[NANO_AMP_LEVEL] = 255;
        s_amp_tick = lv_tick_get();
        send('v', 8 << 16 | 255);   // (main.c: SRC_AMP_LEVEL)
    }
    s_lv_count_until = lv_tick_get() + LV_COUNTDOWN_MS;   // (first the countdown: time to stand up and play)
    if (!s_lv_count_until) s_lv_count_until = 1;
    char t[128];
    snprintf(t, sizeof(t), "Get ready: when it reaches 0, play for 8 seconds as you would for %s.", LV_KIND_NAME[s_loop.lv_kind % LV_KINDS]);
    set_text(s_lv_result, t);
    set_hidden(s_lv_undo, true);
    set_num_prop(s_lv_prog, LV_STYLE_WIDTH, 0);
    show_level_sheet();
}
static void on_lv_undo(lv_event_t *e)
{
    if (s_lv_undo_cap >= 0) {
        s_capture_volume = s_lv_undo_cap;
        s_volume_tick = lv_tick_get();
        send('v', 0 << 16 | s_lv_undo_cap);
    }
    if (s_lv_undo_lvl >= 0) {
        s_amp_values[NANO_AMP_LEVEL] = s_lv_undo_lvl;
        s_amp_tick = lv_tick_get();
        send('v', 8 << 16 | s_lv_undo_lvl);
    }
    s_lv_undo_cap = s_lv_undo_lvl = -1;
    set_hidden(s_lv_undo, true);
    set_text(s_lv_result, "Set back.");
}
static void on_lv_close(lv_event_t *e)
{
    (void)e;
    if (s_lv_waiting) phone_action(UI_LEVEL_MEASURE, 0);
    s_lv_waiting = false;
    s_lv_count_until = 0;
    lv_obj_set_hidden(s_lvs, true);
}

static lv_obj_t *lv_caption(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *c = label(parent, CAPTION_FONT, CAPTION);
    lv_label_set_text(c, text);
    lv_obj_set_style_text_letter_space(c, 2, 0);
    lv_obj_set_pos(c, x, y);
    return c;
}
static lv_obj_t *lv_chip(lv_obj_t *parent, int x, int y, int w, int h, const char *text, lv_event_cb_t cb, int arg)
{
    lv_obj_t *b = small_button(parent, x, y, w, h, text, cb, arg);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    return b;
}
static lv_obj_t *lv_line(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 3, 0);
    return o;
}

static void build_level_sheet(lv_obj_t *screen)
{
    s_lvs = dialog_full(screen);
    dialog_title(s_lvs, "Level");
    s_lv_sub = label(s_lvs, HINT_FONT, MUTED);
    lv_obj_set_pos(s_lv_sub, 20, 48);
    close_button(s_lvs, DLG_W - 60, 16, on_lv_close, 0);
    // the meter
    lv_obj_t *m = box(s_lvs, 20, 84, 320, 236);
    lv_caption(m, 16, 12, "SHORT-TERM");
    s_lv_short = label(m, &ui_font_title_54, TEXT);
    lv_obj_set_pos(s_lv_short, 14, 30);
    lv_obj_t *u = label(m, HINT_FONT, MUTED);
    lv_label_set_text(u, "LUFS");
    lv_obj_set_pos(u, 236, 62);
    static const char *const ROWS[3] = { "MOMENTARY", "PEAK", "TARGET" };
    lv_obj_t **vals[3] = { &s_lv_mom, &s_lv_peak, &s_lv_target };
    for (int i = 0; i < 3; i++) {
        lv_caption(m, 16, 112 + i * 32, ROWS[i]);
        *vals[i] = label(m, &ui_font_title_18, i == 2 ? GREEN : TEXT);
        lv_obj_align(*vals[i], LV_ALIGN_TOP_RIGHT, -16, 108 + i * 32);
    }
    lv_obj_t *track = lv_line(m, 16, 212, LV_BAR_W, 6, HAIRLINE);
    s_lv_bar = lv_line(track, 0, 0, 0, 6, TEXT);
    s_lv_mark = lv_line(m, 16, 204, 3, 22, GREEN);
    // kind, profile, base
    int kw = DLG_W - 20 - 352;
    lv_obj_t *k = box(s_lvs, 352, 84, kw, 236);
    lv_caption(k, 16, 12, "THIS PRESET IS");
    for (int i = 0; i < LV_KINDS; i++) s_lv_kind[i] = lv_chip(k, 16 + i * 120, 34, 114, 46, LV_KIND_NAME[i], on_lv_kind, i);
    lv_caption(k, 16, 96, "LISTENING AT");
    static const char *const PROF[2] = { "Rehearsal", "Quiet" };
    for (int i = 0; i < 2; i++) s_lv_prof[i] = lv_chip(k, 16 + i * 160, 118, 152, 46, PROF[i], on_lv_prof, i);
    lv_caption(k, 352, 96, "CLEAN AT");
    lv_chip(k, 352, 118, 46, 46, LV_SYMBOL_MINUS, on_lv_base, 0);
    s_lv_base = label(k, &ui_font_title_18, TEXT);
    lv_obj_set_width(s_lv_base, 112);
    lv_obj_set_style_text_align(s_lv_base, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lv_base, 400, 130);
    lv_chip(k, 514, 118, 46, 46, LV_SYMBOL_PLUS, on_lv_base, 1);
    lv_obj_t *h = label(k, &ui_font_14, MUTED);
    lv_obj_set_width(h, kw - 32);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(h, 16, 180);
    lv_label_set_text(h, "Edge +1.5 \xC2\xB7 Drive +3 \xC2\xB7 Fuzz +3 \xC2\xB7 Lead +3 over Drive, as in the editor and the LUFS Meter.\n"
                         "Rehearsal: the louder kinds -0.5 dB \xC2\xB7 Quiet: Drive and Lead -2, Fuzz -1 (they seem louder).");
    // what auto level turns
    lv_obj_t *c = box(s_lvs, 20, 332, 620, DLG_H - 16 - 332);
    lv_caption(c, 16, 12, "WHAT AUTO LEVEL TURNS");
    lv_obj_t *r1 = box(c, 10, 36, 600, 52);
    lv_caption(r1, 14, 6, "CAPTURE VOLUME \xC2\xB7 SAVED WITH THE PRESET");
    s_lv_cap_name = label(r1, &ui_font_16, TEXT);
    lv_obj_set_pos(s_lv_cap_name, 14, 24);
    lv_obj_set_width(s_lv_cap_name, 400);
    lv_label_set_long_mode(s_lv_cap_name, LV_LABEL_LONG_DOT);
    s_lv_cap_val = label(r1, &ui_font_title_18, TEXT);
    lv_obj_align(s_lv_cap_val, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_t *r2 = box(c, 10, 96, 600, 52);
    lv_caption(r2, 14, 6, "LEVEL \xC2\xB7 THE NANO'S KNOB, FOR ALL PRESETS");
    s_lv_lvl_note = label(r2, &ui_font_16, MUTED);
    lv_label_set_text(s_lv_lvl_note, "The sound on USB follows it");
    lv_obj_set_pos(s_lv_lvl_note, 14, 24);
    s_lv_lvl_val = label(r2, &ui_font_title_18, TEXT);
    lv_obj_align(s_lv_lvl_val, LV_ALIGN_RIGHT_MID, -14, 0);
    // measure
    int gx = 652, gw = DLG_W - 20 - gx;
    lv_obj_t *g = box(s_lvs, gx, 332, gw, DLG_H - 16 - 332);
    s_lv_go = small_button(g, 14, 12, 188, 56, LV_SYMBOL_PLAY "  Measure", on_lv_go, 0);
    primary(s_lv_go);
    lv_obj_set_style_text_font(lv_obj_get_child(s_lv_go, 0), &ui_font_title_18, 0);
    s_lv_undo = small_button(g, 210, 12, gw - 224, 56, LV_SYMBOL_REFRESH "  Undo", on_lv_undo, 0);
    lv_obj_set_hidden(s_lv_undo, true);
    s_lv_count = label(g, &ui_font_title_54, 0xFFB020);   // the countdown / the seconds still to play, beside the button
    lv_obj_set_width(s_lv_count, gw - 224);
    lv_obj_set_style_text_align(s_lv_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lv_count, 210, 4);
    lv_obj_set_hidden(s_lv_count, true);
    lv_obj_t *pt = lv_line(g, 14, 76, 300, 6, HAIRLINE);
    s_lv_prog = lv_line(pt, 0, 0, 0, 6, GREEN);
    s_lv_result = label(g, &ui_font_14, TEXT);
    lv_obj_set_pos(s_lv_result, 14, 86);   // (room for four lines: the question about the LEVEL knob has four)
    lv_obj_set_width(s_lv_result, gw - 28);
    lv_label_set_long_mode(s_lv_result, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lv_result, "Choose what the preset is, then measure while you play.");
    lv_obj_set_hidden(s_lvs, true);
}

static void open_level_sheet(void)
{
    lv_obj_set_hidden(s_lvs, false);
    lv_obj_move_foreground(s_lvs);
    s_lv_shown_key = -1;
    s_lv_level_asked = false;
    if (!s_lv_waiting) {   // (a new start: what the last time said is gone, the undo too)
        s_lv_undo_cap = s_lv_undo_lvl = -1;
        set_hidden(s_lv_undo, true);
        set_num_prop(s_lv_prog, LV_STYLE_WIDTH, 0);
        set_text(s_lv_result, "Choose what the preset is, then measure while you play.");
    }
    show_level_sheet();
}
static void on_level_open(lv_event_t *e) { (void)e; open_level_sheet(); }

// ---- the drum menu (looper mode: the drums' tile or footswitch 1 held) ----
// The groove by style, a looper part's own groove, the kit, the level, and how a part's first take starts; at the
// top right "Save loops" - the looper's parts to the card as WAV files (p4_loops.c, 2026-11-28).
#define DRUM_STYLES 10
#define DRUM_PICK 15                       // grooves shown per style
#define DRUM_CHIP 0xE8EAED                 // the chosen part / kit
static lv_obj_t *s_drums, *s_drum_style[DRUM_STYLES], *s_drum_pick[DRUM_PICK], *s_drum_part[3], *s_drum_kit[3];
static const int DRUM_KIT_OF[3] = { DRUM_KIT_ACOUSTIC, DRUM_KIT_COMPUTED, DRUM_KIT_LOFI };   // the kit buttons, left to right
static lv_obj_t *s_drum_now, *s_drum_level, *s_drum_level_value;
static lv_obj_t *s_drum_switch[4];   // start with the first take, count in, on the bar lines, the first note starts it
static lv_obj_t *s_drum_save;        // "Save loops"
static int s_drum_save_shown = -1;
static const ui_phone_action_t DRUM_SWITCH_ACTION[4] = { UI_DRUM_SYNC, UI_DRUM_COUNT, UI_LOOP_QUANTIZE, UI_LOOP_ARM };
static int s_drum_style_shown = -1, s_drum_groove_shown = -1, s_drum_kit_shown = -1, s_drum_parts_shown = -999;
static int s_drum_pick_groove[DRUM_PICK];  // the groove a button plays (-1 = none)
static int s_drum_edit = -1;               // what a groove tap sets: -1 the common groove (A = B), 0 / 1 part A / B's own
static const char *s_drum_styles[DRUM_STYLES];
static int s_drum_style_count;

static int groove_style(int groove)
{
    for (int i = 0; groove >= 0 && groove < DRUM_GROOVE_COUNT && i < s_drum_style_count; i++)
        if (!strcmp(s_drum_styles[i], DRUM_GROOVES[groove].style)) return i;
    return 0;
}

// The groove the picker marks: the common one, or the edited part's own (else the common one).
static int drum_marked(void)
{
    if (s_drum_edit >= 0 && s_loop.part_groove[s_drum_edit] >= 0) return s_loop.part_groove[s_drum_edit];
    return s_loop.groove;
}
static int drum_parts_key(void) { return (s_drum_edit + 1) | (s_loop.part_groove[0] + 1) << 2 | (s_loop.part_groove[1] + 1) << 10; }

static void drum_chip(lv_obj_t *b, uint32_t color, bool on)
{
    style_frame_button(b, color, on, false);
    set_color_prop(lv_obj_get_child(b, 0), LV_STYLE_TEXT_COLOR, on ? ink_for(color) : TEXT);
}

// The grooves of one style on the buttons; the marked one filled.
static void drum_menu_fill(int style)
{
    s_drum_style_shown = style;
    s_drum_groove_shown = s_loop.groove;
    s_drum_parts_shown = drum_parts_key();
    int marked = drum_marked();
    for (int i = 0; i < s_drum_style_count; i++) drum_chip(s_drum_style[i], DRUM_ORANGE, i == style);
    for (int i = 0; i < 3; i++) {   // A = B (no own grooves), Part A, Part B
        bool on = i == 0 ? s_drum_edit < 0 : s_drum_edit == i - 1;
        drum_chip(s_drum_part[i], DRUM_CHIP, on);
        if (i && !on && s_loop.part_groove[i - 1] >= 0)   // (has its own groove)
            lv_obj_set_style_border_color(s_drum_part[i], lv_color_hex(DRUM_ORANGE), 0);
    }
    int n = 0;
    for (int g = 0; g < DRUM_GROOVE_COUNT && n < DRUM_PICK; g++) {
        if (strcmp(DRUM_GROOVES[g].style, s_drum_styles[style])) continue;
        s_drum_pick_groove[n] = g;
        lv_obj_t *l = lv_obj_get_child(s_drum_pick[n], 0);
        set_text(l, DRUM_GROOVES[g].name);
        set_ptr_prop(l, LV_STYLE_TEXT_FONT, strlen(DRUM_GROOVES[g].name) > 16 ? &ui_font_16 : &ui_font_20);
        drum_chip(s_drum_pick[n], DRUM_ORANGE, g == marked);
        set_hidden(s_drum_pick[n], false);
        n++;
    }
    for (; n < DRUM_PICK; n++) {
        set_hidden(s_drum_pick[n], true);
        s_drum_pick_groove[n] = -1;
    }
    set_text(s_drum_now, marked >= 0 && marked < DRUM_GROOVE_COUNT ? DRUM_GROOVES[marked].name : "");
}

static void on_drum_style(lv_event_t *e) { drum_menu_fill((int)(intptr_t)lv_event_get_user_data(e)); }
static void on_drum_groove(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= DRUM_PICK || s_drum_pick_groove[k] < 0) return;
    if (s_drum_edit < 0) phone_action(UI_DRUM_GROOVE, s_drum_pick_groove[k]);
    else phone_action(UI_PART_GROOVE, s_drum_edit << 8 | (s_drum_pick_groove[k] + 1));
}
static void on_drum_part(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    // (A = B tapped again while a part has its own groove: both back to the common one)
    if (i == 0 && s_drum_edit < 0 && (s_loop.part_groove[0] >= 0 || s_loop.part_groove[1] >= 0)) phone_action(UI_PART_GROOVE, 2 << 8);
    s_drum_edit = i - 1;
    drum_menu_fill(groove_style(drum_marked()));
}
static void on_drum_kit(lv_event_t *e)
{
    phone_action(UI_DRUM_KIT, DRUM_KIT_OF[(int)(intptr_t)lv_event_get_user_data(e)]);
}
static void on_drum_close(lv_event_t *e) { (void)e; lv_obj_set_hidden(s_drums, true); }
static void on_drum_save(lv_event_t *e) { (void)e; phone_action(UI_LOOP_SAVE, 0); }

// "Save loops" as the board has it: to save, saving (percent; another tap does nothing), saved as they are (a tap
// says they are on the card already), nothing to save / no card (dimmed).
static void drum_save_show(void)
{
    int st = s_loop.save_state, key = st * 1000 + (st == UI_SAVE_BUSY ? s_loop.save_pct : 0);
    if (key == s_drum_save_shown) return;
    s_drum_save_shown = key;
    char text[32];
    if (st == UI_SAVE_BUSY) snprintf(text, sizeof(text), "Saving %d %%", s_loop.save_pct);
    else snprintf(text, sizeof(text), "%s", st == UI_SAVE_DONE ? LV_SYMBOL_OK "  Saved" : st == UI_SAVE_NO_CARD ? "No card" : "Save loops");
    lv_obj_t *l = lv_obj_get_child(s_drum_save, 0);
    set_text(l, text);
    set_color_prop(l, LV_STYLE_TEXT_COLOR, st == UI_SAVE_DONE ? GREEN : TEXT);
    if (st == UI_SAVE_NONE || st == UI_SAVE_NO_CARD) lv_obj_add_state(s_drum_save, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_drum_save, LV_STATE_DISABLED);   // (saving: a tap does nothing more)
}
static void on_drum_switch(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    phone_action(DRUM_SWITCH_ACTION[i], lv_obj_has_state(s_drum_switch[i], LV_STATE_CHECKED));
}

static void on_drum_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = !lv_obj_has_state(s_drum_switch[i], LV_STATE_CHECKED);
    if (on) lv_obj_add_state(s_drum_switch[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(s_drum_switch[i], LV_STATE_CHECKED);
    phone_action(DRUM_SWITCH_ACTION[i], on);
}
static void on_drum_level(lv_event_t *e)
{
    int v = lv_slider_get_value(s_drum_level);   // dB
    char text[24];
    snprintf(text, sizeof(text), "%s%d dB", v < 0 ? "\xE2\x88\x92" : v > 0 ? "+" : "", abs(v));
    set_text(s_drum_level_value, text);
    phone_action(lv_event_get_code(e) == LV_EVENT_RELEASED ? UI_DRUM_LEVEL_KEEP : UI_DRUM_LEVEL, v * 10);
}

// While it shows (10 times a second): the grooves, the kit and the switches as the board has them.
static void drum_menu_show(void)
{
    if (s_loop.groove != s_drum_groove_shown || drum_parts_key() != s_drum_parts_shown)
        drum_menu_fill(s_drum_style_shown >= 0 ? s_drum_style_shown : groove_style(drum_marked()));
    int kit = s_loop.drum_kit == DRUM_KIT_ACOUSTIC && !s_loop.acoustic_kit ? DRUM_KIT_COMPUTED : s_loop.drum_kit;
    if (kit != s_drum_kit_shown) {
        s_drum_kit_shown = kit;
        for (int i = 0; i < 3; i++) drum_chip(s_drum_kit[i], DRUM_CHIP, kit == DRUM_KIT_OF[i]);
        if (!s_loop.acoustic_kit) set_color_prop(lv_obj_get_child(s_drum_kit[0], 0), LV_STYLE_TEXT_COLOR, DIMMED);   // (its samples are not on the board)
    }
    drum_save_show();
    const bool want[4] = { s_loop.drum_sync, s_loop.count_in, s_loop.quantize, s_loop.arm };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *sw = s_drum_switch[i];
        if (lv_obj_has_state(sw, LV_STATE_PRESSED) || want[i] == lv_obj_has_state(sw, LV_STATE_CHECKED)) continue;
        if (want[i]) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

void ui_drum_menu_toggle(void)
{
    lvgl_port_lock(0);
    if (!lv_obj_is_hidden(s_drums)) {
        lv_obj_set_hidden(s_drums, true);
    } else {
        lv_obj_set_hidden(s_drums, false);
        lv_obj_move_foreground(s_drums);
        s_drum_kit_shown = -1;
        s_drum_save_shown = -1;
        drum_menu_fill(groove_style(drum_marked()));
        int v = (int)lroundf(s_loop.drum_level_db);
        lv_slider_set_value(s_drum_level, v, LV_ANIM_OFF);
        char text[24];
        snprintf(text, sizeof(text), "%s%d dB", v < 0 ? "\xE2\x88\x92" : v > 0 ? "+" : "", abs(v));
        set_text(s_drum_level_value, text);
        drum_menu_show();
    }
    lvgl_port_unlock();
}

static lv_obj_t *drum_button(int x, int y, int w, int h, const lv_font_t *font, const char *text, lv_event_cb_t cb, int data)
{
    lv_obj_t *b = frame_button(s_drums);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t *l = label(b, font, TEXT);
    lv_obj_set_width(l, w - 12);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void build_drums(lv_obj_t *screen)
{
    for (int g = 0; g < DRUM_GROOVE_COUNT && s_drum_style_count < DRUM_STYLES; g++) {   // the styles in the groove table's order
        bool seen = false;
        for (int i = 0; i < s_drum_style_count; i++) seen |= !strcmp(s_drum_styles[i], DRUM_GROOVES[g].style);
        if (!seen) s_drum_styles[s_drum_style_count++] = DRUM_GROOVES[g].style;
    }
    s_drums = dialog_full(screen);
    dialog_title(s_drums, "Drums");
    lv_obj_t *sub = label(s_drums, HINT_FONT, MUTED);
    char st[96];
    snprintf(st, sizeof(st), "In the Nano's tempo \xC2\xB7 %d grooves \xC2\xB7 a new one starts at the bar line", DRUM_GROOVE_COUNT);
    lv_label_set_text(sub, st);
    lv_obj_set_pos(sub, 20, 48);
    close_button(s_drums, DLG_W - 60, 16, on_drum_close, 0);
    // the loops to the card: beside the close button, as "Look again" in the list of networks
    s_drum_save = small_button(s_drums, DLG_W - 76 - 184, 12, 184, 52, "Save loops", on_drum_save, 0);
    int lw = 624;   // the left column: the groove, whose it is, styles, grooves
    s_drum_now = label(s_drums, &ui_font_title_26, TEXT);
    lv_obj_set_pos(s_drum_now, 20, 84);
    lv_obj_set_size(s_drum_now, lw - 3 * 104 - 8, lv_font_get_line_height(&ui_font_title_26));   // (one line)
    lv_label_set_long_mode(s_drum_now, LV_LABEL_LONG_DOT);
    static const char *const PARTS[3] = { "A = B", "Part A", "Part B" };   // the looper parts' own grooves
    for (int i = 0; i < 3; i++) s_drum_part[i] = drum_button(20 + lw - (3 - i) * 104 + 6, 80, 98, 40, &ui_font_14, PARTS[i], on_drum_part, i);
    for (int i = 0; i < s_drum_style_count; i++) {   // two rows of five
        int w = (lw - 4 * 6) / 5;
        s_drum_style[i] = drum_button(20 + (i % 5) * (w + 6), 130 + (i / 5) * 44, w, 38, &ui_font_14, s_drum_styles[i], on_drum_style, i);
    }
    for (int n = 0; n < DRUM_PICK; n++) {
        int w = (lw - 2 * 8) / 3;
        s_drum_pick[n] = drum_button(20 + (n % 3) * (w + 8), 222 + (n / 3) * 58, w, 52, &ui_font_20, "", on_drum_groove, n);
        s_drum_pick_groove[n] = -1;
    }
    // the right column: kit, level, the looper's first takes (the drum menu is the looper's menu as well)
    int rx = 20 + lw + 20, rw = DLG_W - rx - 20;
    lv_obj_t *cap = label(s_drums, CAPTION_FONT, MUTED);
    lv_label_set_text(cap, "KIT");
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_set_pos(cap, rx, 84);
    static const char *const KITS[3] = { "Acoustic", "Electronic", "Lo-Fi" };   // (DRUM_KIT_OF)
    int kw = (rw - 2 * 6) / 3;
    for (int i = 0; i < 3; i++) s_drum_kit[i] = drum_button(rx + i * (kw + 6), 108, kw, 46, &ui_font_16, KITS[i], on_drum_kit, i);
    s_drum_level = fill_bar(s_drums, rx, 170, rw, "Level", &s_drum_level_value);
    lv_slider_set_range(s_drum_level, -40, 6);
    lv_obj_add_event_cb(s_drum_level, on_drum_level, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_drum_level, on_drum_level, LV_EVENT_RELEASED, NULL);
    static const char *const SWITCHES[4] = { "Drums start with the take", "Count in one bar", "Takes on the drums' bars",
                                             "The first note starts it" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = box(s_drums, rx, 240 + i * 58, rw, 52);
        lv_obj_t *t = label(row, &ui_font_16, TEXT);
        lv_label_set_text_static(t, SWITCHES[i]);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 14, 0);
        s_drum_switch[i] = calm_switch(row);
        lv_obj_align(s_drum_switch[i], LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_event_cb(s_drum_switch[i], on_drum_switch, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_set_clickable(row, true);   // the whole row is the switch (a finger on stage hits the words as often)
        lv_obj_set_style_bg_color(row, lv_color_hex(SURF_ON), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, on_drum_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_set_hidden(s_drums, true);
}

// The top bar's player field with a jam track (when the jam tracks are its source).
static void show_jam_field(void)
{
    const ui_jam_t *j = &s_jam_st;
    bool track = j->card && j->count && j->current >= 0;
    char pos[12];
    fmt_time(pos, sizeof(pos), j->pos_s);
    set_text(s_phone_title, track && s_jam_names ? jam_title(jam_name(j->current))
                            : !j->card ? j->unreadable ? "Card not readable" : "No card" : "Jam tracks");
    set_color_prop(s_phone_title, LV_STYLE_TEXT_COLOR, track ? TEXT : CAPTION);
    set_text(s_phone_time, track ? pos : "");
    set_text(s_phone_play, j->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    set_color_prop(s_phone_play, LV_STYLE_TEXT_COLOR, j->card && j->count ? TEXT : DIMMED);
    float k = j->len_s > 0 ? (float)j->pos_s / j->len_s : 0;
    set_hidden(s_phone_track, !track || !j->len_s);
    set_num_prop(s_phone_fill, LV_STYLE_WIDTH, (int)(164 * (k > 1 ? 1 : k)));
}

// ---- the jam player (Jam A: list and player side by side) ----

static void player_do(int action);
static void on_jam_row(lv_event_t *e) { phone_action(UI_JAM_PLAY, (int)(intptr_t)lv_event_get_user_data(e)); }
// (in player mode the row along the bottom is the footswitches 5-7: the selection, as they)
static void on_jam_toggle(lv_event_t *e) { if (s_player_mode) player_do(UI_PLAYER_START); else phone_action(UI_JAM_TOGGLE, 0); }
static void on_jam_prev(lv_event_t *e) { if (s_player_mode) player_do(UI_PLAYER_SONG_PREV); else phone_action(UI_JAM_PREV, 0); }
static void on_jam_next(lv_event_t *e) { if (s_player_mode) player_do(UI_PLAYER_SONG_NEXT); else phone_action(UI_JAM_NEXT, 0); }
static void on_jam_play(lv_event_t *e) { if (!s_jam_st.playing) phone_action(UI_JAM_TOGGLE, 0); }
static void on_jam_pause(lv_event_t *e) { if (s_jam_st.playing) phone_action(UI_JAM_TOGGLE, 0); }
static void close_jam(lv_event_t *e) { lv_obj_set_hidden(s_jam, true); }

static void on_jam_level(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_jam_st.current < 0) return;
    phone_action(UI_JAM_LEVEL, (int)lroundf(s_jam_st.level_db * 10) + d);
}

static void on_jam_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(s_jam_vol);
    char text[16];
    snprintf(text, sizeof(text), "%s%.1f dB", v < 0 ? "\xE2\x88\x92" : "", fabsf(v / 10.0f));
    lv_label_set_text(s_jam_vol_value, text);
    phone_action(UI_JAM_VOLUME, v);
}

static void show_jam(void);
static void on_jam_album(lv_event_t *e);

static void on_net_switch(lv_event_t *e) { phone_action(UI_NET_ON, lv_obj_has_state(s_net_switch, LV_STATE_CHECKED)); }

static void show_rename(char kind, const char *title, const char *ok, const char *text, int max_length);
static void on_net_network(lv_event_t *e)
{
    (void)e;
    netlist_open();   // (the networks around; "Other network ..." in it is the keyboard as before)
}

static void show_net(void)
{
    const ui_net_t *n = &s_net;
    char cap[96], text[96];
    switch (n->state) {
    case UI_NET_STARTING: snprintf(cap, sizeof(cap), "WI-FI"); snprintf(text, sizeof(text), "Starting \xE2\x80\xA6"); break;
    case UI_NET_JOINING: snprintf(cap, sizeof(cap), "WI-FI \xC2\xB7 JOINING %.32s", n->ssid); snprintf(text, sizeof(text), "\xE2\x80\xA6"); break;
    case UI_NET_STATION: snprintf(cap, sizeof(cap), "WI-FI \xC2\xB7 %.32s", n->ssid); break;
    case UI_NET_AP: snprintf(cap, sizeof(cap), "NANO BOARD \xC2\xB7 PASSWORD %.11s", n->ap_pass); break;
    case UI_NET_FAILED: snprintf(cap, sizeof(cap), "WI-FI"); snprintf(text, sizeof(text), "The Wi-Fi part does not answer"); break;
    default: snprintf(cap, sizeof(cap), "WI-FI \xC2\xB7 TRACKS FROM THE MAC"); snprintf(text, sizeof(text), "Off"); break;
    }
    if (n->state == UI_NET_STATION || n->state == UI_NET_AP) {
        if (n->receiving[0]) snprintf(text, sizeof(text), "Receiving %.60s \xC2\xB7 %d %%", n->receiving, n->percent);
        else if (n->received) snprintf(text, sizeof(text), "nano-board.local \xC2\xB7 %d received", n->received);
        else snprintf(text, sizeof(text), "nano-board.local  \xC2\xB7  %.40s", n->addr);
    }
    for (char *c = cap; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    set_text(s_net_caption, cap);
    set_text(s_net_text, text);
    set_color_prop(s_net_text, LV_STYLE_TEXT_COLOR, n->state == UI_NET_OFF ? CAPTION : n->state == UI_NET_FAILED ? 0xFF4B4B : TEXT);
    bool on = n->state != UI_NET_OFF;
    if (!lv_obj_has_state(s_net_switch, LV_STATE_PRESSED) && on != lv_obj_has_state(s_net_switch, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(s_net_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_net_switch, LV_STATE_CHECKED);
    }
}

static void open_source(int source)
{
    s_source = source;
    lv_obj_set_hidden(s_phone, source != 0);
    lv_obj_set_hidden(s_jam, source != 1);
    lv_obj_move_foreground(source ? s_jam : s_phone);
    if (source) {
        s_jam_list_count = -1;   // the list is filled again
        show_jam();
        show_jam_field();
    } else {
        ui_set_phone(&s_phone_state);
    }
}

static void on_source(lv_event_t *e) { open_source((int)(intptr_t)lv_event_get_user_data(e)); }

void ui_player_hide(void)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_phone, true);
    lv_obj_set_hidden(s_jam, true);
    s_sel = -1;   // (a selection not started does not wait for the next time)
    lvgl_port_unlock();
}

// After a press: the stage, the tiles and the jam page at once (not at the next look, 100 ms later)
static void player_refresh(void)
{
    if (!s_player_mode) return;
    if (!lv_obj_is_hidden(s_playerbar)) show_playerbar();
    if (s_tiles_drawn) {
        s_player_sig = player_sig();
        show_player_tiles();
        show_tile_extras();
        fit_names();
        s_tiles_stale = true;
    }
    if (!lv_obj_is_hidden(s_jam)) show_jam();
}

// What a player switch does (its footswitch, its tile, the jam page's row in player mode). In the screen's task.
static void player_do(int action)
{
    bool jam = s_source == 1, playing = jam ? s_jam_st.playing : s_ph_playing;
    ui_phone_action_t prev = jam ? UI_JAM_PREV : UI_PHONE_PREV, next = jam ? UI_JAM_NEXT : UI_PHONE_NEXT;
    ui_phone_action_t toggle = jam ? UI_JAM_TOGGLE : UI_PHONE_TOGGLE;
    switch (action) {
    case UI_PLAYER_BACK: phone_action(prev, 0); break;
    case UI_PLAYER_NEXT: phone_action(next, 0); break;
    case UI_PLAYER_PLAY: if (!playing) phone_action(toggle, 0); break;
    case UI_PLAYER_PAUSE: if (playing) phone_action(toggle, 0); break;
    case UI_PLAYER_TOGGLE: phone_action(toggle, 0); break;
    case UI_PLAYER_SOURCE:   // jam tracks <-> phone; its page follows if it shows
        if (!lv_obj_is_hidden(s_phone) || !lv_obj_is_hidden(s_jam)) open_source(!s_source);
        else s_source = !s_source;
        s_sel = -1;
        break;
    case UI_PLAYER_SONG_PREV:   // the selection moves - the song that plays goes on (the phone: it skips at once)
    case UI_PLAYER_SONG_NEXT:
        if (player_listed()) player_song_step(action == UI_PLAYER_SONG_NEXT ? 1 : -1);
        else if (!jam) phone_action(action == UI_PLAYER_SONG_NEXT ? UI_PHONE_NEXT : UI_PHONE_PREV, 0);
        break;
    case UI_PLAYER_ALBUM_PREV:
    case UI_PLAYER_ALBUM_NEXT:
        if (player_listed()) player_select(player_album_target(action == UI_PLAYER_ALBUM_NEXT ? 1 : -1));
        break;
    case UI_PLAYER_START:   // the selected song from its start; else play / pause (the selection goes when it plays)
        if (player_pending()) {
            phone_action(UI_JAM_PLAY, player_sel());
            s_sel = player_sel();
        } else phone_action(toggle, 0);
        break;
    }
    player_refresh();
}

void ui_player_action(int action)
{
    lvgl_port_lock(0);
    player_do(action);
    lvgl_port_unlock();
}

void ui_player_toggle(void)
{
    lvgl_port_lock(0);
    if (lv_obj_is_hidden(s_phone) && lv_obj_is_hidden(s_jam)) {
        open_source(s_source);
    } else {
        lv_obj_set_hidden(s_phone, true);
        lv_obj_set_hidden(s_jam, true);
    }
    lvgl_port_unlock();
}

// "Phone | Jam tracks" at the top left of both players: the chosen one filled.
static void source_chooser(lv_obj_t *parent, int x, int y, int chosen)
{
    // (2026-11-26: the jam tracks first, the phone second; `chosen` and the events stay the source, 0 phone / 1 jam)
    static const char *const NAMES[2] = { "Jam tracks", "Phone" };
    lv_obj_t *b = box(parent, x, y, 2 * 124 + 3, 44);
    for (int i = 0; i < 2; i++) {
        int source = i == 0 ? 1 : 0;
        lv_obj_t *f = seg(b, i * 125, 124, NULL);
        lv_obj_set_clickable(f, true);
        lv_obj_add_event_cb(f, on_source, LV_EVENT_CLICKED, (void *)(intptr_t)source);
        if (source == chosen) {
            lv_obj_set_style_bg_color(f, lv_color_hex(TEXT), 0);
            lv_obj_set_style_bg_color(f, lv_color_white(), LV_STATE_PRESSED);
        }
        lv_obj_t *l = label(f, &ui_font_title_14, source == chosen ? INK : CAPTION);
        lv_label_set_text(l, NAMES[i]);
        lv_obj_center(l);
        if (i == 0) vline(b, 124);
    }
}

static void build_jam(lv_obj_t *screen)
{
    s_jam = box(screen, MARGIN, 72, SCREEN_W - 2 * MARGIN, SCREEN_H - 72 - MARGIN);
    lv_obj_set_style_bg_color(s_jam, lv_color_black(), 0);
    lv_obj_set_style_border_color(s_jam, lv_color_hex(FILL), 0);
    lv_obj_set_clickable(s_jam, true);
    const int X = 24, PX = X + JAM_LIST_W + 22, PW = SCREEN_W - 2 * MARGIN - 2 - PX - X;
    source_chooser(s_jam, X, 14, 1);
    player_close(s_jam, close_jam);
    {   // the album in view: arrows to the one before / after (hidden while all tracks are in one album)
        int ax = X + 2 * 124 + 3 + 16, aw = SCREEN_W - 2 * MARGIN - 2 - 24 - 150 - 16 - ax;
        s_jam_albums = box(s_jam, ax, 14, aw, 44);
        lv_obj_t *a = seg(s_jam_albums, 0, 56, on_jam_album);
        lv_obj_set_user_data(a, (void *)(intptr_t)-1);
        lv_obj_t *l = label(a, &ui_font_20, TEXT);
        lv_label_set_text(l, LV_SYMBOL_LEFT);
        lv_obj_center(l);
        vline(s_jam_albums, 56);
        a = seg(s_jam_albums, aw - 2 - 56, 56, on_jam_album);
        lv_obj_set_user_data(a, (void *)(intptr_t)1);
        l = label(a, &ui_font_20, TEXT);
        lv_label_set_text(l, LV_SYMBOL_RIGHT);
        lv_obj_center(l);
        vline(s_jam_albums, aw - 2 - 57);
        s_jam_album_name = label(s_jam_albums, &ui_font_title_16, TEXT);
        lv_obj_set_width(s_jam_album_name, aw - 2 - 2 * 57 - 32 - 70);
        lv_label_set_long_mode(s_jam_album_name, LV_LABEL_LONG_DOT);
        lv_obj_align(s_jam_album_name, LV_ALIGN_LEFT_MID, 57 + 16, 0);
        s_jam_album_pos = label(s_jam_albums, &ui_font_12, CAPTION);
        lv_obj_set_style_text_letter_space(s_jam_album_pos, 1, 0);
        lv_obj_align(s_jam_album_pos, LV_ALIGN_RIGHT_MID, -57 - 16, 0);
        lv_obj_set_hidden(s_jam_albums, true);
    }

    s_jam_list = lv_obj_create(s_jam);
    lv_obj_remove_style_all(s_jam_list);
    lv_obj_set_pos(s_jam_list, X, 74);
    lv_obj_set_size(s_jam_list, JAM_LIST_W, 4 * 58 + 2);   // (four tracks in sight: the footswitch row below)
    lv_obj_set_style_bg_color(s_jam_list, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_opa(s_jam_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_jam_list, 1, 0);
    lv_obj_set_style_border_color(s_jam_list, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_radius(s_jam_list, RADIUS, 0);
    lv_obj_set_style_clip_corner(s_jam_list, true, 0);
    lv_obj_set_flex_flow(s_jam_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_jam_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_jam_list, LV_SCROLLBAR_MODE_OFF);

    // Wi-Fi for tracks from the Mac: status and switch | the network
    lv_obj_t *nb = box(s_jam, X, 320, JAM_LIST_W, 60);
    lv_obj_t *nf = seg(nb, 0, JAM_LIST_W - 2 - 111, NULL);
    s_net_caption = label(nf, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_net_caption, 1, 0);
    lv_obj_set_width(s_net_caption, JAM_LIST_W - 2 - 111 - 16 - 70);
    lv_obj_set_height(s_net_caption, lv_font_get_line_height(&ui_font_12));
    lv_label_set_long_mode(s_net_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_net_caption, 16, 8);
    s_net_text = label(nf, &ui_font_title_16, TEXT);
    lv_obj_set_width(s_net_text, JAM_LIST_W - 2 - 111 - 16 - 70);
    lv_obj_set_height(s_net_text, lv_font_get_line_height(&ui_font_title_16));
    lv_label_set_long_mode(s_net_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_net_text, 16, 28);
    s_net_switch = calm_switch(nf);
    lv_obj_align(s_net_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_event_cb(s_net_switch, on_net_switch, LV_EVENT_VALUE_CHANGED, NULL);
    vline(nb, JAM_LIST_W - 2 - 111);
    lv_obj_t *nw = seg(nb, JAM_LIST_W - 2 - 110, 110, on_net_network);
    lv_obj_t *nl = label(nw, &ui_font_title_14, TEXT);
    lv_label_set_text(nl, "Network");
    lv_obj_center(nl);

    s_jam_caption = label(s_jam, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_jam_caption, 1, 0);
    lv_obj_set_pos(s_jam_caption, PX, 78);
    s_jam_title = label(s_jam, &ui_font_title_34, TEXT);
    lv_obj_set_width(s_jam_title, PW);
    lv_label_set_long_mode(s_jam_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_jam_title, PX, 96);
    lv_obj_t *track = lv_obj_create(s_jam);
    lv_obj_remove_style_all(track);
    lv_obj_set_style_bg_color(track, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_set_size(track, PW, 4);
    lv_obj_set_pos(track, PX, 154);
    s_jam_scrub_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_jam_scrub_fill);
    lv_obj_set_style_bg_color(s_jam_scrub_fill, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_jam_scrub_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_jam_scrub_fill, 2, 0);
    lv_obj_set_size(s_jam_scrub_fill, 0, 4);
    s_jam_knob = lv_obj_create(s_jam);
    lv_obj_remove_style_all(s_jam_knob);
    lv_obj_set_style_bg_color(s_jam_knob, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_jam_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_jam_knob, 2, 0);
    lv_obj_set_size(s_jam_knob, 4, 16);
    lv_obj_set_pos(s_jam_knob, PX - 2, 148);
    s_jam_pos = label(s_jam, &ui_font_12, CAPTION);
    lv_obj_set_pos(s_jam_pos, PX, 166);
    s_jam_len = label(s_jam, &ui_font_12, CAPTION);
    lv_obj_set_width(s_jam_len, PW);
    lv_obj_set_style_text_align(s_jam_len, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_jam_len, PX, 166);

    // the footswitch row along the bottom: Back | Play | Pause | Next
    static const lv_event_cb_t JAM_CB[4] = { on_jam_prev, on_jam_toggle, on_jam_next, on_player_row_out };
    build_transport(&s_jam_tr, s_jam, JAM_CB);
    lv_obj_t *l, *f;

    lv_obj_t *c = label(s_jam, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, "JAM VOLUME INTO THE NANO");
    lv_obj_set_pos(c, PX, 192);
    s_jam_vol = fill_bar(s_jam, PX, 212, PW, "Jam", &s_jam_vol_value);
    lv_slider_set_range(s_jam_vol, -400, 60);
    lv_obj_add_event_cb(s_jam_vol, on_jam_volume, LV_EVENT_VALUE_CHANGED, NULL);
    c = label(s_jam, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, "THIS TRACK \xC2\xB7 BY EAR");
    lv_obj_set_pos(c, PX, 288);
    lv_obj_t *lb = box(s_jam, PX, 308, PW, 60);
    f = seg(lb, 0, 72, NULL);
    lv_obj_set_clickable(f, true);
    lv_obj_add_event_cb(f, on_jam_level, LV_EVENT_CLICKED, (void *)(intptr_t)-5);
    l = label(f, &ui_font_28, TEXT);
    lv_label_set_text(l, LV_SYMBOL_MINUS);
    lv_obj_center(l);
    vline(lb, 72);
    s_jam_level_value = label(lb, &ui_font_title_18, TEXT);
    lv_obj_align(s_jam_level_value, LV_ALIGN_CENTER, 0, 0);
    vline(lb, PW - 2 - 73);
    f = seg(lb, PW - 2 - 72, 72, NULL);
    lv_obj_set_clickable(f, true);
    lv_obj_add_event_cb(f, on_jam_level, LV_EVENT_CLICKED, (void *)(intptr_t)5);
    l = label(f, &ui_font_28, TEXT);
    lv_label_set_text(l, LV_SYMBOL_PLUS);
    lv_obj_center(l);
    s_jam_foot = label(s_jam, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_jam_foot, 1, 0);
    lv_obj_set_pos(s_jam_foot, PX, 374);
    lv_obj_set_hidden(s_jam, true);
}

// The tracks of the list: those of the album in view. Each row keeps its track's number as its user data.
static void jam_list_row(int i, bool line)
{
    lv_obj_t *r = lv_button_create(s_jam_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, JAM_LIST_W - 2, 58);
    lv_obj_set_style_bg_color(r, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(SURF_ON), LV_STATE_PRESSED);
    if (line) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(HAIRLINE), 0);
    }
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    lv_obj_add_event_cb(r, on_jam_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *n = label(r, &ui_font_12, CAPTION);
    lv_obj_set_width(n, 24);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *t = label(r, &ui_font_title_16, TEXT);
    lv_obj_set_width(t, JAM_LIST_W - 2 - 48 - 90);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_label_set_text(t, jam_title(jam_name(i)));
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_t *v = label(r, &ui_font_12, CAPTION);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -16, 0);
}

static void jam_fill_list(void)
{
    lv_obj_clean(s_jam_list);
    int first = 0, last = -1;
    if (s_jam_view >= 0 && s_jam_view < s_jam_st.count) jam_album_range(s_jam_view, &first, &last);
    for (int i = first; i <= last; i++) jam_list_row(i, i > first);
    s_jam_list_count = s_jam_st.count;
    s_jam_list_current = -2;
}

// The rows: the current one lit with its play / pause mark, the others numbered in their album; each with its
// correction.
static int s_jam_list_sel = -1, s_jam_list_sel_shown = -1;   // the selected row (player mode), -1 = none
static int jam_list_sel(void) { return s_player_mode && player_pending() ? player_sel() : -1; }
static void jam_mark_list(void)
{
    s_jam_list_sel = jam_list_sel();
    uint32_t n_children = lv_obj_get_child_count(s_jam_list);
    for (uint32_t c = 0; c < n_children; c++) {
        lv_obj_t *r = lv_obj_get_child(s_jam_list, c);
        int i = (int)(intptr_t)lv_obj_get_user_data(r);
        if (i < 0 || i >= s_jam_st.count) continue;
        bool cur = i == s_jam_st.current, sel = i == s_jam_list_sel;
        // (player mode: the selected song framed in blue, as on the stage - the one that plays keeps its green mark)
        set_color_prop(r, LV_STYLE_BG_COLOR, cur ? SURF_ON : sel ? blend(SEL_BLUE, SURF, 0.10f) : SURF);
        set_num_prop(r, LV_STYLE_BORDER_SIDE, sel ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_TOP);
        set_num_prop(r, LV_STYLE_BORDER_WIDTH, sel ? 2 : c > 0 ? 1 : 0);
        set_color_prop(r, LV_STYLE_BORDER_COLOR, sel ? blend(SEL_BLUE, SURF, 0.85f) : HAIRLINE);
        set_num_prop(r, LV_STYLE_RADIUS, sel ? 10 : 0);
        if ((cur && s_jam_list_current != s_jam_st.current) || (sel && s_jam_list_sel_shown != i))
            lv_obj_scroll_to_view(r, LV_ANIM_OFF);   // (the playing row in sight, or the selected one)
        int first, last;
        jam_album_range(i, &first, &last);
        char text[16];
        if (cur) snprintf(text, sizeof(text), "%s", s_jam_st.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        else snprintf(text, sizeof(text), "%d", i - first + 1);
        lv_obj_t *n = lv_obj_get_child(r, 0), *v = lv_obj_get_child(r, 2);
        set_text(n, text);
        set_color_prop(n, LV_STYLE_TEXT_COLOR, cur ? (s_jam_st.playing ? GREEN : TEXT) : sel ? SEL_BLUE : CAPTION);
        char note[48] = "";
        if (s_jam_notes) s_jam_notes(i, note, sizeof(note));
        set_text(v, note);
    }
    s_jam_list_current = s_jam_st.current;
    s_jam_list_playing = s_jam_st.playing;
    s_jam_list_sel_shown = s_jam_list_sel;
}

// The album in view: it follows the playing track when that changes; the arrows step through the albums.
static void jam_show_albums(void)
{
    if (!s_jam_albums) return;
    const ui_jam_t *j = &s_jam_st;
    if (j->current != s_jam_view_current) {
        s_jam_view_current = j->current;
        if (j->current >= 0) s_jam_view = j->current;
    }
    if (s_player_mode && player_listed()) s_jam_view = player_sel();   // (player mode: the selection's album)
    if (s_jam_view >= j->count) s_jam_view = j->count ? 0 : -1;
    if (s_jam_view < 0 && j->count) s_jam_view = 0;
    int albums = jam_album_count();
    set_hidden(s_jam_albums, albums < 2);
    if (albums < 2 || s_jam_view < 0) return;
    int first, last, k = 0;
    jam_album_range(s_jam_view, &first, &last);
    s_jam_view = first;
    for (int i = 0; i <= first; i++) k += i == 0 || !jam_same_album(i - 1, i);
    char album[72], text[32];
    jam_album_name(first, album, sizeof(album), false);
    set_text(s_jam_album_name, album);
    snprintf(text, sizeof(text), "ALBUM %d / %d", k, albums);
    set_text(s_jam_album_pos, text);
}

static void on_jam_album(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (s_player_mode && player_listed()) {   // (player mode: the arrows move the selection, as the footswitches)
        player_do(step > 0 ? UI_PLAYER_ALBUM_NEXT : UI_PLAYER_ALBUM_PREV);
        return;
    }
    const int n = s_jam_st.count;
    if (n <= 0 || s_jam_view < 0) return;
    int first, last;
    jam_album_range(s_jam_view, &first, &last);
    if (step > 0) s_jam_view = last + 1 < n ? last + 1 : 0;   // (round: after the last album the first)
    else {
        int prev = first > 0 ? first - 1 : n - 1;
        jam_album_range(prev, &first, &last);
        s_jam_view = first;
    }
    show_jam();
}

static void show_jam(void)
{
    const ui_jam_t *j = &s_jam_st;
    jam_show_albums();
    uint32_t sig = jam_names_sig() ^ (uint32_t)(s_jam_view + 2) * 2654435761u;
    if (j->count != s_jam_list_count || sig != s_jam_list_sig) {
        s_jam_list_sig = sig;
        jam_fill_list();
    }
    if (j->current != s_jam_list_current || j->playing != s_jam_list_playing || jam_list_sel() != s_jam_list_sel) jam_mark_list();
    char text[128], pos[12], len[12];
    if (!j->card) snprintf(text, sizeof(text), j->unreadable ? "CARD NOT READABLE \xC2\xB7 FORMAT IT AS MS-DOS (FAT32)" : "NO MICROSD CARD");
    else if (!j->count) snprintf(text, sizeof(text), "NO WAV FILES IN THE FOLDER JAM");
    else if (j->current < 0) snprintf(text, sizeof(text), "%d TRACKS", j->count);
    else {
        int first, last;
        char album[72];
        jam_album_range(j->current, &first, &last);
        jam_album_name(j->current, album, sizeof(album), true);
        snprintf(text, sizeof(text), "%s \xC2\xB7 %.28s \xC2\xB7 %d / %d", j->playing ? "PLAYING" : "PAUSED", album,
                 j->current - first + 1, last - first + 1);
    }
    set_text(s_jam_caption, text);
    set_text(s_jam_title, j->current >= 0 && s_jam_names ? jam_title(jam_name(j->current))
                          : !j->card ? j->unreadable ? "Card not readable" : "No card" : "Jam tracks");
    fmt_time(pos, sizeof(pos), j->pos_s);
    fmt_time(len, sizeof(len), j->len_s);
    set_text(s_jam_pos, j->current >= 0 ? pos : "");
    set_text(s_jam_len, j->current >= 0 ? len : "");
    float k = j->len_s > 0 ? (float)j->pos_s / j->len_s : 0;
    if (k > 1) k = 1;
    lv_obj_t *track = lv_obj_get_parent(s_jam_scrub_fill);
    int w = lv_obj_get_style_width(track, 0);
    set_num_prop(s_jam_scrub_fill, LV_STYLE_WIDTH, (int)(w * k));
    set_num_prop(s_jam_knob, LV_STYLE_X, lv_obj_get_style_x(track, 0) - 2 + (int)(w * k));
    show_transport(&s_jam_tr, j->playing);
    if (!lv_obj_has_state(s_jam_vol, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_jam_vol, (int)lroundf(j->volume_db * 10), LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%s%.1f dB", j->volume_db < 0 ? "\xE2\x88\x92" : "", fabsf(j->volume_db));
        set_text(s_jam_vol_value, text);
    }
    if (j->current < 0) snprintf(text, sizeof(text), "\xE2\x80\x93");
    else snprintf(text, sizeof(text), "%s%.1f dB", j->level_db < 0 ? "\xE2\x88\x92" : j->level_db > 0 ? "+" : "", fabsf(j->level_db));
    set_text(s_jam_level_value, text);
    int target = (int)lroundf(-j->target_lufs);
    if (j->measuring) snprintf(text, sizeof(text), "LEVELLED TO \xE2\x88\x92%d LUFS \xC2\xB7 MEASURING, %d TO GO", target, j->measuring);
    else {
        int albums = jam_album_count();
        if (albums > 1) snprintf(text, sizeof(text), "LEVELLED TO \xE2\x88\x92%d LUFS \xC2\xB7 THEN THE ALBUM'S NEXT \xC2\xB7 %d ALBUMS", target, albums);
        else snprintf(text, sizeof(text), "LEVELLED TO \xE2\x88\x92%d LUFS \xC2\xB7 THEN THE NEXT TRACK \xC2\xB7 %d TRACK%s", target, j->count, j->count == 1 ? "" : "S");
    }
    set_text(s_jam_foot, text);
    show_net();
    static int shown_measuring = -1;   // a track was measured: its note changes
    if (j->measuring != shown_measuring) {
        shown_measuring = j->measuring;
        jam_mark_list();
    }
    static float shown_level = 1000;   // a correction changed: the list shows it too
    if (j->current >= 0 && j->level_db != shown_level) {
        shown_level = j->level_db;
        jam_mark_list();
    }
}

static void loop_poll(lv_timer_t *t)
{
    if (!s_loop_source) return;
    ui_loop_t l;
    ui_jam_t j;
    ui_net_t n;
    memset(&l, 0, sizeof(l));
    memset(&j, 0, sizeof(j));
    memset(&n, 0, sizeof(n));
    j.current = -1;
    s_loop_source(&l, &j, &n);
    s_net = n;
    if (s_netlist && !lv_obj_is_hidden(s_netlist)) netlist_fill(false);
    if (s_drums && !lv_obj_is_hidden(s_drums)) { s_loop = l; drum_menu_show(); }
    if (s_set_page[SET_WIFI] && !lv_obj_is_hidden(s_settings) && s_set_topic == SET_WIFI) show_wifi_page();
    s_loop = l;
    s_jam_st = j;
    cover_refresh(j.cover_seq);
    if (!lv_obj_is_hidden(s_loopbar)) show_loopbar();
    uint32_t sig = (uint32_t)l.ready | (uint32_t)l.state << 1 | (uint32_t)l.to_bar_line << 4 | (uint32_t)l.can_undo << 5
                 | (uint32_t)l.can_redo << 6 | (uint32_t)j.card << 7 | (uint32_t)j.playing << 8
                 | (uint32_t)((j.current + 1) & 0x7F) << 9 | (uint32_t)(j.count & 0x7F) << 16 | (uint32_t)(j.pos_s & 0xFF) << 24;
    sig ^= (uint32_t)(j.current + 1) * 0x85EBCA6Bu ^ (uint32_t)j.count * 0xC2B2AE35u;   // (256 tracks: more than 7 bits)
    // (P4: the parts and the drums change the tiles too)
    sig ^= ((uint32_t)l.part | (uint32_t)(l.next_part + 1) << 1 | (uint32_t)l.paused << 3 | (uint32_t)l.drums_on << 4 | (uint32_t)l.counting << 31 | (uint32_t)l.armed << 30
            | (uint32_t)l.drums_ready << 5 | (uint32_t)l.part_state[0] << 6 | (uint32_t)l.part_state[1] << 9
            | (uint32_t)(l.groove & 0x3F) << 12 | (uint32_t)lroundf(l.part_len_s[0] * 10) << 18 | (uint32_t)lroundf(l.part_len_s[1] * 10) << 25) * 2654435761u;
    sig ^= (uint32_t)(l.groove + 1) * 0x9E3779B1u;   // (more than 64 grooves: the six bits above are not enough)
    if (s_looper_mode && s_tiles_drawn && sig != s_loop_sig) {
        s_loop_sig = sig;
        show_onboard_looper_tiles();
        show_tile_extras();
        fit_names();
        s_tiles_stale = true;   // (the editor's strip: these tiles changed without a ui_show_state, see tiles_copy)
    }
    player_check_sel();
    if (s_source == 1) show_jam_field();
    if (!lv_obj_is_hidden(s_jam)) show_jam();
    if (s_player_mode) {
        if (!lv_obj_is_hidden(s_playerbar)) show_playerbar();
        uint32_t ps = player_sig();
        if (s_tiles_drawn && ps != s_player_sig) {
            s_player_sig = ps;
            show_player_tiles();
            show_tile_extras();
            fit_names();
            s_tiles_stale = true;
        }
    }
    if (s_lvs && !lv_obj_is_hidden(s_lvs)) show_level_sheet();
    if (s_gr_row && !lv_obj_is_hidden(s_editor)) show_gr_row();
}

void ui_set_loop_source(ui_loop_source_cb source, ui_jam_name_cb names, ui_jam_note_cb notes)
{
    lvgl_port_lock(0);
    s_loop_source = source;
    s_jam_names = names;
    s_jam_notes = notes;
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(loop_poll, 100, NULL);
    loop_poll(NULL);
    lvgl_port_unlock();
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
    lv_obj_set_size(s_tuner, SCREEN_W, SCREEN_H);
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
    lv_obj_set_pos(ref, 24, 428 + (SCREEN_H - 480));
    lv_obj_t *down = button(s_tuner, 166, 412 + (SCREEN_H - 480), 56, 48, on_tuner_down);
    lv_obj_add_event_cb(down, on_tuner_down, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *down_label = label(down, &ui_font_28, 0xF2F2F2);
    lv_label_set_text(down_label, LV_SYMBOL_MINUS);
    lv_obj_center(down_label);
    s_tuner_hz = label(s_tuner, &ui_font_28, 0xF2F2F2);
    lv_obj_set_width(s_tuner_hz, 120);
    lv_obj_set_style_text_align(s_tuner_hz, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_tuner_hz, 226, 421 + (SCREEN_H - 480));
    lv_obj_t *up = button(s_tuner, 350, 412 + (SCREEN_H - 480), 56, 48, on_tuner_up);
    lv_obj_add_event_cb(up, on_tuner_up, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *up_label = label(up, &ui_font_28, 0xF2F2F2);
    lv_label_set_text(up_label, LV_SYMBOL_PLUS);
    lv_obj_center(up_label);

    // Mute: the whole area is the button; the switch only shows the state (it comes from the Nano)
    lv_obj_t *mute = lv_button_create(s_tuner);
    lv_obj_set_size(mute, 290, 48);
    lv_obj_set_pos(mute, 486 + (SCREEN_W - 800), 412 + (SCREEN_H - 480));
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
static void build_scene_capture(lv_obj_t *screen);
static void build_expression(lv_obj_t *screen);
static void style_scene_editor(void);
static void style_scene_chip(void);
static bool scene_panel_open(void);
static void close_expression(void);

// ---- start screen (2026-11-27): the seven symbols light up one after another as the board gets ready - screen,
//      footswitches, the Nano found, its presets, the drum kit, the sound; then it goes (or a tap). The newest one
//      glows softly; those not reached yet stay dark. ----
#define SPLASH_ICONS 7
static lv_obj_t *s_splash_sq[SPLASH_ICONS];
static uint32_t s_splash_color[SPLASH_ICONS];
static unsigned s_splash_mask;      // UI_SPLASH_* reached
static lv_timer_t *s_splash_timer;  // closes it: all reached, or the fallback after the presets

static void splash_glow(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)v, 0);
}

static void splash_hide(void)
{
    if (!s_splash) return;
    if (s_splash_timer) lv_timer_delete(s_splash_timer), s_splash_timer = NULL;
    for (int i = 0; i < SPLASH_ICONS; i++) if (s_splash_sq[i]) lv_anim_delete(s_splash_sq[i], splash_glow), s_splash_sq[i] = NULL;
    lv_obj_fade_out(s_splash, 400, 0);
    lv_obj_delete_delayed(s_splash, 450);
    s_splash = s_splash_status = s_splash_version = NULL;
}

static void on_splash_tap(lv_event_t *e)
{
    splash_hide();
}

static void splash_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_splash_timer = NULL;   // (one-shot: LVGL deletes it)
    splash_hide();
}

// Lit: the symbol in its colour; not yet: a dark square with the symbol in its colour, dimmed.
static void splash_show_steps(void)
{
    int newest = -1;
    for (int i = 0; i < SPLASH_ICONS; i++) if (s_splash_mask & (1u << i)) newest = i;
    for (int i = 0; i < SPLASH_ICONS; i++) {
        lv_obj_t *sq = s_splash_sq[i];
        if (!sq) continue;
        bool lit = s_splash_mask & (1u << i);
        uint32_t c = s_splash_color[i];
        set_color_prop(sq, LV_STYLE_BG_COLOR, lit ? c : blend(c, 0x000000, 0.16f));
        set_color_prop(sq, LV_STYLE_BORDER_COLOR, lit ? c : blend(c, 0x000000, 0.40f));
        set_color_prop(lv_obj_get_child(sq, 0), LV_STYLE_IMAGE_RECOLOR, lit ? ink_for(c) : blend(c, 0x000000, 0.55f));
        lv_anim_delete(sq, splash_glow);
        if (i == newest && s_splash_mask != (1u << SPLASH_ICONS) - 1) {   // the newest: a soft, slow glow
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, sq);
            lv_anim_set_exec_cb(&a, splash_glow);
            lv_anim_set_values(&a, 20, 120);
            lv_anim_set_duration(&a, 900);
            lv_anim_set_reverse_duration(&a, 900);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_start(&a);
        } else {
            lv_obj_set_style_shadow_opa(sq, lit ? 45 : 0, 0);
        }
    }
}

static void build_splash(lv_obj_t *screen)
{
    // The effect categories in their colours, one per step of the start.
    static const struct { int icon; uint32_t color; } GLOW[SPLASH_ICONS] = {
        { NANO_ICON_OVERDRIVE, 0xFF7000 }, { NANO_ICON_COMPRESSOR, 0x45F862 }, { NANO_ICON_EQUALIZER, 0x0A74E0 },
        { NANO_ICON_MODULATION, 0x8A5CFF }, { NANO_ICON_PITCH, 0xFFD236 }, { NANO_ICON_FILTER, 0x87DAFF },
        { NANO_ICON_REVERB, 0x00FFDD },
    };
    const int count = SPLASH_ICONS, size = 62, gap = 26;
    s_splash = lv_obj_create(screen);
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, SCREEN_W, SCREEN_H);
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

    int x = (SCREEN_W - count * size - (count - 1) * gap) / 2;
    for (int i = 0; i < count; i++) {
        lv_obj_t *sq = icon_square(s_splash, size);
        lv_obj_set_pos(sq, x + i * (size + gap), 246);
        set_icon_square(sq, GLOW[i].color, NANO_ICONS[GLOW[i].icon], true);
        lv_obj_set_style_border_width(sq, 2, 0);
        lv_obj_set_style_shadow_color(sq, lv_color_hex(GLOW[i].color), 0);
        lv_obj_set_style_shadow_width(sq, 30, 0);
        lv_obj_set_style_shadow_spread(sq, 2, 0);
        lv_obj_set_style_shadow_opa(sq, 0, 0);
        s_splash_sq[i] = sq;
        s_splash_color[i] = GLOW[i].color;
    }
    s_splash_mask = 1u << UI_SPLASH_SCREEN;
    splash_show_steps();

    s_splash_status = label(s_splash, HINT_FONT, MUTED);
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

void ui_splash_step(int step)
{
    if (step < 0 || step >= SPLASH_ICONS) return;
    lvgl_port_lock(0);
    if (s_splash) {
        s_splash_mask |= 1u << step;
        bool presets = s_splash_mask & (1u << UI_SPLASH_PRESETS), sound = s_splash_mask & (1u << UI_SPLASH_SOUND);
        if (presets && sound) s_splash_mask = (1u << SPLASH_ICONS) - 1;   // ready: all of them, a moment, then away
        splash_show_steps();
        if (s_splash_timer) lv_timer_delete(s_splash_timer), s_splash_timer = NULL;
        if (presets) {   // away when all is there (0.7 s to see it) - with the presets, at most 12 s later
            s_splash_timer = lv_timer_create(splash_timer_cb, sound ? 700 : 12000, NULL);
            lv_timer_set_repeat_count(s_splash_timer, 1);
        }
    }
    lvgl_port_unlock();
}

void ui_splash_done(void)
{
    lvgl_port_lock(0);
    splash_hide();
    lvgl_port_unlock();
}

// A sign of life of the task that runs the screen (see ui_stalled_ms).
static volatile uint32_t s_alive_tick;

static void alive_cb(lv_timer_t *timer)
{
    s_alive_tick = lv_tick_get();
}

uint32_t ui_stalled_ms(void)
{
    return lv_tick_elaps(s_alive_tick);
}

#ifdef NANO_WDTEST
void ui_test_hang(void)
{
    lvgl_port_lock(0);   // never given back: the screen's task waits for it
}
#endif

void ui_init(ui_command_cb on_command, ui_param_cb on_param, ui_text_cb on_text)
{
    s_on_command = on_command;
    s_on_param = on_param;
    s_on_text = on_text;
    lvgl_port_lock(0);
    s_alive_tick = lv_tick_get();
    lv_timer_create(alive_cb, 500, NULL);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    s_main = lv_obj_create(screen);
    lv_obj_remove_style_all(s_main);
    lv_obj_set_size(s_main, SCREEN_W, SCREEN_H);
    lv_obj_set_scrollable(s_main, false);
    lv_obj_set_clickable(s_main, false);

    // P4 top bar: four boxes over the four tile columns - Settings | Reload; preset number, bank, USB | tempo; the
    // phone; Pedal | Scenes | Save. Below: the arrows and the large preset name, then the signal chain.
    lv_obj_t *preset = lv_obj_create(s_main);
    lv_obj_remove_style_all(preset);
    lv_obj_set_pos(preset, 0, 0);
    lv_obj_set_size(preset, SCREEN_W, 156);
    lv_obj_set_scrollable(preset, false);
    s_preset_card = preset;

    lv_obj_t *b1 = box(preset, COL_X(0), 10, COL_W, 52);
    lv_obj_t *sg = seg(b1, 0, 119, open_settings);
    top_word(sg, ui_icon(sg, UI_ICON_GEAR, 20, TEXT), "Settings");
    vline(b1, 119);
    sg = seg(b1, 120, 119, on_refresh);
    top_word(sg, ui_icon(sg, UI_ICON_REFRESH, 20, TEXT), "Reload");
    s_midi_icon = NULL;
    s_midi_label = NULL;

    lv_obj_t *b2 = box(preset, COL_X(1), 10, COL_W, 52);
    lv_obj_t *status_row = lv_obj_create(b2);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, 151, 50);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 6, 0);
    lv_obj_set_style_pad_left(status_row, 12, 0);
    lv_obj_set_clickable(status_row, true);   // a tap: the set list
    lv_obj_add_event_cb(status_row, on_status_tap, LV_EVENT_CLICKED, NULL);
    s_status_dot = lv_obj_create(status_row);
    lv_obj_set_size(s_status_dot, 7, 7);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    s_status = label(status_row, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(s_status, 1, 0);
    // The preset number, next to it the bank or the song
    s_preset_chip = lv_obj_create(status_row);
    lv_obj_remove_style_all(s_preset_chip);
    lv_obj_set_size(s_preset_chip, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(s_preset_chip, 8, 0);
    lv_obj_set_style_pad_hor(s_preset_chip, 2, 0);
    lv_obj_set_style_bg_opa(s_preset_chip, LV_OPA_TRANSP, 0);
    lv_obj_set_hidden(s_preset_chip, true);
    s_preset_chip_text = label(s_preset_chip, &ui_font_title_18, TEXT);
    lv_obj_center(s_preset_chip_text);
    s_preset_number = label(status_row, &ui_font_12, TEXT);
    lv_obj_set_style_text_letter_space(s_preset_number, 1, 0);
    s_app = label(status_row, &ui_font_12, CAPTION);    // editor connected through the controller
    lv_obj_set_style_text_letter_space(s_app, 1, 0);
    lv_label_set_text(s_app, "APP");
    lv_obj_set_hidden(s_app, true);
    s_usb_mark = lv_obj_create(status_row);   // the Nano hangs on the cable
    lv_obj_remove_style_all(s_usb_mark);
    lv_obj_set_size(s_usb_mark, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_usb_mark, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_usb_mark, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_usb_mark, 3, 0);
    ui_icon(s_usb_mark, UI_ICON_USB, 14, CAPTION);
    // (the symbol alone: "SONG 4/8" leaves no room for the word)
    lv_obj_set_style_margin_left(s_usb_mark, 2, 0);
    lv_obj_set_hidden(s_usb_mark, true);
    // Tempo (NanOS 2.3.0): metronome symbol and BPM, green while the metronome is on; hidden on older firmware
    vline(b2, 152);
    s_tempo_line = lv_obj_get_child(b2, -1);
    lv_obj_set_hidden(s_tempo_line, true);
    s_tempo_button = seg(b2, 153, 86, on_tempo_button);
    s_tempo_button_icon = ui_icon(s_tempo_button, UI_ICON_METRONOME, 20, TEXT);
    top_word(s_tempo_button, s_tempo_button_icon, "120");
    s_tempo_button_text = lv_obj_get_child(s_tempo_button, -1);
    lv_obj_set_hidden(s_tempo_button, true);

    // The phone: play / pause at the left, title, time and progress; a tap on the rest opens the player
    lv_obj_t *b3 = box(preset, COL_X(2), 10, COL_W, 52);
    s_phone_box = b3;
    sg = seg(b3, 0, 50, on_phone_toggle);
    s_phone_play = label(sg, &ui_font_20, TEXT);
    lv_label_set_text(s_phone_play, LV_SYMBOL_PLAY);
    lv_obj_center(s_phone_play);
    vline(b3, 50);
    sg = seg(b3, 51, 188, open_phone);
    s_phone_title = label(sg, &ui_font_title_14, TEXT);
    lv_obj_set_width(s_phone_title, 128);
    lv_label_set_long_mode(s_phone_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_phone_title, 12, 9);
    s_phone_time = label(sg, &ui_font_12, CAPTION);
    lv_obj_align(s_phone_time, LV_ALIGN_TOP_RIGHT, -12, 11);
    s_phone_track = lv_obj_create(sg);
    lv_obj_remove_style_all(s_phone_track);
    lv_obj_set_style_bg_color(s_phone_track, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_bg_opa(s_phone_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_phone_track, 2, 0);
    lv_obj_set_size(s_phone_track, 164, 3);
    lv_obj_set_pos(s_phone_track, 12, 34);
    s_phone_fill = lv_obj_create(s_phone_track);
    lv_obj_remove_style_all(s_phone_fill);
    lv_obj_set_style_bg_color(s_phone_fill, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(s_phone_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_phone_fill, 2, 0);
    lv_obj_set_size(s_phone_fill, 0, 3);

    lv_obj_t *b4 = box(preset, COL_X(3), 10, COL_W, 52);
    s_exp_button = seg(b4, 0, 79, open_expression);   // what the Nano's pedal moves in this preset
    top_word(s_exp_button, ui_icon(s_exp_button, UI_ICON_EXP, 20, TEXT), "Pedal");
    vline(b4, 79);
    s_scene_button = seg(b4, 80, 79, on_scene_button);   // footswitches 3-8: the scenes of this preset
    top_word(s_scene_button, ui_icon(s_scene_button, UI_ICON_SCENES, 20, TEXT), "Scenes");
    vline(b4, 159);
    s_save_button = seg(b4, 160, 79, on_save_button);
    top_word(s_save_button, ui_icon(s_save_button, UI_ICON_SAVE, 20, TEXT), "Save");

    lv_obj_t *ab = s_prev_box = box(preset, MARGIN, 72, 72, 80);
    lv_obj_t *prev = seg(ab, 0, 70, on_previous);
    lv_obj_t *prev_label = label(prev, &ui_font_28, TEXT);
    lv_label_set_text(prev_label, LV_SYMBOL_LEFT);
    lv_obj_center(prev_label);
    ab = s_next_box = box(preset, SCREEN_W - MARGIN - 72, 72, 72, 80);
    lv_obj_t *next = seg(ab, 0, 70, on_next);
    lv_obj_t *next_label = label(next, &ui_font_28, TEXT);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);
    s_preset_name = label(preset, &ui_font_title_54, 0xFFFFFF);
    lv_obj_set_width(s_preset_name, PRESET_NAME_W);
    lv_obj_set_style_text_align(s_preset_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_preset_name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_preset_name, (SCREEN_W - PRESET_NAME_W) / 2, 75);
    lv_obj_set_clickable(s_preset_name, true);   // a tap: the preset list; long press: rename
    lv_obj_add_event_cb(s_preset_name, open_presets, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_preset_name, open_rename, LV_EVENT_LONG_PRESSED, NULL);
    s_edited = lv_obj_create(preset);   // unsaved changes
    lv_obj_set_size(s_edited, 10, 10);
    lv_obj_set_style_radius(s_edited, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_edited, 0, 0);
    lv_obj_set_style_bg_color(s_edited, lv_color_hex(ORANGE), 0);
    lv_obj_set_hidden(s_edited, true);

    // Gig view (swipe up): slim status bar over the tiles - connection, preset and bank, name, unsaved dot, mode.
    s_gig_bar = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_gig_bar);
    lv_obj_set_pos(s_gig_bar, 0, 0);
    lv_obj_set_size(s_gig_bar, SCREEN_W, GIG_BAR_H);
    lv_obj_set_scrollable(s_gig_bar, false);
    // P4 (chosen 2026-11-13, B): the top bar's twin - a box at the left (connection, number, bank), the name in the
    // middle, a box at the right with the two fields (filled = on)
    box(s_gig_bar, MARGIN, 4, 220, GIG_BAR_H - 8);
    s_gig_dot = lv_obj_create(s_gig_bar);
    lv_obj_set_size(s_gig_dot, 9, 9);
    lv_obj_set_style_radius(s_gig_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_gig_dot, 0, 0);
    lv_obj_align(s_gig_dot, LV_ALIGN_LEFT_MID, MARGIN + 14, 0);
    lv_obj_t *badge = lv_obj_create(s_gig_bar);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_min_width(badge, 32, 0);
    lv_obj_set_style_pad_hor(badge, 9, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, MARGIN + 26, 0);
    s_gig_number = label(badge, &ui_font_title_18, TEXT);
    lv_obj_center(s_gig_number);
    s_gig_bank = label(s_gig_bar, &ui_font_12, TEXT);
    lv_obj_set_style_text_letter_space(s_gig_bank, 1, 0);
    lv_obj_align_to(s_gig_bank, badge, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    s_gig_name = label(s_gig_bar, &ui_font_title_26, TEXT);
    lv_obj_set_width(s_gig_name, 520);   // (between the number at the left and the two fields at the right)
    lv_obj_set_style_text_align(s_gig_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_gig_name, LV_LABEL_LONG_DOT);
    lv_obj_align(s_gig_name, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_clickable(s_gig_name, true);   // a tap: the preset list, as on the name of the other view
    lv_obj_set_ext_click_area(s_gig_name, 8);
    lv_obj_add_event_cb(s_gig_name, open_presets, LV_EVENT_SHORT_CLICKED, NULL);
    s_gig_dirty = lv_obj_create(s_gig_bar);
    lv_obj_set_size(s_gig_dirty, 9, 9);
    lv_obj_set_style_radius(s_gig_dirty, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_gig_dirty, 0, 0);
    lv_obj_set_style_bg_color(s_gig_dirty, lv_color_hex(ORANGE), 0);
    lv_obj_set_hidden(s_gig_dirty, true);
    lv_obj_t *gig_right = lv_obj_create(s_gig_bar);   // the two fields at the right: PEDALS | the mode
    lv_obj_remove_style_all(gig_right);
    lv_obj_set_size(gig_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gig_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gig_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gig_right, 4, 0);
    lv_obj_set_style_pad_all(gig_right, 3, 0);
    lv_obj_set_style_bg_color(gig_right, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_opa(gig_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gig_right, 1, 0);
    lv_obj_set_style_border_color(gig_right, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_radius(gig_right, RADIUS, 0);
    lv_obj_align(gig_right, LV_ALIGN_RIGHT_MID, -MARGIN, 0);
    s_gig_view = label(gig_right, &ui_font_14, TEXT);
    lv_label_set_text(s_gig_view, "PEDALS");
    lv_obj_set_style_text_letter_space(s_gig_view, 1, 0);
    lv_obj_set_style_pad_hor(s_gig_view, 16, 0);
    lv_obj_set_style_pad_ver(s_gig_view, 10, 0);
    lv_obj_set_style_radius(s_gig_view, RADIUS - 3, 0);
    lv_obj_set_style_border_width(s_gig_view, 0, 0);
    lv_obj_set_style_border_color(s_gig_view, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_color(s_gig_view, lv_color_hex(TEXT), 0);
    lv_obj_set_clickable(s_gig_view, true);
    lv_obj_set_ext_click_area(s_gig_view, 12);
    lv_obj_add_event_cb(s_gig_view, on_pedal_view, LV_EVENT_CLICKED, NULL);
    s_gig_mode = label(gig_right, &ui_font_14, TEXT);
    lv_obj_set_style_text_letter_space(s_gig_mode, 1, 0);
    lv_obj_set_style_pad_hor(s_gig_mode, 16, 0);
    lv_obj_set_style_pad_ver(s_gig_mode, 10, 0);
    lv_obj_set_style_radius(s_gig_mode, RADIUS - 3, 0);
    lv_obj_set_style_border_width(s_gig_mode, 0, 0);
    lv_obj_set_style_border_color(s_gig_mode, lv_color_hex(TEXT), 0);
    lv_obj_set_clickable(s_gig_mode, true);   // a tap: as the Scenes button of the top bar, which is hidden here
    lv_obj_set_ext_click_area(s_gig_mode, 14);
    lv_obj_add_event_cb(s_gig_mode, on_scene_button, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_gig_bar, true);

    build_chain(s_main);
    build_loopbar(s_main);
    build_playerbar(s_main);

    for (int i = 0; i < TILES; i++) build_tile(s_main, i);
    build_pedals(s_main);
    build_picker(screen);
    build_metro(screen);
    build_bank_editor(screen);
    build_looper_editor(screen);
    build_editor(screen);
    build_scene_editor(screen);
    build_expression(screen);
    build_tuner(screen);
    build_ask(screen);
    build_rename(screen);
    build_usb(screen);
    build_cab_settings(screen);
    build_amp(screen);
    build_mix_editor(screen);
    build_learn(screen);
    build_midi(screen);
    build_layout(screen);
    build_setlist(screen);
    build_presets(screen);
    build_system(screen);
    build_settings(screen);   // (after MIDI, USB and System: it takes them in)
    build_capture(screen);
    build_rotate(screen);
    build_scene_capture(screen);
    build_phone(screen);
    build_jam(screen);
    build_netlist(screen);
    build_drums(screen);
    build_level_sheet(screen);
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
    lv_obj_set_hidden(s_chain, on || s_looper_mode || s_player_mode);
    lv_obj_set_hidden(s_loopbar, on || !s_looper_mode);
    lv_obj_set_hidden(s_playerbar, on || !s_player_mode);
    lv_obj_set_hidden(s_gig_bar, !on);
    if (!on) s_pedals_shown = false, lv_obj_set_hidden(s_pedals, true);   // (the pedals belong to the full-screen view)
    for (int i = 0; i < s_full_dialog_count; i++) {   // the dialogs cover the tiles from their top edge (64) in the gig view
        lv_obj_set_y(s_full_dialogs[i], on ? FULL_TILE_Y : 72);
        lv_obj_set_height(s_full_dialogs[i], on ? DLG_H + 72 - FULL_TILE_Y : DLG_H);
    }
    for (int i = 0; i < TILES; i++) place_tile(i);
    fit_names();
    if (on && s_pedal_view) send('_', 1);   // the state once more: the pedals come up with it
}

void ui_set_app(bool connected)
{
    lvgl_port_lock(0);
    lv_obj_set_hidden(s_app, !connected);
    lvgl_port_unlock();
}

void ui_set_link_way(bool usb)
{
    lvgl_port_lock(0);
    s_link_usb = usb;
    lvgl_port_unlock();
}

void ui_set_link(bool connected)
{
    lvgl_port_lock(0);
    // (the settings text is set under the lock as well: set before it, from the task the link reports in, it ran
    // into the screen's own task now and then - on the P4 board that crashed every start, 2026-11-18)
    s_nano_connected = connected;
    if (s_set_nano_text) settings_nano_text();
    s_linked = connected;
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(connected ? GREEN : 0x555555), 0);
    lv_obj_set_style_bg_color(s_gig_dot, lv_color_hex(connected ? GREEN : 0x555555), 0);
    lv_obj_set_hidden(s_usb_mark, !connected || !s_link_usb);
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
        lv_obj_set_hidden(s_preset_chip, true);
        set_usb_enabled(false);   // (the settings screen stays: MIDI and the rest work without the Nano)
        lv_obj_set_hidden(s_cab_settings, true);
        lv_obj_set_hidden(s_amp, true);
        lv_obj_set_hidden(s_mix_editor, true);
        lv_obj_set_hidden(s_tuner, true);
        lv_obj_set_hidden(s_editor, true);
        lv_obj_set_hidden(s_picker, true);
        lv_obj_set_hidden(s_bank_editor, true);
        lv_obj_set_hidden(s_scene_editor, true);
        lv_obj_set_hidden(s_scene_cap, true);
        lv_obj_set_hidden(s_setlist, true);
        lv_obj_set_hidden(s_presets, true);
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
#define MODE_BLUE 0x4F8CFF
#define LOOPER_ORANGE 0xFF8A3D
static void show_fixed_tiles(const ui_view_t *view)
{
    // Tile 1: always MODE, the mode it is in large. (Bank, preset and song are in the top bar.)
    bool fx = view->fx_mode, scenes = !fx && view->scenes;
    // P4 (2026-11-27, draft): MODE in blue with the grid of the presets, LOOPER in orange with its loop - switch 2
    // pressed is the looper, held the tuner.
    set_tile(0, "MODE", fx ? "FX" : scenes ? "Scenes" : view->setlist_on && view->setlist_count ? "Songs" : "Presets",
             MODE_BLUE, false, false, UI_ICONS[UI_ICON_GRID], NULL);
    set_tile(1, "LOOPER", "Looper", LOOPER_ORANGE, false, false, NULL, LV_SYMBOL_LOOP);
    s_tile_note[0] = LV_SYMBOL_PLAY " Hold: Player";
    s_tile_note[1] = "Hold: Tuner";
    s_tile_hint[0] = s_tile_hint[1] = true;
}

static const char *const WAH_NOTES[2][UI_WAH_RATES] = {
    { "Auto: 2 bars", "Auto: 1 bar", "Auto: 1/2", "Auto: 1/4", "Auto: 1/8", "Auto: 1/16" },
    { "Hold: auto 2 bars", "Hold: auto 1 bar", "Hold: auto 1/2", "Hold: auto 1/4", "Hold: auto 1/8", "Hold: auto 1/16" },
};

static void show_fx_tiles(const nano_state_t *st, const ui_view_t *view)
{
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {
        const nano_fx_model_t *model = nano_fx_model(st->fx_type[slot]);
        bool empty = !st->fx_known || !st->fx_type[slot];
        bool on = !empty && st->fx_on[slot];
        // A second effect: the tile's colour says which one runs - each effect has its own, and B of the same effect
        // (a second setting of it) a lighter shade of it
        bool second = !empty && view->ab_b_type[slot], b_runs = second && ((view->ab_active >> slot) & 1);
        // (UI_AB_OFF, the reverb's slot: its switch only switches on and off - an ordinary tile; the reverb switch's
        // tile is the one in two halves)
        bool has_b = second && view->ab_mode[slot] != UI_AB_OFF;
        uint32_t color = model ? model->color : 0x6A6A6A;
        if (b_runs && view->ab_a_type[slot] == st->fx_type[slot]) color = blend(color, 0xFFFFFF, 0.5f);
        set_tile(2 + slot, NANO_FX_SLOT_NAMES[slot], empty ? "Empty" : nano_fx_name(st->fx_type[slot]),
                 color, on, empty, model ? NANO_ICONS[model->icon] : NULL, NULL);
        if (!empty) set_tile_pedal(2 + slot, st->fx_type[slot]);
        // ... and both symbols, A at the left and B at the right edge, the one that runs filled
        // The Nano's expression pedal moves or switches this effect (an assignment of the preset): said on the tile,
        // because what the pedal sets comes back whenever the Nano loads a model - whatever was set by hand
        bool pedal_amount = !empty && ((view->exp_amount >> slot) & 1), pedal_switch = !empty && ((view->exp_switch >> slot) & 1);
        if (has_b) {   // the tile in two halves: A above, B below, the one that runs in its colour
            uint32_t a_type = b_runs ? view->ab_a_type[slot] : st->fx_type[slot], b_type = view->ab_b_type[slot];
            const nano_fx_model_t *ma = nano_fx_model(a_type), *mb = nano_fx_model(b_type);
            const char *a_name = a_type ? nano_fx_name(a_type) : "A", *b_name = a_type == b_type ? "2nd setting" : nano_fx_name(b_type);
            set_tile(2 + slot, NANO_FX_SLOT_NAMES[slot], "", color, on, false, NULL, NULL);   // (the caption: for the editor)
            tile_split_t *sp = &s_tile_split[2 + slot];
            sp->shown = true;
            sp->on = on;
            sp->runs = b_runs ? 1 : 0;
            sp->color[0] = ma ? ma->color : 0x6A6A6A;
            sp->color[1] = mb ? (a_type == b_type ? blend(mb->color, 0xFFFFFF, 0.5f) : mb->color) : 0x6A6A6A;
            sp->icon[0] = ma ? NANO_ICONS[ma->icon] : NULL;
            sp->icon[1] = mb ? NANO_ICONS[mb->icon] : NULL;
            snprintf(sp->names[0], sizeof(sp->names[0]), "%s", a_name);
            snprintf(sp->names[1], sizeof(sp->names[1]), "%s", b_name);
            bool top_lit = !b_runs && on;   // the number sits on A's half
            set_color_prop(s_tile_number[2 + slot], LV_STYLE_TEXT_COLOR, top_lit ? ink_for(blend(sp->color[0], SURF, 0.80f)) : 0x9AA0A6);
            set_color_prop(s_tile[2 + slot], LV_STYLE_BG_COLOR, 0x0D0E10);
            set_color_prop(s_tile[2 + slot], LV_STYLE_BORDER_COLOR, 0x24272B);
        }
        // A wah with auto-wah: under the caption how it moves ("Hold": while its footswitch is held)
        else if (!empty && view->wah_mode[slot] && view->wah_rate[slot] == UI_WAH_FOLLOWS_RATE) {
            s_tile_note[2 + slot] = view->wah_mode[slot] == UI_WAH_HELD ? "Hold: auto, playing" : "Auto: playing";
        } else if (!empty && view->wah_mode[slot] && view->wah_rate[slot] < UI_WAH_RATES) {
            s_tile_note[2 + slot] = WAH_NOTES[view->wah_mode[slot] == UI_WAH_HELD][view->wah_rate[slot]];
        } else if (pedal_amount || pedal_switch) {
            s_tile_note[2 + slot] = pedal_amount && pedal_switch ? "Pedal, on/off" : pedal_amount ? "Pedal" : "Pedal: on/off";
            s_tile_note_icon[2 + slot] = UI_ICONS[UI_ICON_EXP];
        }
    }

    // Footswitch 8: one of two - the reverb's two settings, its A / B or its mix Pos 1 / Pos 2 - as rows on its tile.
    const lv_image_dsc_t *icon = NANO_ICONS[NANO_ICON_REVERB];
    if (view->rp_used && view->mix_slot >= 0) {
        // The switch between two settings of the reverb: the tile carries both names as rows, the one that is in
        // filled (draw_pair)
        int slot = view->mix_slot, n = view->rp_active == 1 ? 1 : 0;
        const nano_fx_model_t *model = nano_fx_model(st->fx_type[slot]);
        uint32_t color = model ? model->color : 0x00FFDD;
        bool neither = view->rp_active > 1;   // the reverb is set to something else: no row is filled, the next press = setting 1
        set_tile(7, "REVERB", "", color, st->fx_on[slot], false, model ? NANO_ICONS[model->icon] : icon, NULL);
        s_tile_pair[7].shown = true;
        s_tile_pair[7].on = st->fx_on[slot];
        s_tile_pair[7].active = neither ? -1 : n;
        s_tile_pair[7].color = color;
        for (int k = 0; k < 2; k++) {
            snprintf(s_tile_pair[7].names[k], sizeof(s_tile_pair[7].names[k]), "%s", view->rp_names[k][0] ? view->rp_names[k] : k ? "Setting 2" : "Setting 1");
        }
    } else if (view->rev_b_type && view->mix_slot >= 0) {
        // The switch between the two reverbs: one of two, as the two settings - the names as rows, the one that
        // runs filled. (The reverb's own tile is an ordinary on / off tile then; this one is the switch.)
        int slot = view->mix_slot;
        uint32_t now = st->fx_type[slot];
        const nano_fx_model_t *model = nano_fx_model(now);
        bool b_runs = view->rev_active == 1, on = now && st->fx_on[slot];
        uint32_t a_type = b_runs ? view->ab_a_type[slot] : now;
        uint32_t color = model ? model->color : 0x00FFDD;
        set_tile(7, "REV A / B", "", color, on, false, model ? NANO_ICONS[model->icon] : icon, NULL);
        s_tile_pair[7].shown = true;
        s_tile_pair[7].on = on;
        s_tile_pair[7].active = b_runs ? 1 : 0;
        s_tile_pair[7].color = color;
        snprintf(s_tile_pair[7].names[0], sizeof(s_tile_pair[7].names[0]), "A  %s", a_type ? nano_fx_name(a_type) : "-");
        snprintf(s_tile_pair[7].names[1], sizeof(s_tile_pair[7].names[1]), "B  %s", a_type == view->rev_b_type ? "2nd setting" : nano_fx_name(view->rev_b_type));
    } else if (view->rev_b_type) {
        set_tile(7, "REVERB", "No reverb", 0x00FFDD, false, true, icon, NULL);
    } else if (view->mix_slot < 0) {
        set_tile(7, "REV MIX", "No reverb", 0x00FFDD, false, true, icon, NULL);
    } else if (!view->mix_known) {
        set_tile(7, "REV MIX", "No Pos 1/2", 0x00FFDD, false, true, icon, NULL);
    } else {
        // The mix switch: Pos 1 and Pos 2 as the tile's halves with their mix, the one that is set lit (none: as the preset has it)
        int slot = view->mix_slot;
        const nano_fx_model_t *model = nano_fx_model(st->fx_type[slot]);
        uint32_t color = model ? model->color : 0x00FFDD;
        {   // the halves (as the A / B slots), each Pos with its per cent and a bar; the reverb's own tile follows
            int a = view->mix_active;
            set_tile(7, "REV MIX", "", color, st->fx_on[slot], false, NULL, NULL);
            tile_split_t *sp = &s_tile_split[7];
            sp->shown = true;
            sp->on = st->fx_on[slot];
            sp->runs = a == 1 ? 1 : 0;
            sp->fills = true;
            for (int k = 0; k < 2; k++) {
                sp->color[k] = k ? blend(color, 0xFFFFFF, 0.5f) : color;
                sp->icon[k] = model ? NANO_ICONS[model->icon] : icon;
                sp->fill[k] = view->mix_pos[k];
                sp->letters[k] = k ? '2' : '1';
                snprintf(sp->names[k], sizeof(sp->names[k]), "%d %%", (int)lroundf(view->mix_pos[k] * 100));
            }
            if (a != 0 && a != 1) sp->runs = -1;   // as the preset has it: neither half lit
            set_color_prop(s_tile_number[7], LV_STYLE_TEXT_COLOR, sp->runs == 0 && sp->on ? ink_for(blend(color, SURF, 0.80f)) : 0x9AA0A6);
            set_color_prop(s_tile[7], LV_STYLE_BG_COLOR, 0x0D0E10);
            set_color_prop(s_tile[7], LV_STYLE_BORDER_COLOR, 0x24272B);
            if (a == 0 || a == 1) {
                s_tile_fill[2 + slot] = view->mix_pos[a];
            }
        }
    }
    if (view->mix_slot >= 0 && view->mix_known) set_tile_pedal(7, st->fx_type[view->mix_slot]);
    if (s_tile_pair[7].shown) {   // the switch as a tile in two halves too (as the A / B slots)
        const tile_pair_t *pr = &s_tile_pair[7];
        tile_split_t sp = { .shown = true, .on = pr->on, .runs = pr->active == 1 ? 1 : 0 };
        for (int k = 0; k < 2; k++) {
            const char *nm = pr->names[k];
            bool lettered = nm[0] && nm[1] == ' ' && nm[2] == ' ';
            snprintf(sp.names[k], sizeof(sp.names[k]), "%s", lettered ? nm + 3 : nm);
            sp.icon[k] = NANO_ICONS[NANO_ICON_REVERB];
        }
        sp.color[0] = pr->color;
        sp.color[1] = view->rev_b_type && !view->rp_used ? (nano_fx_model(view->rev_b_type) ? nano_fx_model(view->rev_b_type)->color : pr->color) : blend(pr->color, 0xFFFFFF, 0.5f);
        set_text(s_tile_caption[7], "");
        set_text(s_tile_name[7], "");
        set_hidden(s_tile_square[7], true);
        s_tile_pair[7].shown = false;
        s_tile_split[7] = sp;
        set_color_prop(s_tile_number[7], LV_STYLE_TEXT_COLOR, sp.runs == 0 && sp.on ? ink_for(blend(sp.color[0], SURF, 0.80f)) : 0x9AA0A6);
        set_color_prop(s_tile[7], LV_STYLE_BG_COLOR, 0x0D0E10);   // (the halves carry the colour, as on the A / B slots)
        set_color_prop(s_tile[7], LV_STYLE_BORDER_COLOR, 0x24272B);
    }
}

// Looper mode: tile 1 is the mode (filled while the phone is connected), 2-8 the switches of the looper app with
// the name, colour and symbol the user gave them (long press; by default Pause and six loops in three colour pairs,
// as a two-column Loopy Pro project shows them). The caption is the controller a switch sends.
__attribute__((unused)) static void show_looper_tiles(const ui_view_t *view)   // (the phone looper of the small board)
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
    bool setlist = view->setlist_on && view->setlist_count;   // the songs of the set list: "SONG n" over the name
    for (int i = 0; i < 6; i++) {
        int preset = view->bank_presets[i], song = view->setlist_first + i;
        char caption[16] = "";
        if (setlist) snprintf(caption, sizeof(caption), "SONG %d", song + 1);
        if (preset < 1 || preset > NANO_PRESETS) {
            set_tile(2 + i, setlist ? "" : "EMPTY", setlist ? "-" : "Hold to set", 0x3A3A3A, false, true, NULL, NULL);
            continue;
        }
        const char *name = st->preset_names[preset - 1];
        uint32_t color = BANK_COLORS[view->bank_colors[i] < UI_BANK_COLOR_COUNT ? view->bank_colors[i] : 0];
        int icon = view->bank_icons[i];
        bool lit = setlist && view->setlist_song >= 0 ? song == view->setlist_song
                 : preset == st->current_preset || (view->bank_presets2[i] && view->bank_presets2[i] == st->current_preset);   // (its second preset is on: lit too)
        set_tile(2 + i, caption, st->names_loaded && name[0] ? name : "-", color, lit, false,
                 icon >= 1 && icon <= PRESET_ICON_COUNT ? PRESET_ICONS[icon - 1].image : NULL, NULL);
        int second = view->bank_presets2[i];
        if (!setlist && second >= 1 && second <= NANO_PRESETS) {   // under the caption: the second preset, held (P4)
            const char *n2 = st->preset_names[second - 1];
            snprintf(s_tile_note_text[2 + i], sizeof(s_tile_note_text[2 + i]), "Hold: %s", st->names_loaded && n2[0] ? n2 : "-");
            s_tile_note[2 + i] = s_tile_note_text[2 + i];
        }
        int scene = setlist && song < UI_SETLIST_MAX ? view->setlist_scenes[song] : 0;
        if (scene >= 1 && scene <= UI_SCENES) {   // under the caption: the scene the song starts with
            if (view->setlist_scene_names[i][0]) snprintf(s_tile_note_text[2 + i], sizeof(s_tile_note_text[2 + i]), "%s", view->setlist_scene_names[i]);
            else snprintf(s_tile_note_text[2 + i], sizeof(s_tile_note_text[2 + i]), "Scene %d", scene);
            s_tile_note[2 + i] = s_tile_note_text[2 + i];
            s_tile_note_icon[2 + i] = UI_ICONS[UI_ICON_SCENES];
        }
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

static void tiles_copy(void);

void ui_show_state(const nano_state_t *st, const ui_view_t *view)
{
    char text[96];
    lvgl_port_lock(0);

    bool looper_changed = s_looper_mode != view->looper || s_player_mode != (view->player && !view->looper)
                          || s_fx_mode != view->fx_mode;   // (FX mode has its own order)
    s_fx_mode = view->fx_mode;
    s_looper_mode = view->looper;
    s_player_mode = view->player && !view->looper;
    if (looper_changed) for (int i = 0; i < TILES; i++) place_tile(i);   // (the looper's fixed places, or the layout's)
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
    if (!lv_obj_is_hidden(s_mix_editor)) {   // the reverb dialog shows the two settings: one may just have been set or named
        s_view.rp_set = view->rp_set;
        memcpy(s_view.rp_names, view->rp_names, sizeof(s_view.rp_names));
        s_view.rp_fxp_count = view->rp_fxp_count;
        memcpy(s_view.rp_fxp_names, view->rp_fxp_names, sizeof(s_view.rp_fxp_names));
        show_rp_rows();
    }
    if (!lv_obj_is_hidden(s_editor)) {   // the FX editor's scene button and list say which scenes carry settings
        bool scenes_changed = memcmp(s_view.scene_set, view->scene_set, sizeof(s_view.scene_set)) || s_view.scene_active != view->scene_active
                           || memcmp(s_view.scene_names, view->scene_names, sizeof(s_view.scene_names));
        s_view = *view;
        style_scene_chip();
        if (scenes_changed && scene_panel_open()) lv_obj_invalidate(s_panel_list);
    }
    if (!view->looper && !s_player_mode) show_fixed_tiles(view);
    if (view->mix_slot >= 0) {
        snprintf(s_mix_model, sizeof(s_mix_model), "%s  -  %s", nano_fx_name(st->fx_type[view->mix_slot]),
                 NANO_FX_SLOT_NAMES[view->mix_slot]);
    }

    char where[24];   // the bank, or the song of the set list
    if (view->setlist_on && view->setlist_count && view->setlist_song >= 0) snprintf(where, sizeof(where), "SONG %d/%d", view->setlist_song + 1, view->setlist_count);
    else if (view->setlist_on && view->setlist_count) snprintf(where, sizeof(where), "SET LIST");
    else snprintf(where, sizeof(where), "BANK %d", view->bank + 1);
    snprintf(text, sizeof(text), "%d", st->current_preset);
    set_text(s_preset_chip_text, text);
    set_hidden(s_preset_chip, false);
    snprintf(text, sizeof(text), "%s", view->not_loaded ? "NOT LOADED" : where);
    set_color_prop(s_preset_number, LV_STYLE_TEXT_COLOR, view->not_loaded ? ORANGE : 0xB9BDC2);
    set_color_prop(s_preset_chip, LV_STYLE_BG_COLOR, view->not_loaded ? 0x6A4410 : 0x24282D);
    set_text(s_preset_number, text);
    set_hidden(s_preset_number, false);
    set_hidden(s_status, true);    // the green dot says "connected"
    // Scenes button: filled while the tiles show scenes (as every switch of the editor: filled = on)
    bool scenes_shown = view->scenes && !view->fx_mode && !view->looper;
    set_color_prop(s_scene_button, LV_STYLE_BG_COLOR, scenes_shown ? 0xECECEC : SURF);
    set_color_prop(lv_obj_get_child(s_scene_button, 0), LV_STYLE_IMAGE_RECOLOR, scenes_shown ? INK : TEXT);
    set_color_prop(lv_obj_get_child(s_scene_button, 1), LV_STYLE_TEXT_COLOR, scenes_shown ? INK : CAPTION);
    // Expression pedal: green while it does something in this preset (as MIDI while a controller is connected)
    set_color_prop(lv_obj_get_child(s_exp_button, 0), LV_STYLE_IMAGE_RECOLOR, view->exp_used ? GREEN : TEXT);
    // Save: neutral, green when there is something to save (as in the editor), plus the orange dot by the name.
    set_color_prop(s_save_button, LV_STYLE_BG_COLOR, st->dirty ? GREEN : SURF);
    set_color_prop(lv_obj_get_child(s_save_button, 0), LV_STYLE_IMAGE_RECOLOR, st->dirty ? INK : TEXT);
    set_color_prop(lv_obj_get_child(s_save_button, 1), LV_STYLE_TEXT_COLOR, st->dirty ? INK : CAPTION);
    const char *name = st->preset_names[st->current_preset - 1];
    const char *shown = st->names_loaded && name[0] ? name : "-";
    set_text(s_preset_name, shown);
    lv_point_t name_size;
    lv_text_get_size(&name_size, shown, &ui_font_title_54, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int half = (name_size.x < PRESET_NAME_W ? name_size.x : PRESET_NAME_W) / 2;
    set_num_prop(s_edited, LV_STYLE_X, SCREEN_W / 2 + half + 12);
    set_num_prop(s_edited, LV_STYLE_Y, 107);
    set_hidden(s_edited, !st->dirty || s_player_mode);
    set_hidden(s_preset_name, s_player_mode);   // player mode: its stage takes the preset's row
    set_hidden(s_prev_box, s_player_mode);
    set_hidden(s_next_box, s_player_mode);

    // Gig view bar
    snprintf(text, sizeof(text), "%d", st->current_preset);
    set_text(s_gig_number, text);
    set_text(s_gig_bank, where);
    set_text(s_gig_name, shown);
    lv_text_get_size(&name_size, shown, &ui_font_title_26, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    half = (name_size.x < 520 ? name_size.x : 520) / 2;
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
    (void)KIND_NAMES;   // (P4: the chain field is too narrow for the type; the capture dialog shows it)
    snprintf(text, sizeof(text), "CAPTURE");
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
    show_chain(st);
    // Capture volume: not while the slider is held or right after a change on the board (older replies).
    if (lv_obj_is_hidden(s_amp) ||
        (lv_tick_elaps(s_volume_tick) > VOLUME_HOLD_MS && !lv_obj_has_state(s_level_slider[0], LV_STATE_PRESSED))) {
        s_capture_volume = st->capture_volume;
        if (!lv_obj_is_hidden(s_amp)) show_capture_volume();
    }
    bool amp_held = false;
    for (int i = 0; i < AMP_ROWS; i++) amp_held |= lv_obj_has_state(s_level_slider[AMP_ROW + i], LV_STATE_PRESSED);
    if (lv_obj_is_hidden(s_amp) || (lv_tick_elaps(s_amp_tick) > VOLUME_HOLD_MS && !amp_held)) {
        memcpy(s_amp_values, st->amp, sizeof(s_amp_values));
        if (!lv_obj_is_hidden(s_amp)) show_amp_values();
    }

    if (s_rec_mark) set_hidden(s_rec_mark, !view->looper);
    set_hidden(s_chain, s_fullscreen || view->looper || s_player_mode);
    set_hidden(s_loopbar, s_fullscreen || !view->looper);
    set_hidden(s_playerbar, s_fullscreen || !s_player_mode);
    if (view->looper) {   // P4: the looper on the board
        show_onboard_looper_tiles();
        if (!lv_obj_is_hidden(s_loopbar)) show_loopbar();
    } else if (s_player_mode) {   // P4: the player's switches
        show_player_tiles();
        if (!lv_obj_is_hidden(s_playerbar)) show_playerbar();
    }
    else if (view->fx_mode) show_fx_tiles(st, view);
    else if (view->scenes) show_scene_tiles(view);
    else show_preset_tiles(st, view);
    show_tile_extras();
    fit_names();
    // The pedal view: its field in the full-screen bar is filled while it is chosen; the pedals take the tiles' place
    // in FX mode
    // (the field only where the pedals can be shown - FX mode, not the looper: in preset mode it did nothing)
    set_hidden(s_gig_view, !view->fx_mode || view->looper);
    set_num_prop(s_gig_view, LV_STYLE_BG_OPA, s_pedal_view ? LV_OPA_COVER : LV_OPA_TRANSP);
    set_color_prop(s_gig_view, LV_STYLE_TEXT_COLOR, s_pedal_view ? INK : TEXT);
    bool pedals = s_pedal_view && s_fullscreen && view->fx_mode && !view->looper;
    if (pedals) show_pedals(st, view);
    pedals_show(pedals);
    tiles_copy();   // (for the editor's strip: ui_tiles_get)

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

#define ED_BAR_W ((SCREEN_W - 2 * MARGIN - 12) / 2)   // two columns over the screen (494 on the P4)
#define ED_BAR_H 64
#define ED_BAR_PITCH 72
#define ED_BODY_Y 148
#define ED_DRAG_START 8       // px sideways before a bar follows the finger
#define ED_PANEL_W 900
#define ED_PANEL_H 500
#define ED_ENTRY_H 64         // panel entries: large enough to hit on stage
#define ED_ENTRY_W 434        // model entry: two columns in the panel
#define ED_OPTION_W 214       // option entry: four columns
#define ED_DIM 0.42f          // frame of a field that is not active: effect colour mixed with the background
#define ED_NEUTRAL 0x2E3237   // frame of an empty field or an unknown value

enum { PANEL_MODEL, PANEL_SECOND, PANEL_OPTION, PANEL_SCENE };
#define PANEL_AB_MODE(m) (-1 - (m))   // entries of PANEL_SECOND that are not models: what the footswitch does (UI_AB_*)
#define PANEL_IS_AB_MODE(data) ((data) < 0 && (data) >= PANEL_AB_MODE(UI_AB_OFF))

static float param_from_normalized(const nano_param_t *p, float n);
static int option_index(const nano_param_t *p, float n);

// The rows of the editor are the model's parameters; a wah gets the auto-wah settings of the preset after them
// (rows as parameters: the same bars and switches, but their values go to the controller, not to the Nano).
static const nano_param_t WAH_ROWS[UI_WAH_ROWS] = {
    [UI_WAH_MODE] = { "Auto-wah", NANO_PARAM_ENUM, 0, 0, 0, 1, "", 0, 3, "Off\nOn\nHold" },
    [UI_WAH_RATE] = { "Rate", NANO_PARAM_ENUM, 0, 0, 0, 1, "", 0, UI_WAH_RATES, "2 bars\n1 bar\n1/2\n1/4\n1/8\n1/16" },
    [UI_WAH_SHAPE] = { "Shape", NANO_PARAM_ENUM, 0, 0, 0, 1, "", 0, UI_WAH_SHAPES, "Triangle\nSine\nUp" },
    [UI_WAH_HEEL] = { "Heel", NANO_PARAM_RANGE, 0, 100, 1, 1, "%", 0, 0, NULL },
    [UI_WAH_TOE] = { "Toe", NANO_PARAM_RANGE, 0, 100, 1, 1, "%", 0, 0, NULL },
    [UI_WAH_TEMPO] = { "Tempo", NANO_PARAM_ENUM, 0, 0, 0, 1, "", 0, 2, "Nano\nOwn" },
    [UI_WAH_BPM] = { "BPM", NANO_PARAM_RANGE, UI_WAH_BPM_MIN, UI_WAH_BPM_MAX, 1, 1, "", 0, 0, NULL },
    [UI_WAH_FOLLOWS] = { "Follows", NANO_PARAM_ENUM, 0, 0, 0, 1, "", 0, 2, "Tempo\nPlaying" },
    [UI_WAH_SENS] = { "Sensitivity", NANO_PARAM_RANGE, 0, 100, 1, 1, "%", 0, 0, NULL },
    [UI_WAH_ATTACK] = { "Attack", NANO_PARAM_RANGE, 5, 200, 1, 1, "ms", 0, 0, NULL },
    [UI_WAH_RELEASE] = { "Release", NANO_PARAM_RANGE, 20, 1000, 1, 1, "ms", 0, 0, NULL },
};
// The rows shown: in the tempo the round's settings, following the playing its sensitivity and times
// (2026-11-27, drafts/entwurf-p4-16-autowah-spiel-b.png).
static const uint8_t WAH_TEMPO_ROWS[] = { UI_WAH_MODE, UI_WAH_FOLLOWS, UI_WAH_RATE, UI_WAH_HEEL, UI_WAH_TOE, UI_WAH_SHAPE, UI_WAH_TEMPO, UI_WAH_BPM };   // (heel and toe side by side)
static const uint8_t WAH_PLAY_ROWS[] = { UI_WAH_MODE, UI_WAH_FOLLOWS, UI_WAH_SENS, UI_WAH_HEEL, UI_WAH_TOE, UI_WAH_ATTACK, UI_WAH_RELEASE };
static ui_autowah_t s_wah = { .rate = 3, .toe = 100, .bpm = 120, .sens = 60, .attack_ms = 15, .release_ms = 250 };
static int wah_rows(const uint8_t **list)
{
    *list = s_wah.follows ? WAH_PLAY_ROWS : WAH_TEMPO_ROWS;
    return s_wah.follows ? (int)sizeof(WAH_PLAY_ROWS) : (int)sizeof(WAH_TEMPO_ROWS);
}
static int wah_row_id(int pos)   // the row at a place below the wah's own parameter
{
    const uint8_t *l;
    int n = wah_rows(&l);
    return pos >= 0 && pos < n ? l[pos] : UI_WAH_MODE;
}

static int ed_model_params(void)
{
    const nano_fx_model_t *m = s_editor_model_def;
    return !m ? 0 : m->param_count < NANO_MAX_PARAMS ? m->param_count : NANO_MAX_PARAMS;
}

static int ed_param_count(void)
{
    const nano_fx_model_t *m = s_editor_model_def;
    int count = ed_model_params();
    const uint8_t *l;
    int rows = wah_rows(&l);
    return m && m->icon == NANO_ICON_WAH && count + rows <= NANO_MAX_PARAMS ? count + rows : count;
}

static const nano_param_t *ed_param(int idx)
{
    int count = ed_model_params();
    return idx < count ? &s_editor_model_def->params[idx] : &WAH_ROWS[wah_row_id(idx - count)];
}

// Value of an auto-wah row as the bars hold it (0-1).
static float wah_row_norm(int row)
{
    const nano_param_t *p = &WAH_ROWS[row];
    float bpm = s_wah.own_tempo || s_wah.nano_bpm < UI_WAH_BPM_MIN ? (float)s_wah.bpm : s_wah.nano_bpm;
    float v = row == UI_WAH_MODE ? s_wah.mode : row == UI_WAH_RATE ? s_wah.rate : row == UI_WAH_SHAPE ? s_wah.shape
            : row == UI_WAH_HEEL ? s_wah.heel : row == UI_WAH_TOE ? s_wah.toe : row == UI_WAH_TEMPO ? s_wah.own_tempo
            : row == UI_WAH_FOLLOWS ? s_wah.follows : row == UI_WAH_SENS ? s_wah.sens : row == UI_WAH_ATTACK ? s_wah.attack_ms
            : row == UI_WAH_RELEASE ? s_wah.release_ms : bpm;
    float max = p->kind == NANO_PARAM_ENUM ? (float)(p->option_count - 1) : p->max, min = p->kind == NANO_PARAM_ENUM ? 0 : p->min;
    float n = max > min ? (v - min) / (max - min) : 0;
    return n < 0 ? 0 : n > 1 ? 1 : n;
}

// A row changed by the finger: the Nano gets a parameter, the controller an auto-wah setting.
static void row_changed(int idx, float n)
{
    int count = ed_model_params();
    if (idx < count) {
        if (s_on_param) s_on_param(s_editor_slot_index, idx, n);
        return;
    }
    int id = wah_row_id(idx - count);
    const nano_param_t *p = &WAH_ROWS[id];
    int value = p->kind == NANO_PARAM_ENUM ? option_index(p, n) : (int)lroundf(param_from_normalized(p, n));
    send('|', id << 16 | (value & 0xFFFF));
}

static void on_fxp_chip(lv_event_t *e);
static void on_fxp_chip_long(lv_event_t *e);
static void on_scene_chip(lv_event_t *e);

// The Nano's values run 0-1 along a parameter's control; many controls are not linear (frequencies, times): the
// Nano's value is the share of the range to the power of the parameter's skew. Shown linearly, a high pass at its
// default of 200 Hz read 394 Hz.
static float param_from_normalized(const nano_param_t *p, float n)
{
    if (p->skew > 0 && p->skew != 1) n = powf(n < 0 ? 0 : n, 1.0f / p->skew);
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

static void editor_leave(void)
{
    s_info_on = false;
    lv_obj_set_hidden(s_info_page, true);
    s_padv.leave = s_padv.prefer = s_padv.page = false;
    s_padv.waiting = 0;
    drop_panel();
    lv_obj_set_hidden(s_editor, true);
    update_main_hidden();
    s_editor_slot_index = -1;
    send('E', 0);
}

static void on_editor_back(lv_event_t *e)
{
    if (s_info_on) {   // from the info page: back to the bars
        info_set(false);
        return;
    }
    if (s_padv.waiting) s_padv.leave = true;   // the pad's last values go first (pad_send closes the editor then)
    else editor_leave();
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

// A | B of the edited slot: the other one swaps (as its footswitch does); the running one - or B while there is no
// second effect - chooses the second effect and what the footswitch does with it.
static void on_ab(lv_event_t *e)
{
    int seg = (int)(intptr_t)lv_event_get_user_data(e), slot = s_editor_slot_index;
    if (slot < 0) return;
    int running = s_view.ab_b_type[slot] && ((s_view.ab_active >> slot) & 1) ? 1 : 0;
    if (s_view.ab_b_type[slot] && seg != running) send('[', slot);
    else {
        s_second_for_switch = false;
        open_panel(PANEL_SECOND, 0);
    }
}

static void on_panel_close(lv_event_t *e) { close_panel(); }

static bool scene_panel_open(void)
{
    return s_panel_mode == PANEL_SCENE && !lv_obj_is_hidden(s_panel);
}

static void set_param_norm(int idx, float n);

static void set_enum(int idx, int choice)
{
    const nano_param_t *p = ed_param(idx);
    if (choice == option_index(p, s_param_norm[idx])) return;
    float n = p->option_count > 1 ? (float)choice / (p->option_count - 1) : 0;
    set_param_norm(idx, n);
    row_changed(idx, n);
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
        int slot = s_editor_slot_index;
        if (slot < 0) return;
        if (PANEL_IS_AB_MODE(data)) send('>', slot << 8 | (int)(-1 - data));
        else if (s_second_for_switch && slot == s_view.mix_slot) {
            // Asked for by the reverb switch's dialog: footswitch 8 swaps this B (the reverb's own switch stays on / off)
            if ((uint32_t)data != s_view.ab_b_type[slot] || (data && !s_view.rev_b_type)) send('A', (int)data);
            if (data) send('-', 16);
        }
        else if ((uint32_t)data != s_view.ab_b_type[slot]) send('A', (8 + slot) << 24 | (int)data);
    } else if (s_editor_model_def && s_panel_param < ed_param_count()) {
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
    bool tap_choice = s_panel_mode == PANEL_SECOND && PANEL_IS_AB_MODE(data);
    bool option = s_panel_mode == PANEL_OPTION || tap_choice;
    const nano_fx_model_t *m = option || !data ? NULL : nano_fx_model((uint32_t)data);
    uint32_t color = option ? s_editor_color : m ? m->color : 0x9AA0A6;
    bool current = data == s_panel_current;
    if (tap_choice) current = (int)(-1 - data) == s_view.ab_mode[s_editor_slot_index];
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
        if (tap_choice) {
            static const char *const CHOICES[] = { "Tap: on / off  \xC2\xB7  hold: A / B", "Tap: A / B  \xC2\xB7  hold: on / off",
                                                   "Press: A / B at once  \xC2\xB7  no on / off",
                                                   "On / off only  \xC2\xB7  the reverb switch changes it" };
            draw_text(layer, CHOICES[-1 - data], false, &ui_font_16, ink, a.x1 + 4, cy - 11, a.x2 - 4, LV_TEXT_ALIGN_CENTER);
            return;
        }
        option_text(ed_param(s_panel_param), (int)data, text, sizeof(text));
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
    draw_text(layer, m ? nano_fx_name(m->type) : "None", false, &ui_font_16, ink, x, cy - 11, a.x2 - 10, LV_TEXT_ALIGN_LEFT);
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
    lv_obj_t *l = label(s_panel_list, CAPTION_FONT, MUTED);
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
    if (mode == PANEL_OPTION && (!cur || param >= ed_param_count())) return;
    int slot = s_editor_slot_index;
    if (slot < 0) return;
    char sub[120];
    // The model lists stay built while the editor shows this slot; an option list is built each time (it is short).
    if (mode == PANEL_OPTION || mode == PANEL_SCENE || mode == PANEL_SECOND || mode != s_panel_built_mode || slot != s_panel_built_slot) {
        lv_async_call_cancel(panel_clean_later, NULL);
        lv_obj_clean(s_panel_list);
        s_panel_built_mode = mode;
        s_panel_built_slot = slot;
        s_panel_mode = mode;
        s_panel_param = param;
        if (mode == PANEL_OPTION) {
            for (int i = 0; i < ed_param(param)->option_count; i++) panel_entry(ED_OPTION_W, i);
        } else if (mode == PANEL_SCENE) {
            for (int i = 0; i < UI_SCENES; i++) if (s_view.scene_names[i][0]) panel_entry(ED_ENTRY_W, i);
        } else {
            if (mode == PANEL_SECOND && s_view.ab_b_type[slot]) {   // what the footswitch does with A and B
                panel_heading("FOOTSWITCH");
                // (the reverb: its own switch only switches it on and off unless one of the others is chosen here)
                if (slot == s_view.mix_slot) panel_entry(ED_ENTRY_W, PANEL_AB_MODE(UI_AB_OFF));
                for (int m = UI_AB_HOLD; m <= UI_AB_PRESS; m++) panel_entry(ED_ENTRY_W, PANEL_AB_MODE(m));
                panel_heading("SECOND EFFECT");
            }
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
        const nano_param_t *p = ed_param(param);
        lv_label_set_text(s_panel_title, p->name);
        snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", nano_fx_name(cur->type), NANO_FX_SLOT_NAMES[s_editor_slot_index]);
        s_panel_current = option_index(p, s_param_norm[param]);
    } else if (mode == PANEL_SCENE) {
        lv_label_set_text(s_panel_title, "Settings for a scene");
        snprintf(sub, sizeof(sub), "Tap a scene: it sets %.24s as it is now  \xC2\xB7  hold: remove", cur ? cur->name : "this effect");
        s_panel_current = -1;
    } else if (mode == PANEL_SECOND) {
        lv_label_set_text(s_panel_title, "Second effect (B)");
        bool reverb = slot == s_view.mix_slot, waits = s_view.ab_b_type[slot] && s_view.ab_mode[slot] == UI_AB_OFF;
        if (reverb && (s_second_for_switch || (waits && s_view.rev_b_type))) {
            snprintf(sub, sizeof(sub), "%.12s  \xC2\xB7  footswitch 8 swaps A and B - the same reverb again gives it two settings",
                     NANO_FX_SLOT_NAMES[slot]);
        } else if (reverb && waits) {
            snprintf(sub, sizeof(sub), "%.12s  \xC2\xB7  B is kept for later: choose below how this switch swaps it",
                     NANO_FX_SLOT_NAMES[slot]);
        } else {
            snprintf(sub, sizeof(sub), "%.12s  \xC2\xB7  its footswitch swaps A and B - the same effect again gives it two settings",
                     NANO_FX_SLOT_NAMES[slot]);
        }
        s_panel_current = (intptr_t)s_view.ab_b_type[slot];
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
    // (the second effect's list stays at its top: what the footswitch does, and None)
    for (uint32_t i = 0; mode != PANEL_SECOND && i < lv_obj_get_child_count(s_panel_list); i++) {
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
    const nano_param_t *p = ed_param(idx);
    s_param_norm[idx] = n;
    s_spec_eq_dirty = true;
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
    const nano_param_t *p = ed_param(idx);
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
        // Over the name (a low value): the line above and below the name, not through its letters
        lv_point_t name_size;
        lv_text_get_size(&name_size, p->name, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        bool over_name = glow.x2 >= a.x1 + 16 - 2 && glow.x1 <= a.x1 + 16 + name_size.x + 2;
        lv_draw_rect_dsc_init(&r);   // a soft glow without a shadow (shadows are slow to draw)
        r.bg_color = lv_color_hex(color);
        for (int part = 0; part < (over_name ? 2 : 1); part++) {
            lv_area_t g = glow, l = edge;
            if (over_name) {   // (the name's line: cy - 11 to cy + 11)
                if (part == 0) g.y2 = l.y2 = cy - 14;
                else g.y1 = l.y1 = cy + 13;
                if (l.y1 > l.y2) continue;
            }
            r.bg_opa = LV_OPA_30;
            r.radius = 5;
            lv_draw_rect(layer, &r, &g);
            r.bg_opa = LV_OPA_COVER;
            r.radius = 2;
            lv_draw_rect(layer, &r, &l);
        }
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
            draw_text(layer, p->unit, false, HINT_FONT, MUTED, right - s_param_unit_w[idx] - 2, cy - 7, right, LV_TEXT_ALIGN_RIGHT);
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
    const nano_param_t *p = ed_param(idx);
    float v = param_from_normalized(p, n < 0 ? 0 : n > 1 ? 1 : n);   // snap to the parameter's step
    n = p->max > p->min ? (v - p->min) / (p->max - p->min) : 0;
    if (p->skew > 0 && p->skew != 1) n = powf(n < 0 ? 0 : n, p->skew);   // (back to the Nano's 0-1)
    if (fabsf(n - s_param_norm[idx]) < 0.0001f) return;
    set_param_norm(idx, n);
    row_changed(idx, n);
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
    if (!s_editor_model_def || idx >= ed_param_count()) return;
    if (code == LV_EVENT_DRAW_MAIN_END) {
        draw_bar(bar, idx, lv_event_get_layer(e));
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST
        && code != LV_EVENT_CLICKED) return;
    const nano_param_t *p = ed_param(idx);
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
// ---- the gain reduction of a compressor, as a row below its knobs (2026-11-27) ----
// The Nano does not report it; the board works it out (p4_audio.c) and the loop source brings it: a bar from the right
// (0 to -20 dB) in the effect's colour, the peak held as a white mark, and where the level came from.
#define GR_RANGE_DB 20.0f
static lv_obj_t *s_gr_fill, *s_gr_peak, *s_gr_value, *s_gr_note;
static int s_gr_bar_x, s_gr_bar_w;

static void build_gr_row(int row)
{
    const int W = 2 * ED_BAR_W + 12;
    s_gr_row = lv_obj_create(s_editor_body);
    lv_obj_remove_style_all(s_gr_row);
    lv_obj_set_size(s_gr_row, W, ED_BAR_H);
    lv_obj_set_pos(s_gr_row, MARGIN, row * ED_BAR_PITCH);
    lv_obj_set_scrollable(s_gr_row, false);
    lv_obj_set_style_radius(s_gr_row, RADIUS, 0);
    lv_obj_set_style_border_width(s_gr_row, 2, 0);
    lv_obj_set_style_border_color(s_gr_row, lv_color_hex(blend(s_editor_color, TILE_OFF_BG, ED_DIM)), 0);
    lv_obj_set_style_bg_color(s_gr_row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_gr_row, LV_OPA_COVER, 0);
    lv_obj_t *c = label(s_gr_row, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, "GAIN REDUCTION \xC2\xB7 ESTIMATED");
    lv_obj_set_pos(c, 16, 10);
    s_gr_note = label(s_gr_row, &ui_font_12, DIMMED);
    lv_obj_set_pos(s_gr_note, 16, 34);
    s_gr_bar_x = 300;
    s_gr_bar_w = W - 4 - s_gr_bar_x - 130;
    lv_obj_t *track = lv_obj_create(s_gr_row);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, s_gr_bar_w, 14);
    lv_obj_set_pos(track, s_gr_bar_x, (ED_BAR_H - 4 - 14) / 2);
    lv_obj_set_style_radius(track, 7, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x1B1E21), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    for (int k = 1; k < 4; k++) {   // -5, -10, -15 dB
        lv_obj_t *t = lv_obj_create(track);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 1, 14);
        lv_obj_set_pos(t, s_gr_bar_w - s_gr_bar_w * k / 4, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x2C3035), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    }
    s_gr_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_gr_fill);
    lv_obj_set_size(s_gr_fill, 0, 14);
    lv_obj_set_align(s_gr_fill, LV_ALIGN_RIGHT_MID);
    lv_obj_set_style_bg_color(s_gr_fill, lv_color_hex(s_editor_color), 0);
    lv_obj_set_style_bg_opa(s_gr_fill, LV_OPA_COVER, 0);
    s_gr_peak = lv_obj_create(track);
    lv_obj_remove_style_all(s_gr_peak);
    lv_obj_set_size(s_gr_peak, 2, 14);
    lv_obj_set_style_bg_color(s_gr_peak, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_gr_peak, LV_OPA_COVER, 0);
    lv_obj_set_hidden(s_gr_peak, true);
    s_gr_value = label(s_gr_row, &ui_font_title_22, TEXT);
    lv_obj_set_width(s_gr_value, 104);
    lv_obj_set_style_text_align(s_gr_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_gr_value, LV_ALIGN_RIGHT_MID, -18, 0);
}

// The compressor in the editor as numbers (as the desktop editor's duoCompModel): threshold, ratio, a soft knee of
// 6 dB, attack and release in seconds, make-up. Legendary 87 has no threshold - more input, more compression, like
// the original - so its threshold is taken from Input (a guess). false: a value is not known yet.
typedef struct { float thr, ratio, knee, attack, release, makeup; } gr_comp_t;

static bool gr_param(const char *name, float *value, const char **text)
{
    const nano_fx_model_t *m = s_editor_model_def;
    for (int i = 0; m && i < m->param_count && i < NANO_MAX_PARAMS; i++) {
        const nano_param_t *p = &m->params[i];
        if (strcasecmp(p->name, name)) continue;
        if (s_param_norm[i] < 0) return false;
        if (p->kind == NANO_PARAM_ENUM) {   // the option's text ("4", "All")
            static char opt[16];
            int k = option_index(p, s_param_norm[i]);
            const char *o = p->options;
            for (int j = 0; j < k && o; j++) { o = strchr(o, '\n'); if (o) o++; }
            size_t n = o ? strcspn(o, "\n") : 0;
            snprintf(opt, sizeof(opt), "%.*s", (int)n, o ? o : "");
            *text = opt;
            *value = strtof(opt, NULL);
        } else {
            *value = param_from_normalized(p, s_param_norm[i]);
            *text = p->unit ? p->unit : "";
        }
        return true;
    }
    return false;
}

static bool gr_comp(gr_comp_t *c)
{
    float v;
    const char *t;
    c->knee = 6;
    if (gr_param("Threshold", &v, &t)) c->thr = v;
    else if (gr_param("Input", &v, &t)) c->thr = -10 - (v + 24);
    else return false;
    if (!gr_param("Ratio", &v, &t)) return false;
    c->ratio = !strcasecmp(t, "All") || !strncasecmp(t, "inf", 3) ? 20 : v > 1 ? v : 4;
    c->attack = gr_param("Attack", &v, &t) ? (!strcmp(t, "ms") ? v / 1000 : v) : 0.01f;
    c->release = gr_param("Release", &v, &t) ? (!strcmp(t, "ms") ? v / 1000 : v) : 0.25f;
    c->makeup = gr_param("Makeup", &v, &t) ? v : 0;
    c->thr = c->thr < -60 ? -60 : c->thr > 0 ? 0 : c->thr;
    c->ratio = c->ratio > 40 ? 40 : c->ratio;
    c->attack = c->attack < 0.0002f ? 0.0002f : c->attack > 1 ? 1 : c->attack;
    c->release = c->release < 0.02f ? 0.02f : c->release > 5 ? 5 : c->release;
    return true;
}

static float gr_curve(const gr_comp_t *c, float level)   // the level that comes out (dB)
{
    float over = level - c->thr;
    if (over <= -c->knee / 2) return level;
    if (over >= c->knee / 2) return c->thr + over / c->ratio;
    return level + (1 / c->ratio - 1) * (over + c->knee / 2) * (over + c->knee / 2) / (2 * c->knee);
}

// Every look (100 ms): the level the compressor sees - the dry input for one before the capture (a good estimate),
// the sound less its make-up for one after it (rough: the capture, the effects after it and the Level are in it);
// the peak for a fast compressor (attack up to 10 ms catches the pick), else the RMS - through its curve, following
// its attack and release; the peak held 1.5 s.
static struct { float gr, peak, in_shown; uint32_t peak_at, shown_at; } s_grv;

static void show_gr_row(void)
{
    if (!s_gr_row) return;
    const ui_loop_t *l = &s_loop;
    gr_comp_t c;
    bool model = gr_comp(&c), pre = s_editor_slot_index < 2;
    float in = -100;
    if (model && l->gr_levels) {
        int k = pre ? 0 : 1;
        in = c.attack <= 0.01f ? l->gr_peak_in_db[k] : l->gr_rms_db[k];
        if (!pre && in > -100) in -= c.makeup;
    }
    float target = model && in > -100 ? in - gr_curve(&c, in) : 0;
    if (target < 0) target = 0;
    float dt = 0.1f;
    if (model) s_grv.gr += (target - s_grv.gr) * (1 - expf(-dt / (target > s_grv.gr ? c.attack : c.release)));
    else s_grv.gr = 0;
    uint32_t now = lv_tick_get();
    if (s_grv.gr >= s_grv.peak) { s_grv.peak = s_grv.gr; s_grv.peak_at = now; }
    else if (now - s_grv.peak_at > 1500) s_grv.peak = fmaxf(s_grv.gr, s_grv.peak - 10 * dt);
    if (in >= s_grv.in_shown || now - s_grv.shown_at > 400) { s_grv.in_shown = in; s_grv.shown_at = now; }   // (held a moment to read)

    char text[64];
    bool live = model && l->gr_levels;
    float gr = live ? s_grv.gr : 0, peak = live ? s_grv.peak : 0;
    int w = (int)(s_gr_bar_w * (gr > GR_RANGE_DB ? 1 : gr / GR_RANGE_DB));
    set_num_prop(s_gr_fill, LV_STYLE_WIDTH, w);
    int px = (int)(s_gr_bar_w * (peak > GR_RANGE_DB ? 1 : peak / GR_RANGE_DB));
    set_hidden(s_gr_peak, !live || peak < 0.3f);
    set_num_prop(s_gr_peak, LV_STYLE_X, s_gr_bar_w - px - 1);
    if (!live) snprintf(text, sizeof(text), "\xE2\x80\x93");
    else if (gr < 0.05f) snprintf(text, sizeof(text), "0.0 dB");
    else snprintf(text, sizeof(text), "\xE2\x88\x92%.1f dB", gr);
    set_text(s_gr_value, text);
    if (!model) snprintf(text, sizeof(text), "READING ITS VALUES \xE2\x80\xA6");
    else if (!l->gr_levels) snprintf(text, sizeof(text), "NO SOUND FROM THE NANO YET");
    else {
        char inb[12], thr[12];
        if (s_grv.in_shown > -90) snprintf(inb, sizeof(inb), "%s%d", s_grv.in_shown < -0.5f ? "\xE2\x88\x92" : "", (int)lroundf(fabsf(s_grv.in_shown)));
        else snprintf(inb, sizeof(inb), "\xE2\x80\x93");
        snprintf(thr, sizeof(thr), "%s%d", c.thr < -0.5f ? "\xE2\x88\x92" : "", (int)lroundf(fabsf(c.thr)));
        snprintf(text, sizeof(text), "%s \xC2\xB7 IN %s / THR %s dB", pre ? "DI" : "\xE2\x89\x88 OUTPUT", inb, thr);
    }
    set_text(s_gr_note, text);
}

// Following the playing: a row below the settings - how loud the playing is now (the fill, 0 = quiet, full = loud
// for the sensitivity) over the heel-toe range, and where that puts the wah (the white mark and the number).
static lv_obj_t *s_env_row, *s_env_fill, *s_env_range, *s_env_mark, *s_env_value, *s_env_note;
static int s_env_bw;

static void build_env_row(int row)
{
    const int W = 2 * ED_BAR_W + 12, BX = 300;
    s_env_bw = W - 4 - BX - 130;
    s_env_row = lv_obj_create(s_editor_body);
    lv_obj_remove_style_all(s_env_row);
    lv_obj_set_size(s_env_row, W, ED_BAR_H);
    lv_obj_set_pos(s_env_row, MARGIN, row * ED_BAR_PITCH);
    lv_obj_set_scrollable(s_env_row, false);
    lv_obj_set_style_radius(s_env_row, RADIUS, 0);
    lv_obj_set_style_border_width(s_env_row, 2, 0);
    lv_obj_set_style_border_color(s_env_row, lv_color_hex(blend(s_editor_color, TILE_OFF_BG, ED_DIM)), 0);
    lv_obj_set_style_bg_color(s_env_row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_env_row, LV_OPA_COVER, 0);
    lv_obj_t *c = label(s_env_row, &ui_font_12, CAPTION);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, "YOUR PLAYING \xC2\xB7 WAH POSITION");
    lv_obj_set_pos(c, 16, 10);
    s_env_note = label(s_env_row, &ui_font_12, DIMMED);
    lv_obj_set_pos(s_env_note, 16, 34);
    lv_obj_t *track = lv_obj_create(s_env_row);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, s_env_bw, 14);
    lv_obj_set_pos(track, BX, (ED_BAR_H - 4 - 14) / 2);
    lv_obj_set_style_radius(track, 7, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x1B1E21), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    s_env_range = lv_obj_create(track);
    lv_obj_remove_style_all(s_env_range);
    lv_obj_set_size(s_env_range, 0, 14);
    lv_obj_set_style_bg_color(s_env_range, lv_color_hex(0x2C3035), 0);
    lv_obj_set_style_bg_opa(s_env_range, LV_OPA_COVER, 0);
    s_env_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_env_fill);
    lv_obj_set_size(s_env_fill, 0, 14);
    lv_obj_set_style_bg_color(s_env_fill, lv_color_hex(s_editor_color), 0);
    lv_obj_set_style_bg_opa(s_env_fill, LV_OPA_COVER, 0);
    s_env_mark = lv_obj_create(track);
    lv_obj_remove_style_all(s_env_mark);
    lv_obj_set_size(s_env_mark, 2, 14);
    lv_obj_set_style_bg_color(s_env_mark, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_env_mark, LV_OPA_COVER, 0);
    s_env_value = label(s_env_row, &ui_font_title_22, TEXT);
    lv_obj_set_width(s_env_value, 104);
    lv_obj_set_style_text_align(s_env_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_env_value, LV_ALIGN_RIGHT_MID, -18, 0);
}

static void show_env_row(void)
{
    if (!s_env_row) return;
    const ui_autowah_t *w = &s_wah;
    int heel = w->heel * s_env_bw / 100, toe = w->toe * s_env_bw / 100;
    if (toe < heel) { int t = toe; toe = heel; heel = t; }
    set_num_prop(s_env_range, LV_STYLE_X, heel);
    set_num_prop(s_env_range, LV_STYLE_WIDTH, toe - heel);
    float lv = w->live ? w->level : 0, pos = w->position < 0 ? 0 : w->position > 1 ? 1 : w->position;
    set_num_prop(s_env_fill, LV_STYLE_WIDTH, (int)(s_env_bw * (lv < 0 ? 0 : lv > 1 ? 1 : lv)));
    set_hidden(s_env_mark, !w->live);
    set_num_prop(s_env_mark, LV_STYLE_X, (int)(s_env_bw * pos) - 1);
    char text[16];
    if (w->live) snprintf(text, sizeof(text), "%.2f", (double)pos);
    else snprintf(text, sizeof(text), "\xE2\x80\x93");
    set_text(s_env_value, text);
    set_text(s_env_note, w->live ? "FROM THE DRY INPUT (DI)" : w->mode ? "SWITCH THE WAH ON TO SEE IT MOVE" : "AUTO-WAH IS OFF");
}

// ---- P4: the live spectrum in the EQ editor (draft 2026-11-28, drafts/eq-spectrum-2026-11-28) ----
// The Nano's sound (USB 3/4, after the whole chain) behind / beside the EQ's controls. One of three ways per build
// (EQ_SPECTRUM_STYLE; the screenshots draw all three):
//   A  a graph above the rows, as the desktop editor's EQ view (FabFilter Pro-Q): the sound as a soft grey shape, the
//      EQ's curve in white over it, its bands as points (filled = on, outlined = off); it stays while the rows scroll
//   B  third-octave bars in a strip below the rows (a classic analyser, no curve)
//   C  a slim graph below the rows (sound + curve) and HOLD: the sound of now kept as a line, to compare with while
//      the EQ changes
// What it costs the screen (REALTIME.md rule 6): only its own area is drawn anew - A 168 rows, B 56, C 112 - at most
// 15 times a second and only when a new spectrum came. A and C are one RGB565 picture made here column by column
// (spec_paint, PSRAM), so LVGL blits one image instead of drawing hundreds of lines. The analysis (main/p4_spectrum.c)
// runs only while this asks: no EQ editor, no spectrum, no work.
#define SPEC_STYLE_A 1
#define SPEC_STYLE_B 2
#define SPEC_STYLE_C 3
#ifndef EQ_SPECTRUM_STYLE
#define EQ_SPECTRUM_STYLE SPEC_STYLE_C
#endif
#define SPEC_MS 66                  // the screen asks ~15 times a second
#define SPEC_A_H 168                // A: the graph's height (above the rows)
#define SPEC_DOCK_H 112             // B, C: the strip's height (below the rows)
#define SPEC_W (SCREEN_W - 2 * MARGIN)
#define SPEC_HOLD_W 96              // C: the HOLD button beside the graph
#define SPEC_TOP_DB -12.0f          // the sound's own scale, as in the desktop editor: top and bottom edge in dBFS
#define SPEC_FLOOR_DB -96.0f
#define SPEC_EQ_SPAN 18.0f          // the curve's scale: +-18 dB
#define SPEC_COLOR 0xBECDE1         // the sound: a cool light grey
#define SPEC_EDGE 0xDCE4EE
#define SPEC_THIRDS 30              // B: third octaves, 20 Hz .. 16 kHz
#define SPEC_BAR_TOP_DB -6.0f       // B: its scale (a third octave holds four of the twelfths: ~6 dB more)
#define SPEC_BAR_FLOOR_DB -78.0f
#define SPEC_POINTS 11

static int s_spec_style = EQ_SPECTRUM_STYLE;   // 0: none (the editor as before)
static ui_spectrum_cb s_spec_get;
static lv_timer_t *s_spec_timer;

enum { EQB_PEAK, EQB_HP, EQB_LP, EQB_HS, EQB_LS, EQB_HPN, EQB_LPN };
typedef struct {
    uint8_t kind;                    // EQB_*
    bool on;
    float f, gain, q;
    int order;                       // Low-High Cut: the slope in steps of 6 dB per octave
    const char *label;               // NULL: a small point
} spec_band_t;

typedef struct {
    ui_spectrum_t data;              // the newest the analysis handed over
    uint32_t seq;
    bool asking, drawn_live, drawn_any, drawn_di;
    lv_obj_t *box, *img, *caption, *hold, *bars, *dot[SPEC_POINTS];
    lv_image_dsc_t dsc;
    uint16_t *px;                    // the picture (A, C): w x h RGB565
    uint16_t tpl[6 * SPEC_A_H];      // per row: the background of the three kinds of column, then each under the sound
    float eq_y[SPEC_W], spec_y[SPEC_W];
    uint8_t col_kind[SPEC_W];        // 0 plain, 1 a faint grid line, 2 a decade
    int w, h;
    bool eq_known;
    spec_band_t band[SPEC_POINTS];
    int bands;
    float out_db;                    // the EQ's Output
    float ref_db[UI_SPEC_BANDS];     // C: the sound held (HOLD)
    float drawn_db[UI_SPEC_BANDS];   // A, C: the sound as drawn last (less than half a dB moved: not drawn again)
    bool ref_on;
    float third_db[SPEC_THIRDS], peak_db[SPEC_THIRDS];   // B
    uint32_t peak_ms[SPEC_THIRDS];
} spec_view_t;
static spec_view_t *s_spec;          // made at the first EQ editor and kept (lv_malloc: PSRAM on the board)

static bool spec_is_eq(const nano_fx_model_t *m) { return m && (m->type == 4001 || m->type == 4003 || m->type == 4005); }
static bool spec_wanted(const nano_fx_model_t *m) { return s_spec_style && spec_is_eq(m); }

static bool spec_value(const char *name, float *v)
{
    const nano_fx_model_t *m = s_editor_model_def;
    for (int i = 0; m && i < m->param_count && i < NANO_MAX_PARAMS; i++) {
        if (strcmp(m->params[i].name, name)) continue;
        const nano_param_t *p = &m->params[i];
        if (s_param_norm[i] < 0) return false;
        *v = p->kind == NANO_PARAM_ENUM ? (float)option_index(p, s_param_norm[i]) : param_from_normalized(p, s_param_norm[i]);
        return true;
    }
    return false;
}

// The EQ's bands from its values - textbook filters, as the desktop editor's EQ view (the Nano's own shapes are
// not known; the HP / LP of Graphic 9 taken as 12 dB per octave)
static void spec_eq_read(spec_view_t *v)
{
    const nano_fx_model_t *m = s_editor_model_def;
    v->bands = 0;
    v->eq_known = false;
    v->out_db = 0;
    float a, b, c, d, e;
    if (!m) return;
    if (m->type == 4001) {   // Parametric 3: Peak, Hi Pass, Lo Pass, Hi Shelf, Lo Shelf
        static const uint8_t kinds[5] = { EQB_PEAK, EQB_HP, EQB_LP, EQB_HS, EQB_LS };
        static const char *const labels[3] = { "1", "2", "3" };
        for (int i = 0; i < 3; i++) {
            char n[5][12];
            static const char *const what[5] = { "Gain", "Freq", "Q", "Type", "Active" };
            for (int k = 0; k < 5; k++) snprintf(n[k], sizeof(n[k]), "%d %s", i + 1, what[k]);
            if (!spec_value(n[0], &a) || !spec_value(n[1], &b) || !spec_value(n[2], &c) || !spec_value(n[3], &d) || !spec_value(n[4], &e)) return;
            v->band[v->bands++] = (spec_band_t){ kinds[(int)d % 5], e >= 0.5f, b, a, c, 0, labels[i] };
        }
    } else if (m->type == 4005) {   // Graphic 9
        static const char *const names[9] = { "65Hz", "125Hz", "250Hz", "500hz", "1kHz", "2kHz", "4kHz", "8kHz", "16kHz" };
        static const float fc[9] = { 65, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
        if (!spec_value("HPF", &a) || !spec_value("LPF", &b)) return;
        v->band[v->bands++] = (spec_band_t){ EQB_HP, a > 19.5f, a, 0, 0.707f, 0, "HP" };
        for (int i = 0; i < 9; i++) {
            if (!spec_value(names[i], &c)) return;
            v->band[v->bands++] = (spec_band_t){ EQB_PEAK, true, fc[i], c, 1.41f, 0, NULL };
        }
        v->band[v->bands++] = (spec_band_t){ EQB_LP, b < 20000, b, 0, 0.707f, 0, "LP" };
    } else if (m->type == 4003) {   // Low-High Cut: Flat, -6 .. -48 dB per octave
        if (!spec_value("HPF Slope", &a) || !spec_value("HPF Freq", &b) || !spec_value("LPF Slope", &c) || !spec_value("LPF Freq", &d)) return;
        v->band[v->bands++] = (spec_band_t){ EQB_HPN, a > 0.5f, b, 0, 0, (int)lroundf(a), "HP" };
        v->band[v->bands++] = (spec_band_t){ EQB_LPN, c > 0.5f, d, 0, 0, (int)lroundf(c), "LP" };
    } else return;
    if (spec_value("Output", &a)) v->out_db = a;
    v->eq_known = true;
}

// What one band does at f, in dB
static float spec_band_db(const spec_band_t *b, float f)
{
    if (!b->on) return 0;
    float w = f / b->f, w2 = w * w;
    if (b->kind == EQB_HPN || b->kind == EQB_LPN) return -10 * log10f(1 + powf(b->kind == EQB_HPN ? 1 / w2 : w2, (float)b->order));
    float q = b->q < 0.05f ? 0.05f : b->q, iq2 = 1 / (q * q), d = (1 - w2) * (1 - w2);
    if (b->kind == EQB_HP) return 10 * log10f(w2 * w2 / (d + w2 * iq2) + 1e-12f);
    if (b->kind == EQB_LP) return 10 * log10f(1 / (d + w2 * iq2) + 1e-12f);
    float g = powf(10, b->gain / 40), g2 = g * g;
    if (b->kind == EQB_PEAK) return 10 * log10f((d + w2 * g2 * iq2) / (d + w2 * iq2 / g2));
    float mid = g * w2 * iq2, lo = (g - w2) * (g - w2) + mid, hi = (1 - g * w2) * (1 - g * w2) + mid;
    return b->gain / 2 + 10 * log10f(b->kind == EQB_LS ? lo / hi : hi / lo);
}

static inline float spec_f_of_x(float x, int w) { return 20.0f * powf(1000.0f, x / (float)(w - 1)); }
static inline float spec_x_of_f(float f, int w) { return log10f(f / 20.0f) / 3.0f * (float)(w - 1); }
static inline float spec_y_of_eq(float db, int h) { return h * 0.5f - db / SPEC_EQ_SPAN * (h * 0.5f - 10); }
static inline float spec_y_of_level(float db, int h)
{
    float t = (db - SPEC_FLOOR_DB) / (SPEC_TOP_DB - SPEC_FLOOR_DB);
    return h - (t < 0 ? 0 : t > 1 ? 1 : t) * (h - 6);
}
static inline uint16_t spec_565(uint32_t rgb) { return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F)); }
// fg over bg at a / 32
static inline uint16_t spec_mix(uint16_t bg, uint16_t fg, uint32_t a)
{
    uint32_t b = (bg | (uint32_t)bg << 16) & 0x07E0F81Fu, f = (fg | (uint32_t)fg << 16) & 0x07E0F81Fu;
    uint32_t r = ((f * a + b * (32 - a)) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(r | r >> 16);
}

// A line through one column from y0 to y1 (fractions: the ends partly covered)
static void spec_vline(uint16_t *col, int stride, int h, float y0, float y1, uint16_t color, uint32_t alpha)
{
    int a = (int)floorf(y0), b = (int)ceilf(y1);
    if (a < 0) a = 0;
    if (b > h) b = h;
    for (int y = a; y < b; y++) {
        float lo = y0 > y ? y0 : (float)y, hi = y1 < y + 1 ? y1 : (float)(y + 1), cover = hi - lo;
        if (cover <= 0) continue;
        uint32_t k = (uint32_t)(alpha * cover + 0.5f);
        if (k) col[y * stride] = spec_mix(col[y * stride], color, k);
    }
}

// The background of each row: the graph's dark gradient with its level lines, as plain, faint-line and decade columns,
// and each of them under the sound's shape (that fades from the top down)
static void spec_templates(spec_view_t *v)
{
    int h = v->h;
    bool big = s_spec_style == SPEC_STYLE_A;
    for (int y = 0; y < h; y++) {
        uint32_t row = blend(0x090A0C, 0x111418, (float)y / (h - 1));
        for (int db = -12; db <= 12; db += big ? 6 : 12) {
            if ((int)lroundf(spec_y_of_eq((float)db, h)) == y) row = blend(0xFFFFFF, row, db ? 0.05f : 0.13f);
        }
        uint32_t kinds[3] = { row, blend(0xFFFFFF, row, 0.035f), blend(0xFFFFFF, row, 0.09f) };
        float a = 0.27f - 0.22f * (float)y / (h - 1);   // the sound's shape: brighter at the top
        for (int k = 0; k < 3; k++) {
            v->tpl[k * h + y] = spec_565(kinds[k]);
            v->tpl[(3 + k) * h + y] = spec_565(blend(SPEC_COLOR, kinds[k], a));
        }
    }
    for (int x = 0; x < v->w; x++) v->col_kind[x] = 0;
    for (int dec = 10; dec <= 10000; dec *= 10) {
        for (int k = 1; k < 10; k++) {
            float f = (float)(dec * k);
            if (f < 20 || f > 20000 || (k > 1 && !big)) continue;
            int x = (int)lroundf(spec_x_of_f(f, v->w));
            if (x >= 0 && x < v->w) v->col_kind[x] = k == 1 ? 2 : 1;
        }
    }
}

// The EQ's curve per column and its points (when a value changed)
static void spec_eq_curve(spec_view_t *v)
{
    spec_eq_read(v);
    int w = v->w, h = v->h;
    if (!v->eq_known) {
        for (int x = 0; x < w; x++) v->eq_y[x] = NAN;
    } else {
        float prev = 0;
        for (int x = 0; x < w; x += 4) {   // (every fourth column, the ones between on a straight line)
            float f = spec_f_of_x((float)x, w), db = v->out_db;
            for (int i = 0; i < v->bands; i++) db += spec_band_db(&v->band[i], f);
            float y = spec_y_of_eq(db < -40 ? -40 : db > 40 ? 40 : db, h);
            v->eq_y[x] = y;
            for (int k = 1; k < 4 && x >= 4; k++) v->eq_y[x - 4 + k] = prev + (y - prev) * k / 4;
            prev = y;
        }
        for (int x = (w - 1) / 4 * 4 + 1; x < w; x++) v->eq_y[x] = prev;
    }
    for (int i = 0; i < SPEC_POINTS; i++) {
        lv_obj_t *dot = v->dot[i];
        if (!dot) continue;
        bool there = v->eq_known && i < v->bands;
        set_hidden(dot, !there);
        if (!there) continue;
        const spec_band_t *b = &v->band[i];
        bool level = b->kind == EQB_PEAK || b->kind == EQB_HS || b->kind == EQB_LS;
        float db = (level ? b->gain : 0) + v->out_db, f = b->f < 20 ? 20 : b->f > 20000 ? 20000 : b->f;
        int size = lv_obj_get_width(dot);
        int x = (int)lroundf(spec_x_of_f(f, w)) - size / 2;
        int y = (int)lroundf(spec_y_of_eq(db < -SPEC_EQ_SPAN ? -SPEC_EQ_SPAN : db > SPEC_EQ_SPAN ? SPEC_EQ_SPAN : db, h)) - size / 2;
        x = x < 2 ? 2 : x > w - size - 2 ? w - size - 2 : x;
        y = y < 2 ? 2 : y > h - size - 2 ? h - size - 2 : y;
        set_num_prop(dot, LV_STYLE_X, x);
        set_num_prop(dot, LV_STYLE_Y, y);
        bool on = b->on;   // filled = on, outlined = off
        set_color_prop(dot, LV_STYLE_BG_COLOR, on ? s_editor_color : 0x15181C);
        set_color_prop(dot, LV_STYLE_BORDER_COLOR, on ? 0x0A0B0D : s_editor_color);
        lv_obj_t *l = lv_obj_get_child(dot, 0);
        if (l) {
            set_text(l, b->label ? b->label : "");
            set_color_prop(l, LV_STYLE_TEXT_COLOR, on ? ink_for(s_editor_color) : s_editor_color);
        }
    }
}

// The picture: background, the sound (and the DI / the held sound as lines), the EQ's area and curve, round corners
static void spec_paint(spec_view_t *v)
{
    const int w = v->w, h = v->h;
    const ui_spectrum_t *d = &v->data;
    bool sound = d->live, di = d->live && d->has_di, ref = v->ref_on && s_spec_style == SPEC_STYLE_C;
    const float per_px = 12.0f * 9.965784f / (float)(w - 1);   // bands per column: 20 Hz .. 20 kHz is log2(1000) octaves
    float sm[UI_SPEC_BANDS];   // (a little smoothed over the bands, as the editor's soft line)
    for (int i = 0; i < UI_SPEC_BANDS; i++) {
        float l = d->out_db[i > 0 ? i - 1 : 0], r = d->out_db[i < UI_SPEC_BANDS - 1 ? i + 1 : i];
        sm[i] = 0.25f * l + 0.5f * d->out_db[i] + 0.25f * r;
    }
    for (int x = 0; x < w; x++) {
        float b = x * per_px;
        int i = (int)b;
        float t = b - i, db = i >= UI_SPEC_BANDS - 1 ? sm[UI_SPEC_BANDS - 1] : sm[i] + (sm[i + 1] - sm[i]) * t;
        v->spec_y[x] = sound ? spec_y_of_level(db, h) : (float)h;
    }
    const uint16_t edge = spec_565(SPEC_EDGE), white = spec_565(0xF4F5F6), fill = spec_565(s_editor_color),
                   di_color = spec_565(0x8C96A3), ref_color = spec_565(0xF4F5F6);
    float zero = spec_y_of_eq(v->out_db, h);
    for (int x = 0; x < w; x++) {
        uint16_t *col = v->px + x;
        const uint16_t *bg = v->tpl + v->col_kind[x] * h, *sp = v->tpl + (3 + v->col_kind[x]) * h;
        float yt = v->spec_y[x];
        int top = (int)ceilf(yt);
        if (top > h) top = h;
        for (int y = 0; y < top; y++) col[y * w] = bg[y];
        for (int y = top; y < h; y++) col[y * w] = sp[y];
        if (sound && yt < h - 0.5f) {   // the shape's edge: through the middle to each neighbour
            float yl = x ? 0.5f * (yt + v->spec_y[x - 1]) : yt, yr = x < w - 1 ? 0.5f * (yt + v->spec_y[x + 1]) : yt;
            float lo = fminf(yt, fminf(yl, yr)), hi = fmaxf(yt, fmaxf(yl, yr));
            if (top > 0) col[(top - 1) * w] = spec_mix(bg[top - 1], sp[top - 1], (uint32_t)((top - yt) * 32));
            spec_vline(col, w, h, lo - 0.6f, hi + 0.6f, edge, 12);
        }
        if (di || ref) {   // the DI (the sound before the chain) / the held sound: a thin line
            const float *src = di ? d->di_db : v->ref_db;
            float bb = x * per_px;
            int k = (int)bb;
            float dbl = k >= UI_SPEC_BANDS - 1 ? src[UI_SPEC_BANDS - 1] : src[k] + (src[k + 1] - src[k]) * (bb - k);
            float y = spec_y_of_level(dbl, h);
            if (!ref || (x / 5) % 2 == 0) spec_vline(col, w, h, y - 0.7f, y + 0.7f, ref ? ref_color : di_color, ref ? 22 : 16);
        }
        if (v->eq_known) {   // the EQ: its area to the output's line in the effect's colour, the curve in white
            float ye = v->eq_y[x];
            float a0 = fminf(ye, zero), a1 = fmaxf(ye, zero);
            for (int y = (int)a0; y < (int)a1 && y < h; y++) if (y >= 0) col[y * w] = spec_mix(col[y * w], fill, 8);
            float yl = x ? 0.5f * (ye + v->eq_y[x - 1]) : ye, yr = x < w - 1 ? 0.5f * (ye + v->eq_y[x + 1]) : ye;
            float lo = fminf(ye, fminf(yl, yr)), hi = fmaxf(ye, fmaxf(yl, yr));
            spec_vline(col, w, h, lo - 1.1f, hi + 1.1f, white, 32);
        }
    }
    // round corners (radius 10) against the black around the graph
    static const float inset[10] = { 5.64f, 3.76f, 2.67f, 1.92f, 1.34f, 0.89f, 0.54f, 0.29f, 0.11f, 0.02f };
    for (int r = 0; r < 10; r++) {
        int rows[2] = { r, h - 1 - r };
        for (int k = 0; k < 2; k++) {
            uint16_t *line = v->px + rows[k] * w;
            for (int x = 0; x < (int)ceilf(inset[r]) && x < w; x++) {
                float cover = inset[r] - x >= 1 ? 1 : inset[r] - x;
                line[x] = spec_mix(line[x], 0, (uint32_t)(cover * 32));
                line[w - 1 - x] = spec_mix(line[w - 1 - x], 0, (uint32_t)(cover * 32));
            }
        }
    }
}



static void spec_caption(spec_view_t *v)
{
    if (!v->caption) return;
    set_text(v->caption, !v->data.live ? "SPECTRUM \xC2\xB7 NO SOUND OVER USB"
                         : v->data.has_di ? "SPECTRUM \xC2\xB7 OUTPUT / DI" : "SPECTRUM \xC2\xB7 OUTPUT");
}

static void spec_tick(lv_timer_t *t)
{
    (void)t;
    spec_view_t *v = s_spec;
    if (v && v->box) set_hidden(v->box, lv_obj_is_hidden(s_editor_body));   // (the pad's page hides the rows: this too)
    bool shown = v && v->box && !lv_obj_is_hidden(s_editor) && !lv_obj_is_hidden(s_editor_body) && !s_info_on
                 && lv_obj_is_hidden(s_panel);
    if (!shown) {
        if (v && v->asking && s_spec_get) s_spec_get(false, NULL);
        if (v) v->asking = false;
        return;
    }
    v->asking = true;
    if (!s_spec_get || !s_spec_get(true, &v->data)) v->data.live = false;
    bool eq = s_spec_eq_dirty && s_spec_style != SPEC_STYLE_B;
    if (!eq && v->drawn_any && v->data.seq == v->seq && v->data.live == v->drawn_live) return;   // nothing new
    if (!eq && v->drawn_any && !v->data.live && !v->drawn_live) return;
    bool was_live = v->drawn_live;
    (void)was_live;
    v->seq = v->data.seq;
    v->drawn_live = v->data.live;
    v->drawn_any = true;
    spec_caption(v);
    if (!eq && v->data.live && was_live && v->data.has_di == v->drawn_di) {   // a steady sound (or the floor in a pause): nothing to draw
        float moved = 0;
        for (int i = 0; i < UI_SPEC_BANDS; i++) {
            float a = fmaxf(v->data.out_db[i], SPEC_FLOOR_DB), b = fmaxf(v->drawn_db[i], SPEC_FLOOR_DB);
            moved = fmaxf(moved, fabsf(a - b));
        }
        if (moved < 0.5f) return;
    }
    memcpy(v->drawn_db, v->data.out_db, sizeof(v->drawn_db));
    v->drawn_di = v->data.has_di;
    if (eq) {
        s_spec_eq_dirty = false;
        spec_eq_curve(v);
    }
    spec_paint(v);
    lv_obj_invalidate(v->img);
}

static void spec_stop(void)
{
    if (s_spec_timer) lv_timer_pause(s_spec_timer);
    if (s_spec && s_spec->asking && s_spec_get) s_spec_get(false, NULL);
    if (s_spec) s_spec->asking = false;
}

static void on_spec_hold(lv_event_t *e)
{
    (void)e;
    spec_view_t *v = s_spec;
    if (!v) return;
    v->ref_on = !v->ref_on && v->data.live;
    if (v->ref_on) memcpy(v->ref_db, v->data.out_db, sizeof(v->ref_db));
    style_frame_button(v->hold, s_editor_color, v->ref_on, false);
    set_color_prop(lv_obj_get_child(v->hold, 0), LV_STYLE_TEXT_COLOR, v->ref_on ? ink_for(s_editor_color) : TEXT);
    s_spec_eq_dirty = true;   // (drawn again at the next look)
}

static lv_obj_t *spec_dot(lv_obj_t *parent, int size, bool text)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_clickable(dot, false);
    lv_obj_set_scrollable(dot, false);
    if (text) lv_obj_center(label(dot, &ui_font_12, INK));
    return dot;
}

static lv_obj_t *spec_text(lv_obj_t *parent, const char *text, int x, int y, lv_text_align_t align)
{
    lv_obj_t *l = label(parent, &ui_font_12, 0x6B7178);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, 40);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_pos(l, align == LV_TEXT_ALIGN_CENTER ? x - 20 : align == LV_TEXT_ALIGN_RIGHT ? x - 40 : x, y);
    return l;
}

// The spectrum's box goes again, the rows get the whole body back (build_param_rows, before the rows are made)
static void spec_remove(void)
{
    if (s_spec && s_spec->box) lv_obj_delete(s_spec->box);
    if (s_spec) s_spec->box = NULL;
    if (lv_obj_get_y(s_editor_body) != ED_BODY_Y || lv_obj_get_height(s_editor_body) != SCREEN_H - ED_BODY_Y) {
        lv_obj_set_pos(s_editor_body, 0, ED_BODY_Y);
        lv_obj_set_size(s_editor_body, SCREEN_W, SCREEN_H - ED_BODY_Y);
    }
}

// The spectrum for the EQ editor (build_param_rows, after the rows): a box of its own above (A) or below (B, C) the
// rows, which scroll in a shorter body. It lies just above the body: the pad, the info page and the panels cover it.
static void spec_build(void)
{
    if (!s_spec) {
        s_spec = lv_malloc(sizeof(spec_view_t));
        if (!s_spec) return;
        memset(s_spec, 0, sizeof(*s_spec));
    }
    spec_view_t *v = s_spec;
    bool a = s_spec_style == SPEC_STYLE_A;
    int bh = a ? SPEC_A_H : SPEC_DOCK_H;
    v->box = lv_obj_create(s_editor);
    lv_obj_remove_style_all(v->box);
    lv_obj_move_to_index(v->box, lv_obj_get_index(s_editor_body) + 1);
    lv_obj_set_size(v->box, SPEC_W, bh);
    lv_obj_set_pos(v->box, MARGIN, a ? ED_BODY_Y : SCREEN_H - 8 - bh);
    lv_obj_set_scrollable(v->box, false);
    lv_obj_set_style_bg_color(v->box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v->box, LV_OPA_COVER, 0);
    if (a) lv_obj_set_pos(s_editor_body, 0, ED_BODY_Y + bh + 8);
    lv_obj_set_height(s_editor_body, SCREEN_H - ED_BODY_Y - bh - 8);
    v->img = v->bars = v->hold = v->caption = NULL;
    memset(v->dot, 0, sizeof(v->dot));
    v->ref_on = false;
    v->drawn_any = false;
    memset(&v->data, 0, sizeof(v->data));
    if (s_spec_style != SPEC_STYLE_B) {   // A, C: the picture with the points on it
        if (!v->px) v->px = lv_malloc((size_t)SPEC_W * SPEC_A_H * 2);
        if (!v->px) return;
        v->w = a ? SPEC_W : SPEC_W - SPEC_HOLD_W - 8;
        v->h = bh;
        v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        v->dsc.header.w = v->w;
        v->dsc.header.h = v->h;
        v->dsc.header.stride = v->w * 2;
        v->dsc.data_size = (uint32_t)(v->w * v->h * 2);
        v->dsc.data = (const uint8_t *)v->px;
        spec_templates(v);
        v->img = lv_image_create(v->box);
        lv_obj_set_pos(v->img, 0, 0);
        lv_obj_set_size(v->img, v->w, v->h);
        lv_image_set_src(v->img, &v->dsc);
        const nano_fx_model_t *m = s_editor_model_def;
        int count = m && m->type == 4005 ? 11 : m && m->type == 4001 ? 3 : 2;
        for (int i = 0; i < count; i++) {
            bool big = a && !(m && m->type == 4005 && i > 0 && i < 10);   // (Graphic 9's nine bands: small points)
            v->dot[i] = spec_dot(v->box, big ? 26 : a ? 14 : 12, big);
        }
        v->caption = label(v->box, &ui_font_12, CAPTION);
        lv_obj_set_style_text_letter_space(v->caption, 1, 0);
        lv_obj_set_pos(v->caption, 14, 8);
        if (!a) {
            static const struct { float f; const char *t; } fl[] = { { 100, "100" }, { 1000, "1k" }, { 10000, "10k" } };
            for (unsigned i = 0; i < sizeof(fl) / sizeof(fl[0]); i++)
                spec_text(v->box, fl[i].t, (int)lroundf(spec_x_of_f(fl[i].f, v->w)) + 4, v->h - 18, LV_TEXT_ALIGN_LEFT);
            v->hold = frame_button(v->box);   // C: HOLD - the sound of now as a line to compare with
            lv_obj_set_size(v->hold, SPEC_HOLD_W, bh);
            lv_obj_set_pos(v->hold, SPEC_W - SPEC_HOLD_W, 0);
            lv_obj_add_event_cb(v->hold, on_spec_hold, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = label(v->hold, &ui_font_16, TEXT);
            lv_label_set_text(l, "HOLD");
            lv_obj_center(l);
            style_frame_button(v->hold, s_editor_color, false, false);
        }
    }
    s_spec_eq_dirty = true;
    v->seq = UINT32_MAX;
    spec_caption(v);
    if (!s_spec_timer) s_spec_timer = lv_timer_create(spec_tick, SPEC_MS, NULL);
    lv_timer_resume(s_spec_timer);
    spec_tick(NULL);
}

void ui_set_spectrum_source(ui_spectrum_cb get)
{
    lvgl_port_lock(0);
    s_spec_get = get;
    lvgl_port_unlock();
}

static void build_param_rows(void)
{
    stop_drag();
    spec_remove();   // (P4: the EQ's spectrum; made again below for an EQ)
    lv_obj_clean(s_editor_body);
    memset(s_param_bar, 0, sizeof(s_param_bar));
    const nano_fx_model_t *m = s_editor_model_def;
    if (!spec_wanted(m)) spec_stop();
    int own = ed_model_params(), count = ed_param_count();   // the model's, then the auto-wah rows of a wah
    for (int i = 0; i < count; i++) {
        lv_point_t size = { 0, 0 };
        const nano_param_t *p = ed_param(i);
        if (p->kind == NANO_PARAM_RANGE && p->unit && p->unit[0])
            lv_text_get_size(&size, p->unit, &ui_font_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        s_param_unit_w[i] = (int16_t)size.x;
        set_param_norm(i, i < own ? -1 : wah_row_norm(wah_row_id(i - own)));
    }
    if (!m) return;
    int pos = 0;
    for (int row = 0; row < count; row++) {
        int idx = row < own && m->order ? m->order[row] : row;
        if (idx >= count) continue;
        lv_obj_t *bar = lv_obj_create(s_editor_body);
        lv_obj_remove_style_all(bar);   // drawn in on_bar_event
        lv_obj_set_size(bar, ED_BAR_W, ED_BAR_H);
        lv_obj_set_pos(bar, MARGIN + (pos % 2) * (ED_BAR_W + 12), (pos / 2) * ED_BAR_PITCH);
        lv_obj_set_scrollable(bar, false);
        lv_obj_set_scroll_on_focus(bar, false);
        lv_obj_set_scroll_chain_hor(bar, false);   // sideways is the bar's own (else the screen would scroll)
        lv_obj_set_clickable(bar, true);
        lv_obj_add_event_cb(bar, on_bar_event, LV_EVENT_ALL, (void *)(intptr_t)idx);
        s_param_bar[idx] = bar;
        pos++;
    }
    s_gr_row = NULL;
    if (m->icon == NANO_ICON_COMPRESSOR) {   // a compressor: its gain reduction below (drafts/entwurf-p4-15-gain-reduction-b.png)
        build_gr_row((pos + 1) / 2);
        memset(&s_grv, 0, sizeof(s_grv));
    }
    s_env_row = NULL;
    if (m->icon == NANO_ICON_WAH && s_wah.follows) build_env_row((pos + 1) / 2);   // (following the playing: live)
    if (spec_wanted(m)) spec_build();   // P4: the EQ's live spectrum
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

// ---- The pad: the editor's second page ----
// The effect large and a lens to drag between four sounds in the corners, as the focus view of the Nano Cortex
// Editor (the sounds are the editor's: main/fx_focus.c). The centre is the effect as it was when the page opened.
// The lens blends the controls along the Nano's 0-1; a switch follows the corner that counts most. Beside the pad:
// the controls the corners move, with their values. The values go to the Nano one at a time, PAD_SEND_MS apart
// (a control that waits longest first) - the rate of one moving bar, whatever the number of controls.
// P4: laid out for 1024 x 600 - the pad and the list fill the page under the header.
#define PAD_PAGE_Y 76
#define PAD_PAGE_H (SCREEN_H - PAD_PAGE_Y)
#define PAD_SEG_H 60
#define PAD_Y 72              // inside the page, under BARS | PAD
#define PAD_W 640
#define PAD_H (PAD_PAGE_H - PAD_Y - MARGIN)   // 440
#define PAD_LENS 72
#define PAD_GLOW 98
#define PAD_FIELD_W 228
#define PAD_FIELD_H 60
#define PAD_LIST_X (MARGIN + PAD_W + 20)
#define PAD_LIST_W (SCREEN_W - MARGIN - PAD_LIST_X)
#define PAD_ROW_H 54
#define PAD_SEND_MS 30
static lv_obj_t *s_pad_page, *s_pad_button, *s_pad_seg[2], *s_pad, *s_pad_picture, *s_pad_symbol, *s_pad_symbol_icon, *s_pad_home, *s_pad_home_text;
static lv_obj_t *s_pad_glow, *s_pad_lens, *s_pad_corner[4], *s_pad_list;
static lv_obj_t *s_pad_row_value[NANO_MAX_PARAMS], *s_pad_row_fill[NANO_MAX_PARAMS];
// The lens has this control: its value is not taken from the Nano while the finger is down or it waits to be sent.
static bool pad_holds(int idx)
{
    return (s_padv.waiting >> idx & 1) || (s_padv.page && s_padv.finger && (s_padv.moved >> idx & 1));
}

static void pad_row_show(int idx)
{
    if (!s_pad_row_value[idx]) return;
    const nano_param_t *p = ed_param(idx);
    char text[32];
    if (p->kind == NANO_PARAM_RANGE && p->unit && p->unit[0] && s_param_norm[idx] >= 0) snprintf(text, sizeof(text), "%s %s", s_param_text[idx], p->unit);
    else strlcpy(text, s_param_text[idx], sizeof(text));
    set_text(s_pad_row_value[idx], text);
    if (s_pad_row_fill[idx]) {
        float n = s_param_norm[idx] < 0 ? 0 : s_param_norm[idx];
        int w = 7 + (int)lroundf(n * (PAD_LIST_W - 14 - 7));
        if (lv_obj_get_width(s_pad_row_fill[idx]) != w) lv_obj_set_width(s_pad_row_fill[idx], w);
    }
}

// The lens where s_padv.x / y say, and the zone it is nearest to filled.
static void pad_place(void)
{
    int cx = PAD_LENS / 2 + (int)lroundf(s_padv.x * (PAD_W - PAD_LENS)), cy = PAD_LENS / 2 + (int)lroundf(s_padv.y * (PAD_H - PAD_LENS));
    lv_obj_set_pos(s_pad_glow, cx - PAD_GLOW / 2, cy - PAD_GLOW / 2);
    lv_obj_set_pos(s_pad_lens, cx - PAD_LENS / 2, cy - PAD_LENS / 2);
    int near = 4;
    float best = hypotf(s_padv.x - 0.5f, s_padv.y - 0.5f);
    for (int i = 0; i < 4; i++) {
        float d = hypotf(s_padv.x - (float)(i % 2), s_padv.y - (float)(i / 2));
        if (d < best) {
            best = d;
            near = i;
        }
    }
    if (best >= 0.36f) near = -1;
    if (near == s_padv.near) return;
    s_padv.near = near;
    uint32_t color = s_editor_color;
    for (int i = 0; i < 4; i++) {
        bool on = i == near;
        lv_obj_set_style_bg_color(s_pad_corner[i], lv_color_hex(on ? color : TILE_OFF_BG), 0);
        lv_obj_set_style_border_color(s_pad_corner[i], lv_color_hex(on ? color : blend(color, PANEL_BG, 0.60f)), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_pad_corner[i], 0), lv_color_hex(on ? ink_for(color) : TEXT), 0);
    }
    lv_obj_set_style_bg_color(s_pad_home, lv_color_hex(near == 4 ? color : 0x15181B), 0);
    lv_obj_set_style_border_color(s_pad_home, lv_color_hex(near == 4 ? color : 0x3A3F45), 0);
    lv_obj_set_style_text_color(s_pad_home_text, lv_color_hex(near == 4 ? ink_for(color) : MUTED), 0);
}

// A value on the control's own steps, back as the Nano's 0-1 (as a bar that is dragged).
static float pad_snap(const nano_param_t *p, float n)
{
    float v = param_from_normalized(p, n < 0 ? 0 : n > 1 ? 1 : n);
    n = p->max > p->min ? (v - p->min) / (p->max - p->min) : 0;
    return p->skew > 0 && p->skew != 1 ? powf(n < 0 ? 0 : n, p->skew) : n;
}

// The lens to x / y (0-1): the controls follow, what differs from what the Nano has waits to be sent.
static void pad_move(float x, float y)
{
    const nano_fx_model_t *m = s_editor_model_def;
    if (!m || !s_padv.page) return;
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    y = y < 0 ? 0 : y > 1 ? 1 : y;
    s_padv.x = x;
    s_padv.y = y;
    float off = fmaxf(fabsf(x - 0.5f), fabsf(y - 0.5f)), centre = powf(off < 0.5f ? 1 - 2 * off : 0, 1.5f);
    float w[4] = { (1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y };
    int top = 0;
    for (int k = 0; k < 4; k++) {
        w[k] *= 1 - centre;
        if (w[k] > w[top]) top = k;
    }
    int count = ed_model_params();
    for (int idx = 0; idx < count; idx++) {
        if (!(s_padv.moved >> idx & 1)) continue;
        const nano_param_t *p = &m->params[idx];
        float base = s_padv.base[idx], n;
        if (centre >= 0.9999f) {
            n = base;   // the centre: exactly as it was (not moved onto a step)
        } else if (p->kind == NANO_PARAM_ENUM) {
            n = centre >= w[top] || s_padv.corner[top][idx] < 0 ? base : s_padv.corner[top][idx];
        } else {
            n = centre * base;
            for (int k = 0; k < 4; k++) n += w[k] * (s_padv.corner[k][idx] < 0 ? base : s_padv.corner[k][idx]);
            n = pad_snap(p, n);
        }
        if (fabsf(n - s_param_norm[idx]) >= 0.0001f) {
            set_param_norm(idx, n);
            pad_row_show(idx);
        }
        if (fabsf(n - s_padv.sent[idx]) >= 0.002f) s_padv.waiting |= 1u << idx;
        else s_padv.waiting &= ~(1u << idx);
    }
    pad_place();
}

static void editor_leave(void);
static void style_fxp_chips(void);

// One waiting value to the Nano (the timer: every PAD_SEND_MS).
static void pad_send(lv_timer_t *t)
{
    (void)t;
    int count = ed_model_params();
    if (!s_padv.waiting || count == 0 || lv_obj_is_hidden(s_editor)) {
        s_padv.waiting = 0;
        if (s_padv.leave) editor_leave();
        return;
    }
    for (int i = 0; i < count; i++) {
        int idx = (s_padv.next + i) % count;
        if (!(s_padv.waiting >> idx & 1)) continue;
        s_padv.waiting &= ~(1u << idx);
        s_padv.next = idx + 1;
        s_padv.sent[idx] = s_param_norm[idx];
        row_changed(idx, s_param_norm[idx]);
        return;
    }
    s_padv.waiting = 0;
}

// The page opens: the values as they are become the centre, the lens goes there, the corners and the list are
// the model's.
static void pad_open(void)
{
    const nano_fx_model_t *m = s_editor_model_def;
    const nano_focus_t *f = s_padv.focus;
    int count = ed_model_params();
    uint32_t color = s_editor_color;
    s_padv.moved = s_padv.waiting = 0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < NANO_MAX_PARAMS; i++) s_padv.corner[k][i] = -1;
        for (int v = 0; v < f->corner[k].count; v++) {
            int idx = f->corner[k].values[v].param;
            if (idx >= count) continue;
            s_padv.corner[k][idx] = f->corner[k].values[v].value;
            s_padv.moved |= 1u << idx;
        }
        // The name in the large size if it fits, else smaller (and dots if it is still too long)
        lv_obj_t *l = lv_obj_get_child(s_pad_corner[k], 0);
        lv_point_t size;
        lv_text_get_size(&size, f->corner[k].name, &ui_font_24, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_style_text_font(l, size.x <= PAD_FIELD_W - 20 ? &ui_font_24 : &ui_font_20, 0);
        set_text(l, f->corner[k].name);
    }
    for (int i = 0; i < NANO_MAX_PARAMS; i++) s_padv.base[i] = s_padv.sent[i] = i < count ? s_param_norm[i] : -1;

    // the picture of the own build, else the effect's symbol
    const lv_image_dsc_t *picture = pedal_asset(m->type);
    set_hidden(s_pad_picture, picture == NULL);
    set_hidden(s_pad_symbol, picture != NULL);
    if (picture) set_image(s_pad_picture, picture);
    else {
        lv_obj_set_style_bg_color(s_pad_symbol, lv_color_hex(color), 0);
        set_hidden(s_pad_symbol_icon, NANO_ICONS[m->icon] == NULL);
        if (NANO_ICONS[m->icon]) set_image(s_pad_symbol_icon, NANO_ICONS[m->icon]);
        lv_obj_set_style_image_recolor(s_pad_symbol_icon, lv_color_hex(ink_for(color)), 0);
    }
    lv_obj_set_style_bg_color(s_pad_glow, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_pad_lens, lv_color_hex(blend(color, 0x15181B, 0.45f)), 0);
    style_frame_button(s_pad_seg[0], color, false, false);
    style_frame_button(s_pad_seg[1], color, true, false);
    lv_obj_set_style_text_color(lv_obj_get_child(s_pad_seg[1], 0), lv_color_hex(ink_for(color)), 0);

    // the controls the corners move, in the model's display order
    lv_obj_clean(s_pad_list);
    memset(s_pad_row_value, 0, sizeof(s_pad_row_value));
    memset(s_pad_row_fill, 0, sizeof(s_pad_row_fill));
    int pos = 0;
    for (int row = 0; row < count; row++) {
        int idx = m->order ? m->order[row] : row;
        if (idx >= count || !(s_padv.moved >> idx & 1)) continue;
        const nano_param_t *p = &m->params[idx];
        lv_obj_t *box = lv_obj_create(s_pad_list);
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, 0, pos * PAD_ROW_H);
        lv_obj_set_size(box, PAD_LIST_W - 14, PAD_ROW_H);
        lv_obj_set_scrollable(box, false);
        lv_obj_set_clickable(box, false);
        lv_obj_t *name = label(box, &ui_font_20, 0xC8CCD1);
        lv_label_set_text_static(name, p->name);
        lv_obj_set_pos(name, 2, 2);
        s_pad_row_value[idx] = label(box, &ui_font_20, TEXT);
        lv_obj_align(s_pad_row_value[idx], LV_ALIGN_TOP_RIGHT, -2, 2);
        if (p->kind == NANO_PARAM_RANGE) {
            lv_obj_t *track = lv_obj_create(box);
            lv_obj_remove_style_all(track);
            lv_obj_set_pos(track, 0, 36);
            lv_obj_set_size(track, PAD_LIST_W - 14, 8);
            lv_obj_set_style_radius(track, 4, 0);
            lv_obj_set_style_bg_color(track, lv_color_hex(0x23272B), 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_clickable(track, false);
            lv_obj_t *fill = lv_obj_create(box);
            lv_obj_remove_style_all(fill);
            lv_obj_set_pos(fill, 0, 36);
            lv_obj_set_size(fill, 8, 8);
            lv_obj_set_style_radius(fill, 4, 0);
            lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
            lv_obj_set_clickable(fill, false);
            s_pad_row_fill[idx] = fill;
        }
        pad_row_show(idx);
        pos++;
    }
    lv_obj_scroll_to_y(s_pad_list, 0, LV_ANIM_OFF);
    s_padv.x = s_padv.y = 0.5f;
    s_padv.near = -2;
    s_padv.finger = false;
    pad_place();
}

// Which page shows: the bars, or the pad when it is wanted and the model has corners and known values. On the bars'
// page the FX presets make room for PAD.
static void pad_show(void)
{
    const nano_fx_model_t *m = s_editor_model_def;
    int count = ed_model_params();
    bool can = m && s_padv.focus;
    for (int i = 0; can && i < count; i++) can = s_param_norm[i] >= 0;   // (effect off or still reading: no values)
    bool page = can && s_padv.prefer;
    if (page && !s_padv.page) {
        s_padv.page = true;
        stop_drag();
        close_panel();
        pad_open();
    }
    s_padv.page = page;
    set_hidden(s_pad_page, !page);
    set_hidden(s_editor_body, page);
    set_hidden(s_pad_button, page || !can);
    if (can) style_frame_button(s_pad_button, s_editor_color, false, false);
    int x = can && !page ? MARGIN + 76 + 8 : MARGIN;
    if (lv_obj_get_x(s_fxp_bar) != x) {
        lv_obj_set_x(s_fxp_bar, x);
        lv_obj_set_width(s_fxp_bar, SCREEN_W - MARGIN - x);
    }
}

static void on_pad_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_padv.finger = false;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t a;
    lv_obj_get_coords(s_pad, &a);
    s_padv.finger = true;
    pad_move((float)(point.x - a.x1 - PAD_LENS / 2) / (PAD_W - PAD_LENS), (float)(point.y - a.y1 - PAD_LENS / 2) / (PAD_H - PAD_LENS));
}

static void on_pad_corner(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k > 3) pad_move(0.5f, 0.5f);   // Start: the effect as it was
    else pad_move((float)(k % 2), (float)(k / 2));
}

static void on_pad_page(lv_event_t *e)
{
    s_padv.prefer = (intptr_t)lv_event_get_user_data(e) != 0;
    pad_show();
    style_fxp_chips();
}

static void build_pad(void)
{
    // PAD on the bars' page, left of the FX presets
    s_pad_button = frame_button(s_editor);
    lv_obj_set_pos(s_pad_button, MARGIN, 80);
    lv_obj_set_size(s_pad_button, 76, 56);
    lv_obj_add_event_cb(s_pad_button, on_pad_page, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *l = label(s_pad_button, &ui_font_16, 0xE6E8EA);
    lv_label_set_text_static(l, "PAD");
    lv_obj_center(l);
    lv_obj_set_hidden(s_pad_button, true);

    s_pad_page = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_pad_page);
    lv_obj_set_pos(s_pad_page, 0, PAD_PAGE_Y);
    lv_obj_set_size(s_pad_page, SCREEN_W, PAD_PAGE_H);
    lv_obj_set_style_bg_color(s_pad_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pad_page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_pad_page, false);
    lv_obj_set_clickable(s_pad_page, false);

    static const char *const seg[2] = { "BARS", "PAD" };
    for (int i = 0; i < 2; i++) {
        s_pad_seg[i] = frame_button(s_pad_page);
        lv_obj_set_pos(s_pad_seg[i], MARGIN + i * 128, 0);
        lv_obj_set_size(s_pad_seg[i], 120, PAD_SEG_H);
        lv_obj_add_event_cb(s_pad_seg[i], on_pad_page, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        l = label(s_pad_seg[i], &ui_font_16, 0xE6E8EA);
        lv_label_set_text_static(l, seg[i]);
        lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_obj_center(l);
    }
    lv_obj_t *start = button(s_pad_page, SCREEN_W - MARGIN - 140, 0, 140, PAD_SEG_H, NULL);
    lv_obj_add_event_cb(start, on_pad_corner, LV_EVENT_CLICKED, (void *)(intptr_t)4);
    lv_obj_set_style_radius(start, 12, 0);
    l = label(start, &ui_font_20, TEXT);
    lv_label_set_text_static(l, "Start");
    lv_obj_center(l);

    s_pad = lv_obj_create(s_pad_page);
    lv_obj_remove_style_all(s_pad);
    lv_obj_set_pos(s_pad, MARGIN, PAD_Y);
    lv_obj_set_size(s_pad, PAD_W, PAD_H);
    lv_obj_set_style_radius(s_pad, 14, 0);
    lv_obj_set_style_bg_color(s_pad, lv_color_hex(0x15181B), 0);
    lv_obj_set_style_bg_opa(s_pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pad, 1, 0);
    lv_obj_set_style_border_color(s_pad, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_scrollable(s_pad, false);
    lv_obj_set_clickable(s_pad, true);
    // (a drag here is the lens only: the page around it does not scroll, and swipes do nothing while the editor is open)
    lv_obj_add_event_cb(s_pad, on_pad_event, LV_EVENT_ALL, NULL);
    for (int i = 0; i < 2; i++) {   // a faint cross through the centre
        lv_obj_t *line = lv_obj_create(s_pad);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, i ? 14 : PAD_W / 2, i ? PAD_H / 2 : 14);
        lv_obj_set_size(line, i ? PAD_W - 28 : 1, i ? 1 : PAD_H - 28);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x262A2F), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_clickable(line, false);
    }
    s_pad_picture = lv_image_create(s_pad);
    lv_obj_align(s_pad_picture, LV_ALIGN_CENTER, 0, -8);
    s_pad_symbol = lv_obj_create(s_pad);
    lv_obj_remove_style_all(s_pad_symbol);
    lv_obj_set_size(s_pad_symbol, 168, 168);
    lv_obj_set_style_radius(s_pad_symbol, 36, 0);
    lv_obj_set_style_bg_opa(s_pad_symbol, LV_OPA_COVER, 0);
    lv_obj_align(s_pad_symbol, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_clickable(s_pad_symbol, false);
    s_pad_symbol_icon = lv_image_create(s_pad_symbol);
    lv_image_set_scale(s_pad_symbol_icon, 640);
    lv_obj_set_style_image_recolor_opa(s_pad_symbol_icon, LV_OPA_COVER, 0);
    lv_obj_center(s_pad_symbol_icon);

    s_pad_home = lv_obj_create(s_pad);   // the centre: the effect as it was when the page opened
    lv_obj_remove_style_all(s_pad_home);
    lv_obj_set_size(s_pad_home, 96, 40);
    lv_obj_set_style_radius(s_pad_home, 20, 0);
    lv_obj_set_style_bg_opa(s_pad_home, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pad_home, 1, 0);
    lv_obj_align(s_pad_home, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_ext_click_area(s_pad_home, 12);
    lv_obj_add_event_cb(s_pad_home, on_pad_corner, LV_EVENT_CLICKED, (void *)(intptr_t)4);
    s_pad_home_text = label(s_pad_home, &ui_font_16, MUTED);
    lv_label_set_text_static(s_pad_home_text, "Start");
    lv_obj_center(s_pad_home_text);

    // the lens: a white ring, a faint wider disc behind it (no shadow - shadows are slow to draw); under the corners
    s_pad_glow = lv_obj_create(s_pad);
    lv_obj_remove_style_all(s_pad_glow);
    lv_obj_set_size(s_pad_glow, PAD_GLOW, PAD_GLOW);
    lv_obj_set_style_radius(s_pad_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pad_glow, 46, 0);
    lv_obj_set_clickable(s_pad_glow, false);
    s_pad_lens = lv_obj_create(s_pad);
    lv_obj_remove_style_all(s_pad_lens);
    lv_obj_set_size(s_pad_lens, PAD_LENS, PAD_LENS);
    lv_obj_set_style_radius(s_pad_lens, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pad_lens, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pad_lens, 4, 0);
    lv_obj_set_style_border_color(s_pad_lens, lv_color_white(), 0);
    lv_obj_set_clickable(s_pad_lens, false);

    for (int k = 0; k < 4; k++) {
        lv_obj_t *field = lv_obj_create(s_pad);
        lv_obj_remove_style_all(field);
        lv_obj_set_pos(field, k % 2 ? PAD_W - PAD_FIELD_W - 12 : 12, k / 2 ? PAD_H - PAD_FIELD_H - 12 : 12);
        lv_obj_set_size(field, PAD_FIELD_W, PAD_FIELD_H);
        lv_obj_set_style_radius(field, 10, 0);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(field, 2, 0);
        lv_obj_set_style_opa(field, 180, LV_STATE_PRESSED);
        lv_obj_set_scrollable(field, false);
        lv_obj_add_event_cb(field, on_pad_corner, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        l = label(field, &ui_font_24, TEXT);
        lv_obj_set_width(l, PAD_FIELD_W - 16);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        s_pad_corner[k] = field;
    }

    s_pad_list = lv_obj_create(s_pad_page);
    lv_obj_remove_style_all(s_pad_list);
    lv_obj_set_pos(s_pad_list, PAD_LIST_X, PAD_Y);
    lv_obj_set_size(s_pad_list, PAD_LIST_W, PAD_H);
    lv_obj_set_scroll_dir(s_pad_list, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_pad_list, lv_color_hex(0x3A3F45), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_pad_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_pad_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_pad_list, 2, LV_PART_SCROLLBAR);

    lv_obj_set_hidden(s_pad_page, true);
    lv_timer_create(pad_send, PAD_SEND_MS, NULL);
}

// ---- The info page: the editor's third page (P4 board only) ----
// What the Nano Cortex Editor's large effect view tells about a model (main/fx_info.c, generated from the editor):
// the device it is based on, its story, where it is heard, starting points and players' sounds to set with one tap,
// tips. "i" in the header opens it over the bars; "i" again or the back arrow goes back. Left the pedal's picture
// of the own build (else the effect's symbol) with the original's name; right one column to scroll.
// Apply sets the values as the pad does: on the bars at once, to the Nano one at a time (pad_send).
#define INFO_LEFT_W 280
#define INFO_X (MARGIN + INFO_LEFT_W + 12)
#define INFO_W (SCREEN_W - MARGIN - INFO_X)   // 708
#define INFO_APPLY_W 132

static lv_obj_t *s_info_left, *s_info_picture, *s_info_symbol, *s_info_symbol_icon, *s_info_orig_caption, *s_info_orig, *s_info_made;
static lv_obj_t *s_info_list, *s_info_hint, *s_info_apply[INFO_MAX_SOUNDS];
static const nano_info_sound_t *s_info_sound[INFO_MAX_SOUNDS];
static int s_info_sounds;

// The editor's values are known (the effect is on and read): only then there is something to set.
static bool info_values_known(void)
{
    int count = ed_model_params();
    for (int i = 0; i < count; i++) {
        if (s_param_norm[i] < 0) return false;
    }
    return count > 0;
}

// The bars show exactly this sound (each control on the same step).
static bool info_sound_on(const nano_info_sound_t *sound)
{
    const nano_fx_model_t *m = s_editor_model_def;
    int count = ed_model_params();
    if (!m || !sound->count) return false;
    for (int v = 0; v < sound->count; v++) {
        int idx = sound->values[v].param;
        if (idx >= count || s_param_norm[idx] < 0) return false;
        const nano_param_t *p = &m->params[idx];
        if (p->kind == NANO_PARAM_ENUM) {
            if (option_index(p, s_param_norm[idx]) != option_index(p, sound->values[v].value)) return false;
        } else {
            float step = p->step > 0 ? p->step : (p->max - p->min) / 1000;
            if (fabsf(param_from_normalized(p, s_param_norm[idx]) - param_from_normalized(p, sound->values[v].value)) > step / 2 + 0.0001f) return false;
        }
    }
    return true;
}

// As everywhere: the sound that is set is filled, the others are framed.
static void info_style_sounds(void)
{
    uint32_t color = s_editor_color;
    bool known = info_values_known();
    set_hidden(s_info_hint, known || s_info_sounds == 0);
    for (int i = 0; i < s_info_sounds; i++) {
        bool on = known && info_sound_on(s_info_sound[i]);
        lv_obj_t *l = lv_obj_get_child(s_info_apply[i], 0);
        style_frame_button(s_info_apply[i], color, on, false);
        set_color_prop(l, LV_STYLE_TEXT_COLOR, on ? ink_for(color) : TEXT);
        set_text(l, on ? "Applied" : "Apply");
        set_num_prop(s_info_apply[i], LV_STYLE_OPA, known ? LV_OPA_COVER : LV_OPA_40);
    }
}

static void info_style_button(void)
{
    style_frame_button(s_info_button, s_editor_color, s_info_on, false);
    lv_obj_set_style_image_recolor(s_info_button_icon, lv_color_hex(s_info_on ? ink_for(s_editor_color) : TEXT), 0);
}

static void on_info_apply(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int count = ed_model_params();
    if (!s_editor_model_def || i >= s_info_sounds || !info_values_known()) return;
    const nano_info_sound_t *sound = s_info_sound[i];
    for (int v = 0; v < sound->count; v++) {
        int idx = sound->values[v].param;
        float n = sound->values[v].value;
        if (idx >= count || fabsf(n - s_param_norm[idx]) < 0.0001f) continue;
        set_param_norm(idx, n);
        s_padv.waiting |= 1u << idx;   // to the Nano one at a time (pad_send), as the pad's values
    }
    info_style_sounds();
}

static lv_obj_t *info_text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = label(parent, font, color);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(l, text);   // (the table's texts live on)
    return l;
}

static void info_caption(const char *text)
{
    lv_obj_t *l = info_text(s_info_list, CAPTION_FONT, MUTED, text);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
}

static lv_obj_t *info_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    return box;
}

static void info_sounds(const char *caption, const nano_info_sound_t *sounds, int count)
{
    if (!count) return;
    uint32_t color = s_editor_color;
    int32_t width = -1;   // of the values' lines
    info_caption(caption);
    for (int i = 0; i < count && s_info_sounds < INFO_MAX_SOUNDS; i++) {
        const nano_info_sound_t *sound = &sounds[i];
        lv_obj_t *c = info_box(s_info_list);
        lv_obj_set_style_bg_color(c, lv_color_hex(SURF), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(HAIRLINE), 0);
        lv_obj_set_style_radius(c, RADIUS, 0);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_left(c, 16, 0);
        lv_obj_set_style_pad_column(c, 14, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *col = info_box(c);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 3, 0);
        info_text(col, &ui_font_title_18, TEXT, sound->name);
        lv_obj_t *vl = info_text(col, &ui_font_16, blend(color, 0xF2F2F2, 0.60f), sound->values_text);
        if (sound->why[0]) info_text(col, &ui_font_16, MUTED, sound->why);
        lv_obj_t *apply = frame_button(c);
        lv_obj_set_size(apply, INFO_APPLY_W, 52);
        lv_obj_add_event_cb(apply, on_info_apply, LV_EVENT_CLICKED, (void *)(intptr_t)s_info_sounds);
        lv_obj_center(label(apply, &ui_font_20, TEXT));
        s_info_apply[s_info_sounds] = apply;
        s_info_sound[s_info_sounds++] = sound;
        {   // the values break only between two controls: "Low Pass 2.50 kHz" stays on one line (LVGL would break it
            // at a space or after the decimal point) - the lines are put together here, item by item (the width
            // measured once: all rows have the same)
            if (width < 0) {
                lv_obj_update_layout(c);
                width = lv_obj_get_content_width(vl);
            }
            char values[300] = "", line[160] = "", item[80];
            const char *v = sound->values_text;
            while (*v && width > 0) {
                const char *sep = strstr(v, " \xC2\xB7 ");
                size_t n = sep ? (size_t)(sep - v) : strlen(v);
                snprintf(item, sizeof(item), "%.*s", (int)(n < sizeof(item) - 1 ? n : sizeof(item) - 1), v);
                char next[160];
                snprintf(next, sizeof(next), "%s%s%s", line, line[0] ? " \xC2\xB7 " : "", item);
                lv_point_t size;
                lv_text_get_size(&size, next, &ui_font_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (line[0] && size.x > width) {   // this item on the next line
                    size_t len = strlen(values);
                    snprintf(values + len, sizeof(values) - len, "%s \xC2\xB7\n", line);
                    snprintf(line, sizeof(line), "%s", item);
                } else {
                    snprintf(line, sizeof(line), "%s", next);
                }
                v += n + (sep ? 4 : 0);
            }
            size_t len = strlen(values);
            snprintf(values + len, sizeof(values) - len, "%s", line);
            if (width > 0) lv_label_set_text(vl, values);
        }
    }
}

// The page for the editor's model.
static void info_fill(void)
{
    const nano_info_t *info = s_info;
    const nano_fx_model_t *m = s_editor_model_def;
    uint32_t color = s_editor_color;
    s_info_for = info;
    s_info_sounds = 0;

    // left: the pedal of the own build - only where it is the original, or the editor shows it too (never a
    // stand-in beside "In the style of"); else the effect's symbol
    const lv_image_dsc_t *picture = info->picture ? pedal_asset(m->type) : NULL;
    set_hidden(s_info_picture, picture == NULL);
    set_hidden(s_info_symbol, picture != NULL);
    if (picture) set_image(s_info_picture, picture);
    else {
        lv_obj_set_style_bg_color(s_info_symbol, lv_color_hex(color), 0);
        set_hidden(s_info_symbol_icon, NANO_ICONS[m->icon] == NULL);
        if (NANO_ICONS[m->icon]) set_image(s_info_symbol_icon, NANO_ICONS[m->icon]);
        lv_obj_set_style_image_recolor(s_info_symbol_icon, lv_color_hex(ink_for(color)), 0);
    }
    bool original = info->original[0] != 0;
    set_hidden(s_info_orig_caption, !original);
    set_hidden(s_info_orig, !original);
    lv_label_set_text_static(s_info_orig_caption, info->based ? "BASED ON" : "IN THE STYLE OF");
    lv_label_set_text_static(s_info_orig, info->original);
    set_hidden(s_info_made, !info->made[0]);
    lv_label_set_text_static(s_info_made, info->made);
    lv_obj_scroll_to_y(s_info_left, 0, LV_ANIM_OFF);

    // right: the story, where it is heard, the sounds to set, tips, the small print
    lv_obj_clean(s_info_list);
    if (info->category[0]) {
        char caps[48];
        size_t n = 0;
        for (const char *c = info->category; *c && n + 1 < sizeof(caps); c++) caps[n++] = (char)toupper((unsigned char)*c);
        caps[n] = 0;
        lv_obj_t *l = label(s_info_list, CAPTION_FONT, blend(color, 0xF2F2F2, 0.60f));
        lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_label_set_text(l, caps);
    }
    if (info->text[0]) info_text(s_info_list, &ui_font_20, 0xE6E8EA, info->text);
    if (info->heard[0]) {
        info_caption("HEARD ON");
        info_text(s_info_list, &ui_font_16, 0xC8CCD1, info->heard);
    }
    s_info_hint = info_text(s_info_list, &ui_font_16, 0xFFB02E, "The effect is off \xE2\x80\x93 switch it on to set one of the sounds below.");
    lv_obj_set_style_pad_top(s_info_hint, 8, 0);
    info_sounds("STARTING POINTS", info->starts, info->start_count);
    info_sounds("PLAYERS' SOUNDS", info->players, info->player_count);
    // An own build: Neural DSP's presets of the same model (private data, main/fx_neural.h; none in the published one)
    int neural_count = 0;
    const nano_info_sound_t *neural = nano_fx_neural(m->type, &neural_count);
    info_sounds("NEURAL DSP PRESETS", neural, neural_count);
    if (info->tip_count) info_caption("ON THE NANO");
    for (int i = 0; i < info->tip_count; i++) {
        lv_obj_t *row = info_box(s_info_list);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_t *dot = label(row, &ui_font_16, blend(color, 0xF2F2F2, 0.60f));
        lv_label_set_text_static(dot, "\xE2\x80\xA2");
        lv_obj_set_flex_grow(info_text(row, &ui_font_16, 0xC8CCD1, info->tips[i]), 1);
    }
    if (original || info->start_count) {
        lv_obj_t *foot = info_text(s_info_list, &ui_font_14, 0x8A8F96, info->based ? NANO_INFO_FOOT_BASED : NANO_INFO_FOOT_LIKE);
        lv_obj_set_style_pad_top(foot, 10, 0);
        if (info->player_count) info_text(s_info_list, &ui_font_14, 0x8A8F96, NANO_INFO_FOOT_PLAYERS);
    }
    if (neural_count) {
        info_text(s_info_list, &ui_font_14, 0x8A8F96, "Neural DSP presets: the factory presets of the same model, read off a Quad Cortex. The line "
                  "names what differs from the model's default; Apply sets every control.");
    }
    lv_obj_scroll_to_y(s_info_list, 0, LV_ANIM_OFF);
}

// The page shows while it is wanted and the model has notes; it lies over the bars.
static void info_show(void)
{
    bool page = s_info_on && s_info && s_editor_model_def;
    s_info_on = page;
    if (page && s_info_for != s_info) info_fill();
    set_hidden(s_info_page, !page);
    if (page) info_style_sounds();
    info_style_button();
}

static void pad_show(void);
static void info_set(bool on)
{
    s_info_on = on;
    if (on) {
        stop_drag();
        close_panel();
        s_padv.prefer = false;   // (the pad's centre would no longer be what the bars show)
        pad_show();
    }
    info_show();
}

static void on_info_button(lv_event_t *e)
{
    (void)e;
    info_set(!s_info_on);
}

static void build_info(void)
{
    // "i" in the header, right of the model's card (placed by show_editor_header)
    s_info_button = frame_button(s_editor);
    lv_obj_set_pos(s_info_button, 0, 10);
    lv_obj_set_size(s_info_button, 60, 60);
    lv_obj_set_style_radius(s_info_button, 12, 0);
    lv_obj_add_event_cb(s_info_button, on_info_button, LV_EVENT_CLICKED, NULL);
    s_info_button_icon = ui_icon(s_info_button, UI_ICON_INFO, 30, TEXT);
    lv_obj_center(s_info_button_icon);
    lv_obj_set_hidden(s_info_button, true);

    s_info_page = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_info_page);
    lv_obj_set_pos(s_info_page, 0, PAD_PAGE_Y);
    lv_obj_set_size(s_info_page, SCREEN_W, PAD_PAGE_H);
    lv_obj_set_style_bg_color(s_info_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_info_page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_info_page, false);
    lv_obj_set_clickable(s_info_page, true);   // (nothing below it is touched through it)

    s_info_left = lv_obj_create(s_info_page);
    lv_obj_remove_style_all(s_info_left);
    lv_obj_set_pos(s_info_left, MARGIN, 4);
    lv_obj_set_size(s_info_left, INFO_LEFT_W, PAD_PAGE_H - 4 - MARGIN);
    lv_obj_set_style_bg_color(s_info_left, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_opa(s_info_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_info_left, 1, 0);
    lv_obj_set_style_border_color(s_info_left, lv_color_hex(HAIRLINE), 0);
    lv_obj_set_style_radius(s_info_left, RADIUS, 0);
    lv_obj_set_style_pad_all(s_info_left, 14, 0);
    lv_obj_set_style_pad_row(s_info_left, 6, 0);
    lv_obj_set_flex_flow(s_info_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_info_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_info_left, LV_DIR_VER);   // (a tall pedal with a long line below it)
    lv_obj_set_scrollbar_mode(s_info_left, LV_SCROLLBAR_MODE_OFF);
    s_info_picture = lv_image_create(s_info_left);
    lv_obj_set_style_margin_bottom(s_info_picture, 8, 0);
    s_info_symbol = lv_obj_create(s_info_left);
    lv_obj_remove_style_all(s_info_symbol);
    lv_obj_set_size(s_info_symbol, 168, 168);
    lv_obj_set_style_radius(s_info_symbol, 36, 0);
    lv_obj_set_style_bg_opa(s_info_symbol, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(s_info_symbol, 24, 0);
    lv_obj_set_style_margin_bottom(s_info_symbol, 24, 0);
    lv_obj_set_clickable(s_info_symbol, false);
    s_info_symbol_icon = lv_image_create(s_info_symbol);
    lv_image_set_scale(s_info_symbol_icon, 640);
    lv_obj_set_style_image_recolor_opa(s_info_symbol_icon, LV_OPA_COVER, 0);
    lv_obj_center(s_info_symbol_icon);
    s_info_orig_caption = info_text(s_info_left, CAPTION_FONT, MUTED, "");
    lv_obj_set_style_text_letter_space(s_info_orig_caption, 2, 0);
    s_info_orig = info_text(s_info_left, &ui_font_title_18, TEXT, "");
    s_info_made = info_text(s_info_left, &ui_font_16, MUTED, "");

    s_info_list = lv_obj_create(s_info_page);
    lv_obj_remove_style_all(s_info_list);
    lv_obj_set_pos(s_info_list, INFO_X, 4);
    lv_obj_set_size(s_info_list, INFO_W, PAD_PAGE_H - 4);
    lv_obj_set_style_pad_right(s_info_list, 14, 0);
    lv_obj_set_style_pad_bottom(s_info_list, MARGIN, 0);
    lv_obj_set_style_pad_row(s_info_list, 8, 0);
    lv_obj_set_flex_flow(s_info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_info_list, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_info_list, lv_color_hex(0x3A3F45), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_info_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_info_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_info_list, 2, LV_PART_SCROLLBAR);

    lv_obj_set_hidden(s_info_page, true);
}

static void build_editor(lv_obj_t *screen)
{
    s_editor = lv_obj_create(screen);
    lv_obj_set_size(s_editor, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_editor, 0, 0);
    lv_obj_set_style_bg_color(s_editor, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_editor, 0, 0);
    lv_obj_set_style_radius(s_editor, 0, 0);
    lv_obj_set_style_pad_all(s_editor, 0, 0);
    lv_obj_set_scrollable(s_editor, false);

    lv_obj_t *back = button(s_editor, MARGIN, 10, 64, 60, on_editor_back);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_center(ui_icon(back, UI_ICON_BACK, 30, TEXT));

    // Header card (tap: choose a model)
    s_editor_card = button(s_editor, 84, 10, 596 + (SCREEN_W - 800), 60, on_model_button);
    lv_obj_set_style_radius(s_editor_card, 12, 0);
    lv_obj_set_style_bg_color(s_editor_card, lv_color_hex(SURF), 0);
    lv_obj_set_style_bg_grad_dir(s_editor_card, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(s_editor_card, lv_color_hex(HAIRLINE), 0);
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
    s_editor_slot = label(s_editor_card, CAPTION_FONT, MUTED);
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
    lv_obj_set_pos(s_editor_ab, 556 + (SCREEN_W - 800), 10);
    lv_obj_set_size(s_editor_ab, 120, 60);
    lv_obj_set_scrollable(s_editor_ab, false);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *seg = frame_button(s_editor_ab);
        lv_obj_set_pos(seg, i * 64, 0);   // (8 px between A and B)
        lv_obj_set_size(seg, 56, 60);
        lv_obj_add_event_cb(seg, on_ab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(label(seg, &ui_font_20, TEXT));
        s_editor_ab_seg[i] = seg;
    }
    lv_obj_set_hidden(s_editor_ab, true);

    // On / off: a large switch in the effect colour; the area around it is the button.
    s_editor_onoff = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_editor_onoff);
    lv_obj_set_pos(s_editor_onoff, 680 + (SCREEN_W - 800), 2);
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
    lv_obj_set_pos(s_fxp_bar, MARGIN, 80);
    lv_obj_set_size(s_fxp_bar, SCREEN_W - 2 * MARGIN, 56);
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
    lv_obj_set_pos(s_editor_info, MARGIN, 80);
    lv_obj_set_size(s_editor_info, SCREEN_W - 2 * MARGIN, 56);
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
    lv_obj_set_size(s_editor_body, SCREEN_W, SCREEN_H - ED_BODY_Y);
    lv_obj_set_style_pad_bottom(s_editor_body, 8, 0);
    lv_obj_set_scroll_dir(s_editor_body, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_editor_body, lv_color_hex(0x3A3F45), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_editor_body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_editor_body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_editor_body, 2, LV_PART_SCROLLBAR);
    build_pad();
    build_info();

    // Model / option panel. Below the header the editor is covered in plain black (no see-through layer to blend);
    // a tap there closes the panel.
    s_panel_scrim = lv_obj_create(s_editor);
    lv_obj_remove_style_all(s_panel_scrim);
    lv_obj_set_pos(s_panel_scrim, 0, 74);
    lv_obj_set_size(s_panel_scrim, SCREEN_W, SCREEN_H - 74);
    lv_obj_set_style_bg_color(s_panel_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel_scrim, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_panel_scrim, true);
    lv_obj_add_event_cb(s_panel_scrim, on_panel_close, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_panel_scrim, true);
    s_panel = panel(s_editor, (SCREEN_W - ED_PANEL_W) / 2, 82, ED_PANEL_W, ED_PANEL_H);
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
    static int shown_active = -2, shown_room;
    static bool shown_hidden;
    bool hidden = !s_fxp_usable || !lv_obj_is_hidden(s_editor_info) || s_padv.page;   // the info line or the pad page is there then
    set_hidden(s_fxp_bar, hidden);
    style_scene_chip();
    int room = 120;   // a name's room (P4: the row has room for PAD beside it)
    if (hidden == shown_hidden && shown_active == s_fxp_active && shown_color == s_editor_color && shown_room == room
        && !memcmp(shown_names, s_fxp_names, sizeof(shown_names))) return;
    shown_room = room;
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
        lv_obj_set_width(l, room);
        lv_obj_set_style_text_font(l, size.x <= room - 4 ? &ui_font_16 : &ui_font_14, 0);
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
    set_text(s_editor_model, m ? nano_fx_name(m->type) : "Choose a model");
    const lv_image_dsc_t *pedal = m ? nano_fx_pedal(m->type) : NULL;
    set_hidden(s_editor_pedal, pedal == NULL);
    if (pedal) {
        lv_image_set_src(s_editor_pedal, pedal);
        bool mask = pedal->header.cf == LV_COLOR_FORMAT_A8;   // (a picture in colour is shown as it is)
        lv_obj_set_style_image_recolor(s_editor_pedal, lv_color_hex(color), 0);
        lv_obj_set_style_image_recolor_opa(s_editor_pedal, mask ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_image_opa(s_editor_pedal, on ? (mask ? LV_OPA_90 : LV_OPA_COVER) : LV_OPA_40, 0);
    }
    // A | B takes the right end of the header card.
    bool ab = slot >= 0 && m != NULL;
    int card_w = ab ? 472 + (SCREEN_W - 800) : 596 + (SCREEN_W - 800);
    // (P4) "i" beside the card where the model has notes (the info page)
    set_hidden(s_info_button, s_info == NULL);
    if (s_info) {
        card_w -= 76 + (ab ? 8 : 0);   // 12 px between card, i, A | B and the switch - no two of them touching
        lv_obj_set_x(s_info_button, 84 + card_w + 12);
        info_style_button();
    }
    lv_obj_set_width(s_editor_card, card_w);
    lv_obj_set_width(s_editor_model, card_w - 64 - (pedal ? 116 : 50));
    set_hidden(s_editor_ab, !ab);
    if (ab) {
        int running = s_view.ab_b_type[slot] && ((s_view.ab_active >> slot) & 1) ? 1 : 0;
        for (int i = 0; i < 2; i++) {
            lv_obj_t *seg = s_editor_ab_seg[i];
            bool none = i == 1 && !s_view.ab_b_type[slot];
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
    if (rebuild) {
        build_param_rows();
        // another effect: the pad starts again with its corners - an own build's from Neural DSP's presets
        // (main/private, tools/gen_neural.py), else the editor's (fx_focus.c)
        const nano_focus_t *neural = m ? nano_fx_neural_focus(m->type) : NULL;
        s_padv.focus = neural ? neural : m ? nano_focus(m->type) : NULL;
        s_info = m ? nano_fx_info(m->type) : NULL;        // ... and the info page tells about it
        s_padv.page = false;
        s_padv.waiting = 0;
    }

    uint32_t b_type = slot >= 0 ? s_view.ab_b_type[slot] : 0;
    int b_active = slot >= 0 ? (s_view.ab_active >> slot) & 1 : 0;
    if (rebuild || shown.slot != slot || shown.model != model || shown.on != on || shown.b_type != b_type
        || shown.b_active != b_active) {
        show_editor_header(slot, m, on);
        shown.slot = slot;
        shown.model = model;
        shown.on = on;
        shown.b_type = b_type;
        shown.b_active = b_active;
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
        if (pad_holds(idx)) continue;                      // ... or the pad's lens
        float v = known && idx < count ? values[idx] : -1;
        v = v < 0 ? -1 : v > 1 ? 1 : v;
        if (v != s_param_norm[idx]) {   // only what changed is drawn again
            set_param_norm(idx, v);
            if (s_padv.page) {
                s_padv.sent[idx] = v;
                pad_row_show(idx);
            }
        }
    }
    pad_show();
    info_show();
    style_fxp_chips();

    set_hidden(s_editor, false);
    update_main_hidden();
    if (s_second_pending == slot) {   // opened from the reverb switch's dialog: straight to the second effect's list
        s_second_pending = -1;
        s_second_for_switch = true;   // (open_panel reads it; on_ab, the editor's own A | B, clears it)
        open_panel(PANEL_SECOND, 0);
    }
    lvgl_port_unlock();
}

// The auto-wah settings of the wah in the FX editor (its rows below the wah's own parameter).
void ui_set_autowah(const ui_autowah_t *wah)
{
    lvgl_port_lock(0);
    bool rows_change = wah->follows != s_wah.follows;
    s_wah = *wah;
    int own = ed_model_params();
    if (!lv_obj_is_hidden(s_editor) && s_editor_model_def && s_editor_model_def->icon == NANO_ICON_WAH) {
        if (rows_change && !s_dragging) {   // tempo <-> playing: other rows (the wah's own value kept)
            float keep[NANO_MAX_PARAMS];
            memcpy(keep, s_param_norm, sizeof(keep));
            build_param_rows();
            for (int i = 0; i < own; i++) set_param_norm(i, keep[i]);
        }
        int rows = ed_param_count() - own;
        for (int row = 0; row < rows; row++) {
            int idx = own + row;
            if (s_dragging && idx == s_drag_param) continue;   // the finger has it
            float n = wah_row_norm(wah_row_id(row));
            if (n != s_param_norm[idx]) set_param_norm(idx, n);   // only what changed is drawn again
        }
        show_env_row();
    }
    lvgl_port_unlock();
}

void ui_fx_editor_close(void)
{
    lvgl_port_lock(0);
    s_padv.prefer = s_padv.page = s_padv.leave = false;
    s_padv.waiting = 0;
    s_info_on = false;
    lv_obj_set_hidden(s_info_page, true);
    drop_panel();
    stop_drag();
    spec_stop();   // (P4: the spectrum's analysis stops with it)
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
        lv_label_set_text(s_se_fx_set[slot], carries ? "SETTINGS" : "");
    }
    lv_label_set_text(s_se_name_label, s_se_name);
    char text[64];
    if (s_se_capture) snprintf(text, sizeof(text), "%d  %.47s", s_se_capture, s_capture_names[s_se_capture - 1][0] ? s_capture_names[s_se_capture - 1] : "-");
    else snprintf(text, sizeof(text), "As it is");
    lv_label_set_text(s_se_capture_label, text);
}

// The capture list over the scene dialog: "As it is" and the Nano's 25 slots.
static void on_scene_capture_choice(lv_event_t *e)
{
    s_se_capture = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_scene_cap, true);
    style_scene_editor();
}

static void on_scene_capture(lv_event_t *e)
{
    char text[64];
    lv_obj_clean(s_scene_cap_list);
    lv_obj_t *current = list_entry(s_scene_cap_list, "As it is  (the scene leaves the capture)", s_se_capture == 0, TEXT, on_scene_capture_choice, 0);
    for (int slot = 1; slot <= NANO_CAPTURE_SLOTS; slot++) {
        snprintf(text, sizeof(text), "%2d   %.47s", slot, s_capture_names[slot - 1][0] ? s_capture_names[slot - 1] : "-");
        lv_obj_t *entry = list_entry(s_scene_cap_list, text, slot == s_se_capture, TEXT, on_scene_capture_choice, slot);
        if (slot == s_se_capture) current = entry;
    }
    lv_obj_set_hidden(s_scene_cap, false);
    lv_obj_move_foreground(s_scene_cap);
    lv_obj_update_layout(s_scene_cap_list);
    lv_obj_scroll_to_view(current, LV_ANIM_OFF);
}

static void on_scene_cap_close(lv_event_t *e)
{
    lv_obj_set_hidden(s_scene_cap, true);
}

static void build_scene_capture(lv_obj_t *screen)
{
    s_scene_cap = dialog_full(screen);
    dialog_title(s_scene_cap, "Capture for this scene");
    lv_obj_t *hint = label(s_scene_cap, HINT_FONT, MUTED);
    lv_label_set_text(hint, "The scene loads this capture slot (a short gap in the sound).");
    lv_obj_set_pos(hint, 20, 46);
    close_button(s_scene_cap, DLG_W - 60, 12, on_scene_cap_close, 0);
    s_scene_cap_list = scroll_list(s_scene_cap, 10, 76, DLG_W - 22, DLG_H - 90);
    lv_obj_set_hidden(s_scene_cap, true);
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
    s_se_capture = stored && s_view.scene_capture[scene] <= NANO_CAPTURE_SLOTS ? s_view.scene_capture[scene] : 0;
    for (int slot = 0; slot < NANO_FX_SLOTS && !stored; slot++) s_se_fx_on |= (uint8_t)(s_slot_on[slot] << slot);
    if (stored) strlcpy(s_se_name, s_view.scene_names[scene], sizeof(s_se_name));
    else snprintf(s_se_name, sizeof(s_se_name), "Scene %d", scene + 1);
    snprintf(text, sizeof(text), "Scene %d  \xC2\xB7  switch %d", scene + 1, scene + 3);
    lv_label_set_text(s_se_title, text);
    snprintf(text, sizeof(text), "Preset %d: tap the effects that are on. Their settings: scene button in the FX editor", s_se_preset);
    lv_label_set_text(s_se_hint, text);
    lv_obj_set_hidden(s_se_clear, !stored);
    style_scene_editor();
    lv_obj_set_hidden(s_scene_editor, false);
    lv_obj_move_foreground(s_scene_editor);
}

static void on_scene_editor_button(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);   // 0 cancel, 1 save, 2 remove the scene
    char text[UI_SCENE_NAME + NANO_FX_SLOTS + 5];
    lv_obj_set_hidden(s_scene_editor, true);
    lv_obj_set_hidden(s_scene_cap, true);
    if (action == 1) {
        int n = snprintf(text, sizeof(text), "%c%c", '3' + s_se_scene, 'a' + s_se_color);
        for (int slot = 0; slot < NANO_FX_SLOTS; slot++) text[n++] = (s_se_fx_on >> slot) & 1 ? '1' : '0';
        text[n++] = (char)('a' + s_se_capture);
        snprintf(text + n, sizeof(text) - (size_t)n, "%s", s_se_name);
    } else if (action == 2) {
        snprintf(text, sizeof(text), "%c!", '3' + s_se_scene);
    } else {
        return;
    }
    if (s_on_text) s_on_text('S', text);
}

static void on_scene_capture(lv_event_t *e);

static void build_scene_editor(lv_obj_t *screen)
{
    s_scene_editor = dialog_full(screen);   // P4: under the top bar, the whole width
    s_se_title = dialog_title(s_scene_editor, "");
    s_se_hint = label(s_scene_editor, HINT_FONT, MUTED);
    lv_obj_set_pos(s_se_hint, 20, 46);

    lv_obj_t *name = button(s_scene_editor, 18, 78, 560, 60, on_scene_name);   // tap: the keyboard
    s_se_name_label = label(name, &ui_font_title_22, TEXT);
    lv_obj_align(s_se_name_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *hint = label(name, HINT_FONT, MUTED);
    lv_label_set_text(hint, "Rename");
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -4, 0);
    // Next to the name: the capture the scene loads (or none: it leaves the capture as it is)
    lv_obj_t *cap = button(s_scene_editor, 590, 78, DLG_W - 2 - 18 - 590, 60, on_scene_capture);
    lv_obj_t *cap_caption = label(cap, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(cap_caption, 1, 0);
    lv_label_set_text(cap_caption, "CAPTURE");
    lv_obj_align(cap_caption, LV_ALIGN_TOP_LEFT, 0, -6);
    s_se_capture_label = label(cap, &ui_font_16, TEXT);
    lv_obj_set_width(s_se_capture_label, DLG_W - 2 - 18 - 590 - 24);
    lv_label_set_long_mode(s_se_capture_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_se_capture_label, LV_ALIGN_BOTTOM_LEFT, 0, 6);

    for (int i = 0; i < UI_BANK_COLOR_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(s_scene_editor);
        s_se_swatch[i] = sw;
        lv_obj_set_size(sw, 64, 64);
        lv_obj_set_pos(sw, 18 + i * 96, 156);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(BANK_COLORS[i]), 0);
        lv_obj_set_scrollable(sw, false);
        lv_obj_set_clickable(sw, true);
        lv_obj_add_event_cb(sw, on_scene_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int slot = 0; slot < NANO_FX_SLOTS; slot++) {   // the five effects in the order of the signal chain
        lv_obj_t *fx = frame_button(s_scene_editor);
        s_se_fx[slot] = fx;
        lv_obj_set_pos(fx, 18 + slot * 194, 240);
        lv_obj_set_size(fx, 186, 140);
        lv_obj_add_event_cb(fx, on_scene_fx, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
        s_se_fx_caption[slot] = label(fx, CAPTION_FONT, MUTED);
        lv_obj_set_style_text_letter_space(s_se_fx_caption[slot], 1, 0);
        // (the short captions of the tiles with two symbols: "POST FX 1" and "OFF" do not fit side by side at 14 px)
        static const char *const SHORT[NANO_FX_SLOTS] = { "PRE 1", "PRE 2", "POST 1", "POST 2", "POST 3" };
        lv_label_set_text(s_se_fx_caption[slot], SHORT[slot]);
        lv_obj_set_pos(s_se_fx_caption[slot], 10, 15);
        s_se_fx_state[slot] = label(fx, CAPTION_FONT, MUTED);
        lv_obj_set_style_text_letter_space(s_se_fx_state[slot], 1, 0);
        lv_obj_align(s_se_fx_state[slot], LV_ALIGN_TOP_RIGHT, -10, 15);
        s_se_fx_name[slot] = label(fx, &ui_font_16, TEXT);
        lv_obj_set_pos(s_se_fx_name[slot], 10, 38);
        lv_obj_set_size(s_se_fx_name[slot], 166, 2 * lv_font_get_line_height(&ui_font_16));
        lv_label_set_long_mode(s_se_fx_name[slot], LV_LABEL_LONG_DOT);
        s_se_fx_set[slot] = label(fx, CAPTION_FONT, MUTED);
        lv_obj_set_style_text_letter_space(s_se_fx_set[slot], 1, 0);
        lv_obj_set_pos(s_se_fx_set[slot], 10, 110);
    }
    s_se_clear = small_button(s_scene_editor, 18, 446, 160, 52, "Remove", on_scene_editor_button, 2);
    small_button(s_scene_editor, DLG_W - 2 - 18 - 330, 446, 160, 52, "Cancel", on_scene_editor_button, 0);
    primary(small_button(s_scene_editor, DLG_W - 2 - 18 - 160, 446, 160, 52, "Save", on_scene_editor_button, 1));
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
    lv_obj_set_pos(field, 18 + (index % 4) * 242, 74 + (index / 4) * 66);   // P4: four wide columns
    lv_obj_set_size(field, 234, 58);
    lv_obj_set_style_outline_color(field, lv_color_white(), 0);
    lv_obj_set_style_outline_pad(field, 2, 0);
    lv_obj_add_event_cb(field, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    *caption_label = label(field, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(*caption_label, 1, 0);
    char upper[16];   // small capitals, as the captions of the tiles
    size_t n = 0;
    for (const char *c = caption; *c && n < sizeof(upper) - 1; c++) upper[n++] = (char)toupper((unsigned char)*c);
    upper[n] = 0;
    lv_label_set_text(*caption_label, upper);
    lv_obj_set_pos(*caption_label, 10, 12);
    *name_label = label(field, &ui_font_16, TEXT);
    lv_obj_set_pos(*name_label, 10, 29);
    lv_obj_set_size(*name_label, 212, lv_font_get_line_height(&ui_font_16));
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
    lv_obj_t *caption = label(toggle, CAPTION_FONT, MUTED);
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
    lv_obj_set_size(page, DLG_W - 2, DLG_H - 2 - 70);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_clickable(page, false);
    return page;
}

static void build_expression(lv_obj_t *screen)
{
    s_exp_dialog = dialog_full(screen);   // P4: under the top bar, the whole width
    dialog_title(s_exp_dialog, "Expression pedal");
    s_ex_sub = label(s_exp_dialog, HINT_FONT, MUTED);
    lv_obj_set_pos(s_ex_sub, 20, 46);
    // Top right: what the Nano's EXP/MIDI connector takes (a setting of the Nano, the same for all presets)
    lv_obj_t *jack = label(s_exp_dialog, CAPTION_FONT, MUTED);
    lv_obj_set_style_text_letter_space(jack, 1, 0);
    lv_label_set_text(jack, "EXP/MIDI JACK");
    lv_obj_set_width(jack, 130);
    lv_obj_set_style_text_align(jack, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(jack, DLG_W - 2 - 18 - 244 - 140, 21);
    s_ex_jack[0] = small_button(s_exp_dialog, DLG_W - 2 - 18 - 244, 8, 118, 38, "Expression", on_ex_jack, NANO_JACK_EXPRESSION);
    s_ex_jack[1] = small_button(s_exp_dialog, DLG_W - 2 - 18 - 118, 8, 118, 38, "MIDI", on_ex_jack, NANO_JACK_MIDI);

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
        lv_obj_set_size(s_ex_slider[k], 560, 16);
        lv_obj_set_pos(s_ex_slider[k], 270, y + 12);
        lv_obj_set_ext_click_area(s_ex_slider[k], 14);
        style_slider(s_ex_slider[k], 0x6A6F75);
        lv_obj_add_event_cb(s_ex_slider[k], on_ex_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        s_ex_value[k] = label(page, &ui_font_28, 0xFFFFFF);
        lv_obj_set_width(s_ex_value[k], 100);
        lv_obj_set_style_text_align(s_ex_value[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_ex_value[k], DLG_W - 2 - 18 - 100, y + 2);
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
    s_ex_sw_invert = small_button(page, DLG_W - 2 - 18 - 142, 242, 142, 34, "Invert", on_ex_switch, 20);
    s_ex_sw_latch = small_button(page, 192, 284, 250, 34, "Latch emulation: off", on_ex_switch, 21);
    s_ex_sw_delay_name = label(page, &ui_font_16, 0xC8CCD1);
    lv_obj_set_style_text_letter_space(s_ex_sw_delay_name, 2, 0);
    lv_label_set_text(s_ex_sw_delay_name, "DELAY");
    lv_obj_set_pos(s_ex_sw_delay_name, 192, 292);
    s_ex_sw_delay = lv_slider_create(page);
    lv_slider_set_range(s_ex_sw_delay, 0, NANO_EXP_DELAY_MAX / 10);   // steps of 10 ms
    lv_obj_set_size(s_ex_sw_delay, 560, 16);
    lv_obj_set_pos(s_ex_sw_delay, 282, 296);
    lv_obj_set_ext_click_area(s_ex_sw_delay, 14);
    style_slider(s_ex_sw_delay, 0x6A6F75);
    lv_obj_add_event_cb(s_ex_sw_delay, on_ex_delay, LV_EVENT_VALUE_CHANGED, NULL);
    s_ex_sw_delay_value = label(page, &ui_font_title_22, 0xFFFFFF);
    lv_obj_set_width(s_ex_sw_delay_value, 120);
    lv_obj_set_style_text_align(s_ex_sw_delay_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ex_sw_delay_value, DLG_W - 2 - 18 - 120, 288);

    s_ex_page_button = small_button(s_exp_dialog, 18, 452, 170, 46, "On / off", on_ex_page, 0);
    s_ex_calibrate = small_button(s_exp_dialog, 196, 452, 150, 46, "Calibrate", on_ex_calibrate, 0);
    small_button(s_exp_dialog, DLG_W - 2 - 18 - 330, 452, 160, 46, "Cancel", on_ex_button, 0);
    s_ex_save = small_button(s_exp_dialog, DLG_W - 2 - 18 - 160, 452, 160, 46, "Save", on_ex_button, 1);
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
    s_cal_text = label(s_cal_panel, HINT_FONT, MUTED);
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


// A text of the screen for the editor: without the screen's own symbols (private code points, three bytes from 0xEF).
static void plain_copy(char *out, size_t size, const char *text)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p && n + 1 < size; p++) {
        if (*p == 0xEF && p[1] && p[2]) { p += 2; continue; }
        if (n == 0 && *p == ' ') continue;
        out[n++] = (char)*p;
    }
    out[n] = 0;
}

// What the tiles show, for the editor's strip: a copy made whenever the screen has been given a state (below), so
// asking for it never waits for the screen. (Before, ui_tiles_get read the tiles under the screen's lock - after
// every event of the controller's task while the editor was connected, and each time that task stood still until
// the picture being drawn was finished: 16 to 40 ms. With the editor the whole controller was slower.)
// ui_show_state and ui_tiles_get both run in the controller's task, so the copy needs no lock of its own.
static ui_tile_info_t s_tiles_copy[TILES];
static ui_view_t s_tiles_copy_view;
static bool s_tiles_copy_ok;
static uint32_t s_tiles_version;   // counts up when the copy changes

static void tiles_copy(void)   // (called with the screen's lock held, at the end of ui_show_state)
{
    static ui_tile_info_t tiles[TILES];
    bool drawn = s_tiles_drawn;
    if (!drawn) return;
    for (int i = 0; i < TILES && drawn; i++) {
        ui_tile_info_t *t = &tiles[i];
        memset(t, 0, sizeof(*t));
        plain_copy(t->caption, sizeof(t->caption), s_tile_caption_text[i]);   // (the screen shows no captions)
        plain_copy(t->name, sizeof(t->name), lv_label_get_text(s_tile_name[i]));
        plain_copy(t->note, sizeof(t->note), s_tile_note[i]);
        t->color = s_tile_color[i];
        t->flags = (s_tile_active[i] ? UI_TILE_ACTIVE : 0) | (s_tile_empty[i] ? UI_TILE_EMPTY : 0);
        t->row_active = -1;
        if (s_tile_pair[i].shown) {
            t->flags |= UI_TILE_ROWS | (s_tile_pair[i].on ? UI_TILE_ROWS_ON : 0);
            t->row_active = (int8_t)s_tile_pair[i].active;
            for (int r = 0; r < 2; r++) plain_copy(t->rows[r], sizeof(t->rows[r]), s_tile_pair[i].names[r]);
        } else if (s_tile_split[i].shown) {   // A / B in two halves: to the editor as two rows "A  name", "B  name"
            const tile_split_t *sp = &s_tile_split[i];
            t->flags |= UI_TILE_ROWS | (sp->on ? UI_TILE_ROWS_ON : 0);
            t->row_active = (int8_t)sp->runs;
            plain_copy(t->name, sizeof(t->name), sp->runs >= 0 ? sp->names[sp->runs] : "");   // (-1: neither half runs)
            for (int r = 0; r < 2; r++) {
                char row[32];
                snprintf(row, sizeof(row), "%c  %s", sp->letters[r] ? sp->letters[r] : r ? 'B' : 'A', sp->names[r]);
                plain_copy(t->rows[r], sizeof(t->rows[r]), row);
            }
        }
    }
    if (s_tiles_copy_ok && !memcmp(tiles, s_tiles_copy, sizeof(tiles)) && !memcmp(&s_view, &s_tiles_copy_view, sizeof(s_view))) return;
    memcpy(s_tiles_copy, tiles, sizeof(tiles));
    s_tiles_copy_view = s_view;
    s_tiles_copy_ok = true;
    s_tiles_version++;
}

uint32_t ui_tiles_version(void)
{
    // P4 only: the onboard looper's tiles change in the screen's own task (loop_poll), without a ui_show_state. Then,
    // and only then, the copy is taken again here - under the screen's lock, as before; in every other case nothing
    // waits. (The small board has no tiles drawn outside ui_show_state.)
    if (s_tiles_stale) {
        lvgl_port_lock(0);
        s_tiles_stale = false;
        tiles_copy();
        lvgl_port_unlock();
    }
    return s_tiles_version;
}

bool ui_tiles_get(ui_tile_info_t tiles[TILES], ui_view_t *view)
{
    if (!s_tiles_copy_ok) return false;
    memcpy(tiles, s_tiles_copy, sizeof(s_tiles_copy));
    if (view) *view = s_tiles_copy_view;
    return true;
}
