// Ring buffer + adaptive resampler for the phone audio (see audio_pipe.h).
#include "audio_pipe.h"

#include <math.h>
#include <string.h>

enum { PIPE_PREFILL, PIPE_RUN, PIPE_FADEOUT };

#define TARGET_MS 25            // fill level the controller keeps: absorbs I2S blocks and USB jitter
#define LOW_FRAMES 4            // below this the buffer counts as dry
#define FADE_IN_MS 6
#define FADE_OUT_FRAMES 96      // 2 ms at 48 kHz
#define GAIN_RAMP_MS 20
#define FILL_TAU_S 0.5          // smoothing of the fill level
#define KP 0.0075               // ratio correction per relative fill error (loop about 3 s; the fill moves 40x faster than the ratio)
#define KI 0.0006               // ... per second and relative fill error (takes the offset away in ~15 s)
#define CORR_MAX 0.002          // +-2000 ppm

static uint32_t fill_now(audio_pipe_t *p)
{
    uint32_t w = atomic_load_explicit(&p->w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&p->r, memory_order_relaxed);
    return w - r;
}

static void reset_reader(audio_pipe_t *p)
{
    memset(p->win, 0, sizeof(p->win));
    p->frac = 0;
    p->fade = 0;
    p->state = PIPE_PREFILL;
}

void audio_pipe_init(audio_pipe_t *p, int16_t *buffer, uint32_t frames, uint32_t in_rate, uint32_t out_rate)
{
    memset(p, 0, sizeof(*p));
    p->buf = buffer;
    p->cap = frames;
    atomic_init(&p->w, 0);
    atomic_init(&p->r, 0);
    atomic_init(&p->rate_request, 0);
    p->out_rate = out_rate;
    p->gain = p->gain_target = 1.0f;
    audio_pipe_set_input_rate(p, in_rate);
}

void audio_pipe_set_input_rate(audio_pipe_t *p, uint32_t in_rate)
{
    p->in_rate = in_rate;
    p->step = (double)in_rate / p->out_rate;
    p->target = in_rate * TARGET_MS / 1000;
    if (p->target > p->cap / 2) p->target = p->cap / 2;
    p->fill_avg = p->target;
    p->integ = p->corr = 0;
    // empty the buffer from the reader's side
    atomic_store_explicit(&p->r, atomic_load_explicit(&p->w, memory_order_acquire), memory_order_release);
    reset_reader(p);
}

uint32_t audio_pipe_write(audio_pipe_t *p, const int16_t *stereo, uint32_t frames)
{
    uint32_t w = atomic_load_explicit(&p->w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&p->r, memory_order_acquire);
    uint32_t space = p->cap - 1 - (w - r);
    uint32_t n = frames < space ? frames : space;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (w + i) & (p->cap - 1);
        p->buf[2 * k] = stereo[2 * i];
        p->buf[2 * k + 1] = stereo[2 * i + 1];
    }
    atomic_store_explicit(&p->w, w + n, memory_order_release);
    p->st.written += n;
    p->st.dropped += frames - n;
    return n;
}

// Moves the window one input frame on. false = the buffer is dry (the window keeps its last frame).
static bool pull(audio_pipe_t *p)
{
    uint32_t r = atomic_load_explicit(&p->r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&p->w, memory_order_acquire);
    memmove(p->win[0], p->win[1], sizeof(p->win[0]) * 3);
    if (r == w) return false;
    uint32_t k = r & (p->cap - 1);
    p->win[3][0] = p->buf[2 * k] * (1.0f / 32768.0f);
    p->win[3][1] = p->buf[2 * k + 1] * (1.0f / 32768.0f);
    atomic_store_explicit(&p->r, r + 1, memory_order_release);
    return true;
}

