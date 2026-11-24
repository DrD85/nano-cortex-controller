// Two looper parts and the drums beside them (see loop_parts.h).
#include "loop_parts.h"

#include <math.h>
#include <string.h>

void loop_parts_init(loop_parts_t *p, int16_t *mem, uint32_t frames, uint32_t rate, int16_t *pre)
{
    memset(p, 0, sizeof(*p));
    p->click_next = -1;
    p->pre = pre;
    if (pre) memset(pre, 0, sizeof(int16_t) * 2 * LOOP_PRE);
    p->floor = 0.001f;
    looper_init(&p->part[0], mem, frames, rate);
    looper_init(&p->part[1], mem + 4 * (size_t)frames, frames, rate);
    p->next = -1;
    atomic_init(&p->want_part, -1);
    atomic_init(&p->want_pause, -1);
    atomic_init(&p->want_drums, -1);
    atomic_init(&p->drum_sync, 1);
    atomic_init(&p->quantize, 1);
    atomic_init(&p->bar_frames, 0);
    atomic_init(&p->i_next, -1);
}

void loop_parts_set_drums(loop_parts_t *p, drum_machine_t *drums) { p->drums = drums; }

void loop_parts_command(loop_parts_t *p, looper_cmd_t cmd)
{
    looper_t *cur = &p->part[atomic_load(&p->i_active) & 1];
    looper_info_t li;
    looper_get_info(cur, &li);
    // a first take: counted in (one bar), armed (with the first note), or on the drums' bar line
    if (cmd == LOOPER_CMD_REC_DUB && li.state == LOOPER_EMPTY) {
        bool bar = atomic_load(&p->bar_frames) != 0, drums = atomic_load(&p->i_drums) != 0;
        if (atomic_load(&p->i_armed) || atomic_load(&p->i_counting)) {   // pressed again: called off
            atomic_store(&p->want_take, -1);
            return;
        }
        int how = atomic_load(&p->arm) ? 3 : atomic_load(&p->count_in) && bar ? 1 : atomic_load(&p->quantize) && bar && drums ? 2 : 0;
        if (how) {
            atomic_store(&p->want_take, how);
            return;
        }
    }
    if (cmd == LOOPER_CMD_UNDO || cmd == LOOPER_CMD_CLEAR) atomic_store(&p->want_take, -1);   // (calls a count-in off)
    looper_command(cur, cmd);
}

void loop_parts_choose(loop_parts_t *p, int part) { if (part == 0 || part == 1) atomic_store(&p->want_part, part); }
void loop_parts_pause(loop_parts_t *p) { atomic_store(&p->want_pause, atomic_load(&p->i_paused) ? 0 : 1); }
void loop_parts_drums(loop_parts_t *p, bool on) { atomic_store(&p->want_drums, on ? 1 : 0); }
void loop_parts_set_drum_sync(loop_parts_t *p, bool on) { atomic_store(&p->drum_sync, on ? 1 : 0); }
void loop_parts_set_count_in(loop_parts_t *p, bool on) { atomic_store(&p->count_in, on ? 1 : 0); }
void loop_parts_set_quantize(loop_parts_t *p, bool on) { atomic_store(&p->quantize, on ? 1 : 0); }
void loop_parts_set_arm(loop_parts_t *p, bool on) { atomic_store(&p->arm, on ? 1 : 0); }

// The last n input frames before frame k of this render (stereo int16), from the kept ones and this render's.
static void pre_copy(loop_parts_t *p, const float *in, uint32_t k, uint32_t n, int16_t *dst)
{
    for (uint32_t j = 0; j < n; j++) {
        int64_t f = (int64_t)k - n + j;   // < 0: from the kept frames
        float l, r;
        if (f >= 0) {
            l = in[2 * f], r = in[2 * f + 1];
        } else {
            if (f < -(int64_t)LOOP_PRE) { dst[2 * j] = dst[2 * j + 1] = 0; continue; }
            uint32_t w = (uint32_t)((p->pre_w + (int64_t)LOOP_PRE + f) % LOOP_PRE);
            dst[2 * j] = p->pre[2 * w];
            dst[2 * j + 1] = p->pre[2 * w + 1];
            continue;
        }
        dst[2 * j] = (int16_t)lrintf(fmaxf(-1, fminf(1, l)) * 32767);
        dst[2 * j + 1] = (int16_t)lrintf(fmaxf(-1, fminf(1, r)) * 32767);
    }
}

