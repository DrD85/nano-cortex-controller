// Loudness of a jam track, as the Nano Cortex Editor and the Jam Player measure it (index.html, trackLoudness):
// - lufs: integrated loudness of BS.1770 (K-weighting, 400 ms windows in 100 ms steps, gates at -70 LUFS and
//   10 LU below the rest; the first windows are shorter, as in that code),
// - play: what counts against a guitar - the same through a second-order high pass at 200 Hz, plus 1.4 dB, but never
//   more than 3 dB below lufs (the bass counts less),
// - peak: the largest sample (full scale 1.0).
// Fed with 16-bit stereo (a mono file as two equal channels: twice the energy, as there). Plain C, tested against
// the JavaScript on the computer (test/).
#pragma once

#include <stdint.h>

#define LOUD_BASS_HZ 200.0
#define LOUD_BASS_USUAL_DB 1.4
#define LOUD_BASS_MOST_DB 3.0
#define LOUD_TARGET_DB (-21.0f)   // where all tracks play (the Jam Player's default)
#define LOUD_CEILING_DB (-1.0f)   // a track's peaks stay below it

typedef struct {
    uint32_t hop, fill, count, max_blocks;
    double s[5], h[5], c[5];             // shelf, high pass, bass cut
    double st[2][6];                     // filter states per channel
    double sum, cut;                     // of the block being filled
    double *energy, *no_bass;            // per 100 ms block
    float peak;
} loudness_t;

typedef struct { float lufs, play, peak; } loudness_result_t;

// blocks: room for max_blocks values each (one per 100 ms; a longer track is measured over its first part).
void loudness_init(loudness_t *m, uint32_t rate, double *energy, double *no_bass, uint32_t max_blocks);
void loudness_feed(loudness_t *m, const int16_t *stereo, uint32_t frames);
loudness_result_t loudness_result(const loudness_t *m);   // lufs / play -INFINITY for silence

// Gain in dB that puts a measured track at `target` (+ a correction by ear), its peaks below the ceiling.
float loudness_gain_db(loudness_result_t r, float target, float trim);
