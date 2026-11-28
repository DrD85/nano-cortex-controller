// Jam-track player: plays a file (decoded by a file task, e.g. WAV from the microSD card) into the mixer at the
// Nano's 48 kHz, with a fixed-ratio resampler (no clock to follow: the file has none).
//
// The file task fills a ring buffer (read ahead, so a slow moment of the card is not heard); the audio side reads
// it. Changing track or seeking: the file task calls track_player_begin, waits for track_player_ready (the audio
// side fades out and empties the buffer) and then writes the new data. Play / pause fade in / out (6 ms).
// A buffer that runs dry (card too slow) fades out and starts again when it is filled - the position is kept.
//
// Volume: track_player_set_gain_db is the level correction of the track (its loudness); the jam volume as such is
// the mixer's gain for this source.
//
// One file task writes, the audio side reads, any task may control. Plain C, tested on the computer.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool playing;               // sounding (or about to: prefilling)
    bool ended;                 // played to the end of the data
    uint32_t pos;               // frames of the file played (at its own rate)
    uint32_t rate;              // the file's sample rate
    uint32_t total;             // the file's length in frames (as given to begin)
    uint32_t underruns;         // times the buffer ran dry
} track_player_info_t;

typedef struct {
    int16_t *buf;
    uint32_t cap;
    _Atomic uint32_t w, r;

    uint32_t out_rate, in_rate, prefill;
    double step;
    float frac;                 // (float: a double is a library call per frame on the P4, 2026-11-27)
    float win[4][2];
    int state;
    float env;
    bool flushing;
    float gain, gain_target, gain_step;
    uint32_t gain_bits_seen;
    uint32_t pos, total;

    _Atomic uint32_t req, ack;
    _Atomic uint32_t b_rate, b_start, b_total;
    _Atomic bool want_play, eof;
    _Atomic uint32_t gain_bits;
    _Atomic uint32_t i_pos, i_flags, i_rate, i_total, i_underruns;
} track_player_t;

// buffer: frames * 2 int16 (frames a power of two; e.g. 262144 = 6 s at 44.1 kHz in 1 MB of PSRAM).
void track_player_init(track_player_t *tp, int16_t *buffer, uint32_t frames, uint32_t out_rate);

// File task: a new track or a new position. `start` = first frame that will be written, `total` = the file's
// length in frames. Then wait for track_player_ready before writing.
void track_player_begin(track_player_t *tp, uint32_t in_rate, uint32_t start, uint32_t total);
bool track_player_ready(track_player_t *tp);
uint32_t track_player_space(track_player_t *tp);
uint32_t track_player_write(track_player_t *tp, const int16_t *stereo, uint32_t frames);
void track_player_end_of_data(track_player_t *tp);

// Any task
void track_player_set_playing(track_player_t *tp, bool play);
void track_player_set_gain_db(track_player_t *tp, float db);
void track_player_get_info(track_player_t *tp, track_player_info_t *out);

// Audio side: stereo float frames (overwritten).
void track_player_render_stereo(track_player_t *tp, float *lr, uint32_t frames);
