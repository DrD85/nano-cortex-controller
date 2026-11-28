// The Lo-Fi kit (drum_machine.h, DRUM_KIT_LOFI): computed like the Duo DAW's kit - no sample files - but voiced like
// drums sampled off an old record, for slow, atmospheric loops: a soft, muffled kick, a dusty snare, a rim click on
// the clap's pad (where the acoustic kit has its side snare), dark short hats, a dark ride and crash, round toms.
// Every sound then goes through what a tape and an old sampler would have done to it: soft, slightly uneven
// saturation, a short dark room (baked in: the looper's drums are dry otherwise), the sampler's 26 kHz and 12 bits
// and its low pass at 7 kHz; cut where it has died away (-60 dB). Float only (the P4 has no double unit).
// Made once by drum_machine_make_lofi; not in internal RAM (linker.lf): it never runs for a packet.
#include "drum_machine.h"

#include <math.h>
#include <stdlib.h>

#define TURN 6.2831853f
#define HOLD_HZ 26040.0f          // the sampler's rate (the images and the jitter it leaves: its grit)
#define LEVELS 2047.0f            // ... its 12 bits
#define TOP_HZ 7000.0f            // ... its output filter (four poles)
#define ROOM_S 0.5f               // the room's reverb time (to -60 dB), damped above 3.5 kHz
#define BIAS 0.2f                 // the tape's unevenness (even harmonics)

// Per pad: how long it may ring (dry and room; cut earlier where it has died away), the tape's drive, the low cut after
// it (the unevenness leaves a thump that follows the hit), the room's share (0.3: the room about 11 dB under the hit).
static const struct { float seconds, drive, low_hz, room; } PAD[DRUM_PADS] = {
    [DRUM_KICK] = { 0.45f, 1.8f, 25, 0.15f },     [DRUM_SNARE] = { 0.60f, 2.2f, 60, 0.45f },
    [DRUM_CLAP] = { 0.35f, 1.5f, 100, 0.40f },    [DRUM_HAT] = { 0.25f, 1.4f, 1000, 0.30f },
    [DRUM_OPENHAT] = { 0.55f, 1.3f, 1000, 0.25f }, [DRUM_LOWTOM] = { 0.60f, 1.8f, 30, 0.30f },
    [DRUM_HIGHTOM] = { 0.50f, 1.8f, 30, 0.30f },  [DRUM_RIDE] = { 1.20f, 1.2f, 500, 0.18f },
    [DRUM_CRASH] = { 1.50f, 1.2f, 500, 0.18f },
};

typedef struct { uint32_t s; } noise_t;   // xorshift32 in -1..1
static inline float noise(noise_t *n)
{
    n->s ^= n->s << 13;
    n->s ^= n->s >> 17;
    n->s ^= n->s << 5;
    return (float)(int32_t)n->s * (1.0f / 2147483648.0f);
}

typedef struct { float v, k; } env_t;     // exp(-t * rate), stepped by one factor per frame
static inline env_t env(float per_second, float fs) { return (env_t){ 1, expf(-per_second / fs) }; }
static inline float env_next(env_t *e) { float v = e->v; e->v *= e->k; return v; }

static inline float sine(float *phase, float hz, float fs)   // by its phase in turns (hz under fs)
{
    float v = sinf(TURN * *phase);
    if ((*phase += hz / fs) >= 1) *phase -= 1;
    return v;
}

