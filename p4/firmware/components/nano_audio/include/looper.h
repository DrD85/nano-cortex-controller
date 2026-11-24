// Looper on the board: records the Nano's sound (USB capture, what the player hears) into PSRAM and plays the loop
// back into the Nano's USB playback, where the Nano mixes it with the live guitar.
//
// One footswitch cycle as on most loopers: REC_DUB records the first take; pressed again the length is fixed and the
// loop plays; pressed again it overdubs, again back to play. PLAY_STOP stops (fade out) and starts from the top.
// UNDO hides the last overdub, REDO brings it back; CLEAR empties the looper.
//
// Timing:
// - `latency` is the round trip in frames (from the Nano's output through USB capture, this board, USB playback and
//   back to the Nano's output). Recordings are shifted by it, so the loop sounds in time with what was played.
//   To be measured on the hardware; 0 until then.
// - With a bar length set (frames per bar, from the Nano's tempo), the loop length is rounded to the nearest whole
//   number of bars: pressed early, the recording goes on to the bar line; pressed late, what was played after the
//   bar line is laid over the start of the loop.
// - The seam is crossfaded: the start of the first take fades in, and what was played just after the end is laid
//   over the start with a fade out, so a held note runs on through the seam.
//
// Memory: two buffers (the loop and the last overdub, int16 stereo). The last overdub is kept apart until the next
// overdub starts, then merged into the loop over a short time (a few frames per frame, no long copy).
//
// Threads: looper_process runs on the audio side only (the USB playback render, see audio_mix.h). Commands come from
// ONE other task (footswitches / screen) through a small queue; looper_get_info reads a snapshot from any task.
// Plain C, no ESP-IDF headers - tested on the computer (test/).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOOPER_EMPTY,       // nothing recorded
    LOOPER_REC,         // first take (also: going on to the bar line)
    LOOPER_PLAY,
    LOOPER_DUB,         // playing and overdubbing
    LOOPER_STOP,        // a loop is there, stopped
} looper_state_t;

typedef enum {
    LOOPER_CMD_REC_DUB = 1,   // empty: record; recording: fix the length and play; playing: overdub; overdub: play;
                              // stopped: play from the top and overdub
    LOOPER_CMD_PLAY_STOP,     // playing / overdub: stop; stopped: play from the top; recording: fix and play
    LOOPER_CMD_UNDO,          // hide the last overdub (stops an overdub first); recording, or a take without an
                              // overdub: throw the take away (faded out, then empty)
    LOOPER_CMD_REDO,
    LOOPER_CMD_CLEAR,
} looper_cmd_t;

typedef struct {
    looper_state_t state;
    uint32_t len;             // loop length in frames (0 while the first take is recorded)
    uint32_t pos;             // play position in frames (while recording: frames recorded)
    uint32_t max_len;         // longest possible loop
    bool to_bar_line;         // the first take goes on to the bar line
    bool can_undo, can_redo;
    uint32_t passes;          // times the loop has wrapped since its length was fixed
    uint32_t clipped;         // samples that were clipped (loop too loud)
} looper_info_t;

#define LOOPER_QUEUE 16

typedef struct {
    int16_t *base, *layer;    // loop and last overdub, stereo frames
    uint32_t cap;             // frames per buffer
    uint32_t rate;

    _Atomic uint32_t set_latency, set_bar;
    _Atomic uint32_t qw, qr;
    uint8_t queue[LOOPER_QUEUE];

    // audio side only
    looper_state_t st;
    uint32_t latency, bar, xfade;
    uint32_t len, pos, passes;
    uint32_t rec_n;           // render frames since the first take started
    uint32_t rec_end;         // the first take is written up to here (len + tail), 0 = not fixed yet
    uint32_t target;          // REC going on to the bar line: the length it stops at (0 = no)
    bool rec_on;              // base is still being written (first take or its tail)
    uint32_t tail;            // frames after the end that are laid over the start (0 = none / done)
    bool tail_pending;        // the tail is recorded and not yet laid over the start
    uint32_t tail_done;
    bool first_pass;

    bool has_layer, layer_on;
    bool fold_on, fold_keep;  // merging the previous overdub into the loop
    uint32_t fold_p, fold_left;
    float dub_env, dub_target;
    int pending_gate;         // -1 none, 0 / 1: the overdub gate changes at frame `pending_at`
    uint32_t pending_at, t;   // t: frames processed in all

    float env, env_target;    // output fade (start / stop / clear)
    int after_fade;           // what to do when the output has faded out (0 none, 1 stop, 2 clear)
    uint32_t clipped;

    _Atomic uint32_t i_state, i_len, i_pos, i_passes, i_clipped, i_flags;
} looper_t;

// mem: frames * 4 int16 (two stereo buffers of `frames` frames each).
void looper_init(looper_t *lp, int16_t *mem, uint32_t frames, uint32_t rate);

void looper_command(looper_t *lp, looper_cmd_t cmd);
void looper_set_latency(looper_t *lp, uint32_t frames);
void looper_set_bar(looper_t *lp, uint32_t frames);   // 0 = free length

// Audio side: `in` is the Nano's sound (stereo float, full scale 1.0), `out` gets the loop (overwritten).
void looper_process(looper_t *lp, const float *in, float *out, uint32_t frames);

void looper_get_info(looper_t *lp, looper_info_t *out);

// Audio side only (loop_parts.h): a command at once, leaving a part at its loop's end, coming back to it.
void looper_command_now(looper_t *lp, looper_cmd_t cmd);
// A first take as if it had been started `frames_ago` render frames ago; `pre` holds the last n input frames
// (stereo int16), which go where they belong (quantised starts a little late, takes started by the first note).
void looper_start_back(looper_t *lp, uint32_t frames_ago, const int16_t *pre, uint32_t n);
void looper_leave(looper_t *lp);
void looper_enter(looper_t *lp);
