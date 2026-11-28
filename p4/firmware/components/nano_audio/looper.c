// Looper (see looper.h).
#include "looper.h"

#include <string.h>

#define XFADE_MS 5              // seam crossfade, overdub punch in / out, start / stop fades
#define FOLD_PER_FRAME 16       // positions merged per frame (a 20 s loop is merged in 1.25 s)
#define MIN_LEN_MS 100          // a shorter first take is not accepted (the press is ignored)

enum { FLAG_BAR = 1, FLAG_UNDO = 2, FLAG_REDO = 4 };
enum { AFTER_NONE, AFTER_STOP, AFTER_CLEAR };

static inline int16_t sat16(looper_t *lp, float v)
{
    v *= 32767.0f;
    if (v > 32767.0f) { lp->clipped++; return 32767; }
    if (v < -32768.0f) { lp->clipped++; return -32768; }
    return (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
}

static inline float f16(int16_t s) { return s * (1.0f / 32768.0f); }

void looper_init(looper_t *lp, int16_t *mem, uint32_t frames, uint32_t rate)
{
    memset(lp, 0, sizeof(*lp));
    lp->base = mem;
    lp->layer = mem + 2 * (size_t)frames;
    lp->cap = frames;
    lp->rate = rate;
    lp->xfade = rate * XFADE_MS / 1000;
    lp->pending_gate = -1;
    lp->env = lp->env_target = 1.0f;
    atomic_init(&lp->set_latency, 0);
    atomic_init(&lp->set_bar, 0);
    atomic_init(&lp->qw, 0);
    atomic_init(&lp->qr, 0);
    atomic_init(&lp->edit, 0);
    atomic_init(&lp->i_quiet, 0);
}

// ---- what a copy of the loop depends on (looper_save_*; audio side) ----

// The audio side writes the loop's buffers in this render: the first take (or its tail), or - while the loop runs -
// an overdub, its gate about to open, the merge of the overdub before, the seam of the first take. (A stopped loop
// that has faded out runs nothing, a merge it was stopped in included: that waits for the next start.)
static bool writes(const looper_t *lp)
{
    if (lp->rec_on) return true;
    bool running = lp->st == LOOPER_PLAY || lp->st == LOOPER_DUB || lp->env > 0 || lp->dub_env > 0 ||
                   lp->pending_gate >= 0 || lp->after_fade != AFTER_NONE;
    return running && (lp->fold_on || lp->tail_pending || lp->dub_env > 0 || lp->dub_target > 0 || lp->pending_gate >= 0);
}

// A loop is there and stays as it is: it can be copied.
static bool quiet(const looper_t *lp)
{
    return lp->len && lp->st != LOOPER_EMPTY && lp->st != LOOPER_REC && !writes(lp) && lp->after_fade != AFTER_CLEAR;
}

// A change opens (the count odd) - nested ones count once - and closes (even again; i_quiet as it is now).
static void edit_open(looper_t *lp)
{
    if (lp->editing++) return;
    atomic_store_explicit(&lp->edit, atomic_load_explicit(&lp->edit, memory_order_relaxed) + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);   // (the odd count before any of the change's writes)
}

static void edit_close(looper_t *lp)
{
    atomic_store_explicit(&lp->i_quiet, quiet(lp), memory_order_relaxed);
    if (!lp->editing || --lp->editing) return;
    atomic_store_explicit(&lp->edit, atomic_load_explicit(&lp->edit, memory_order_relaxed) + 1, memory_order_release);
}

// A command changes the loop - all but play / stop of a loop that is not being written.
static bool command_edits(const looper_t *lp, looper_cmd_t cmd) { return cmd != LOOPER_CMD_PLAY_STOP || !quiet(lp); }

void looper_edit_begin(looper_t *lp) { edit_open(lp); }
void looper_edit_end(looper_t *lp) { edit_close(lp); }

void looper_command(looper_t *lp, looper_cmd_t cmd)
{
    uint32_t w = atomic_load_explicit(&lp->qw, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&lp->qr, memory_order_acquire);
    if (w - r >= LOOPER_QUEUE) return;   // full: the press is lost (the audio side is not running)
    lp->queue[w % LOOPER_QUEUE] = (uint8_t)cmd;
    atomic_store_explicit(&lp->qw, w + 1, memory_order_release);
}

void looper_set_latency(looper_t *lp, uint32_t frames) { atomic_store(&lp->set_latency, frames); }
void looper_set_bar(looper_t *lp, uint32_t frames) { atomic_store(&lp->set_bar, frames); }

// ---- state changes (audio side) ----

static void reset(looper_t *lp)
{
    lp->st = LOOPER_EMPTY;
    lp->len = lp->pos = lp->passes = lp->rec_n = lp->rec_end = lp->target = 0;
    lp->rec_on = lp->tail_pending = lp->first_pass = false;
    lp->tail = lp->tail_done = 0;
    lp->has_layer = lp->layer_on = lp->fold_on = false;
    lp->dub_env = lp->dub_target = 0;
    lp->pending_gate = -1;
    lp->env = lp->env_target = 1.0f;
    lp->after_fade = AFTER_NONE;
}

static void start_rec(looper_t *lp)
{
    reset(lp);
    lp->latency = atomic_load(&lp->set_latency);
    lp->st = LOOPER_REC;
    lp->rec_on = true;
}

static uint32_t reserve(const looper_t *lp) { return lp->xfade + lp->latency + 64; }

// The length is fixed at render frame P = rec_n (len <= P: the take ended at an earlier bar line).
// Only the seam's crossfade is laid over the start - not what was played after the bar line. (Until 2026-11-19 all of
// it was, up to half a bar at full level: the chord played again on the bar line to end the take then sat a few ms
// beside the loop's first chord - a flam, a "tinny" sound, heard by the user with the drums, whose bars round the end.)
static void fix_at(looper_t *lp, uint32_t len)
{
    uint32_t P = lp->rec_n;
    uint32_t over = P - len;
    uint32_t tail = lp->xfade;
    if (2 * (tail + lp->latency) > len) {   // pressed very late on a short loop: keep only the crossfade
        tail = lp->xfade;
        if (2 * (tail + lp->latency) > len) tail = 0;
    }
    if (len + tail > lp->cap) tail = lp->cap - len;
    lp->len = len;
    lp->tail = tail;
    lp->rec_end = len + tail;
    lp->pos = over < len ? over : 0;
    lp->target = 0;
    lp->first_pass = true;
    lp->passes = 0;
    lp->st = LOOPER_PLAY;
    if (lp->rec_end <= (P > lp->latency ? P - lp->latency : 0)) {   // nothing left to record (no tail)
        lp->rec_on = false;
        lp->tail_pending = tail > 0;
    }
}

static void fix(looper_t *lp)
{
    uint32_t P = lp->rec_n;
    if (P < lp->rate * MIN_LEN_MS / 1000) return;   // too short: keep recording
    uint32_t len = P;
    uint32_t bar = atomic_load(&lp->set_bar);
    lp->bar = bar;
    if (bar) {
        uint32_t n = (P + bar / 2) / bar;
        if (n < 1) n = 1;
        uint32_t q = n * bar;
        if (q > P) {
            if (q + reserve(lp) <= lp->cap) {
                lp->target = q;   // go on to the bar line
                return;
            }
        } else {
            len = q;
        }
    }
    fix_at(lp, len);
}

static void gate(looper_t *lp, int on)
{
    lp->pending_gate = on;
    lp->pending_at = lp->t + lp->latency;
}

static void dub_start(looper_t *lp)
{
    bool merging = lp->fold_on || lp->tail_pending || lp->rec_on;
    if (lp->has_layer && merging) {
        // the previous merge is still running: this overdub goes on in the same layer (one undo step)
    } else {
        // merge the last overdub into the loop (or just clear the layer) ahead of the play position
        lp->fold_on = true;
        lp->fold_keep = lp->has_layer && lp->layer_on;
        lp->fold_p = lp->pos;
        lp->fold_left = lp->len;
    }
    lp->has_layer = lp->layer_on = true;
    lp->st = LOOPER_DUB;
    gate(lp, 1);
}

static void play_from_top(looper_t *lp)
{
    lp->pos = 0;
    lp->env = 0;
    lp->env_target = 1.0f;
    lp->after_fade = AFTER_NONE;
    lp->st = LOOPER_PLAY;
}

static void handle(looper_t *lp, looper_cmd_t cmd)
{
    switch (cmd) {
    case LOOPER_CMD_REC_DUB:
        switch (lp->st) {
        case LOOPER_EMPTY: start_rec(lp); break;
        case LOOPER_REC: if (!lp->target) fix(lp); break;
        case LOOPER_PLAY: dub_start(lp); break;
        case LOOPER_DUB: gate(lp, 0); lp->st = LOOPER_PLAY; break;
        case LOOPER_STOP: play_from_top(lp); dub_start(lp); break;
        }
        break;
    case LOOPER_CMD_PLAY_STOP:
        switch (lp->st) {
        case LOOPER_EMPTY: break;
        case LOOPER_REC: if (!lp->target) fix(lp); break;
        case LOOPER_PLAY:
        case LOOPER_DUB:
            if (lp->st == LOOPER_DUB) gate(lp, 0);
            lp->env_target = 0;
            lp->after_fade = AFTER_STOP;
            lp->st = LOOPER_STOP;
            break;
        case LOOPER_STOP: play_from_top(lp); break;
        }
        break;
    case LOOPER_CMD_UNDO:
        if (lp->st == LOOPER_REC) { reset(lp); break; }
        if (lp->has_layer) {
            if (lp->layer_on) {
                if (lp->st == LOOPER_DUB) { gate(lp, 0); lp->st = LOOPER_PLAY; }
                lp->layer_on = false;
            }
            break;
        }
        // no overdub on the take yet: the take itself is undone - faded out, then the part is empty (asked for by the
        // user on 2026-11-19: "undo after the first take of part B did nothing")
        if (lp->st != LOOPER_EMPTY) {
            lp->dub_target = 0;
            lp->pending_gate = -1;
            lp->env_target = 0;
            lp->after_fade = AFTER_CLEAR;
        }
        break;
    case LOOPER_CMD_REDO:
        if (lp->has_layer && !lp->layer_on) lp->layer_on = true;
        break;
    case LOOPER_CMD_CLEAR:
        if (lp->st == LOOPER_EMPTY || lp->st == LOOPER_REC) { reset(lp); break; }
        lp->dub_target = 0;
        lp->pending_gate = -1;
        lp->env_target = 0;
        lp->after_fade = AFTER_CLEAR;
        break;
    }
}

// ---- audio ----

static void fold_steps(looper_t *lp)
{
    for (int k = 0; k < FOLD_PER_FRAME && lp->fold_on; k++) {
        uint32_t p = lp->fold_p;
        if (lp->fold_keep) {
            lp->base[2 * p] = sat16(lp, f16(lp->base[2 * p]) + f16(lp->layer[2 * p]));
            lp->base[2 * p + 1] = sat16(lp, f16(lp->base[2 * p + 1]) + f16(lp->layer[2 * p + 1]));
        }
        lp->layer[2 * p] = lp->layer[2 * p + 1] = 0;
        lp->fold_p = p + 1 == lp->len ? 0 : p + 1;
        if (--lp->fold_left == 0) lp->fold_on = false;
    }
}

static inline float gain_in(const looper_t *lp, uint32_t p) { return p < lp->xfade ? (float)p / lp->xfade : 1.0f; }

static inline float gain_tail(const looper_t *lp, uint32_t p)
{
    return p + lp->xfade >= lp->tail ? (float)(lp->tail - p) / lp->xfade : 1.0f;
}

static void tail_steps(looper_t *lp)
{
    // lays what was played after the end over the start: start fades in, the tail fades out
    for (int k = 0; k < FOLD_PER_FRAME && lp->tail_pending; k++) {
        uint32_t p = lp->tail_done, s = lp->len + p;
        float gi = gain_in(lp, p), gt = gain_tail(lp, p);
        lp->base[2 * p] = sat16(lp, f16(lp->base[2 * p]) * gi + f16(lp->base[2 * s]) * gt);
        lp->base[2 * p + 1] = sat16(lp, f16(lp->base[2 * p + 1]) * gi + f16(lp->base[2 * s + 1]) * gt);
        if (++lp->tail_done >= lp->tail) lp->tail_pending = false;
    }
}

void looper_process(looper_t *lp, const float *in, float *out, uint32_t frames)
{
    bool open = false;   // (a change of the loop is open in this render: see looper_save_begin)
    for (uint32_t r = atomic_load_explicit(&lp->qr, memory_order_relaxed);
         r != atomic_load_explicit(&lp->qw, memory_order_acquire); r++) {
        looper_cmd_t cmd = (looper_cmd_t)lp->queue[r % LOOPER_QUEUE];
        if (!open && command_edits(lp, cmd)) edit_open(lp), open = true;
        handle(lp, cmd);
        atomic_store_explicit(&lp->qr, r + 1, memory_order_release);
    }
    if (!open && (writes(lp) || lp->after_fade == AFTER_CLEAR)) edit_open(lp), open = true;
    if (lp->st == LOOPER_EMPTY) lp->latency = atomic_load(&lp->set_latency);
    const float env_step = 1.0f / (lp->xfade ? lp->xfade : 1);

    for (uint32_t k = 0; k < frames; k++) {
        float il = in[2 * k], ir = in[2 * k + 1];
        out[2 * k] = out[2 * k + 1] = 0;
        lp->t++;
        if (lp->pending_gate >= 0 && (int32_t)(lp->t - lp->pending_at) >= 0) {
            lp->dub_target = (float)lp->pending_gate;
            lp->pending_gate = -1;
        }

        // first take (and after its end, the tail), written in a line
        if (lp->rec_on) {
            int64_t i = (int64_t)lp->rec_n - lp->latency;
            uint32_t limit = lp->rec_end ? lp->rec_end : lp->cap;
            if (i >= 0 && (uint64_t)i < limit) {
                lp->base[2 * i] = sat16(lp, il);
                lp->base[2 * i + 1] = sat16(lp, ir);
                if (lp->rec_end && (uint64_t)i + 1 >= lp->rec_end) {
                    lp->rec_on = false;
                    lp->tail_pending = lp->tail > 0;
                    lp->tail_done = 0;
                }
            }
            lp->rec_n++;
            if (lp->st == LOOPER_REC) {
                lp->pos = lp->rec_n;
                if (lp->target && lp->rec_n >= lp->target) fix_at(lp, lp->target);
                else if (!lp->target && lp->rec_n + reserve(lp) >= lp->cap) fix_at(lp, lp->rec_n);
                continue;   // nothing plays while the first take is recorded
            }
        }
        if (!lp->len) continue;

        // a stopped loop still runs until it has faded out and the overdub gate is shut
        bool running = lp->st == LOOPER_PLAY || lp->st == LOOPER_DUB || lp->env > 0 || lp->dub_env > 0 ||
                       lp->pending_gate >= 0 || lp->after_fade != AFTER_NONE;
        if (!running) continue;

        // overdub into the layer, at the position the input belongs to (latency behind)
        if (lp->dub_env > 0 || lp->dub_target > 0) {
            uint32_t w = (lp->pos + lp->len - lp->latency % lp->len) % lp->len;
            if (lp->dub_env > 0) {
                lp->layer[2 * w] = sat16(lp, f16(lp->layer[2 * w]) + il * lp->dub_env);
                lp->layer[2 * w + 1] = sat16(lp, f16(lp->layer[2 * w + 1]) + ir * lp->dub_env);
            }
            if (lp->dub_env < lp->dub_target) { lp->dub_env += env_step; if (lp->dub_env > 1) lp->dub_env = 1; }
            else if (lp->dub_env > lp->dub_target) { lp->dub_env -= env_step; if (lp->dub_env < 0) lp->dub_env = 0; }
        }
        fold_steps(lp);
        tail_steps(lp);

        uint32_t p = lp->pos;
        float bl = f16(lp->base[2 * p]), br = f16(lp->base[2 * p + 1]);
        if (lp->first_pass && p < lp->tail && (lp->rec_on || (lp->tail_pending && p >= lp->tail_done))) {
            float g = gain_in(lp, p);   // not merged yet: the start fades in (the tail is still being played live)
            bl *= g;
            br *= g;
        }
        if (lp->layer_on) {
            bl += f16(lp->layer[2 * p]);
            br += f16(lp->layer[2 * p + 1]);
        }
        if (lp->env != lp->env_target) {
            if (lp->env < lp->env_target) { lp->env += env_step; if (lp->env > lp->env_target) lp->env = lp->env_target; }
            else { lp->env -= env_step; if (lp->env < lp->env_target) lp->env = lp->env_target; }
        }
        out[2 * k] = bl * lp->env;
        out[2 * k + 1] = br * lp->env;

        if (++lp->pos >= lp->len) {
            lp->pos = 0;
            lp->passes++;
            lp->first_pass = false;
        }

        if (lp->env <= 0 && lp->after_fade != AFTER_NONE && lp->dub_env <= 0 && lp->pending_gate < 0) {
            if (lp->after_fade == AFTER_CLEAR) reset(lp);
            else { lp->pos = 0; lp->after_fade = AFTER_NONE; }
        }
    }

    atomic_store_explicit(&lp->i_state, lp->st, memory_order_relaxed);
    atomic_store_explicit(&lp->i_len, lp->len, memory_order_relaxed);
    atomic_store_explicit(&lp->i_pos, lp->pos, memory_order_relaxed);
    atomic_store_explicit(&lp->i_passes, lp->passes, memory_order_relaxed);
    atomic_store_explicit(&lp->i_clipped, lp->clipped, memory_order_relaxed);
    uint32_t f = (lp->target ? FLAG_BAR : 0) | ((lp->has_layer ? lp->layer_on : lp->st != LOOPER_EMPTY) ? FLAG_UNDO : 0) |
                 (lp->has_layer && !lp->layer_on ? FLAG_REDO : 0);
    atomic_store_explicit(&lp->i_flags, f, memory_order_release);
    if (open) edit_close(lp);
    else atomic_store_explicit(&lp->i_quiet, quiet(lp), memory_order_relaxed);
}

void looper_get_info(looper_t *lp, looper_info_t *out)
{
    uint32_t f = atomic_load_explicit(&lp->i_flags, memory_order_acquire);
    out->state = (looper_state_t)atomic_load_explicit(&lp->i_state, memory_order_relaxed);
    out->len = atomic_load_explicit(&lp->i_len, memory_order_relaxed);
    out->pos = atomic_load_explicit(&lp->i_pos, memory_order_relaxed);
    out->passes = atomic_load_explicit(&lp->i_passes, memory_order_relaxed);
    out->clipped = atomic_load_explicit(&lp->i_clipped, memory_order_relaxed);
    out->to_bar_line = f & FLAG_BAR;
    out->can_undo = f & FLAG_UNDO;
    out->can_redo = f & FLAG_REDO;
    out->max_len = lp->cap > 2 * lp->xfade + 64 ? lp->cap - 2 * lp->xfade - 64 : 0;
}

// ---- for loop_parts (audio side only) ----

void looper_command_now(looper_t *lp, looper_cmd_t cmd)
{
    bool open = command_edits(lp, cmd);
    if (open) edit_open(lp);
    handle(lp, cmd);
    if (open) edit_close(lp);
    else atomic_store_explicit(&lp->i_quiet, quiet(lp), memory_order_relaxed);
}

void looper_start_back(looper_t *lp, uint32_t frames_ago, const int16_t *pre, uint32_t n)
{
    edit_open(lp);
    start_rec(lp);
    lp->rec_n = frames_ago;
    // the input of the last n frames belongs to loop frames frames_ago - latency - n ... (those below 0: before it)
    int64_t first = (int64_t)frames_ago - lp->latency - n;
    for (uint32_t k = 0; pre && k < n; k++) {   // (pre NULL: the caller fills them in)
        int64_t i = first + k;
        if (i < 0 || (uint64_t)i >= lp->cap) continue;
        lp->base[2 * i] = pre[2 * k];
        lp->base[2 * i + 1] = pre[2 * k + 1];
    }
    lp->pos = lp->rec_n;
    edit_close(lp);
}

// The part is left at its loop's end: an overdub stops on the spot (the other part follows in the same frame), the
// next start is from the top. The overdub's last xfade frames in the layer are faded into the frame after them: cut
// there, the step clicked on every later pass of the part (2026-11-27). (That frame is 0 after a fresh overdub's
// fold, or an earlier pass in the same layer, which then runs on without a step.)
void looper_leave(looper_t *lp)
{
    bool open = !quiet(lp);   // (an overdub stops here: what the loop holds changes; a loop that only played: nothing)
    if (open) edit_open(lp);
    if (lp->dub_env > 0 && lp->len > lp->xfade) {
        uint32_t end = (lp->pos + lp->len - lp->latency % lp->len) % lp->len;   // the frame it would have written next
        float nl = lp->layer[2 * end], nr = lp->layer[2 * end + 1];
        for (uint32_t j = 1; j <= lp->xfade; j++) {
            uint32_t w = (end + lp->len - j) % lp->len;
            float g = (float)(j - 1) / lp->xfade;   // 0 at the last written frame
            lp->layer[2 * w] = (int16_t)(nl + (lp->layer[2 * w] - nl) * g);
            lp->layer[2 * w + 1] = (int16_t)(nr + (lp->layer[2 * w + 1] - nr) * g);
        }
    }
    if (lp->st == LOOPER_DUB) lp->st = LOOPER_PLAY;
    lp->dub_env = lp->dub_target = 0;
    lp->pending_gate = -1;
    lp->pos = 0;
    atomic_store_explicit(&lp->i_state, lp->st, memory_order_relaxed);   // (it is not processed until it comes back)
    atomic_store_explicit(&lp->i_pos, 0, memory_order_relaxed);
    if (open) edit_close(lp);
    else atomic_store_explicit(&lp->i_quiet, quiet(lp), memory_order_relaxed);
}

// Back to a part that was left: from the top, at full level at once (the other part ended in the frame before).
void looper_enter(looper_t *lp)
{
    bool open = lp->after_fade == AFTER_CLEAR;   // (a clear's fade cut short: the loop stays after all)
    if (open) edit_open(lp);
    lp->pos = 0;
    lp->env = lp->env_target = 1.0f;
    if (lp->st == LOOPER_STOP) lp->st = LOOPER_PLAY;
    lp->after_fade = AFTER_NONE;
    if (open) edit_close(lp);
    else atomic_store_explicit(&lp->i_quiet, quiet(lp), memory_order_relaxed);
}

// ---- saving the loop (another task; see looper.h) ----

bool looper_save_begin(looper_t *lp, looper_save_t *s)
{
    uint32_t e = atomic_load_explicit(&lp->edit, memory_order_acquire);
    if ((e & 1) || !atomic_load_explicit(&lp->i_quiet, memory_order_relaxed)) return false;
    // (the audio side's own fields: read as they are, the count tells whether they belong together)
    *s = (looper_save_t){ .edit = e, .len = lp->len, .bar = lp->bar, .xfade = lp->xfade, .tail = lp->tail,
                          .tail_done = lp->tail_done, .fold_p = lp->fold_p, .fold_left = lp->fold_left,
                          .layer_on = lp->layer_on, .tail_pending = lp->tail_pending, .fold_on = lp->fold_on,
                          .fold_keep = lp->fold_keep };
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&lp->edit, memory_order_relaxed) != e) return false;
    if (!s->len || s->len > lp->cap) return false;
    if (s->tail_pending && (s->tail > lp->cap - s->len || s->tail_done > s->tail)) return false;   // (never: checked
    if (s->fold_on && (s->fold_p >= s->len || s->fold_left > s->len)) return false;                //  for safety)
    return true;
}

