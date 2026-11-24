// Two parts for the looper (part A and part B, as verse and chorus) and the drums beside them, as the user asked for
// on 2026-11-19: two loops of their own, each recorded and overdubbed on its own; a tap on the other part switches at
// the end of the loop that plays (an empty part starts recording there); pause holds loop and drums together, a
// second tap lets both go on where they were.
//
// The drums follow the loop: when a part's first take starts they start from the top with it (if wanted), and
// every time a loop of whole bars comes round they are set back to its bar line - so they never drift apart.
//
// Threads: loop_parts_process runs on the audio side only (audio_mix, in place of looper_process). Commands come from
// ONE other task (footswitches / screen) as atomic values; the looper commands (rec / dub, play / stop, undo, redo,
// clear) go to the part that plays, through looper_command. Plain C, tested on the computer (test/).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "drum_machine.h"
#include "looper.h"

typedef struct {
    looper_t part[2];
    drum_machine_t *drums;           // may be NULL (no kit yet)

    _Atomic int want_part;           // -1 none, 0 / 1: switch to that part at the end of the loop that plays
    _Atomic int want_pause;          // -1 none, 0 go on, 1 pause
    _Atomic int want_drums;          // -1 none, 0 off, 1 on (in step with the loop when one plays)
    _Atomic int drum_sync;           // 1: the drums start with a part's first take
    _Atomic int count_in;            // 1: a part's first take waits for one bar counted in (the drums, or four clicks)
    _Atomic int want_take;           // a first take asked for: 1 count-in, 2 on the drums' bar line, 3 armed
    _Atomic int quantize;            // 1: with the drums running a first take starts and ends on their bar lines
    _Atomic int arm;                 // 1: Rec on an empty part arms it - the take starts with the first note
    int16_t *pre;                    // the last LOOP_PRE frames of the input (stereo): starts a little in the past
    uint32_t pre_w;
    float floor;                     // the input's noise floor (armed)
    // A take started back at the bar line: the kept input still to be copied into its loop, a little per render
    // (all at once it took the audio up to 13 ms - clicks at the start of a take without count-in, 2026-11-19).
    int bf_part;                     // the part it belongs to
    uint32_t bf_left, bf_src;        // frames still to copy, the oldest's place in `pre`
    int64_t bf_dst;                  // its frame in the loop

    _Atomic uint32_t bar_frames;     // frames per bar of the Nano's tempo (0 = not known): for the drums' bar lines

    // audio side only
    int active, next;                // next = -1: no switch waiting
    bool paused;
    uint32_t counting;               // frames of the count-in (or to the bar line) still to go (0 = none)
    bool armed;                      // waiting for the first note
    uint32_t count_beat;             // frames per beat of the count-in
    int click_next;                  // the count-in's next click (0-3) when the drums are off, -1 = no clicks
    uint32_t count_total;            // frames of the count-in in all

    _Atomic int i_active, i_next, i_paused, i_drums, i_counting, i_armed;
} loop_parts_t;

#define LOOP_PRE 24000               // half a second of input kept
#define ARM_LEVEL 0.02f              // a note: above -34 dBFS ...
#define ARM_OVER_FLOOR 8.0f          // ... and 8 times the noise before it
#define ARM_MARGIN 480               // the take begins 10 ms before the note (its attack)
#define BACKFILL_PER_RENDER 512      // kept input copied into a loop started back at the bar line, per render

// mem: frames * 8 int16 (two parts, each two stereo buffers of `frames` frames); pre: LOOP_PRE * 2 int16 (or NULL).
void loop_parts_init(loop_parts_t *p, int16_t *mem, uint32_t frames, uint32_t rate, int16_t *pre);
void loop_parts_set_drums(loop_parts_t *p, drum_machine_t *drums);   // before the audio runs, or with it stopped

void loop_parts_command(loop_parts_t *p, looper_cmd_t cmd);   // to the part that plays
void loop_parts_choose(loop_parts_t *p, int part);              // tap on part A (0) / B (1)
void loop_parts_pause(loop_parts_t *p);                         // tap on pause: pause / go on
void loop_parts_drums(loop_parts_t *p, bool on);
void loop_parts_set_drum_sync(loop_parts_t *p, bool on);
void loop_parts_set_count_in(loop_parts_t *p, bool on);
void loop_parts_set_quantize(loop_parts_t *p, bool on);
void loop_parts_set_arm(loop_parts_t *p, bool on);
// Frames per bar of the Nano's tempo for the drums (0 = not known), and the looper's bar lines (0 = free length).
void loop_parts_set_bar(loop_parts_t *p, uint32_t drum_bar, uint32_t looper_bar);

// Audio side: `in` the Nano's sound, `out` gets the loop (overwritten).
void loop_parts_process(loop_parts_t *p, const float *in, float *out, uint32_t frames);

typedef struct {
    int active, next;                // next -1 = none
    bool paused, drums_on;
    bool counting;                   // the count-in runs (the first take starts at its end)
    bool armed;                      // the part waits for the first note
    looper_info_t part[2];
} loop_parts_info_t;
void loop_parts_info(loop_parts_t *p, loop_parts_info_t *out);
