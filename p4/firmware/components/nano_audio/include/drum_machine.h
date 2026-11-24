// Drum machine for the looper: the computed kit of the Duo DAW (DuoEngine/DrumKit.swift, "StartKit": nine sounds
// made from sines, noise and square-wave clusters - no sample files) and its grooves (DuoKit/DrumLoops.swift, by pad
// role, 16 steps per bar, "X" accent, "x" hit, "o" ghost note), played in the Nano's tempo into the Nano's USB
// playback beside the loop, the jam track and the phone.
//
// Threads: drum_machine_render runs on the audio side only (audio_mix). Commands (start, stop, groove, tempo) come from
// other tasks as single atomic values; drum_machine_info reads a snapshot. Plain C, tested on the computer (test/).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { DRUM_KICK, DRUM_SNARE, DRUM_CLAP, DRUM_HAT, DRUM_OPENHAT, DRUM_LOWTOM, DRUM_HIGHTOM, DRUM_RIDE, DRUM_CRASH, DRUM_PADS };

typedef struct {
    const char *name, *style;
    uint8_t steps_per_bar, bars;
    float swing;                              // 0-0.75 of a step: the off steps come that much later
    const char *lane[DRUM_PADS];              // NULL = this pad does not play
} drum_groove_t;

extern const drum_groove_t DRUM_GROOVES[];
extern const int DRUM_GROOVE_COUNT;

#define DRUM_VOICES 12

typedef struct {
    // the kit (made once by drum_machine_init)
    int16_t *sound[DRUM_PADS];                // stereo frames
    uint32_t length[DRUM_PADS];
    float gain[DRUM_PADS];                    // the pads' levels (as the DAW's KitBalance: by the loudness of the attack)

    // set from any task
    _Atomic int want_groove;                  // -1 = no change
    _Atomic int want_run;                     // 0 stop, 1 start at the next frame, 2 nothing to do
    _Atomic uint32_t bpm_x100;
    _Atomic uint32_t level_bits;              // float, linear

    // audio side only
    int groove;
    bool running;
    bool paused;                              // set by loop_parts: no new hits, the place in the groove is kept
    double step_pos;                          // in steps since the start of the groove (fractions = between steps)
    uint32_t next_step;                       // the next step whose hits are still to come
    struct { int pad; uint32_t pos, wait; float gain; bool on; } voice[DRUM_VOICES];   // wait: frames before it sounds
    uint32_t rate;

    _Atomic uint32_t i_step, i_bar, i_running, i_groove;
} drum_machine_t;

// alloc: where the kit's sounds go (PSRAM on the board), NULL = malloc. About 2.3 MB at 48 kHz. False without memory.
bool drum_machine_init(drum_machine_t *d, uint32_t rate, void *(*alloc)(size_t));
void drum_machine_yield(void);   // weak, empty: the board defines it (a short pause between the kit's sounds)

void drum_machine_start(drum_machine_t *d);       // from the top of the groove at the next frame
void drum_machine_stop(drum_machine_t *d);        // the sounds that ring go on, nothing new starts
void drum_machine_set_groove(drum_machine_t *d, int groove);   // takes over at the next bar line while it runs
void drum_machine_set_bpm(drum_machine_t *d, float bpm);
void drum_machine_set_level_db(drum_machine_t *d, float db);

// Audio side (loop_parts.h): start in the frame `delay` of the next render, `steps` into the groove (fractions too).
void drum_machine_start_now(drum_machine_t *d, uint32_t delay, double steps);
// Audio side (the count-in): one sound of the kit, `delay` frames into the next render, at velocity 0-1.
void drum_machine_hit_now(drum_machine_t *d, int pad, uint32_t delay, float velocity);
// Audio side: frames per step at the tempo set now, steps per bar of the groove that plays.
double drum_machine_step_frames(drum_machine_t *d);
int drum_machine_steps_per_bar(drum_machine_t *d);

// Audio side: adds the drums to `out` (stereo float), frames frames.
void drum_machine_render(drum_machine_t *d, float *out, uint32_t frames);

typedef struct { bool running; int groove, bar, step; } drum_machine_info_t;
void drum_machine_info(drum_machine_t *d, drum_machine_info_t *out);

// One sound of the kit at full level (for tests and pictures): writes up to `max` stereo frames, returns the count.
uint32_t drum_kit_sound(int pad, uint32_t rate, float *lr, uint32_t max);
