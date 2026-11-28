// Jam-track player (see track_player.h).
#include "track_player.h"

#include <math.h>
#include <string.h>

enum { TP_IDLE, TP_PREFILL, TP_RUN, TP_FADEOUT, TP_PAUSED, TP_ENDED };
enum { F_PLAYING = 1, F_ENDED = 2 };

#define FADE_MS 6
#define SEEK_FADE_FRAMES 96
#define GAIN_RAMP_MS 20

static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

void track_player_init(track_player_t *tp, int16_t *buffer, uint32_t frames, uint32_t out_rate)
{
    memset(tp, 0, sizeof(*tp));
    tp->buf = buffer;
    tp->cap = frames;
    tp->out_rate = out_rate;
    tp->in_rate = out_rate;
    tp->step = 1.0;
    tp->gain = tp->gain_target = 1.0f;
    tp->state = TP_IDLE;
    atomic_init(&tp->w, 0);
    atomic_init(&tp->r, 0);
    atomic_init(&tp->req, 0);
    atomic_init(&tp->ack, 0);
    atomic_init(&tp->want_play, false);
    atomic_init(&tp->eof, false);
    atomic_init(&tp->gain_bits, fbits(1.0f));
    tp->gain_bits_seen = fbits(1.0f);
}

void track_player_begin(track_player_t *tp, uint32_t in_rate, uint32_t start, uint32_t total)
{
    atomic_store(&tp->b_rate, in_rate);
    atomic_store(&tp->b_start, start);
    atomic_store(&tp->b_total, total);
    atomic_store(&tp->eof, false);
    atomic_fetch_add_explicit(&tp->req, 1, memory_order_release);
}

bool track_player_ready(track_player_t *tp)
{
    return atomic_load_explicit(&tp->ack, memory_order_acquire) == atomic_load_explicit(&tp->req, memory_order_relaxed);
}

uint32_t track_player_space(track_player_t *tp)
{
    uint32_t w = atomic_load_explicit(&tp->w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&tp->r, memory_order_acquire);
    return tp->cap - 1 - (w - r);
}

uint32_t track_player_write(track_player_t *tp, const int16_t *stereo, uint32_t frames)
{
    if (!track_player_ready(tp)) return 0;   // a flush is pending: this data belongs to the old position
    uint32_t w = atomic_load_explicit(&tp->w, memory_order_relaxed);
    uint32_t space = track_player_space(tp);
    uint32_t n = frames < space ? frames : space;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (w + i) & (tp->cap - 1);
        tp->buf[2 * k] = stereo[2 * i];
        tp->buf[2 * k + 1] = stereo[2 * i + 1];
    }
    atomic_store_explicit(&tp->w, w + n, memory_order_release);
    return n;
}

void track_player_end_of_data(track_player_t *tp) { atomic_store_explicit(&tp->eof, true, memory_order_release); }

void track_player_set_playing(track_player_t *tp, bool play) { atomic_store(&tp->want_play, play); }

void track_player_set_gain_db(track_player_t *tp, float db)
{
    atomic_store(&tp->gain_bits, fbits(db <= -90.0f ? 0.0f : powf(10.0f, db / 20.0f)));
}

void track_player_get_info(track_player_t *tp, track_player_info_t *out)
{
    uint32_t f = atomic_load_explicit(&tp->i_flags, memory_order_acquire);
    out->playing = f & F_PLAYING;
    out->ended = f & F_ENDED;
    out->pos = atomic_load_explicit(&tp->i_pos, memory_order_relaxed);
    out->rate = atomic_load_explicit(&tp->i_rate, memory_order_relaxed);
    out->total = atomic_load_explicit(&tp->i_total, memory_order_relaxed);
    out->underruns = atomic_load_explicit(&tp->i_underruns, memory_order_relaxed);
}

// ---- audio side ----

static uint32_t fill(track_player_t *tp)
{
    return atomic_load_explicit(&tp->w, memory_order_acquire) - atomic_load_explicit(&tp->r, memory_order_relaxed);
}

static bool pull(track_player_t *tp)
{
    uint32_t r = atomic_load_explicit(&tp->r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&tp->w, memory_order_acquire);
    memmove(tp->win[0], tp->win[1], sizeof(tp->win[0]) * 3);
    if (r == w) {
        tp->win[3][0] = tp->win[3][1] = 0;   // past the end: the interpolation runs out into silence
        return false;
    }
    uint32_t k = r & (tp->cap - 1);
    tp->win[3][0] = tp->buf[2 * k] * (1.0f / 32768.0f);
    tp->win[3][1] = tp->buf[2 * k + 1] * (1.0f / 32768.0f);
    atomic_store_explicit(&tp->r, r + 1, memory_order_release);
    tp->pos++;
    return true;
}

static void flush(track_player_t *tp)
{
    tp->in_rate = atomic_load(&tp->b_rate);
    tp->pos = atomic_load(&tp->b_start);
    tp->total = atomic_load(&tp->b_total);
    tp->step = (double)tp->in_rate / tp->out_rate;
    tp->prefill = tp->in_rate / 4;                 // 250 ms before it starts
    if (tp->prefill > tp->cap / 2) tp->prefill = tp->cap / 2;
    atomic_store_explicit(&tp->r, atomic_load_explicit(&tp->w, memory_order_acquire), memory_order_release);
    memset(tp->win, 0, sizeof(tp->win));
    tp->frac = 0;
    tp->env = 0;
    tp->flushing = false;
    tp->state = TP_PREFILL;
    atomic_store_explicit(&tp->ack, atomic_load_explicit(&tp->req, memory_order_acquire), memory_order_release);
}