bool looper_save_same(looper_t *lp, const looper_save_t *s)
{
    atomic_thread_fence(memory_order_acquire);   // (the piece's reads before the count is looked at again)
    return atomic_load_explicit(&lp->edit, memory_order_relaxed) == s->edit;
}

uint32_t looper_edit_count(looper_t *lp) { return atomic_load_explicit(&lp->edit, memory_order_acquire); }

// sat16 without counting (the copy changes nothing of the looper's)
static inline int16_t round16(float v)
{
    v *= 32767.0f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
}

static inline int16_t clip16(int32_t v, uint32_t *clipped)
{
    if (v > 32767) { (*clipped)++; return 32767; }
    if (v < -32768) { (*clipped)++; return -32768; }
    return (int16_t)v;
}

void looper_save_read(const looper_t *lp, const looper_save_t *s, uint32_t from, uint32_t n, int16_t *dst, uint32_t *clipped)
{
    const float xf = (float)(s->xfade ? s->xfade : 1);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t p = from + k;
        if (p >= s->len) { dst[2 * k] = dst[2 * k + 1] = 0; continue; }
        int32_t b[2] = { lp->base[2 * p], lp->base[2 * p + 1] };
        if (s->tail_pending && p >= s->tail_done && p < s->tail) {   // the seam, as tail_steps will lay it
            float gi = p < s->xfade ? (float)p / xf : 1.0f;
            float gt = p + s->xfade >= s->tail ? (float)(s->tail - p) / xf : 1.0f;
            for (int c = 0; c < 2; c++) b[c] = round16(f16((int16_t)b[c]) * gi + f16(lp->base[2 * (s->len + p) + c]) * gt);
        }
        bool unmerged = s->fold_on && (p + s->len - s->fold_p) % s->len < s->fold_left;
        for (int c = 0; c < 2; c++) {
            int32_t v = b[c];
            if (unmerged) {   // the overdub before, merged into the loop (kept) or dropped (undone) as fold_steps will
                if (s->fold_keep) v = round16(f16((int16_t)v) + f16(lp->layer[2 * p + c]));
            } else if (s->layer_on) {
                v += lp->layer[2 * p + c];
            }
            dst[2 * k + c] = clip16(v, clipped);
        }
    }
}
