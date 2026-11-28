// Drum machine (see drum_machine.h). The sounds are the Duo DAW's StartKit, computed the same way (same formulas,
// same noise seeds), the levels the DAW's KitBalance (the Lo-Fi kit's sounds: drum_lofi.c, levelled the same way).
#include "drum_machine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHOKE_FRAMES 96   // a closed hat stops an open one over 2 ms

static const char *const ROLE[DRUM_PADS] = { "kick", "snare", "clap", "hat", "openhat", "lowtom", "hightom", "ride", "crash" };

// ---- the kit ----

typedef struct { uint32_t state; } noise_t;

static float noise_next(noise_t *n)   // xorshift32 in -1..1, as the DAW's Noise
{
    n->state ^= n->state << 13;
    n->state ^= n->state >> 17;
    n->state ^= n->state << 5;
    return (float)(int32_t)n->state * (1.0f / 2147483648.0f);
}

static uint32_t stable_hash(const char *s)   // FNV-1a, as the DAW's hashValueStable
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } bq_t;

static bq_t band_pass(double hz, double q, double fs)
{
    double w = 2 * M_PI * hz / fs, alpha = sin(w) / (2 * q), a0 = 1 + alpha;   // (once per sound: double is fine)
    return (bq_t){ (float)(alpha / a0), 0, (float)(-alpha / a0), (float)(-2 * cos(w) / a0), (float)((1 - alpha) / a0), 0, 0, 0, 0 };
}

static bq_t high_pass(double hz, double q, double fs)
{
    double w = 2 * M_PI * hz / fs, alpha = sin(w) / (2 * q), a0 = 1 + alpha, c = cos(w);
    return (bq_t){ (float)((1 + c) / 2 / a0), (float)(-(1 + c) / a0), (float)((1 + c) / 2 / a0), (float)(-2 * c / a0), (float)((1 - alpha) / a0), 0, 0, 0, 0 };
}

static float bq_run(bq_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1, f->x1 = x, f->y2 = f->y1, f->y1 = y;
    return y;
}

static uint32_t sound_length(int pad, uint32_t rate)
{
    static const double SECONDS[DRUM_PADS] = { 0.6, 0.35, 0.3, 0.35, 1.8, 0.7, 0.7, 4.0, 3.5 };
    return (uint32_t)(SECONDS[pad] * rate);
}

// In float (the P4 has a float unit, none for double: in double the kit took more than 45 s there): the decays are
// stepped by one factor per frame instead of exp(t), phases wrap at a full turn - the same sound as the DAW's to
// within the float's rounding (test/test_drums.c compares them).
typedef struct { float v, k; } decay_t;   // v = exp(-t * rate), stepped
static inline decay_t decay_of(double per_second, double fs) { return (decay_t){ 1.0f, (float)exp(-per_second / fs) }; }
static inline float decay_next(decay_t *d) { float v = d->v; d->v *= d->k; return v; }

typedef struct { float phase; } osc_t;    // a sine by its phase in turns (0-1)
static inline float osc_sin(osc_t *o, float hz, float fs)
{
    float v = sinf(6.2831853f * o->phase);
    o->phase += hz / fs;
    o->phase -= floorf(o->phase);
    return v;
}

