// The live spectrum for the EQ editor (spectrum.h). Runs in a low-priority task on core 1, never on the audio path.
#include "spectrum.h"

#include <string.h>

#define N SPECTRUM_N
#define N2 (SPECTRUM_N / 2)   // the real FFT of N samples is a complex FFT of N / 2 values and a last step

bool spectrum_init(spectrum_t *s, void *(*alloc)(size_t))
{
    memset(s, 0, sizeof(*s));
    s->hist = alloc(sizeof(float) * N);
    s->work = alloc(sizeof(float) * N);
    s->win = alloc(sizeof(float) * N);
    s->tw = alloc(sizeof(float) * N);
    s->bands = alloc(sizeof(spectrum_band_t) * SPECTRUM_BANDS);
    if (!s->hist || !s->work || !s->win || !s->tw || !s->bands) return false;
    memset(s->hist, 0, sizeof(float) * N);
    const double pi = 3.14159265358979323846;
    for (int n = 0; n < N; n++) s->win[n] = (float)(0.5 - 0.5 * cos(2 * pi * n / N));   // (periodic Hann)
    for (int k = 0; k < N2; k++) {
        s->tw[2 * k] = (float)cos(2 * pi * k / N);
        s->tw[2 * k + 1] = (float)-sin(2 * pi * k / N);
    }
    const double df = SPECTRUM_RATE / N, half = pow(2.0, 0.5 / SPECTRUM_PER_OCT);
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        double fc = SPECTRUM_F0 * pow(2.0, (double)i / SPECTRUM_PER_OCT), lo = fc / half, hi = fc * half;
        int k0 = (int)ceil(lo / df), k1 = (int)ceil(hi / df);   // the bins whose centre lies in [lo, hi)
        if (k0 < 1) k0 = 1;
        if (k1 > N2 - 1) k1 = N2 - 1;
        spectrum_band_t *b = &s->bands[i];
        b->pos = (float)(fc / df);
        b->width = (float)((hi - lo) / df);
        if (k1 > k0) b->k0 = (uint16_t)k0, b->k1 = (uint16_t)k1;
        else b->k0 = b->k1 = 0;
    }
    return true;
}

void spectrum_reset(spectrum_t *s) { s->fill = 0; }

void spectrum_push(spectrum_t *s, const float *x, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        s->hist[s->pos] = x[i];
        s->pos = (s->pos + 1) & (N - 1);
    }
    s->fill = s->fill + n > N ? N : s->fill + n;
}

void spectrum_push_lr(spectrum_t *s, const float *ring, uint32_t mask, uint32_t from, uint32_t to)
{
    uint32_t n = to - from;
    for (uint32_t f = from; f != to; f++) {
        uint32_t k = f & mask;
        s->hist[s->pos] = 0.5f * (ring[2 * k] + ring[2 * k + 1]);
        s->pos = (s->pos + 1) & (N - 1);
    }
    s->fill = s->fill + n > N ? N : s->fill + n;
}

// In-place radix-2 FFT of N2 complex values (re, im interleaved).
static void fft(float *z, const float *tw)
{
    for (uint32_t i = 1, j = 0; i < N2; i++) {
        uint32_t bit = N2 >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = z[2 * i]; z[2 * i] = z[2 * j]; z[2 * j] = t;
            t = z[2 * i + 1]; z[2 * i + 1] = z[2 * j + 1]; z[2 * j + 1] = t;
        }
    }
    for (uint32_t len = 2; len <= N2; len <<= 1) {
        uint32_t half = len >> 1, step = N / len;   // W_len^j = W_N^(j * N / len), always below N / 2
        for (uint32_t j = 0; j < half; j++) {
            float wr = tw[2 * j * step], wi = tw[2 * j * step + 1];
            for (uint32_t i = j; i < N2; i += len) {
                float *a = z + 2 * i, *b = z + 2 * (i + half);
                float ar = a[0], ai = a[1], br = b[0] * wr - b[1] * wi, bi = b[0] * wi + b[1] * wr;   // (all read first:
                b[0] = ar - br;                                                                        // no reloads)
                b[1] = ai - bi;
                a[0] = ar + br;
                a[1] = ai + bi;
            }
        }
    }
}

void spectrum_compute(spectrum_t *s, float out_db[SPECTRUM_BANDS])
{
    float *w = s->work;
    for (uint32_t n = 0; n < N; n++) w[n] = s->hist[(s->pos + n) & (N - 1)] * s->win[n];   // oldest first
    fft(w, s->tw);   // the even samples as real, the odd as imaginary parts
    // The real spectrum from the complex one, two bins at a time (k and N2 - k need the same two values); each
    // bin's power is written over its own value: P(k) in w[2k].
    w[0] = 0;   // (DC: not shown)
    for (uint32_t k = 1; k <= N2 / 2; k++) {
        uint32_t m = N2 - k;
        float ar = w[2 * k], ai = w[2 * k + 1], br = w[2 * m], bi = -w[2 * m + 1];
        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);          // even part
        float or_ = 0.5f * (ai - bi), oi = -0.5f * (ar - br);        // odd part (times -i)
        float wr = s->tw[2 * k], wi = s->tw[2 * k + 1];
        float tr = or_ * wr - oi * wi, ti = or_ * wi + oi * wr;      // W^k times the odd part
        float pk = (er + tr) * (er + tr) + (ei + ti) * (ei + ti);    // X(k)
        float pm = (er - tr) * (er - tr) + (ti - ei) * (ti - ei);    // X(N2 - k) = conj(even - W^k odd)
        w[2 * k] = pk;
        w[2 * m] = pm;
    }
    // A full-scale sine's power summed over its bins: N^2 * 3/32 (Hann: mean square 3/8) - that is 0 dB.
    const float norm = 32.0f / (3.0f * (float)N * (float)N);
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        const spectrum_band_t *b = &s->bands[i];
        float p = 0;
        if (b->k1 > b->k0) {
            for (uint32_t k = b->k0; k < b->k1; k++) p += w[2 * k];
        } else {   // narrower than a bin: the power between the two nearest bins, for the band's width
            uint32_t k = (uint32_t)b->pos;
            float f = b->pos - (float)k;
            if (k < 1) k = 1, f = 0;
            p = (w[2 * k] * (1 - f) + w[2 * (k + 1)] * f) * b->width;
        }
        float db = 10.0f * log10f(p * norm + 1e-13f);
        out_db[i] = db < SPECTRUM_FLOOR_DB ? SPECTRUM_FLOOR_DB : db;
    }
}

void spectrum_smooth(float *shown, const float *now, int n, float dt_s, float release_db_s)
{
    float down = release_db_s * dt_s;
    for (int i = 0; i < n; i++) {
        if (now[i] > shown[i]) shown[i] += 0.65f * (now[i] - shown[i]);
        else {
            float d = shown[i] - down;
            shown[i] = d > now[i] ? d : now[i];
        }
    }
}
