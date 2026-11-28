// A looper part saved as a WAV file (the P4 board's main/p4_loops.c, 2026-11-28): its header and its file name.
// Plain C, tested on the computer (test/test_save.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOOP_WAV_HEAD 512          // the samples start here: the header is one card sector of its own, so every
                                   // later write is whole sectors (the card takes them straight, without a copy)

// The header of a 16-bit stereo WAV of `frames` frames at `rate`: LOOP_WAV_HEAD bytes (what the header does not need
// is a JUNK chunk). With bpm > 0 and beats > 0 also an "acid" chunk - the loop's tempo, beats and meter
// (beats_per_bar / 4), which DAWs such as Logic take a loop's tempo from.
void loop_wav_header(uint8_t *out, uint32_t frames, uint32_t rate, float bpm, uint32_t beats, uint32_t beats_per_bar);

// What a loop's file name says.
typedef struct {
    unsigned number;               // the save's number: 1, 2, ... in the order they were made (the board has no clock)
    int part;                      // 0 = A, 1 = B
    unsigned bars;                 // 0 = free length (then no tempo either)
    float bpm;
    float seconds;
    bool dated;                    // the date below is known (the Jam Player told the board the time)
    int year, month, day, hour, minute;
} loop_name_t;

// "Loop 0007 A, 120 BPM, 4 bars.wav", free length "Loop 0007 B, 8.3 s.wav"; dated: ", 2026-11-28 14.05" before
// ".wav". Returns the length (cut to n - 1 at most).
size_t loop_wav_name(char *out, size_t n, const loop_name_t *l);
// The number of a loop's file name ("Loop 0007 A, ....wav" -> 7); 0: not a loop's name.
unsigned loop_wav_number(const char *name);
// A name from outside (Wi-Fi) that can be a loop's file in the loops folder: "Loop <digits> ....wav" of letters,
// digits, spaces and , . - ( ) only, shorter than LOOP_WAV_NAME_MAX - no path, nothing hidden.
#define LOOP_WAV_NAME_MAX 96
bool loop_wav_name_ok(const char *name);
