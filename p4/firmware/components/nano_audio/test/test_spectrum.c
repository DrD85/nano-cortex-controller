// The EQ editor's spectrum (spectrum.c) on the computer: tones read their level in their band, pink noise reads flat,
// white noise rises 3 dB per octave, silence reads the floor, the stereo ring wraps, the smoothing; and the time one
// spectrum takes here (the board's own number comes from its log: p4_spectrum.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spectrum.h"

static int s_fails, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_fails++; printf("FAILED: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *alloc(size_t n) { return malloc(n); }

static int band_of(float hz)
{
    return (int)lroundf(SPECTRUM_PER_OCT * log2f(hz / SPECTRUM_F0));
}

static void tone(spectrum_t *s, float hz, float amp)
{
    static float x[SPECTRUM_N];
    for (int n = 0; n < SPECTRUM_N; n++) x[n] = amp * sinf(2 * (float)M_PI * hz * n / SPECTRUM_RATE + 0.3f);
    spectrum_reset(s);
    spectrum_push(s, x, SPECTRUM_N);
}

static uint32_t s_rand = 12345;
static float white(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return ((s_rand >> 8) / 8388608.0f) - 1.0f;
}

// Pink noise (Paul Kellet's filter of white noise): -3 dB per octave, flat in fractional-octave bands.
static float pink(float *b)
{
    float w = white();
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return out * 0.11f;
}

// Mean level per band over many windows of noise
static void noise_levels(spectrum_t *s, int kind, float *mean_db)
{
    static float x[SPECTRUM_N], out[SPECTRUM_BANDS];
    double acc[SPECTRUM_BANDS] = { 0 };
    float b[7] = { 0 };
    const int runs = 64;
    spectrum_reset(s);
    for (int r = 0; r < runs; r++) {
        for (int n = 0; n < SPECTRUM_N; n++) x[n] = kind ? pink(b) : 0.25f * white();
        spectrum_push(s, x, SPECTRUM_N);
        spectrum_compute(s, out);
        for (int i = 0; i < SPECTRUM_BANDS; i++) acc[i] += pow(10.0, out[i] / 10.0);
    }
    for (int i = 0; i < SPECTRUM_BANDS; i++) mean_db[i] = (float)(10 * log10(acc[i] / runs));
}

// The mean of bands [a, b] in dB
static float span(const float *db, int a, int b)
{
    double p = 0;
    for (int i = a; i <= b; i++) p += pow(10.0, db[i] / 10.0);
    return (float)(10 * log10(p / (b - a + 1)));
}

int main(void)
{
    spectrum_t s;
    if (!spectrum_init(&s, alloc)) { printf("no memory\n"); return 1; }
    float out[SPECTRUM_BANDS];

    printf("== spectrum (EQ editor): %d-point FFT, %d bands of 1/%d octave from %.0f Hz (%.1f Hz .. %.0f Hz)\n",
           SPECTRUM_N, SPECTRUM_BANDS, SPECTRUM_PER_OCT, SPECTRUM_F0, spectrum_band_hz(0), spectrum_band_hz(SPECTRUM_BANDS - 1));
    CHECK(!spectrum_ready(&s), "ready before a whole window");

    // A full-scale tone reads 0 dB in its band (where the bands are wider than the FFT's bins: from about 200 Hz),
    // -20 dBFS reads -20; far from it the window keeps the level down
    const float tones[] = { 250, 1000, 1003.7f, 2960, 6400, 15000 };
    for (unsigned t = 0; t < sizeof(tones) / sizeof(tones[0]); t++) {
        for (int k = 0; k < 2; k++) {
            float amp = k ? 0.1f : 1.0f, want = k ? -20.0f : 0.0f;
            tone(&s, tones[t], amp);
            CHECK(spectrum_ready(&s), "not ready after a window");
            spectrum_compute(&s, out);
            int b = band_of(tones[t]);
            // (a tone between two bands shares its power: the two together read its level)
            float got = 10 * log10f(powf(10, out[b] / 10) + powf(10, out[b - 1] / 10) + powf(10, out[b + 1] / 10));
            CHECK(fabsf(got - want) < 0.7f, "tone %.1f Hz at %.0f dBFS read %.2f dB (band %d: %.2f)", tones[t], want, got, b, out[b]);
            float below = out[b - 24], above = b + 24 < SPECTRUM_BANDS ? out[b + 24] : SPECTRUM_FLOOR_DB;
            CHECK(below < want - 60 && above < want - 60, "tone %.1f Hz: two octaves away %.1f / %.1f dB", tones[t], below, above);
        }
    }
    // Below the bins' width: lower, but in the right place
    tone(&s, 82.4f, 1.0f);   // the low E
    spectrum_compute(&s, out);
    {
        int best = 0;
        for (int i = 1; i < SPECTRUM_BANDS; i++) if (out[i] > out[best]) best = i;
        CHECK(abs(best - band_of(82.4f)) <= 1, "low E: loudest band %d (%.1f Hz), not %d", best, spectrum_band_hz(best), band_of(82.4f));
        printf("  low E (82.4 Hz, full scale): %.1f dB in its band - narrower than a bin there\n", out[band_of(82.4f)]);
    }

    // Silence
    float zero[SPECTRUM_N] = { 0 };
    spectrum_reset(&s);
    spectrum_push(&s, zero, SPECTRUM_N);
    spectrum_compute(&s, out);
    int quiet = 1;
    for (int i = 0; i < SPECTRUM_BANDS; i++) if (out[i] > SPECTRUM_FLOOR_DB + 1) quiet = 0;
    CHECK(quiet, "silence did not read the floor");

    // Pink noise is flat in fractional octaves, white noise rises 3 dB per octave. Below ~200 Hz the bands are
    // narrower than a bin and the window's skirt brings in the louder lows: pink reads up to ~2 dB high there.
    float pk[SPECTRUM_BANDS], wh[SPECTRUM_BANDS];
    noise_levels(&s, 1, pk);
    noise_levels(&s, 0, wh);
    float p_lo = span(pk, band_of(50), band_of(150)), p_low = span(pk, band_of(200), band_of(400)),
          p_mid = span(pk, band_of(500), band_of(1500)), p_hi = span(pk, band_of(4000), band_of(12000));
    printf("  pink noise: %.1f / %.1f / %.1f / %.1f dB around 90 Hz / 280 Hz / 870 Hz / 7 kHz\n", p_lo, p_low, p_mid, p_hi);
    CHECK(fabsf(p_low - p_mid) < 1.0f && fabsf(p_hi - p_mid) < 1.0f, "pink noise not flat: %.1f / %.1f / %.1f", p_low, p_mid, p_hi);
    CHECK(fabsf(p_lo - p_mid) < 3.0f, "pink noise in the lows: %.1f against %.1f", p_lo, p_mid);
    float w1 = span(wh, band_of(400), band_of(600)), w2 = span(wh, band_of(3200), band_of(4800));
    CHECK(fabsf((w2 - w1) - 9.03f) < 1.0f, "white noise: %.1f dB over three octaves, not 9", w2 - w1);

    // The stereo ring: wrapping, mono = (L + R) / 2
    {
        static float ring[2048 * 2];
        for (int f = 0; f < 2048; f++) ring[2 * f] = ring[2 * f + 1] = 0;
        spectrum_t m;
        spectrum_init(&m, alloc);
        uint32_t start = 0xFFFFF700u;   // frame counters wrap as on the board (here after 2304 frames)
        for (uint32_t f = start; f != start + 5000; f++) {
            float v = sinf(2 * (float)M_PI * 1000 * (f - start) / SPECTRUM_RATE + 0.3f);
            ring[2 * (f & 2047)] = 1.5f * v;
            ring[2 * (f & 2047) + 1] = 0.5f * v;
            if (((f - start) % 700) == 699) spectrum_push_lr(&m, ring, 2047, f - 699, f + 1);   // (pieces of 700 frames)
        }
        CHECK(m.fill == SPECTRUM_N, "ring: %u samples counted", (unsigned)m.fill);
        spectrum_compute(&m, out);
        int b = band_of(1000);
        float got = 10 * log10f(powf(10, out[b] / 10) + powf(10, out[b - 1] / 10) + powf(10, out[b + 1] / 10));
        CHECK(fabsf(got) < 0.7f, "ring: tone read %.2f dB", got);
    }

    // Smoothing: up most of the way at once, down at the release rate
    {
        float shown[2] = { -100, 0 }, now[2] = { 0, -100 };
        spectrum_smooth(shown, now, 2, 0.1f, 30);
        CHECK(fabsf(shown[0] - -35) < 0.01f, "up: %.2f", shown[0]);
        CHECK(fabsf(shown[1] - -3) < 0.01f, "down: %.2f", shown[1]);
    }

    // Time per spectrum here (the board: about 30-40 times as long - see the note in main/p4_spectrum.c)
    {
        static float x[SPECTRUM_N];
        for (int n = 0; n < SPECTRUM_N; n++) x[n] = white();
        spectrum_push(&s, x, SPECTRUM_N);
        const int reps = 2000;
        clock_t t0 = clock();
        for (int r = 0; r < reps; r++) spectrum_compute(&s, out);
        double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / reps;
        printf("  one spectrum: %.1f us on this computer\n", us);
    }

    printf("spectrum: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
