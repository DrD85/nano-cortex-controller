// P4 board: what the board plays into the Nano over USB (phone, jam track, looper) - the calls the screen and the
// footswitches use. The audio itself: p4_audio.c (USB, Bluetooth bridge, mixer) and p4_jam.c (microSD, tracks).
//
// STATUS: builds; not yet run on hardware. No screen for the looper / jam player yet (drafts first).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_mix.h"
#include "looper.h"
#include "track_player.h"

void p4_audio_start(void);   // before the screen: USB audio, mixer, Bluetooth bridge
void p4_audio_show(void);    // after the screen: the looper's memory (what PSRAM is left), the microSD card

// ---- looper ----
bool p4_looper_ready(void);                         // false = no memory for it
void p4_looper_command(looper_cmd_t cmd);           // from ONE task (footswitches / screen)
void p4_looper_info(looper_info_t *out);
void p4_looper_dump(void);             // console 'Y': the loop as text (its start and its last second)
void p4_audio_raw_dump(void);          // console 'K': the next capture packets as they came
void p4_audio_restart_streams(void);   // console 'J': the USB audio streams anew
float p4_audio_di_peak(void);   // the dry input's peak (0-1) since the last call (the auto-wah that follows the playing)
void p4_looper_set_bar_lines(bool on, int beats);   // round the first take to bars in the Nano's tempo
void p4_looper_set_level_db(float db);
void p4_looper_set_latency(uint32_t frames);

// ---- drums (the Duo DAW's kit and grooves, drum_machine.h), in the Nano's tempo ----
bool p4_drums_ready(void);
void p4_drums_start(bool on);
void p4_drums_groove(int groove);                   // index into DRUM_GROOVES; changes at the next bar line
void p4_part_groove(int part, int groove);           // a looper part's own groove (-1 none; part 2: both none)
void p4_drums_kit(int kit);                          // DRUM_KIT_* (kept)
void p4_drums_level_db(float db);
void p4_drums_info(drum_machine_info_t *out);
void p4_drums_level_keep(void);                     // the level is stored (after the bar is let go)
void p4_drums_sync(bool on);                        // the drums start with a part's first take
void p4_drums_count_in(bool on);                    // a first take waits for one bar counted in
void p4_looper_quantize(bool on);                   // with the drums: takes start and end on their bar lines
void p4_looper_arm(bool on);                        // Rec arms the empty part: the first note starts the take
bool p4_drums_on(void);

// ---- the looper's two parts (loop_parts.h) ----
void p4_parts_choose(int part);                     // 0 = A, 1 = B: at the end of the loop that plays
void p4_parts_pause(void);                          // loop and drums: pause / go on        // round trip, measured on the hardware (default 0)

// ---- jam tracks (WAV files in /sdcard/jam, measured like the Jam Player on the Mac, corrections by ear) ----
// Albums (2026-11-27): /sdcard/jam/<Album>/<Title>.wav, one folder level; the WAVs directly in /sdcard/jam are the
// album "" (the tracks from before albums, and the board's own upload page). A track's name everywhere is its file
// relative to /sdcard/jam without ".wav": "Album/Title", a loose one just "Title".
#ifndef P4_JAM_DIR
#define P4_JAM_DIR "/sdcard/jam"                    // (a host test points it elsewhere)
#endif
#define P4_JAM_ALBUM_MAX 60                         // bytes of an album's name
#define P4_JAM_TITLE_MAX 78                         // ... of a title (the Mac sends 76; 78 = what loose names could be)
#define P4_JAM_NAME 144                             // "Album/Title" with the terminating 0
#define P4_JAM_PATH (sizeof(P4_JAM_DIR) + P4_JAM_NAME + 8)   // a path buffer: P4_JAM_DIR "/" <name> ".jpeg"
#define P4_JAM_SOURCE 41                            // the Mac's stamp of the source file ("<size>-<mtimeMs>"), with the 0
bool p4_jam_card(void);                             // the microSD card is mounted
bool p4_jam_card_unreadable(void);                  // a card is in the slot, but it has no FAT32 file system
int p4_jam_count(void);
int p4_jam_left_out(void);                          // tracks on the card the list had no room for (MAX_TRACKS)
const char *p4_jam_name(int i);                     // "Album/Title" (loose: "Title"); the list: loose first, then by album, title
uint32_t p4_jam_size(int i);                      // its WAV's bytes on the card (the Jam Player compares them)
const char *p4_jam_source(int i);                   // the stamp the Mac sent with track i ("" = none)
void p4_jam_play(int i);                            // from the start
void p4_jam_toggle(void);                           // play / pause
void p4_jam_prev(void);                             // to the start (after 3 s, and at an album's first), else the track before
void p4_jam_next(void);                             // the next track of the same album (none at the album's end)
void p4_jam_seek(uint32_t seconds);
void p4_jam_info(int *current, track_player_info_t *out);
void p4_jam_set_level_db(float db);                 // the jam volume (mixer)
float p4_jam_level_db(void);
float p4_jam_level(int i);                          // level correction of track i (dB)
void p4_jam_set_track_level(float db);              // ... of the current track, kept in levels.txt on the card
int p4_jam_measuring(void);                         // tracks still to be measured
void p4_jam_note(int i, char *out, size_t n);       // "-17.3 LUFS · +1.5" for the track list
void p4_jam_transfer(bool on);                      // Wi-Fi transfer begins / ends (p4_net.c)
void p4_jam_measured(const char *name, uint32_t size, float lufs, float play, float peak);   // values not usable (NAN): "not measured"
void p4_jam_set_trim(const char *name, float db);
void p4_jam_trims(const char *text);                // "<file><TAB><dB>" lines from the Mac, appended to levels.txt
// A track name from outside (Wi-Fi): made the way FAT keeps it (an album's trailing spaces / dots dropped; leading
// spaces stay, as FAT keeps them), then checked - one '/' at most, no part empty or starting with '.', no character FAT refuses,
// the lengths above. False: not usable.
bool p4_jam_clean_name(char *name);
void p4_jam_make_album(const char *name);           // the folder of "Album/Title" made, if it is not there
void p4_jam_set_source(const char *name, const char *stamp);   // the stamp a file came with ("" = none), in sources.txt
void p4_jam_sources(const char *text);              // "<file><TAB><stamp>" lines from the Mac (tracks on the card already)
int p4_jam_move(const char *text);                  // "<from file><TAB><to file>" lines: moved with cover and keys; how many
void p4_jam_remove_loose(const char *name);         // after "Album/Title" came: a loose "Title" (and its cover) goes
float p4_nano_tempo(void);                          // main.c

// shared with p4_jam.c
extern audio_mix_t g_p4_mix;
extern track_player_t g_p4_track;
void p4_jam_start(void);
