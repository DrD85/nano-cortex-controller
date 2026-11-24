// Mixer for the Nano's USB playback (see audio_mix.h).
#include "audio_mix.h"

#include <math.h>
#include <string.h>

#define CEILING 0.98f           // the limiter keeps the sum below this
#define RELEASE_MS 150
#define GAIN_RAMP_MS 20

static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

void audio_mix_init(audio_mix_t *m, audio_pipe_t *phone, track_player_t *track, looper_t *looper)
{
    memset(m, 0, sizeof(*m));
    m->phone = phone;
    m->track = track;
    atomic_init(&m->looper, looper);
    m->lim = 1.0f;
    for (int i = 0; i < LIM_AHEAD; i++) m->lim_t[i] = m->lim_box[i] = 1.0f;
    m->lim_gr = 1.0f;
    m->lim_sum = LIM_AHEAD;
    for (int i = 0; i < AUDIO_SRC_COUNT; i++) {
        m->gain[i] = 1.0f;
        atomic_init(&m->gain_bits[i], fbits(1.0f));
    }
    atomic_init(&m->cw, 0);
    atomic_init(&m->cr, 0);
    atomic_init(&m->lb_delay, 0);
    atomic_init(&m->lb_gain_bits, fbits(0.0f));
}

void audio_mix_set_looper(audio_mix_t *m, looper_t *looper) { atomic_store_explicit(&m->looper, looper, memory_order_release); }
void audio_mix_set_drums(audio_mix_t *m, drum_machine_t *drums) { atomic_store_explicit(&m->drums, drums, memory_order_release); }
void audio_mix_set_parts(audio_mix_t *m, loop_parts_t *parts) { atomic_store_explicit(&m->parts, parts, memory_order_release); }

void audio_mix_set_gain_db(audio_mix_t *m, audio_src_t src, float db)
{
    if (src >= AUDIO_SRC_COUNT) return;
    atomic_store(&m->gain_bits[src], fbits(db <= -90.0f ? 0.0f : powf(10.0f, db / 20.0f)));
}

void audio_mix_set_loopback(audio_mix_t *m, uint32_t delay_frames, float gain)
{
    atomic_store(&m->lb_delay, delay_frames);
    atomic_store(&m->lb_gain_bits, fbits(gain));
}

void audio_mix_capture(audio_mix_t *m, const int32_t *slots, uint32_t frames, int channels, int ch_left, int ch_right)
{
    if (ch_left >= channels || ch_right >= channels) return;
    uint32_t w = atomic_load_explicit(&m->cw, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&m->cr, memory_order_acquire);
    for (uint32_t i = 0; i < frames; i++) {
        if (w - r >= MIX_CAP_RING - 1) {   // the render is not taking them: drop the oldest
            r++;
            atomic_fetch_add(&m->s_dropped, 1);
            atomic_store_explicit(&m->cr, r, memory_order_release);   // (render side re-reads cr)
        }
        uint32_t k = w & (MIX_CAP_RING - 1);
        m->cap[2 * k] = (float)(slots[i * channels + ch_left] >> 8) * (1.0f / 8388608.0f);
        m->cap[2 * k + 1] = (float)(slots[i * channels + ch_right] >> 8) * (1.0f / 8388608.0f);
        w++;
    }
    atomic_store_explicit(&m->cw, w, memory_order_release);
}