static inline float cubic(float x0, float x1, float x2, float x3, float t)
{
    float c1 = 0.5f * (x2 - x0);
    float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

static void control(audio_pipe_t *p, uint32_t frames, uint32_t fill)
{
    double dt = (double)frames / p->out_rate;
    double a = dt / FILL_TAU_S;
    if (a > 1) a = 1;
    p->fill_avg += (fill - p->fill_avg) * a;
    double err = (p->fill_avg - p->target) / p->target;
    p->integ += KI * err * dt;
    if (p->integ > CORR_MAX) p->integ = CORR_MAX;
    if (p->integ < -CORR_MAX) p->integ = -CORR_MAX;
    double c = KP * err + p->integ;
    if (c > CORR_MAX) c = CORR_MAX;
    if (c < -CORR_MAX) c = -CORR_MAX;
    p->corr = c;
}

void audio_pipe_request_rate(audio_pipe_t *p, uint32_t in_rate)
{
    atomic_store_explicit(&p->rate_request, in_rate, memory_order_release);
}

void audio_pipe_render_stereo(audio_pipe_t *p, float *lr, uint32_t frames)
{
    uint32_t want = atomic_exchange_explicit(&p->rate_request, 0, memory_order_acq_rel);
    if (want && want != p->in_rate) audio_pipe_set_input_rate(p, want);
    memset(lr, 0, sizeof(float) * 2 * frames);
    uint32_t fill = fill_now(p);

    if (p->state == PIPE_PREFILL) {
        if (fill < p->target) return;   // silence until there is enough
        p->state = PIPE_RUN;
        p->fade = 0;
        p->fade_step = 1000.0f / (FADE_IN_MS * p->out_rate);
        p->fill_avg = fill;
        // fill the window so the first output frame starts on real samples
        for (int i = 0; i < 3; i++) pull(p);
        p->frac = 0;
    } else if (p->state == PIPE_RUN) {
        control(p, frames, fill);
    }

    double step = p->step * (1.0 + p->corr);
    for (uint32_t i = 0; i < frames; i++) {
        if (p->state == PIPE_PREFILL) break;
        while (p->frac >= 1.0) {
            p->frac -= 1.0;
            if (!pull(p) && p->state == PIPE_RUN) {
                // dry: hold the last frame and fade it out
                p->st.underruns++;
                p->state = PIPE_FADEOUT;
                p->fade_step = -1.0f / FADE_OUT_FRAMES;
            }
        }
        float t = (float)p->frac;
        float l = cubic(p->win[0][0], p->win[1][0], p->win[2][0], p->win[3][0], t);
        float r = cubic(p->win[0][1], p->win[1][1], p->win[2][1], p->win[3][1], t);
        if (p->state == PIPE_FADEOUT) {   // the held value
            l = p->win[2][0];
            r = p->win[2][1];
        }

        if (p->gain != p->gain_target) {
            p->gain += p->gain_step;
            if ((p->gain_step > 0 && p->gain > p->gain_target) || (p->gain_step < 0 && p->gain < p->gain_target))
                p->gain = p->gain_target;
        }
        p->fade += p->fade_step;
        if (p->fade >= 1.0f) p->fade = 1.0f, p->fade_step = 0;
        float g = p->gain * p->fade;
        lr[2 * i] = l * g;
        lr[2 * i + 1] = r * g;

        if (p->fade <= 0.0f && p->state == PIPE_FADEOUT) {
            reset_reader(p);   // wait for the buffer to fill again
            break;
        }
        if (p->state == PIPE_RUN) p->frac += step;
    }
}

void audio_pipe_render(audio_pipe_t *p, int32_t *out, uint32_t frames, int channels, int ch_left, int ch_right)
{
    float lr[2 * 64];
    memset(out, 0, sizeof(int32_t) * frames * channels);
    for (uint32_t done = 0; done < frames;) {
        uint32_t n = frames - done > 64 ? 64 : frames - done;
        audio_pipe_render_stereo(p, lr, n);
        for (uint32_t i = 0; i < n; i++) {
            out[(done + i) * channels + ch_left] = audio_to_slot(lr[2 * i]);
            out[(done + i) * channels + ch_right] = audio_to_slot(lr[2 * i + 1]);
        }
        done += n;
    }
}

void audio_pipe_set_gain_db(audio_pipe_t *p, float db)
{
    p->gain_target = db <= -90.0f ? 0.0f : powf(10.0f, db / 20.0f);
    p->gain_step = (p->gain_target - p->gain) / (GAIN_RAMP_MS * p->out_rate / 1000.0f);
}

void audio_pipe_stats(audio_pipe_t *p, audio_pipe_stats_t *out)
{
    *out = p->st;
    out->fill = fill_now(p);
    out->ppm = (int32_t)lrint(p->corr * 1e6);
    out->playing = p->state == PIPE_RUN;
}
