// What the board plays into the Nano's USB playback: phone (Bluetooth, audio_pipe), jam track (track_player) and
// looper, each with its own volume, summed and kept below full scale by a limiter.
//
// The Nano's own sound (USB capture, channels 3/4) comes in through audio_mix_capture and goes to the looper.
// If the Nano also sends back what the board plays (to be checked on the hardware), set the loopback delay and gain:
// the board then takes its own playback out of the capture again, so the looper records only the guitar.
// Loopback delay = capture frame index minus the render frame index of the same sound (both counted from the start
// of the streams); measured on the hardware.
//
// Both calls run on the USB client task (capture and playback callbacks). Gains may be set from any task.
// Plain C, tested on the computer.
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#include "audio_pipe.h"
#include "drum_machine.h"
#include "looper.h"
#include "loop_parts.h"
#include "track_player.h"

typedef enum { AUDIO_SRC_PHONE, AUDIO_SRC_TRACK, AUDIO_SRC_LOOPER, AUDIO_SRC_DRUMS, AUDIO_SRC_COUNT } audio_src_t;

#define MIX_CAP_RING 2048       // capture frames waiting for the render (power of two; 1024 until the render got a
                                //     task of its own, 2026-11-26: while that task is held up the capture goes on)
#define MIX_CUSHION 384         // capture frames kept in hand (8 ms), so the render never waits for them (192 / 4 ms ran dry
                                //     while the screen redrew: capture gaps of up to 6.6 ms measured 2026-11-21)
#define MIX_SLACK 1440          // more than cushion + this waiting (a start, a restart): back to the cushion
                                //     (384 cut short piles after a redraw held the render back - 431 frames dropped
                                //     each time; cushion + slack stays below the ring. 600 until 2026-11-26: a render
                                //     held up for up to 30 ms now takes the whole pile when it goes on)
#define MIX_SETTLE 24000        // render frames after the cushion is there (0.5 s): its lowest fill is measured ...
#define MIX_SETTLE_END 48000    // ... until here, then again every second; brought back to MIX_CUSHION when off
                                //     (the fill swings by a transfer of 48 above it)
#define MIX_HIST 2048           // rendered frames kept for the loopback (42 ms)
#define MIX_BLOCK 64
// The look-ahead limiter (1) or the one that turns down at once (0): brought back one step at a time, 2026-11-19.
#define MIX_LOOKAHEAD 0
#define LIM_AHEAD 48             // the look-ahead limiter's look-ahead: 1 ms - the board's playback is that much later
#define MIX_OUT_DELAY (MIX_LOOKAHEAD ? LIM_AHEAD : 0)   // frames the mix goes out late (for the looper's latency)
#define AUDIO_MIX_STAGES 6       // capture, phone, track, looper, drums, sum

typedef struct {
    uint32_t capture_short;     // render frames for which no capture frame was there (silence went to the looper)
    uint32_t capture_dropped;   // capture frames thrown away (far too many waiting)
    uint32_t limited;           // render frames in which the limiter turned the sum down
} audio_mix_stats_t;

typedef struct {
    audio_pipe_t *phone;
    track_player_t *track;
    _Atomic(looper_t *) looper;   // may be set later (audio_mix_set_looper), when its memory is there
    _Atomic(drum_machine_t *) drums;   // the same (audio_mix_set_drums)
    _Atomic(loop_parts_t *) parts;     // the looper in two parts (audio_mix_set_parts): played instead of `looper`

    float cap[MIX_CAP_RING * 2];
    _Atomic uint32_t cw, cr;
    uint32_t cap_index;         // capture index of the next frame to be read
    int primed, seen;           // the cushion is there; it was there once (a later wait for it counts as missing frames)
    uint32_t since_primed;      // render frames since the cushion was there
    uint32_t low;               // lowest fill seen while settling
    uint32_t hold;              // frames still to hold back (silence to the looper) to make the cushion larger
    int holding;                // the frame just asked for was held back
    int32_t settled;            // frames the last settling took out (> 0) or held back (< 0), for the report
    uint32_t settle_count;

    float hist[MIX_HIST * 2];
    uint32_t render_index;
    _Atomic uint32_t lb_delay, lb_gain_bits;

    _Atomic uint32_t gain_bits[AUDIO_SRC_COUNT];
    float gain[AUDIO_SRC_COUNT];
    float lim;                  // (not used since the look-ahead limiter)
    // Look-ahead limiter (2026-11-19): the sum goes out LIM_AHEAD frames late; the gain comes down smoothly over that
    // time before a peak instead of at once on it (heard as distortion with the drums).
    float lim_dl[LIM_AHEAD * 2];   // the delayed sum
    float lim_t[LIM_AHEAD];        // the gain each of the last frames needs
    float lim_box[LIM_AHEAD];      // the released gain of the last frames (averaged)
    float lim_gr, lim_sum;
    uint32_t lim_i;
    // Measuring (the board sets clock to its cycle counter; NULL = off): the longest time of each part of one render
    // since the last audio_mix_take_stages - capture, phone, track, looper, drums, sum.
    uint32_t (*clock)(void);
    uint32_t stage_max[AUDIO_MIX_STAGES];
    float peak_src[AUDIO_SRC_COUNT], peak_sum, lim_min, peak_in;   // (peak_in: the capture the looper gets)   // since the last audio_mix_take_levels (after the gains)
    float clean[MIX_BLOCK * 2];   // the last block of capture as the looper got it (for tests)
    uint32_t clean_n;

    _Atomic uint32_t s_short, s_dropped, s_limited;
} audio_mix_t;

// Any source may be NULL.
void audio_mix_init(audio_mix_t *m, audio_pipe_t *phone, track_player_t *track, looper_t *looper);

// USB capture packet: `frames` frames of `channels` slots; the Nano's sound is on ch_left / ch_right (0-based).
void audio_mix_capture(audio_mix_t *m, const int32_t *slots, uint32_t frames, int channels, int ch_left, int ch_right);

// USB playback packet: renders `frames` frames into the slots (the sum on ch_left / ch_right, the rest zero).
void audio_mix_render(audio_mix_t *m, int32_t *out, uint32_t frames, int channels, int ch_left, int ch_right);

// The looper once its memory is allocated (the audio may already be running).
void audio_mix_set_looper(audio_mix_t *m, looper_t *looper);
void audio_mix_set_drums(audio_mix_t *m, drum_machine_t *drums);
void audio_mix_set_parts(audio_mix_t *m, loop_parts_t *parts);
void audio_mix_set_gain_db(audio_mix_t *m, audio_src_t src, float db);   // ramped; <= -90 = off
void audio_mix_set_loopback(audio_mix_t *m, uint32_t delay_frames, float gain);   // gain 0 = none
void audio_mix_get_stats(audio_mix_t *m, audio_mix_stats_t *out);
// The longest time of each part of one render since the last call (clock units), then set back to 0.
void audio_mix_take_stages(audio_mix_t *m, uint32_t out[AUDIO_MIX_STAGES]);
// The highest level of each source (after its gain) and of the sum, and the limiter's lowest gain, since the last
// call; then set back.
void audio_mix_take_levels(audio_mix_t *m, float src[AUDIO_SRC_COUNT], float *sum, float *lim_min);
float audio_mix_take_input_peak(audio_mix_t *m);   // the capture's highest level since the last call
void audio_mix_last_clean(audio_mix_t *m, float *lr, uint32_t frames);