// The next capture frame for the looper (with the board's own playback taken out). false = none there.
static bool next_capture(audio_mix_t *m, float *l, float *r)
{
    uint32_t w = atomic_load_explicit(&m->cw, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&m->cr, memory_order_relaxed);
    if (!m->primed) {
        if (w - rd < MIX_CUSHION) return false;
        if (w - rd > MIX_CUSHION + 48) {   // more than the cushion came before the first render (363 frames at one
            rd = w - MIX_CUSHION;           // start of the board): begin with the newest cushion, not with old sound
            atomic_store_explicit(&m->cr, rd, memory_order_release);
        }
        m->primed = m->seen = 1;
        m->cap_index = rd;
        m->since_primed = 0;
        m->low = UINT32_MAX;
    }
    // Settling: the lowest fill is measured over every second and brought back to the cushion when it is off by
    // more than a few frames - but only while the looper is empty (a correction moves the round trip, and a loop
    // in the making must not move). The streams are locked packet by packet, so this is only needed after starts
    // and other upsets (seen once on the board: 96 frames lower after the board's start than after a reconnect).
    m->since_primed++;
    if (m->since_primed >= MIX_SETTLE && w - rd < m->low) m->low = w - rd;
    if (m->since_primed >= MIX_SETTLE_END) {
        looper_t *lp = atomic_load_explicit(&m->looper, memory_order_relaxed);
        loop_parts_t *lparts = atomic_load_explicit(&m->parts, memory_order_relaxed);
        bool empty = lparts ? lparts->part[0].st == LOOPER_EMPTY && lparts->part[1].st == LOOPER_EMPTY : !lp || lp->st == LOOPER_EMPTY;
        int32_t d = m->low == UINT32_MAX ? 0 : (int32_t)m->low - MIX_CUSHION;
        if (empty && (d > 60 || d < -60)) {   // (not within one transfer: the order of the callbacks moves the lowest fill by 48)
            m->settled = d;
            m->settle_count++;
            if (d > 0) {   // too much waiting: drop the oldest
                rd += (uint32_t)d;
                atomic_store_explicit(&m->cr, rd, memory_order_release);
            } else {
                m->hold = (uint32_t)-d;
            }
        }
        m->since_primed = MIX_SETTLE_END - 48000;   // the next window: one second
        m->low = UINT32_MAX;
    }
    if (m->hold) {   // too little waiting: the looper hears silence for these frames, the capture piles up
        m->hold--;
        m->holding = 1;
        return false;
    }
    if (w == rd) {
        // Ran dry - the streams stopped or started anew: wait for the whole cushion again. (Left as it was, the
        // cushion stayed wherever the restart put it: 103 and 113 frames instead of 192 on the board, and the
        // looper's round trip moved by as much.)
        m->primed = 0;
        return false;
    }
    if (w - rd > MIX_CUSHION + MIX_SLACK) {   // far too many waiting (capture ran without the render): the newest cushion
        uint32_t skip = w - rd - MIX_CUSHION;
        atomic_fetch_add(&m->s_dropped, skip);
        rd += skip;
    }
    m->cap_index = rd;   // capture frames are counted by the ring's read index (drops included)
    uint32_t k = rd & (MIX_CAP_RING - 1);
    *l = m->cap[2 * k];
    *r = m->cap[2 * k + 1];
    atomic_store_explicit(&m->cr, rd + 1, memory_order_release);

    float g = bitsf(atomic_load_explicit(&m->lb_gain_bits, memory_order_relaxed));
    if (g != 0) {
        uint32_t d = atomic_load_explicit(&m->lb_delay, memory_order_relaxed);
        uint32_t src = m->cap_index - d;   // render index of the sound that came back in this frame
        uint32_t age = m->render_index - src;
        if (age >= 1 && age <= MIX_HIST) {
            uint32_t h = src & (MIX_HIST - 1);
            *l -= g * m->hist[2 * h];
            *r -= g * m->hist[2 * h + 1];
        }
    }
    return true;
}

static void ramp_gains(audio_mix_t *m, float *step)
{
    for (int i = 0; i < AUDIO_SRC_COUNT; i++) {
        float target = bitsf(atomic_load_explicit(&m->gain_bits[i], memory_order_relaxed));
        step[i] = (target - m->gain[i]) / (GAIN_RAMP_MS * 48.0f);   // per frame at 48 kHz
        if (fabsf(target - m->gain[i]) < 1e-6f) { m->gain[i] = target; step[i] = 0; }
    }
}