static inline float cubic(float x0, float x1, float x2, float x3, float t)
{
    float c1 = 0.5f * (x2 - x0);
    float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

void track_player_render_stereo(track_player_t *tp, float *lr, uint32_t frames)
{
    memset(lr, 0, sizeof(float) * 2 * frames);
    if (atomic_load_explicit(&tp->req, memory_order_acquire) != atomic_load_explicit(&tp->ack, memory_order_relaxed)) {
        if (tp->state == TP_RUN && tp->env > 0) tp->flushing = true;   // fade out first, then empty
        else flush(tp);
    }
    uint32_t gb = atomic_load_explicit(&tp->gain_bits, memory_order_relaxed);
    if (gb != tp->gain_bits_seen) {
        tp->gain_bits_seen = gb;
        tp->gain_target = bitsf(gb);
        tp->gain_step = (tp->gain_target - tp->gain) / (GAIN_RAMP_MS * tp->out_rate / 1000.0f);
    }
    bool want = atomic_load_explicit(&tp->want_play, memory_order_relaxed);
    const float fade_step = 1000.0f / (FADE_MS * tp->out_rate);
    float step = (float)tp->step;   // (1 exactly at 48 kHz; at 44.1 kHz under a frame in 5 minutes off the double's)

    if (tp->state == TP_PREFILL && want && !tp->flushing) {
        bool eof = atomic_load_explicit(&tp->eof, memory_order_acquire);
        if (fill(tp) >= tp->prefill || (eof && fill(tp) > 0)) {
            for (int i = 0; i < 3; i++) pull(tp);
            tp->frac = 0;
            tp->env = 0;
            tp->state = TP_RUN;
        } else if (eof) {
            tp->state = TP_ENDED;
        }
    }
    if (tp->state == TP_PAUSED && want) tp->state = TP_RUN;

    for (uint32_t i = 0; i < frames; i++) {
        if (tp->state != TP_RUN && tp->state != TP_FADEOUT) break;
        float target = tp->flushing || tp->state == TP_FADEOUT || !want ? 0.0f : 1.0f;
        if (tp->flushing) {
            tp->env -= 1.0f / SEEK_FADE_FRAMES;
        } else if (tp->env < target) {
            tp->env += fade_step;
            if (tp->env > 1) tp->env = 1;
        } else if (tp->env > target) {
            tp->env -= fade_step;
        }
        if (tp->env <= 0 && target == 0) {
            tp->env = 0;
            if (tp->flushing) { flush(tp); break; }
            if (tp->state == TP_FADEOUT) {   // dry: wait for the buffer, keep the position
                tp->state = TP_PREFILL;
                memset(tp->win, 0, sizeof(tp->win));
                break;
            }
            tp->state = TP_PAUSED;           // paused: the window and position stay
            break;
        }

        while (tp->frac >= 1.0f && tp->state == TP_RUN) {
            tp->frac -= 1.0f;
            if (!pull(tp)) {
                if (atomic_load_explicit(&tp->eof, memory_order_acquire)) {
                    if (tp->win[2][0] == 0 && tp->win[2][1] == 0 && tp->win[1][0] == 0 && tp->win[1][1] == 0) {
                        tp->state = TP_ENDED;   // the last real frames have been played
                        atomic_store(&tp->want_play, false);
                    }
                } else {
                    atomic_fetch_add(&tp->i_underruns, 1);
                    tp->state = TP_FADEOUT;
                }
            }
        }
        if (tp->state == TP_ENDED) break;
        float t = tp->frac;
        float l = cubic(tp->win[0][0], tp->win[1][0], tp->win[2][0], tp->win[3][0], t);
        float r = cubic(tp->win[0][1], tp->win[1][1], tp->win[2][1], tp->win[3][1], t);
        if (tp->state == TP_FADEOUT) { l = tp->win[2][0]; r = tp->win[2][1]; }
        if (tp->gain != tp->gain_target) {
            tp->gain += tp->gain_step;
            if ((tp->gain_step > 0 && tp->gain > tp->gain_target) || (tp->gain_step < 0 && tp->gain < tp->gain_target))
                tp->gain = tp->gain_target;
        }
        float g = tp->gain * tp->env;
        lr[2 * i] = l * g;
        lr[2 * i + 1] = r * g;
        if (tp->state == TP_RUN) tp->frac += step;
    }

    bool playing = want && (tp->state == TP_PREFILL || tp->state == TP_RUN || tp->state == TP_FADEOUT);
    atomic_store_explicit(&tp->i_pos, tp->pos, memory_order_relaxed);
    atomic_store_explicit(&tp->i_rate, tp->in_rate, memory_order_relaxed);
    atomic_store_explicit(&tp->i_total, tp->total, memory_order_relaxed);
    atomic_store_explicit(&tp->i_flags, (playing ? F_PLAYING : 0) | (tp->state == TP_ENDED ? F_ENDED : 0),
                          memory_order_release);
}