enum { LP, HP, BP };
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bq_t;   // RBJ, transposed direct form II
static bq_t biquad(int kind, float hz, float q, float fs)
{
    float w = TURN * hz / fs, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al, b0, b1;
    if (kind == LP) b0 = (1 - c) / 2, b1 = 1 - c;
    else if (kind == HP) b0 = (1 + c) / 2, b1 = -(1 + c);
    else b0 = al, b1 = 0;
    return (bq_t){ b0 / a0, b1 / a0, (kind == BP ? -al : b0) / a0, -2 * c / a0, (1 - al) / a0, 0, 0 };
}
static inline float bq(bq_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static inline float soft(float x)   // tanh, near enough (Padé), flat beyond +-3
{
    return x >= 3 ? 1 : x <= -3 ? -1 : x * (27 + x * x) / (27 + 9 * x * x);
}
// (fmaxf / fminf call the library on the P4 for their NaN rules - twice per frame and pass)
static inline float most(float a, float b) { return a > b ? a : b; }
static inline float least(float a, float b) { return a < b ? a : b; }

// Hats and cymbals: the DAW kit's six square waves at unrelated frequencies and noise, band passed lower than there.
static void metal(float *lr, uint32_t n, float fs, noise_t *nl, noise_t *nr, float scale, float decay_s, float band, float hp, float nz)
{
    static const float FREQ[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
    float ph[6] = { 0 }, step[6];
    for (int k = 0; k < 6; k++) step[k] = FREQ[k] * scale / fs;
    bq_t bl = biquad(BP, band, 0.9f, fs), br = bl, hl = biquad(HP, hp, 0.7f, fs), hr = hl;
    env_t e = env(1 / decay_s, fs);
    float rise = 1 / (0.0008f * fs);
    for (uint32_t i = 0; i < n; i++) {
        float c = 0;
        for (int k = 0; k < 6; k++) {
            if ((ph[k] += step[k]) >= 1) ph[k] -= 1;
            c += ph[k] < 0.5f ? 1 : -1;
        }
        c *= 1.0f / 6;
        float g = least(1, i * rise) * env_next(&e);
        lr[2 * i] = bq(&hl, bq(&bl, c + nz * noise(nl))) * g;
        lr[2 * i + 1] = bq(&hr, bq(&br, c + nz * noise(nr))) * g;
    }
}

// The tape, the room and the sampler (see the top), in place; the frames left, 0 without memory for the room.
static uint32_t finish(int pad, float *lr, uint32_t n, float fs)
{
    // the room: what goes in without its lows (no boom), two all-passes smear the hit, four lines that feed each other
    // (Householder) and lose their highs ring after it
    static const uint16_t LEN[6] = { 131, 211, 557, 709, 887, 1063 };   // frames at 48 kHz: 3-22 ms, a small room
    uint32_t len[6], at[6] = { 0 }, total = 0;
    for (int k = 0; k < 6; k++) total += len[k] = (uint32_t)(LEN[k] * fs / 48000) + 1;
    float *buf[6], gain[4], damp[4] = { 0 }, d = 1 - expf(-TURN * 3500 / fs), low = 0, c = 1 - expf(-TURN * 250 / fs);
    if (!(buf[0] = calloc(total, sizeof(float)))) return 0;
    for (int k = 1; k < 6; k++) buf[k] = buf[k - 1] + len[k - 1];
    for (int k = 0; k < 4; k++) gain[k] = powf(10, -3.0f * len[k + 2] / (ROOM_S * fs));
    float peak = 0;
    for (uint32_t i = 0; i < 2 * n; i++) peak = most(peak, fabsf(lr[i]));
    float in = peak > 0 ? PAD[pad].drive / peak : 0, off = soft(BIAS), share = PAD[pad].room;
    bq_t cl = biquad(HP, PAD[pad].low_hz, 0.7071f, fs), cr = cl;
    peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        float yl = bq(&cl, soft(in * lr[2 * i] + BIAS) - off), yr = bq(&cr, soft(in * lr[2 * i + 1] + BIAS) - off);
        float x = 0.5f * (yl + yr), y[4], sum = 0;
        low += c * (x - low);
        x -= low;
        for (int k = 0; k < 2; k++) {
            float v = buf[k][at[k]], w = x + 0.6f * v;
            buf[k][at[k]] = w;
            x = v - 0.6f * w;
            if (++at[k] == len[k]) at[k] = 0;
        }
        for (int k = 0; k < 4; k++) sum += y[k] = damp[k] += d * (buf[k + 2][at[k + 2]] - damp[k]);
        for (int k = 0; k < 4; k++) {
            buf[k + 2][at[k + 2]] = x + gain[k] * (y[k] - 0.5f * sum);
            if (++at[k + 2] == len[k + 2]) at[k + 2] = 0;
        }
        lr[2 * i] = yl + share * (y[0] + y[2]);
        lr[2 * i + 1] = yr + share * (y[1] + y[3]);
        peak = most(peak, most(fabsf(lr[2 * i]), fabsf(lr[2 * i + 1])));
    }
    free(buf[0]);
    // the sampler: held at its rate, its steps, its filter
    bq_t f[4] = { biquad(LP, TOP_HZ, 0.5412f, fs), biquad(LP, TOP_HZ, 1.3066f, fs) };
    f[2] = f[0], f[3] = f[1];
    float scale = peak > 0 ? LEVELS / peak : 0, acc = 1, hl = 0, hr = 0, hold = HOLD_HZ / fs;
    peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((acc += hold) >= 1) {
            acc -= 1;
            hl = roundf(lr[2 * i] * scale) * (1 / LEVELS);
            hr = roundf(lr[2 * i + 1] * scale) * (1 / LEVELS);
        }
        lr[2 * i] = bq(&f[1], bq(&f[0], hl));
        lr[2 * i + 1] = bq(&f[3], bq(&f[2], hr));
        peak = most(peak, most(fabsf(lr[2 * i]), fabsf(lr[2 * i + 1])));
    }
    // cut where it has died away, the last quarter faded (a cymbal cut early ends softly), at the kit's 0.85 peak
    while (n > 1 && most(fabsf(lr[2 * n - 2]), fabsf(lr[2 * n - 1])) < 0.001f * peak) n--;
    uint32_t fade = n / 4, from = n - fade;
    scale = peak > 0 ? 0.85f / peak : 0;
    for (uint32_t i = 0; i < n; i++) {
        float g = i < from ? scale : scale * (float)(n - i) / fade * (float)(n - i) / fade;
        lr[2 * i] *= g;
        lr[2 * i + 1] *= g;
    }
    return n;
}

