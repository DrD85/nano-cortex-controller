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
void p4_looper_set_bar_lines(bool on, int beats);   // round the first take to bars in the Nano's tempo
void p4_looper_set_level_db(float db);
void p4_looper_set_latency(uint32_t frames);

// ---- drums (the Duo DAW's kit and grooves, drum_machine.h), in the Nano's tempo ----
bool p4_drums_ready(void);
void p4_drums_start(bool on);
void p4_drums_groove(int groove);                   // index into DRUM_GROOVES; changes at the next bar line
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
#define P4_JAM_DIR "/sdcard/jam"
#define P4_JAM_NAME 80                              // file name without ".wav", with the terminating 0
bool p4_jam_card(void);                             // the microSD card is mounted
bool p4_jam_card_unreadable(void);                  // a card is in the slot, but it has no FAT32 file system
int p4_jam_count(void);
const char *p4_jam_name(int i);                     // file name without ".wav"
void p4_jam_play(int i);                            // from the start
void p4_jam_toggle(void);                           // play / pause
void p4_jam_prev(void);                             // to the start, or (in the first 3 s) the track before
void p4_jam_next(void);
void p4_jam_seek(uint32_t seconds);
void p4_jam_info(int *current, track_player_info_t *out);
void p4_jam_set_level_db(float db);                 // the jam volume (mixer)
float p4_jam_level_db(void);
float p4_jam_level(int i);                          // level correction of track i (dB)
void p4_jam_set_track_level(float db);              // ... of the current track, kept in levels.txt on the card
int p4_jam_measuring(void);                         // tracks still to be measured
void p4_jam_note(int i, char *out, size_t n);       // "-17.3 LUFS · +1.5" for the track list
void p4_jam_transfer(bool on);                      // Wi-Fi transfer begins / ends (p4_net.c)
void p4_jam_measured(const char *name, uint32_t size, float lufs, float play, float peak);
void p4_jam_set_trim(const char *name, float db);
void p4_jam_trims(const char *text);                // "<file><TAB><dB>" lines from the Mac, merged into levels.txt
float p4_nano_tempo(void);                          // main.c

// shared with p4_jam.c
extern audio_mix_t g_p4_mix;
extern track_player_t g_p4_track;
void p4_jam_start(void);