void audio_mix_render(audio_mix_t *m, int32_t *out, uint32_t frames, int channels, int ch_left, int ch_right)
{
    float a[MIX_BLOCK * 2], b[MIX_BLOCK * 2], c[MIX_BLOCK * 2], dr[MIX_BLOCK * 2], in[MIX_BLOCK * 2];
    float step[AUDIO_SRC_COUNT];
    const float release = 1.0f / (RELEASE_MS * 48.0f);
    memset(out, 0, sizeof(int32_t) * frames * channels);
    uint32_t (*clock)(void) = m->clock;
    uint32_t t_prev = clock ? clock() : 0, took[AUDIO_MIX_STAGES] = { 0 };
#define STAGE(i) do { if (clock) { uint32_t t_now = clock(); took[i] += t_now - t_prev; t_prev = t_now; } } while (0)

    for (uint32_t done = 0; done < frames;) {
        uint32_t n = frames - done > MIX_BLOCK ? MIX_BLOCK : frames - done;
        ramp_gains(m, step);

        for (uint32_t i = 0; i < n; i++) {
            bool was_primed = m->primed;
            m->holding = 0;
            if (!next_capture(m, &in[2 * i], &in[2 * i + 1])) {
                in[2 * i] = in[2 * i + 1] = 0;
                if ((was_primed || m->seen) && !m->holding) atomic_fetch_add(&m->s_short, 1);   // (also while the cushion fills again)
            }
        }
        for (uint32_t i = 0; i < 2 * n; i++) if (fabsf(in[i]) > m->peak_in) m->peak_in = fabsf(in[i]);
        memcpy(m->clean, in, sizeof(float) * 2 * n);
        m->clean_n = n;
        STAGE(0);

        if (m->phone) audio_pipe_render_stereo(m->phone, a, n); else memset(a, 0, sizeof(float) * 2 * n);
        STAGE(1);
        if (m->track) track_player_render_stereo(m->track, b, n); else memset(b, 0, sizeof(float) * 2 * n);
        STAGE(2);
        looper_t *lp = atomic_load_explicit(&m->looper, memory_order_acquire);
        loop_parts_t *lparts = atomic_load_explicit(&m->parts, memory_order_acquire);
        if (lparts) loop_parts_process(lparts, in, c, n);
        else if (lp) looper_process(lp, in, c, n);
        else memset(c, 0, sizeof(float) * 2 * n);
        STAGE(3);
        memset(dr, 0, sizeof(float) * 2 * n);
        drum_machine_t *dm = atomic_load_explicit(&m->drums, memory_order_acquire);
        if (dm) drum_machine_render(dm, dr, n);
        STAGE(4);

        for (uint32_t i = 0; i < n; i++) {
            for (int s = 0; s < AUDIO_SRC_COUNT; s++) m->gain[s] += step[s];
            float l = a[2 * i] * m->gain[0] + b[2 * i] * m->gain[1] + c[2 * i] * m->gain[2] + dr[2 * i] * m->gain[3];
            float r = a[2 * i + 1] * m->gain[0] + b[2 * i + 1] * m->gain[1] + c[2 * i + 1] * m->gain[2] + dr[2 * i + 1] * m->gain[3];
            const float *src[AUDIO_SRC_COUNT] = { a, b, c, dr };
            for (int s = 0; s < AUDIO_SRC_COUNT; s++) {
                float v = fmaxf(fabsf(src[s][2 * i]), fabsf(src[s][2 * i + 1])) * m->gain[s];
                if (v > m->peak_src[s]) m->peak_src[s] = v;
            }
#if MIX_LOOKAHEAD
            // Look-ahead limiter: each frame's needed gain (ceiling / peak) holds for the next LIM_AHEAD frames (the
            // window's lowest), comes back slowly (release), and is averaged over LIM_AHEAD frames - so the gain has
            // come down smoothly by the time the delayed peak goes out, and never later than it.
            float peak = fmaxf(fabsf(l), fabsf(r));
            if (peak > m->peak_sum) m->peak_sum = peak;
            uint32_t li = m->lim_i;
            m->lim_t[li] = peak > CEILING ? CEILING / peak : 1.0f;
            float need = 1.0f;
            for (int j = 0; j < LIM_AHEAD; j++) need = fminf(need, m->lim_t[j]);
            m->lim_gr = fminf(need, fminf(1.0f, m->lim_gr + release));
            m->lim_sum += m->lim_gr - m->lim_box[li];
            m->lim_box[li] = m->lim_gr;
            if (li == 0) {   // (now and then summed afresh: no drift)
                float sum = 0;
                for (int j = 0; j < LIM_AHEAD; j++) sum += m->lim_box[j];
                m->lim_sum = sum;
            }
            float g = m->lim_sum * (1.0f / LIM_AHEAD);
            float dl = m->lim_dl[2 * li], dr_ = m->lim_dl[2 * li + 1];
            m->lim_dl[2 * li] = l;
            m->lim_dl[2 * li + 1] = r;
            m->lim_i = li + 1 == LIM_AHEAD ? 0 : li + 1;
            l = dl;
            r = dr_;
            if (g < 0.9999f) {
                atomic_fetch_add_explicit(&m->s_limited, 1, memory_order_relaxed);
                l *= g;
                r *= g;
                if (g < m->lim_min) m->lim_min = g;
            }
#else
            // limiter: turns down at once when the sum would go over the ceiling, comes back slowly
            float peak = fmaxf(fabsf(l), fabsf(r));
            if (peak > m->peak_sum) m->peak_sum = peak;
            if (peak * m->lim > CEILING) m->lim = CEILING / peak;
            if (m->lim < 1.0f) {
                atomic_fetch_add_explicit(&m->s_limited, 1, memory_order_relaxed);
                l *= m->lim;
                r *= m->lim;
                if (m->lim < m->lim_min) m->lim_min = m->lim;
                m->lim += release;
                if (m->lim > 1.0f) m->lim = 1.0f;
            }
#endif
            uint32_t h = m->render_index & (MIX_HIST - 1);
            m->hist[2 * h] = l;
            m->hist[2 * h + 1] = r;
            m->render_index++;
            out[(done + i) * channels + ch_left] = audio_to_slot(l);
            out[(done + i) * channels + ch_right] = audio_to_slot(r);
        }
        STAGE(5);
        done += n;
    }
#undef STAGE
    if (clock)
        for (int i = 0; i < AUDIO_MIX_STAGES; i++) if (took[i] > m->stage_max[i]) m->stage_max[i] = took[i];
}