// Hi-hats and cymbals: six square waves at unrelated frequencies, band passed and high passed, with noise.
static void metal(int pad, double fs, noise_t *nl, noise_t *nr, float *lr, uint32_t n)
{
    static const double FREQ[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
    double scale, decay, attack, noise, band, hp;
    switch (pad) {
    case DRUM_HAT: scale = 1.0, decay = 0.045, attack = 0.0005, noise = 0.35, band = 10000, hp = 7000; break;
    case DRUM_OPENHAT: scale = 1.0, decay = 0.45, attack = 0.0005, noise = 0.35, band = 10000, hp = 7000; break;
    case DRUM_RIDE: scale = 1.7, decay = 1.4, attack = 0.001, noise = 0.15, band = 6500, hp = 3500; break;
    default: scale = 1.35, decay = 1.1, attack = 0.004, noise = 0.6, band = 7500, hp = 4000; break;   // crash
    }
    bq_t bl = band_pass(band, 0.9, fs), br = bl, hl = high_pass(hp, 0.7, fs), hr = hl, h2l = hl, h2r = hl;
    // The square waves' phases stay in double, added exactly as the DAW adds them: 800 Hz has an edge exactly on
    // every 30th frame, and which side of it a frame falls on depends on the double's rounding (in float or fixed
    // point the hats came out different). About a second for all four on the P4, in the background.
    double ph[6] = { 0 }, step[6];
    float peak = 0, att = (float)(1.0 / (attack * fs)), nz = (float)noise;
    for (int k = 0; k < 6; k++) step[k] = FREQ[k] * scale / fs;
    decay_t env = decay_of(1 / decay, fs), quick = decay_of(1 / 0.03, fs);
    for (uint32_t i = 0; i < n; i++) {
        float cluster = 0;
        for (int k = 0; k < 6; k++) {
            ph[k] += step[k];
            ph[k] -= floor(ph[k]);
            cluster += ph[k] < 0.5 ? 1 : -1;
        }
        cluster *= 1.0f / 6;
        float e = fminf(1, i * att) * decay_next(&env), q = decay_next(&quick);
        if (pad != DRUM_HAT) e *= 0.6f + 0.4f * q;
        float l = bq_run(&h2l, bq_run(&hl, bq_run(&bl, cluster + nz * noise_next(nl))));
        float r = bq_run(&h2r, bq_run(&hr, bq_run(&br, cluster + nz * noise_next(nr))));
        lr[2 * i] = l * e;
        lr[2 * i + 1] = r * e;
        peak = fmaxf(peak, fmaxf(fabsf(lr[2 * i]), fabsf(lr[2 * i + 1])));
    }
    float sc = peak > 0 ? 0.8f / peak : 0;
    for (uint32_t i = 0; i < 2 * n; i++) lr[i] *= sc;
}

uint32_t drum_kit_sound(int pad, uint32_t rate, float *lr, uint32_t max)
{
    if (pad < 0 || pad >= DRUM_PADS) return 0;
    double fs = rate;
    float ffs = (float)rate, dt = 1.0f / ffs;
    uint32_t n = sound_length(pad, rate);
    if (n > max) n = max;
    uint32_t h = stable_hash(ROLE[pad]);
    noise_t nl = { 0x12345678u + h }, nr = { 0x9ABCDEF0u + h };
    if (!nl.state) nl.state = 0x9E3779B9u;
    if (!nr.state) nr.state = 0x9E3779B9u;
    switch (pad) {
    case DRUM_KICK: {   // a sine falling from 150 to 48 Hz with a click at the start
        osc_t o = { 0 };
        decay_t sweep = decay_of(30, fs), body = decay_of(6, fs), click = decay_of(400, fs);
        for (uint32_t i = 0; i < n; i++) {
            float f = 48 + 102 * decay_next(&sweep);
            o.phase += f / ffs;   // (the DAW adds the phase before taking the sine)
            o.phase -= floorf(o.phase);
            float v = 0.9f * tanhf(1.6f * (sinf(6.2831853f * o.phase) * decay_next(&body) + decay_next(&click) * 0.5f));
            lr[2 * i] = lr[2 * i + 1] = v;
        }
        break;
    }
    case DRUM_SNARE: {   // two tones and band-limited noise, the noise slightly wider
        float lpl = 0, lpr = 0;
        osc_t a = { 0 }, b = { 0 };
        decay_t env = decay_of(14, fs), tone = decay_of(25, fs);
        for (uint32_t i = 0; i < n; i++) {
            float e = decay_next(&env);
            float t = (0.5f * osc_sin(&a, 185, ffs) + 0.3f * osc_sin(&b, 330, ffs)) * decay_next(&tone);
            lpl += 0.55f * (noise_next(&nl) - lpl);
            lpr += 0.55f * (noise_next(&nr) - lpr);
            lr[2 * i] = 0.7f * (t + 0.8f * lpl * e);
            lr[2 * i + 1] = 0.7f * (t + 0.8f * lpr * e);
        }
        break;
    }
    case DRUM_CLAP: {   // three quick bursts, then a short tail
        float bp = 0, last = 0;
        static const float AT[3] = { 0.0f, 0.011f, 0.023f };
        decay_t burst[3] = { decay_of(180, fs), decay_of(180, fs), decay_of(180, fs) }, tail = decay_of(18, fs);
        for (uint32_t i = 0; i < n; i++) {
            float t = i * dt, env = 0;
            for (int k = 0; k < 3; k++) if (t >= AT[k]) env += decay_next(&burst[k]);
            if (t > 0.023f) env += 0.6f * decay_next(&tail);
            float x = noise_next(&nl);
            bp = 0.6f * bp + (x - last);
            last = x;
            float r = noise_next(&nr);
            lr[2 * i] = 0.8f * tanhf(0.45f * bp * env);
            lr[2 * i + 1] = 0.8f * tanhf(0.45f * (0.7f * bp + 0.3f * r) * env);
        }
        break;
    }
    case DRUM_HAT: case DRUM_OPENHAT: case DRUM_RIDE: case DRUM_CRASH:
        metal(pad, fs, &nl, &nr, lr, n);
        break;
    default: {   // toms: a falling sine with a little noise
        bool high = pad == DRUM_HIGHTOM;
        osc_t o = { 0 };
        decay_t sweep = decay_of(25, fs), body = decay_of(7, fs), stick = decay_of(60, fs);
        for (uint32_t i = 0; i < n; i++) {
            float f = (high ? 165.0f : 105.0f) * (1 + 0.5f * decay_next(&sweep));
            o.phase += f / ffs;
            o.phase -= floorf(o.phase);
            float b = sinf(6.2831853f * o.phase) * decay_next(&body), st = decay_next(&stick) * 0.15f;
            lr[2 * i] = 0.8f * tanhf(1.3f * (b + st * noise_next(&nl)));
            lr[2 * i + 1] = 0.8f * tanhf(1.3f * (b + st * noise_next(&nr)));
        }
        break;
    }
    }
    return n;
}

// KitBalance: where each role sits, as the loudness of its first 300 ms in dB RMS.
static double balance_target(int pad)
{
    switch (pad) {
    case DRUM_KICK: case DRUM_SNARE: return -15;
    case DRUM_CLAP: return -18;
    case DRUM_LOWTOM: case DRUM_HIGHTOM: return -17;
    case DRUM_HAT: case DRUM_OPENHAT: return -24;
    case DRUM_CRASH: return -25;
    case DRUM_RIDE: return -28;
    default: return -18;
    }
}

// Called between the kit's sounds while drum_machine_init makes them; the board's own version lets other tasks run.
__attribute__((weak)) void drum_machine_yield(void) {}

static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// One computed kit: its sounds (`most` frames at most) into memory from alloc (NULL = malloc), each with its level;
// false without memory (what was taken goes back: heap_caps memory is freed with free as well).
static bool make_sounds(drum_sounds_t *k, uint32_t rate, uint32_t (*sound)(int, uint32_t, float *, uint32_t), uint32_t most,
                        void *(*alloc)(size_t))
{
    float *work = malloc(sizeof(float) * 2 * most);
    if (!work) return false;
    for (int p = 0; p < DRUM_PADS; p++) {
        drum_machine_yield();   // (between the sounds: the board lets its other tasks run)
        uint32_t n = sound(p, rate, work, most);
        k->sound[p] = !n ? NULL : alloc ? alloc(sizeof(int16_t) * 2 * n) : malloc(sizeof(int16_t) * 2 * n);
        if (!k->sound[p]) {
            while (p--) free(k->sound[p]), k->sound[p] = NULL;
            free(work);
            return false;
        }
        k->length[p] = n;
        uint32_t a = (uint32_t)(0.3 * rate) < n ? (uint32_t)(0.3 * rate) : n;
        double sum = 0;
        for (uint32_t i = 0; i < 2 * a; i++) sum += (double)work[i] * work[i];
        double attack_db = 10 * log10(fmax(sum / (2.0 * a), 1e-12));
        double g = fmin(12, fmax(-30, balance_target(p) - attack_db));
        k->gain[p] = (float)pow(10, g / 20);
        for (uint32_t i = 0; i < 2 * n; i++) {
            float v = work[i] * 32767.0f;
            k->sound[p][i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
        }
    }
    free(work);
    return true;
}

bool drum_machine_init(drum_machine_t *d, uint32_t rate, void *(*alloc)(size_t))
{
    memset(d, 0, sizeof(*d));
    d->rate = rate;
    atomic_init(&d->want_groove, -1);
    atomic_init(&d->want_run, 2);
    atomic_init(&d->want_kit, -1);
    atomic_init(&d->lofi_made, false);
    atomic_init(&d->bpm_x100, 12000);
    atomic_init(&d->level_bits, fbits(0.35f));   // -9 dB: under the guitar (the kit peaks near 2 at 0 dB)
    return make_sounds(&d->computed, rate, drum_kit_sound, sound_length(DRUM_RIDE, rate), alloc);
}

bool drum_machine_make_lofi(drum_machine_t *d, void *(*alloc)(size_t))
{
    if (atomic_load(&d->lofi_made)) return true;
    uint32_t most = 0;
    for (int p = 0; p < DRUM_PADS; p++) {
        uint32_t n = drum_lofi_sound(p, d->rate, NULL, 0);
        if (n > most) most = n;
    }
    if (!make_sounds(&d->lofi, d->rate, drum_lofi_sound, most, alloc)) return false;
    atomic_store(&d->lofi_made, true);   // (after the sounds: the audio side takes them from here on)
    return true;
}

bool drum_machine_load_kit(drum_machine_t *d, const uint8_t *file, size_t size)
{
    if (size < 16 || memcmp(file, "NDK1", 4)) return false;
    uint32_t head[4];
    memcpy(head, file, 16);
    uint32_t count = head[2], data = head[3];
    if (head[1] != d->rate || count > DRUM_PADS * DRUM_LAYERS * DRUM_TAKES || data != 16 + 16 * count || data > size) return false;
    memset(d->take, 0, sizeof(d->take));
    memset(d->takes, 0, sizeof(d->takes));
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = file + 16 + 16 * i;
        uint32_t off, frames;
        float gain;
        memcpy(&off, e + 4, 4);
        memcpy(&frames, e + 8, 4);
        memcpy(&gain, e + 12, 4);
        int pad = e[0], layer = e[1], take = e[2], ch = e[3];
        if (pad >= DRUM_PADS || layer >= DRUM_LAYERS || take >= DRUM_TAKES || (ch != 1 && ch != 2)) return false;
        if ((uint64_t)data + off + (uint64_t)frames * ch * 2 > size) return false;
        d->take[pad][layer][take] = (drum_take_t){ (const int16_t *)(file + data + off), frames, (uint8_t)ch };
        if (take + 1 > d->takes[pad][layer]) d->takes[pad][layer] = (uint8_t)(take + 1);
        d->kit_gain[pad] = gain;
    }
    d->has_samples = true;
    return true;
}
void drum_machine_set_kit(drum_machine_t *d, int kit) { if (kit >= 0 && kit < DRUM_KITS) atomic_store(&d->want_kit, kit); }
int drum_machine_kit(drum_machine_t *d)
{
    int want = atomic_load(&d->want_kit);
    return want >= 0 ? want : d->kit;
}

void drum_machine_start(drum_machine_t *d) { atomic_store(&d->want_run, 1); }
void drum_machine_stop(drum_machine_t *d) { atomic_store(&d->want_run, 0); }
void drum_machine_set_groove(drum_machine_t *d, int groove)
{
    if (groove >= 0 && groove < DRUM_GROOVE_COUNT) atomic_store(&d->want_groove, groove);
}
void drum_machine_set_bpm(drum_machine_t *d, float bpm)
{
    if (bpm >= 30 && bpm <= 300) atomic_store(&d->bpm_x100, (uint32_t)lrintf(bpm * 100));
}
void drum_machine_set_level_db(drum_machine_t *d, float db) { atomic_store(&d->level_bits, fbits(db <= -90 ? 0 : powf(10, db / 20))); }

// ---- playing ----

static void trigger(drum_machine_t *d, int pad, float gain)
{
    if (pad == DRUM_HAT || pad == DRUM_OPENHAT) {   // one hi-hat: a new hit stops the one that rings
        for (int v = 0; v < DRUM_VOICES; v++)
            if (d->voice[v].on && d->voice[v].gain > 0 && (d->voice[v].pad == DRUM_HAT || d->voice[v].pad == DRUM_OPENHAT))
                d->voice[v].gain = -d->voice[v].gain;   // negative = fading out over about CHOKE_FRAMES (see the render)
    }
    int slot = -1;
    uint32_t oldest = 0;
    for (int v = 0; v < DRUM_VOICES; v++) {
        if (!d->voice[v].on) { slot = v; break; }
        if (d->voice[v].pos >= oldest) oldest = d->voice[v].pos, slot = v;   // all busy: the one that rang longest
    }
    d->voice[slot].on = true;
    d->voice[slot].pad = pad;
    d->voice[slot].pos = 0;
    d->voice[slot].wait = 0;
    if (d->kit == DRUM_KIT_ACOUSTIC && d->has_samples) {
        // the layer the velocity asks for (the recording carries the loudness); without it the next one, softer
        int layer = gain >= 0.9f ? 2 : gain >= 0.55f ? 1 : 0;
        float extra = 1;
        while (layer < DRUM_LAYERS - 1 && !d->takes[pad][layer]) layer++, extra *= 0.5f;
        while (layer > 0 && !d->takes[pad][layer]) layer--;
        int n = d->takes[pad][layer];
        if (n) {
            const drum_take_t *t = &d->take[pad][layer][d->next_take[pad] % n];
            d->next_take[pad]++;
            d->voice[slot].data = t->data;
            d->voice[slot].len = t->frames;
            d->voice[slot].channels = t->channels;
            d->voice[slot].gain = extra * d->kit_gain[pad];
            return;
        }
    }
    const drum_sounds_t *k = d->kit == DRUM_KIT_LOFI && atomic_load(&d->lofi_made) ? &d->lofi : &d->computed;
    d->voice[slot].data = k->sound[pad];
    d->voice[slot].len = k->length[pad];
    d->voice[slot].channels = 2;
    d->voice[slot].gain = gain * k->gain[pad];
}

static float velocity(char mark) { return mark == 'X' ? 1.0f : mark == 'x' ? 0.75f : mark == 'o' ? 0.35f : 0; }

static void hits_of_step(drum_machine_t *d, uint32_t step)
{
    const drum_groove_t *g = &DRUM_GROOVES[d->groove];
    uint32_t total = (uint32_t)g->steps_per_bar * g->bars, s = step % total;
    for (int p = 0; p < DRUM_PADS; p++) {
        const char *lane = g->lane[p];
        if (!lane || strlen(lane) <= s) continue;
        float v = velocity(lane[s]);
        if (v > 0) trigger(d, p, v);
    }
}

void drum_machine_render(drum_machine_t *d, float *out, uint32_t frames)
{
    int run = atomic_exchange(&d->want_run, 2);
    int want = atomic_load(&d->want_groove);
    if (run == 1) {
        if (want >= 0) d->groove = atomic_exchange(&d->want_groove, -1);
        d->running = true;
        d->step_pos = 0;
        d->next_step = 0;
    } else if (run == 0) {
        d->running = false;
    }
    if (!d->running && want >= 0) d->groove = atomic_exchange(&d->want_groove, -1);
    const drum_groove_t *g = &DRUM_GROOVES[d->groove];
    double bpm = atomic_load(&d->bpm_x100) / 100.0;
    double step_frames = d->rate * 60.0 / bpm * 4.0 / g->steps_per_bar;   // a bar is four beats (12 steps: triplets)
    // (one divide per render, not per frame: the P4 has no double unit, a soft __divdf3 costs some 200 cycles,
    // 2026-11-27; the same quotient is added, so the steps are exactly as before)
    double inc = 1.0 / step_frames;
    float level = bitsf(atomic_load(&d->level_bits));
    int kit = atomic_exchange(&d->want_kit, -1);
    if (kit >= 0) d->kit = kit;

    for (uint32_t k = 0; k < frames; k++) {
        // the hits that fall on this frame
        while (d->running && !d->paused) {
            double at = d->next_step + ((d->next_step & 1) ? g->swing : 0);
            if (at > d->step_pos) break;
            if (d->next_step % g->steps_per_bar == 0 && atomic_load(&d->want_groove) >= 0) {   // a new groove at the bar line
                d->groove = atomic_exchange(&d->want_groove, -1);
                g = &DRUM_GROOVES[d->groove];
                step_frames = d->rate * 60.0 / bpm * 4.0 / g->steps_per_bar;
                inc = 1.0 / step_frames;
                d->step_pos -= d->next_step;
                d->next_step = 0;
            }
            hits_of_step(d, d->next_step);
            d->next_step++;
        }
        if (d->running && !d->paused) d->step_pos += inc;

        float l = 0, r = 0;
        for (int v = 0; v < DRUM_VOICES; v++) {
            if (!d->voice[v].on) continue;
            if (d->voice[v].wait) { d->voice[v].wait--; continue; }
            uint32_t pos = d->voice[v].pos;
            float gain = d->voice[v].gain;
            if (gain < 0) {   // choked: fading out
                gain = -gain;
                d->voice[v].gain = -(gain * 0.93f);   // about 2 ms to -60 dB
                if (gain < 0.001f) { d->voice[v].on = false; continue; }
            }
            if (pos >= d->voice[v].len || !d->voice[v].data) { d->voice[v].on = false; continue; }
            const int16_t *x = d->voice[v].data;
            if (d->voice[v].channels == 2) {
                l += x[2 * pos] * (1.0f / 32768) * gain;
                r += x[2 * pos + 1] * (1.0f / 32768) * gain;
            } else {
                float m = x[pos] * (1.0f / 32768) * gain;
                l += m;
                r += m;
            }
            d->voice[v].pos = pos + 1;
        }
        out[2 * k] += l * level;
        out[2 * k + 1] += r * level;
    }
    uint32_t total = (uint32_t)g->steps_per_bar * g->bars;
    uint32_t step = d->next_step ? (d->next_step - 1) % total : 0;
    atomic_store(&d->i_step, step % g->steps_per_bar);
    atomic_store(&d->i_bar, step / g->steps_per_bar);
    atomic_store(&d->i_running, d->running);
    atomic_store(&d->i_groove, (uint32_t)d->groove);
}

void drum_machine_start_now(drum_machine_t *d, uint32_t delay, double steps)
{
    int want = atomic_exchange(&d->want_groove, -1);
    if (want >= 0) d->groove = want;
    atomic_store(&d->want_run, 2);
    d->running = true;
    double step_frames = drum_machine_step_frames(d);
    d->step_pos = steps - delay / step_frames;   // (negative: the first hit comes `delay` frames into the render)
    d->next_step = steps <= 0 ? 0 : (uint32_t)ceil(steps - 1e-9);
}

void drum_machine_hit_now(drum_machine_t *d, int pad, uint32_t delay, float velocity)
{
    if (pad < 0 || pad >= DRUM_PADS) return;
    trigger(d, pad, velocity);
    for (int v = 0; v < DRUM_VOICES; v++)   // (the voice just started: the one at the start of its sound)
        if (d->voice[v].on && d->voice[v].pad == pad && d->voice[v].pos == 0 && !d->voice[v].wait) { d->voice[v].wait = delay; break; }
}

double drum_machine_step_frames(drum_machine_t *d)
{
    double bpm = atomic_load(&d->bpm_x100) / 100.0;
    return d->rate * 60.0 / bpm * 4.0 / DRUM_GROOVES[d->groove].steps_per_bar;
}

int drum_machine_steps_per_bar(drum_machine_t *d) { return DRUM_GROOVES[d->groove].steps_per_bar; }

void drum_machine_info(drum_machine_t *d, drum_machine_info_t *out)
{
    out->running = atomic_load(&d->i_running);
    out->groove = (int)atomic_load(&d->i_groove);
    out->bar = (int)atomic_load(&d->i_bar);
    out->step = (int)atomic_load(&d->i_step);
}