// Frames since the drums' last bar line, at frame k of this render (-1: not known).
static int64_t since_bar(loop_parts_t *p, uint32_t k)
{
    drum_machine_t *d = p->drums;
    if (!d || !d->running) return -1;
    double sf = drum_machine_step_frames(d), spb = drum_machine_steps_per_bar(d);
    double at = d->step_pos + k / sf;   // (the drums render after the parts: their position at the start of this render)
    if (at < 0) return -1;
    return (int64_t)llround(fmod(at, spb) * sf);
}

// A first take on the drums' bar line: a little late (up to half a bar, the input since then is kept) - from the bar
// line back; early - at the next bar line.
static void start_on_bar(loop_parts_t *p, const float *in, uint32_t k)
{
    looper_t *cur = &p->part[p->active];
    uint32_t bar = atomic_load(&p->bar_frames);
    int64_t since = since_bar(p, k);
    uint32_t lat = atomic_load(&cur->set_latency);
    if (since < 0 || !bar) {
        looper_command_now(cur, LOOPER_CMD_REC_DUB);
        return;
    }
    if (since <= bar / 2 && (since <= lat || since - lat <= LOOP_PRE) && p->pre) {
        uint32_t n = since > lat ? (uint32_t)(since - lat) : 0;   // input frames that belong to the take already
        looper_start_back(cur, (uint32_t)since, NULL, 0);
        int64_t first = (int64_t)since - cur->latency - n;        // their first frame in the loop
        uint32_t from_ring = n > k ? n - k : 0, from_in = n - from_ring;
        for (uint32_t j = 0; j < from_in; j++) {                  // the ones of this render: now
            int64_t i = first + from_ring + j;
            uint32_t f = k - from_in + j;
            if (i < 0 || (uint64_t)i >= cur->cap) continue;
            cur->base[2 * i] = (int16_t)lrintf(fmaxf(-1, fminf(1, in[2 * f])) * 32767);
            cur->base[2 * i + 1] = (int16_t)lrintf(fmaxf(-1, fminf(1, in[2 * f + 1])) * 32767);
        }
        p->bf_part = p->active;                                   // the kept ones: oldest first, over the next renders
        p->bf_left = from_ring;
        p->bf_src = (p->pre_w + LOOP_PRE - from_ring) % LOOP_PRE;
        p->bf_dst = first;
    } else {
        p->counting = (uint32_t)(bar - since);   // (at the next bar line, as after a count-in)
        p->click_next = -1;                      // (no clicks: the drums play)
    }
}

void loop_parts_set_bar(loop_parts_t *p, uint32_t drum_bar, uint32_t looper_bar)
{
    atomic_store(&p->bar_frames, drum_bar);
    looper_set_bar(&p->part[0], looper_bar);
    looper_set_bar(&p->part[1], looper_bar);
}

static bool playing(const looper_t *lp) { return lp->len > 0 && (lp->st == LOOPER_PLAY || lp->st == LOOPER_DUB); }

// The drums to the loop's place: `delay` frames into this render the loop is at frame `pos`.
static void drums_to_loop(loop_parts_t *p, uint32_t delay, uint32_t pos)
{
    drum_machine_t *d = p->drums;
    uint32_t bar = atomic_load(&p->bar_frames);
    if (!d || !bar) return;
    double steps = (double)(pos % bar) / bar * drum_machine_steps_per_bar(d);
    drum_machine_start_now(d, delay, steps);
}

// The other part takes over in frame `at` of this render.
static void switch_part(loop_parts_t *p, uint32_t at)
{
    looper_t *old = &p->part[p->active], *nu = &p->part[p->next];
    looper_leave(old);
    p->active = p->next;
    p->next = -1;
    if (nu->st == LOOPER_EMPTY) {
        looper_command_now(nu, LOOPER_CMD_REC_DUB);   // an empty part: its first take starts here
    } else {
        looper_enter(nu);
    }
    if (p->drums && p->drums->running) drums_to_loop(p, at, 0);   // (a part begins on a bar line)
}