void audio_mix_take_stages(audio_mix_t *m, uint32_t out[AUDIO_MIX_STAGES])
{
    for (int i = 0; i < AUDIO_MIX_STAGES; i++) {
        out[i] = m->stage_max[i];
        m->stage_max[i] = 0;   // (a race with the render loses at most one peak)
    }
}

void audio_mix_get_stats(audio_mix_t *m, audio_mix_stats_t *out)
{
    out->capture_short = atomic_load(&m->s_short);
    out->capture_dropped = atomic_load(&m->s_dropped);
    out->limited = atomic_load(&m->s_limited);
}

void audio_mix_last_clean(audio_mix_t *m, float *lr, uint32_t frames)
{
    if (frames > m->clean_n) frames = m->clean_n;
    memcpy(lr, m->clean, sizeof(float) * 2 * frames);
}

void audio_mix_take_levels(audio_mix_t *m, float src[AUDIO_SRC_COUNT], float *sum, float *lim_min)
{
    for (int i = 0; i < AUDIO_SRC_COUNT; i++) {
        src[i] = m->peak_src[i];
        m->peak_src[i] = 0;
    }
    *sum = m->peak_sum;
    *lim_min = m->lim_min ? m->lim_min : 1.0f;
    m->peak_sum = 0;
    m->lim_min = 1.0f;   // (a race with the render loses at most one peak)
}

float audio_mix_take_input_peak(audio_mix_t *m)
{
    float p = m->peak_in;
    m->peak_in = 0;
    return p;
}