uint32_t drum_lofi_sound(int pad, uint32_t rate, float *lr, uint32_t max)
{
    if (pad < 0 || pad >= DRUM_PADS) return 0;
    float fs = (float)rate, pa = 0, pb = 0, rise = 1 / (0.0015f * fs);   // a soft attack: a sampled hit has no edge
    uint32_t n = (uint32_t)(PAD[pad].seconds * fs);
    if (!lr) return n;
    if (n > max) n = max;
    noise_t nl = { 0x2545F491u + 977u * (uint32_t)pad }, nr = { 0x6C8E9CF5u + 977u * (uint32_t)pad };
    switch (pad) {   // (dry in the middle, as on an old record - the room spreads them; hats and cymbals a little wider)
    case DRUM_KICK: {   // a sine falling from 125 to 50 Hz and a soft felt knock, muffled at 1.5 kHz
        env_t sweep = env(30, fs), body = env(12, fs), knock = env(120, fs);
        bq_t lp = biquad(LP, 1500, 0.7f, fs), kb = biquad(BP, 700, 0.7f, fs);
        for (uint32_t i = 0; i < n; i++) {
            float v = sine(&pa, 50 + 75 * env_next(&sweep), fs) * env_next(&body) + 0.3f * bq(&kb, noise(&nl)) * env_next(&knock);
            lr[2 * i] = lr[2 * i + 1] = bq(&lp, v) * least(1, i * rise);
        }
        break;
    }
    case DRUM_SNARE: {   // a short body at 185 / 285 Hz and papery noise in the middle (band passed, dull above 5 kHz)
        env_t tone = env(32, fs), wires = env(15, fs), snap = env(70, fs);
        bq_t bp = biquad(BP, 2000, 0.6f, fs), lp = biquad(LP, 5000, 0.7f, fs);
        for (uint32_t i = 0; i < n; i++) {
            float t = (0.55f * sine(&pa, 185, fs) + 0.3f * sine(&pb, 285, fs)) * env_next(&tone);
            float w = 2.0f * env_next(&wires) * (0.7f + 0.6f * env_next(&snap));
            lr[2 * i] = lr[2 * i + 1] = least(1, i * rise) * (t + w * bq(&lp, bq(&bp, noise(&nl))));
        }
        break;
    }
    case DRUM_CLAP: {   // a rim click (side stick): a hollow knock at 480 Hz and 1.6 kHz, a crack of noise at 3 kHz
        env_t lo = env(40, fs), hi = env(90, fs), crack = env(300, fs);
        bq_t bp = biquad(BP, 3000, 1.2f, fs);
        for (uint32_t i = 0; i < n; i++)
            lr[2 * i] = lr[2 * i + 1] = 0.6f * sine(&pa, 480, fs) * env_next(&lo) + 0.45f * sine(&pb, 1600, fs) * env_next(&hi) +
                                        1.5f * env_next(&crack) * bq(&bp, noise(&nl));
        break;
    }
    case DRUM_HAT: metal(lr, n, fs, &nl, &nr, 0.9f, 0.03f, 5500, 2800, 0.5f); break;
    case DRUM_OPENHAT: metal(lr, n, fs, &nl, &nr, 0.9f, 0.16f, 5500, 2800, 0.5f); break;
    case DRUM_RIDE: metal(lr, n, fs, &nl, &nr, 1.5f, 0.35f, 4200, 2000, 0.15f); break;
    case DRUM_CRASH: metal(lr, n, fs, &nl, &nr, 1.25f, 0.45f, 4300, 2000, 0.6f); break;
    default: {   // toms: a round falling sine with a little stick, muffled
        env_t sweep = env(18, fs), body = env(8, fs), stick = env(90, fs);
        bq_t lp = biquad(LP, 2200, 0.7f, fs);
        float hz = pad == DRUM_HIGHTOM ? 150 : 98;
        for (uint32_t i = 0; i < n; i++) {
            float b = sine(&pa, hz * (1 + 0.35f * env_next(&sweep)), fs) * env_next(&body);
            lr[2 * i] = lr[2 * i + 1] = least(1, i * rise) * bq(&lp, b + 0.12f * env_next(&stick) * noise(&nl));
        }
        break;
    }
    }
    return finish(pad, lr, n, fs);
}