void loop_parts_process(loop_parts_t *p, const float *in, float *out, uint32_t frames)
{
    drum_machine_t *d = p->drums;
    int want = atomic_exchange(&p->want_part, -1);
    if (want >= 0) {
        looper_t *cur = &p->part[p->active];
        if (want == p->active) {
            p->next = -1;                                // the part that plays: a waiting switch is called off
        } else if (cur->st == LOOPER_REC) {
            // (while the first take of the part that plays is recorded, a tap on the other part is ignored)
        } else if (!playing(cur)) {
            p->next = want;                              // nothing plays (empty or stopped): at once
            switch_part(p, 0);
        } else {
            p->next = want;                              // at the end of the loop that plays
        }
    }
    int pause = atomic_exchange(&p->want_pause, -1);
    if (pause >= 0) {
        p->paused = pause == 1;
        if (d) d->paused = p->paused;
    }
    int drums = atomic_exchange(&p->want_drums, -1);
    if (drums == 0 && d) drum_machine_stop(d);
    if (drums == 1 && d) {
        looper_t *cur = &p->part[p->active];
        if (playing(cur)) drums_to_loop(p, 0, cur->pos);   // in step with the loop that plays
        else drum_machine_start_now(d, 0, 0);
        d->paused = p->paused;
    }

    // the count-in: one bar of the drums (or four clicks when they are off), then the first take
    int take = atomic_exchange(&p->want_take, 0);
    uint32_t bar = atomic_load(&p->bar_frames);
    if (take == -1) p->counting = 0, p->armed = false, p->click_next = -1;
    if (take == 3) p->armed = true;
    bool late_start = false;   // (a quantised start in this render, after the parts have been processed up to it)
    if (take == 2 && !p->counting && !p->paused) late_start = true;
    if (take == 1 && !p->counting && bar) {
        p->counting = p->count_total = bar;
        p->count_beat = bar / 4;
        p->click_next = -1;
        if (d && d->running) drums_to_loop(p, 0, 0);    // the groove from its bar line, counted in
        else if (d) p->click_next = 0;                  // four clicks, each in the render its beat falls into
    }
    // (One at a time: struck together with their delays, each new hi-hat choked the waiting ones - only one click
    // was left, the user heard no count-in without the drums, 2026-11-19.)
    if (p->counting && p->click_next >= 0 && d) {
        uint32_t elapsed = p->count_total - p->counting;
        while (p->click_next < 4 && (uint32_t)p->click_next * p->count_beat < elapsed + frames) {
            uint32_t at = (uint32_t)p->click_next * p->count_beat;
            drum_machine_hit_now(d, DRUM_HAT, at > elapsed ? at - elapsed : 0, p->click_next ? 0.6f : 1.0f);
            p->click_next++;
        }
        if (p->click_next >= 4) p->click_next = -1;
    }
    if (late_start) {
        start_on_bar(p, in, 0);
        if (!p->counting) {   // started (back at the bar line): this render goes on recording
            looper_process(&p->part[p->active], in, out, frames);
            goto keep;
        }
    }
    if (p->armed && !p->paused) {   // waiting for the first note: the input's level against its noise
        memset(out, 0, sizeof(float) * 2 * frames);
        for (uint32_t k = 0; k < frames; k++) {
            float v = fmaxf(fabsf(in[2 * k]), fabsf(in[2 * k + 1]));
            if (v > ARM_LEVEL && v > ARM_OVER_FLOOR * p->floor) {
                p->armed = false;
                looper_t *cur = &p->part[p->active];
                if (atomic_load(&p->quantize) && d && d->running && bar) {
                    start_on_bar(p, in, k);
                    if (p->counting) {   // (the note came early: the take starts at the bar line)
                        if (p->counting > frames - k) p->counting -= frames - k;
                        else { uint32_t at = k + p->counting; p->counting = 0; looper_command_now(cur, LOOPER_CMD_REC_DUB); looper_process(cur, in + 2 * at, out + 2 * at, frames - at); }
                        goto keep;
                    }
                } else {
                    static int16_t pre[2 * ARM_MARGIN];
                    uint32_t lat = atomic_load(&cur->set_latency);
                    pre_copy(p, in, k, ARM_MARGIN, pre);
                    looper_start_back(cur, lat + ARM_MARGIN, pre, ARM_MARGIN);
                }
                looper_process(cur, in + 2 * k, out + 2 * k, frames - k);
                goto keep;
            }
            p->floor = v > p->floor ? p->floor + (v - p->floor) * 0.0005f : p->floor + (v - p->floor) * 0.01f;   // (slow up, faster down)
            if (p->floor < 0.0003f) p->floor = 0.0003f;
        }
        goto keep;
    }
    if (p->paused) {
        memset(out, 0, sizeof(float) * 2 * frames);
    } else if (p->counting) {
        memset(out, 0, sizeof(float) * 2 * frames);       // (nothing plays yet: the part is empty)
        if (p->counting > frames) {
            p->counting -= frames;
        } else {   // the bar is over in this render: the first take starts on its frame
            uint32_t at = p->counting;
            p->counting = 0;
            looper_t *cur = &p->part[p->active];
            looper_process(cur, in, out, at);
            looper_command_now(cur, LOOPER_CMD_REC_DUB);
            looper_process(cur, in + 2 * at, out + 2 * at, frames - at);
        }
    } else {
        uint32_t done = 0;
        while (done < frames) {
            looper_t *cur = &p->part[p->active];
            uint32_t n = frames - done;
            bool switching = p->next >= 0 && playing(cur);
            if (switching && cur->len - cur->pos < n) n = cur->len - cur->pos;   // up to the end of the loop
            looper_state_t before = cur->st;
            uint32_t passes = cur->passes;
            looper_process(cur, in + 2 * done, out + 2 * done, n);
            // a first take starts: the drums from the top with it
            if (before == LOOPER_EMPTY && cur->st == LOOPER_REC && d && atomic_load(&p->drum_sync) && !atomic_load(&p->count_in)) {
                drum_machine_start_now(d, done, 0);
                d->paused = false;
            }
            bool wrapped = cur->passes != passes;
            done += n;
            // the part was emptied (its take undone, or cleared) while the other one has a loop: that one plays again,
            // from the top - so undoing a fresh part B goes back to A
            looper_t *other = &p->part[p->active ^ 1];
            if (before != LOOPER_EMPTY && cur->st == LOOPER_EMPTY && other->st != LOOPER_EMPTY && other->st != LOOPER_REC) {
                p->next = p->active ^ 1;
                switch_part(p, done);
                continue;
            }
            if (switching && wrapped) {   // the loop came round: the other part from here
                switch_part(p, done);
                continue;
            }
            // a loop of whole bars came round: the drums back to its bar line (they never drift away from it)
            uint32_t bar = atomic_load(&p->bar_frames);
            if (wrapped && d && d->running && bar && cur->len % bar == 0 && cur->pos < n) drums_to_loop(p, done - cur->pos, 0);
        }
    }
keep:
    // a take started back at the bar line gets its kept input, the oldest first - before this render overwrites it
    if (p->bf_left) {
        looper_t *lp = &p->part[p->bf_part];
        uint32_t n = p->bf_left < BACKFILL_PER_RENDER ? p->bf_left : BACKFILL_PER_RENDER;
        if (lp->st == LOOPER_EMPTY) n = p->bf_left;   // (the take was called off: nothing to copy any more)
        for (uint32_t j = 0; j < n; j++) {
            int64_t i = p->bf_dst + j;
            uint32_t src = (p->bf_src + j) % LOOP_PRE;
            if (lp->st == LOOPER_EMPTY || i < 0 || (uint64_t)i >= lp->cap) continue;
            lp->base[2 * i] = p->pre[2 * src];
            lp->base[2 * i + 1] = p->pre[2 * src + 1];
        }
        p->bf_left -= n;
        p->bf_src = (p->bf_src + n) % LOOP_PRE;
        p->bf_dst += n;
    }
    // the input of this render is kept (starts a little in the past)
    if (p->pre) {
        for (uint32_t k = 0; k < frames; k++) {
            uint32_t w = p->pre_w;
            p->pre[2 * w] = (int16_t)lrintf(fmaxf(-1, fminf(1, in[2 * k])) * 32767);
            p->pre[2 * w + 1] = (int16_t)lrintf(fmaxf(-1, fminf(1, in[2 * k + 1])) * 32767);
            p->pre_w = (w + 1) % LOOP_PRE;
        }
    }
    atomic_store(&p->i_armed, p->armed);
    atomic_store(&p->i_active, p->active);
    atomic_store(&p->i_next, p->next);
    atomic_store(&p->i_paused, p->paused);
    atomic_store(&p->i_drums, d && d->running);
    atomic_store(&p->i_counting, p->counting > 0);
}

void loop_parts_info(loop_parts_t *p, loop_parts_info_t *out)
{
    out->active = atomic_load(&p->i_active);
    out->next = atomic_load(&p->i_next);
    out->paused = atomic_load(&p->i_paused);
    out->drums_on = atomic_load(&p->i_drums);
    out->counting = atomic_load(&p->i_counting);
    out->armed = atomic_load(&p->i_armed);
    looper_get_info(&p->part[0], &out->part[0]);
    looper_get_info(&p->part[1], &out->part[1]);
}
