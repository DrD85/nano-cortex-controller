// Tests for the looper, the track player, the WAV reader and the mixer, on the computer.
// Run: components/nano_audio/test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_mix.h"
#include "looper.h"
#include "track_player.h"
#include "wav.h"

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) printf("  ok    ");                                          \
        else { printf("  FAIL  "); failures++; }                               \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0)

#define RATE 48000
#define BLOCK 6                 // frames per USB microframe

// ---- a little world: the player plays g(t); the Nano's output reaches the looper LIN frames later (capture),
// the loop is heard LOUT frames after it was rendered ----

typedef struct {
    looper_t lp;
    int16_t *mem;
    uint32_t lin, lout;
    float *played, *heard;      // stereo, per render frame t
    uint32_t n, t;
} world_t;

static float play_sig(uint32_t t, int which)
{
    // a deterministic noise-like signal that is never zero for long (amplitude 0.2)
    uint32_t x = t * 2654435761u + (uint32_t)which * 40503u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return ((float)(x & 0xffff) / 65535.0f - 0.5f) * 0.4f;
}

static void world_init(world_t *w, uint32_t cap, uint32_t seconds, uint32_t lin, uint32_t lout)
{
    memset(w, 0, sizeof(*w));
    w->mem = calloc((size_t)cap * 4, sizeof(int16_t));
    looper_init(&w->lp, w->mem, cap, RATE);
    looper_set_latency(&w->lp, lin + lout);
    w->lin = lin;
    w->lout = lout;
    w->n = seconds * RATE;
    w->played = calloc((size_t)w->n * 2, sizeof(float));
    w->heard = calloc((size_t)w->n * 2, sizeof(float));
}

static void world_free(world_t *w)
{
    free(w->mem);
    free(w->played);
    free(w->heard);
}

// Runs `frames` render frames; play(t) gives what the player plays at t (stereo), NULL = silence.
typedef void (*play_fn)(uint32_t t, float *lr);

static void world_run(world_t *w, uint32_t frames, play_fn play)
{
    float in[2 * BLOCK], out[2 * BLOCK];
    for (uint32_t done = 0; done < frames && w->t + BLOCK <= w->n; done += BLOCK) {
        for (int k = 0; k < BLOCK; k++) {
            uint32_t t = w->t + k;
            if (play) play(t, &w->played[2 * t]);
            uint32_t src = t >= w->lin ? t - w->lin : 0;   // capture is LIN behind
            in[2 * k] = t >= w->lin ? w->played[2 * src] : 0;
            in[2 * k + 1] = t >= w->lin ? w->played[2 * src + 1] : 0;
        }
        looper_process(&w->lp, in, out, BLOCK);
        for (int k = 0; k < BLOCK; k++) {
            uint32_t t = w->t + k + w->lout;               // heard LOUT later
            if (t < w->n) {
                w->heard[2 * t] = out[2 * k];
                w->heard[2 * t + 1] = out[2 * k + 1];
            }
        }
        w->t += BLOCK;
    }
}

static void play_a(uint32_t t, float *lr) { lr[0] = play_sig(t, 0); lr[1] = play_sig(t, 1); }
static void play_b(uint32_t t, float *lr) { lr[0] = play_sig(t, 2); lr[1] = play_sig(t, 3); }
static void play_sine(uint32_t t, float *lr) { lr[0] = lr[1] = 0.4f * (float)sin(2 * M_PI * 220.0 * t / RATE); }

// The heard loop against what was played: heard(t) vs played(t0 + (t - t0) mod len), over [from, to).
static double loop_error(world_t *w, uint32_t t0, uint32_t len, uint32_t from, uint32_t to, uint32_t skip_start)
{
    double worst = 0;
    for (uint32_t t = from; t < to; t++) {
        // the take is what was played from LOUT after the first press to LOUT after the second one, in phase
        uint32_t p = (t - t0) % len;
        if (p >= w->lout && p < w->lout + skip_start) continue;   // the crossfade
        uint32_t s = t0 + p + (p < w->lout ? len : 0);
        double e = fabs(w->heard[2 * t] - w->played[2 * s]);
        double e2 = fabs(w->heard[2 * t + 1] - w->played[2 * s + 1]);
        if (e2 > e) e = e2;
        if (e > worst) worst = e;
    }
    return worst;
}

static void press(world_t *w, looper_cmd_t c) { looper_command(&w->lp, c); }

static void test_free_loop(uint32_t lin, uint32_t lout)
{
    world_t w;
    world_init(&w, RATE * 10, 12, lin, lout);
    world_run(&w, RATE / 2, NULL);                 // 0.5 s nothing
    uint32_t t0 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, 2 * RATE, play_a);               // 2 s take
    uint32_t t1 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);                 // fix: plays
    world_run(&w, RATE / 10, play_a);              // goes on playing a moment (the next start, played live)
    world_run(&w, 9 * RATE, NULL);                 // then listens
    looper_info_t in;
    looper_get_info(&w.lp, &in);
    uint32_t len = t1 - t0;
    CHECK(in.state == LOOPER_PLAY && in.len == len, "free loop: plays, length %u (want %u)", in.len, len);
    // positions after the 5 ms crossfade, from the second pass on
    double e = loop_error(&w, t0, len, t1 + len, t1 + 4 * len, 240);
    CHECK(e < 1e-3, "latency %u + %u: the loop is heard in time with what was played (worst error %.5f)", lin, lout, e);
    // seam: the first 5 ms of the take fade in, what was played just after the press fades out over them
    uint32_t p = 100;
    double want = w.played[2 * (t0 + lout + p)] * p / 240.0 + w.played[2 * (t1 + lout + p)] * (240 - p) / 240.0;
    double got = w.heard[2 * (t1 + 2 * len + lout + p)];
    CHECK(fabs(got - want) < 1e-3, "seam crossfade: take fades in, the note after the press fades out (%.4f vs %.4f)",
          got, want);
    world_free(&w);
}

static void test_bar_lines(void)
{
    world_t w;
    world_init(&w, RATE * 10, 12, 0, 0);
    looper_set_bar(&w.lp, RATE);                   // 60 bpm, 1 bar = 1 s
    uint32_t t0 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, RATE * 9 / 10, play_a);          // pressed early, at 0.9 s
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, BLOCK * 2, play_a);
    looper_info_t in;
    looper_get_info(&w.lp, &in);
    CHECK(in.state == LOOPER_REC && in.to_bar_line, "pressed early: records on to the bar line");
    world_run(&w, RATE / 5, play_a);
    looper_get_info(&w.lp, &in);
    CHECK(in.state == LOOPER_PLAY && in.len == RATE, "... and plays from there, 1 bar (%u)", in.len);
    world_free(&w);

    world_init(&w, RATE * 10, 12, 0, 0);
    looper_set_bar(&w.lp, RATE);
    t0 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, RATE * 22 / 10, play_a);         // pressed late, at 2.2 s
    uint32_t t1 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, BLOCK, play_a);
    looper_get_info(&w.lp, &in);
    CHECK(in.state == LOOPER_PLAY && in.len == 2 * RATE, "pressed late: 2 bars (%u), plays at once", in.len);
    CHECK(in.pos > RATE / 5 - 100 && in.pos < RATE / 5 + 100, "... in time: the play position is 0.2 s in (%u)", in.pos);
    world_run(&w, 6 * RATE, NULL);
    uint32_t len = 2 * RATE, p = 4800;             // 0.1 s into the loop: covered by the late part
    double want = w.played[2 * (t0 + p)];
    double got = w.heard[2 * (t0 + 3 * len + p)];
    CHECK(fabs(got - want) < 1e-3, "what was played after the bar line is NOT laid over the start (%.4f vs %.4f)", got, want);
    // ... only the seam: within the crossfade the start fades in while what followed the bar line fades out
    uint32_t q = RATE * 5 / 1000 / 2;              // the middle of the 5 ms crossfade
    double mix = 0.5 * w.played[2 * (t0 + q)] + 0.5 * w.played[2 * (t0 + len + q)];
    got = w.heard[2 * (t0 + 3 * len + q)];
    CHECK(fabs(got - mix) < 2e-2, "... the seam is crossfaded (%.4f vs %.4f)", got, mix);
    (void)t1;
    world_free(&w);
}

static void test_overdub_undo(void)
{
    world_t w;
    world_init(&w, RATE * 10, 30, 96, 96);
    uint32_t t0 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, 2 * RATE, play_a);
    uint32_t t1 = w.t, len = t1 - t0;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, len, NULL);                      // one pass listening
    uint32_t d0 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);                 // overdub b over one whole pass
    world_run(&w, len, play_b);
    press(&w, LOOPER_CMD_REC_DUB);                 // back to play
    world_run(&w, 2 * len, NULL);
    // heard now: a (the take) + b (the overdub), both in time
    uint32_t t = w.t - len / 2 - 2000;             // somewhere in the last pass, well away from punch in / out
    uint32_t pa = (t - t0) % len, pb = (t - d0) % len;
    double want = w.played[2 * (t0 + pa)] + w.played[2 * (d0 + pb)];
    CHECK(fabs(w.heard[2 * t] - want) < 2e-3, "overdub: take and overdub heard together, in time (%.4f vs %.4f)",
          w.heard[2 * t], want);
    looper_info_t in;
    looper_get_info(&w.lp, &in);
    CHECK(in.can_undo && !in.can_redo, "undo possible after an overdub");

    press(&w, LOOPER_CMD_UNDO);
    world_run(&w, len, NULL);
    t = w.t - len / 2;
    double e = fabs(w.heard[2 * t] - w.played[2 * (t0 + (t - t0) % len)]);
    CHECK(e < 1e-3, "undo: only the take is heard (error %.5f)", e);
    press(&w, LOOPER_CMD_REDO);
    world_run(&w, len, NULL);
    t = w.t - len / 2;
    want = w.played[2 * (t0 + (t - t0) % len)] + w.played[2 * (d0 + (t - d0) % len)];
    CHECK(fabs(w.heard[2 * t] - want) < 2e-3, "redo: the overdub is back");

    // a second overdub (silence in the right channel only to keep it simple: plays a again, shifted)
    uint32_t d1 = w.t;
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, len / 2, play_a);
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, 2 * len, NULL);
    press(&w, LOOPER_CMD_UNDO);                    // removes only the second overdub
    world_run(&w, 2 * len, NULL);
    t = w.t - len / 2 - 3000;
    want = w.played[2 * (t0 + (t - t0) % len)] + w.played[2 * (d0 + (t - d0) % len)];
    CHECK(fabs(w.heard[2 * t] - want) < 2e-3, "undo after a second overdub keeps the first one (%.4f vs %.4f)",
          w.heard[2 * t], want);
    (void)d1;
    world_free(&w);
}

static void test_stop_clear_seam(void)
{
    world_t w;
    world_init(&w, RATE * 10, 12, 0, 0);
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, RATE, play_sine);
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, RATE / 20, play_sine);           // the note rings on through the press
    world_run(&w, 3 * RATE, NULL);
    // seam: a 220 Hz sine moves at most 2*pi*220/48000*0.4 = 0.0115 per frame; the crossfade must not jump more
    double worst = 0;
    for (uint32_t t = RATE + RATE / 20; t < w.t; t++) {
        double d = fabs(w.heard[2 * t] - w.heard[2 * (t - 1)]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 0.03, "seam: no jump where the loop wraps (largest step %.4f)", worst);

    press(&w, LOOPER_CMD_PLAY_STOP);
    world_run(&w, RATE / 10, NULL);
    looper_info_t in;
    looper_get_info(&w.lp, &in);
    double tail = 0;
    for (uint32_t t = w.t - RATE / 20; t < w.t; t++) tail = fmax(tail, fabs(w.heard[2 * t]));
    CHECK(in.state == LOOPER_STOP && in.pos == 0 && tail == 0, "stop: silent, back at the top");
    press(&w, LOOPER_CMD_PLAY_STOP);
    world_run(&w, BLOCK, NULL);
    looper_get_info(&w.lp, &in);
    CHECK(in.state == LOOPER_PLAY && in.pos == BLOCK, "play: from the top");
    press(&w, LOOPER_CMD_CLEAR);
    world_run(&w, RATE / 10, NULL);
    looper_get_info(&w.lp, &in);
    CHECK(in.state == LOOPER_EMPTY && in.len == 0, "clear: empty");
    world_free(&w);

    // longer than the memory: fixed by itself
    world_init(&w, RATE, 4, 0, 0);
    press(&w, LOOPER_CMD_REC_DUB);
    world_run(&w, 2 * RATE, play_a);
    looper_get_info(&w.lp, &in);
    CHECK(in.state == LOOPER_PLAY && in.len > RATE * 9 / 10 && in.len < RATE, "full memory: plays by itself (%u)",
          in.len);
    world_free(&w);
}

// ---- WAV ----

static size_t make_wav(uint8_t *buf, uint32_t rate, int channels, int bits, int format, uint32_t frames,
                       bool extra_chunk)
{
    uint8_t *p = buf;
#define PUT32(v) do { uint32_t _v = (v); memcpy(p, &_v, 4); p += 4; } while (0)
#define PUT16(v) do { uint16_t _v = (v); memcpy(p, &_v, 2); p += 2; } while (0)
    int bytes = bits / 8;
    uint32_t data = frames * channels * bytes;
    memcpy(p, "RIFF", 4); p += 4;
    PUT32(0);
    memcpy(p, "WAVE", 4); p += 4;
    if (extra_chunk) {
        memcpy(p, "LIST", 4); p += 4;
        PUT32(5);
        memcpy(p, "abcde\0", 6); p += 6;   // odd size + pad byte
    }
    memcpy(p, "fmt ", 4); p += 4;
    PUT32(16);
    PUT16(format);
    PUT16(channels);
    PUT32(rate);
    PUT32(rate * channels * bytes);
    PUT16(channels * bytes);
    PUT16(bits);
    memcpy(p, "data", 4); p += 4;
    PUT32(data);
    for (uint32_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) {
            float v = (c == 0 ? 0.5f : -0.25f) * (float)sin(i * 0.01);
            if (format == 3) { memcpy(p, &v, 4); p += 4; }
            else if (bits == 16) { int16_t s = (int16_t)lrintf(v * 32767); memcpy(p, &s, 2); p += 2; }
            else if (bits == 24) { int32_t s = (int32_t)lrintf(v * 8388607); p[0] = s; p[1] = s >> 8; p[2] = s >> 16; p += 3; }
        }
    uint32_t riff = (uint32_t)(p - buf - 8);
    memcpy(buf + 4, &riff, 4);
    return (size_t)(p - buf);
}

static void test_wav(void)
{
    static uint8_t buf[200000];
    int16_t out[2 * 1000];
    struct { int ch, bits, fmt; bool extra; const char *name; } cases[] = {
        { 2, 16, 1, false, "16-bit stereo" }, { 1, 16, 1, true, "16-bit mono with a LIST chunk" },
        { 2, 24, 1, false, "24-bit stereo" }, { 2, 32, 3, false, "32-bit float stereo" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t n = make_wav(buf, 44100, cases[c].ch, cases[c].bits, cases[c].fmt, 1000, cases[c].extra);
        wav_info_t wi;
        bool ok = wav_parse(buf, n, &wi);
        CHECK(ok && wi.rate == 44100 && wi.frames == 1000 && wi.channels == cases[c].ch, "wav %s: read (%u frames)",
              cases[c].name, ok ? wi.frames : 0);
        if (!ok) continue;
        wav_to_stereo16(&wi, buf + wi.data_offset, 1000, out);
        int16_t l = out[2 * 500], r = out[2 * 500 + 1];
        int16_t wl = (int16_t)lrintf(0.5f * (float)sin(500 * 0.01) * 32767);
        int16_t wr = cases[c].ch == 1 ? wl : (int16_t)lrintf(-0.25f * (float)sin(500 * 0.01) * 32767);
        CHECK(abs(l - wl) <= 1 && abs(r - wr) <= 1, "wav %s: samples (%d %d, want %d %d)", cases[c].name, l, r, wl, wr);
    }
    wav_info_t wi;
    CHECK(!wav_parse((const uint8_t *)"RIFF\0\0\0\0WAVEjunk", 16, &wi), "not a usable WAV: refused");
    size_t n = make_wav(buf, 44100, 6, 16, 1, 10, false);
    CHECK(!wav_parse(buf, n, &wi), "6 channels: refused");
}

// ---- track player ----

static void test_track_player(void)
{
    static int16_t ring[8192 * 2];
    track_player_t tp;
    track_player_init(&tp, ring, 8192, RATE);
    track_player_begin(&tp, 44100, 0, 44100 * 3);

    // the file side: a 1 kHz sine at 44.1 kHz, 3 s, written whenever there is room
    uint32_t written = 0, total = 44100 * 3;
    int16_t block[2 * 512];
    float lr[2 * 48];
    size_t outn = (size_t)4 * RATE;
    float *mono = calloc(outn, sizeof(float));
    size_t got = 0;
    bool began = false, ended = false;
    track_player_set_playing(&tp, true);
    for (int ms = 0; ms < 4000; ms++) {
        if (!began) began = track_player_ready(&tp);
        while (began && written < total && track_player_space(&tp) >= 512) {
            for (int i = 0; i < 512; i++) {
                int16_t v = (int16_t)lrint(sin(2 * M_PI * 1000.0 * (written + i) / 44100) * 0.5 * 32767);
                block[2 * i] = block[2 * i + 1] = v;
            }
            uint32_t n = total - written < 512 ? total - written : 512;
            track_player_write(&tp, block, n);
            written += n;
            if (written == total) track_player_end_of_data(&tp);
        }
        track_player_render_stereo(&tp, lr, 48);
        for (int i = 0; i < 48 && got < outn; i++) mono[got++] = lr[2 * i];
        track_player_info_t ti;
        track_player_get_info(&tp, &ti);
        if (ti.ended) ended = true;
    }
    // frequency: zero crossings over the steady middle second
    int cross = 0;
    for (size_t i = RATE + 1; i < 2 * RATE; i++) if (mono[i - 1] < 0 && mono[i] >= 0) cross++;
    CHECK(abs(cross - 1000) <= 1, "track: 44.1 kHz file played at 48 kHz, pitch right (%d Hz)", cross);
    double peak = 0;
    for (size_t i = RATE; i < 2 * RATE; i++) peak = fmax(peak, fabs(mono[i]));
    CHECK(fabs(peak - 0.5) < 0.01, "track: level right (peak %.3f)", peak);
    track_player_info_t ti;
    track_player_get_info(&tp, &ti);
    CHECK(ended && !ti.playing, "track: ends by itself at the end of the file");
    double after = 0;
    for (size_t i = (size_t)(3.1 * RATE); i < got; i++) after = fmax(after, fabs(mono[i]));
    CHECK(after == 0, "track: silent after the end");

    // pause fades, resume goes on where it was; seek (begin at an offset) jumps
    track_player_begin(&tp, 44100, 0, total);
    written = 0;
    began = false;
    track_player_set_playing(&tp, true);
    for (int ms = 0; ms < 1500; ms++) {
        if (!began) began = track_player_ready(&tp);
        while (began && written < total && track_player_space(&tp) >= 512) {
            for (int i = 0; i < 512; i++) block[2 * i] = block[2 * i + 1] = 1000;
            track_player_write(&tp, block, 512);
            written += 512;
        }
        if (ms == 1000) track_player_set_playing(&tp, false);
        track_player_render_stereo(&tp, lr, 48);
    }
    track_player_get_info(&tp, &ti);
    uint32_t pos = ti.pos;
    track_player_render_stereo(&tp, lr, 48);
    CHECK(!ti.playing && lr[0] == 0 && pos > 44100 * 9 / 10 && pos < 44100 * 11 / 10, "pause: silent, position kept (%u)", pos);
    track_player_set_playing(&tp, true);
    for (int ms = 0; ms < 20; ms++) track_player_render_stereo(&tp, lr, 48);
    track_player_get_info(&tp, &ti);
    CHECK(ti.playing && ti.pos > pos && fabs(lr[0] - 1000 / 32768.0) < 1e-3, "resume: goes on where it was");
    track_player_set_gain_db(&tp, -6.0f);
    for (int ms = 0; ms < 50; ms++) track_player_render_stereo(&tp, lr, 48);
    CHECK(fabs(lr[0] - 1000 / 32768.0 * 0.501) < 1e-4, "gain -6 dB");
    track_player_begin(&tp, 48000, 48000 * 60, 48000 * 120);   // a seek to 1:00
    for (int ms = 0; ms < 5; ms++) track_player_render_stereo(&tp, lr, 48);
    track_player_get_info(&tp, &ti);
    CHECK(track_player_ready(&tp) && ti.pos == 48000 * 60 && ti.rate == 48000, "seek: emptied, new position %u", ti.pos);
    free(mono);
}

// ---- mixer ----

static void test_mixer(void)
{
    static int16_t ring[4096 * 2], tring[4096 * 2];
    static int16_t lmem[RATE * 4];
    audio_pipe_t phone;
    track_player_t tp;
    looper_t lp;
    audio_pipe_init(&phone, ring, 4096, 48000, 48000);
    track_player_init(&tp, tring, 4096, RATE);
    looper_init(&lp, lmem, RATE, RATE);
    static audio_mix_t mix;
    audio_mix_init(&mix, &phone, &tp, &lp);

    // phone: constant 0.25 on both channels; the Nano's capture: constant 0.1 on 3/4 (the guitar)
    int16_t pb[2 * 480];
    for (int i = 0; i < 960; i++) pb[i] = 8192;
    int32_t cap[6 * 8], out[6 * 8];
    for (int i = 0; i < 6; i++) for (int c = 0; c < 8; c++) cap[i * 8 + c] = c == 2 || c == 3 ? audio_to_slot(0.1f) : 0;
    for (int ms = 0; ms < 200; ms++) {
        audio_pipe_write(&phone, pb, 48);
        for (int m = 0; m < 8; m++) {
            audio_mix_capture(&mix, cap, 6, 8, 2, 3);
            audio_mix_render(&mix, out, 6, 8, 0, 1);
        }
    }
    double l = (out[0] >> 8) / 8388607.0, r = (out[1] >> 8) / 8388607.0;
    CHECK(fabs(l - 0.25) < 1e-3 && fabs(r - 0.25) < 1e-3 && out[2] == 0, "mixer: phone on 1/2, other channels silent");

    // the looper records the capture and plays it back on top
    looper_command(&lp, LOOPER_CMD_REC_DUB);   // 0.5 s take (the memory holds 1 s)
    for (int m = 0; m < 4000; m++) { audio_pipe_write(&phone, pb, 6); audio_mix_capture(&mix, cap, 6, 8, 2, 3); audio_mix_render(&mix, out, 6, 8, 0, 1); }
    looper_command(&lp, LOOPER_CMD_REC_DUB);
    for (int m = 0; m < 4000; m++) { audio_pipe_write(&phone, pb, 6); audio_mix_capture(&mix, cap, 6, 8, 2, 3); audio_mix_render(&mix, out, 6, 8, 0, 1); }
    l = (out[0] >> 8) / 8388607.0;
    CHECK(fabs(l - 0.35) < 2e-3, "mixer: phone + loop (%.4f, want 0.35)", l);

    // loud sources: the limiter keeps the sum below full scale
    audio_mix_set_gain_db(&mix, AUDIO_SRC_LOOPER, 18.0f);
    double peak = 0;
    for (int m = 0; m < 4000; m++) {
        audio_pipe_write(&phone, pb, 6);
        audio_mix_capture(&mix, cap, 6, 8, 2, 3);
        audio_mix_render(&mix, out, 6, 8, 0, 1);
        peak = fmax(peak, fabs((out[0] >> 8) / 8388607.0));
    }
    audio_mix_stats_t ms;
    audio_mix_get_stats(&mix, &ms);
    CHECK(peak <= 0.99 && ms.limited > 0, "mixer: limiter holds the sum below full scale (peak %.3f)", peak);
    CHECK(ms.capture_short == 0, "mixer: no capture frames missing (%u)", ms.capture_short);

    {   // a start with too much or too little waiting: after a second the lowest fill is cushion - one burst
        for (int extra = 0; extra < 2; extra++) {
            static audio_mix_t st;
            audio_mix_init(&st, NULL, NULL, NULL);
            int32_t c48[48 * 8], o48[48 * 8];
            for (int i = 0; i < 48 * 8; i++) c48[i] = 0;
            for (int m = 0; m < (extra ? 9 : 4); m++) audio_mix_capture(&st, c48, 48, 8, 2, 3);   // 432 / 192 frames ahead
            uint32_t low = UINT32_MAX;
            for (int ms = 0; ms < 3000; ms++) {
                audio_mix_render(&st, o48, 48, 8, 0, 1);
                uint32_t f = atomic_load(&st.cw) - atomic_load(&st.cr);   // (the lowest point: right after a render)
                if (ms > 2000 && f < low) low = f;
                audio_mix_capture(&st, c48, 48, 8, 2, 3);
            }
            CHECK(low + 50 >= MIX_CUSHION && low <= MIX_CUSHION + 50, "mixer: settled cushion with a %s start (lowest fill %u, settled %d)",
                  extra ? "large" : "small", low, (int)st.settled);
        }
    }
    {   // the capture starts before the render (as at the board's start): the render begins with the cushion
        static audio_mix_t early;
        audio_mix_init(&early, NULL, NULL, NULL);
        for (int m = 0; m < 60; m++) audio_mix_capture(&early, cap, 6, 8, 2, 3);       // 360 frames wait
        for (int m = 0; m < 400; m++) { audio_mix_capture(&early, cap, 6, 8, 2, 3); audio_mix_render(&early, out, 6, 8, 0, 1); }
        uint32_t f = atomic_load(&early.cw) - atomic_load(&early.cr);
        audio_mix_stats_t es;
        audio_mix_get_stats(&early, &es);
        CHECK(f >= MIX_CUSHION - 6 && f <= MIX_CUSHION + 6 && es.capture_short == 0, "mixer: a head start of the capture is cut to the cushion (%u frames)", f);
    }
    // The cushion comes back by itself: after the capture stops for a while (the streams start anew) ...
    #define FILL(m) (atomic_load(&(m).cw) - atomic_load(&(m).cr))
    for (int m = 0; m < 200; m++) audio_mix_render(&mix, out, 6, 8, 0, 1);            // 25 ms without capture
    for (int m = 0; m < 800; m++) { audio_mix_capture(&mix, cap, 6, 8, 2, 3); audio_mix_render(&mix, out, 6, 8, 0, 1); }
    uint32_t fill = FILL(mix);
    CHECK(fill >= MIX_CUSHION - 6 && fill <= MIX_CUSHION + 6, "mixer: after running dry the cushion is whole again (%u frames, want %d)", fill, MIX_CUSHION);
    // ... and after the capture ran without the render
    for (int m = 0; m < (MIX_SLACK + 300) / 6; m++) audio_mix_capture(&mix, cap, 6, 8, 2, 3);   // more than the slack piles up
    for (int m = 0; m < 800; m++) { audio_mix_capture(&mix, cap, 6, 8, 2, 3); audio_mix_render(&mix, out, 6, 8, 0, 1); }
    fill = FILL(mix);
    CHECK(fill >= MIX_CUSHION - 6 && fill <= MIX_CUSHION + 6, "mixer: a pile of waiting capture frames is cut back to the cushion (%u frames)", fill);
    // ... while bursts as on the board (48 frames in, 48 out, in either order, now and then 3 ms late) change nothing
    audio_mix_get_stats(&mix, &ms);
    uint32_t dropped = ms.capture_dropped, shorts = ms.capture_short;
    int32_t cap48[48 * 8], out48[48 * 8];
    for (int i = 0; i < 48; i++) for (int c = 0; c < 8; c++) cap48[i * 8 + c] = c == 2 || c == 3 ? audio_to_slot(0.1f) : 0;
    unsigned seed = 7;
    for (int n = 0; n < 20000; n++) {
        seed = seed * 1103515245u + 12345u;
        int late = (seed >> 20) % 400 == 0 ? 3 : 1;                                    // transfers handled together
        bool capture_first = (seed >> 16) & 1;
        for (int side = 0; side < 2; side++)
            for (int k = 0; k < late; k++) {
                if ((side == 0) == capture_first) audio_mix_capture(&mix, cap48, 48, 8, 2, 3);
                else audio_mix_render(&mix, out48, 48, 8, 0, 1);
            }
    }
    audio_mix_get_stats(&mix, &ms);
    CHECK(ms.capture_dropped == dropped && ms.capture_short == shorts, "mixer: bursts and late transfers lose nothing (%u dropped, %u missing)",
          ms.capture_dropped - dropped, ms.capture_short - shorts);
}

// Loopback: the Nano's capture also carries what the board plays, D frames later - the mixer takes it out again.
static void test_loopback(void)
{
    static int16_t ring[4096 * 2], tring[4096 * 2];
    static int16_t lmem[RATE * 4];
    audio_pipe_t phone;
    track_player_t tp;
    looper_t lp;
    audio_pipe_init(&phone, ring, 4096, 48000, 48000);
    track_player_init(&tp, tring, 4096, RATE);
    looper_init(&lp, lmem, RATE, RATE);
    static audio_mix_t mix;
    audio_mix_init(&mix, &phone, &tp, &lp);
    const int D = 300;
    audio_mix_set_loopback(&mix, D, 1.0f);
    static float played[RATE * 4][2];
    int32_t cap[6 * 8], out[6 * 8];
    int16_t pb[12];
    uint32_t t = 0;
    double worst = 0;
    for (int m = 0; m < 20000; m++) {
        for (int i = 0; i < 6; i++) {
            int16_t v = (int16_t)lrint(sin((t + i) * 0.05) * 6000);
            pb[2 * i] = pb[2 * i + 1] = v;
        }
        audio_pipe_write(&phone, pb, 6);
        for (int i = 0; i < 6; i++) {
            uint32_t s = t + i;
            float guitar = 0.05f;
            float back = s >= (uint32_t)D ? played[(s - D) % (RATE * 4)][0] : 0;
            for (int c = 0; c < 8; c++) cap[i * 8 + c] = c == 2 || c == 3 ? audio_to_slot(guitar + back) : 0;
        }
        audio_mix_capture(&mix, cap, 6, 8, 2, 3);
        audio_mix_render(&mix, out, 6, 8, 0, 1);
        for (int i = 0; i < 6; i++) played[(t + i) % (RATE * 4)][0] = (float)((out[i * 8] >> 8) / 8388607.0);
        if (m > 12000) {   // (after the cushion has settled: 1 s)
            float clean[12];
            audio_mix_last_clean(&mix, clean, 6);
            for (int i = 0; i < 6; i++) worst = fmax(worst, fabs(clean[2 * i] - 0.05));
        }
        t += 6;
    }
    CHECK(worst < 1e-3, "loopback: the board's own playback is taken out of the capture (error %.5f)", worst);
}

int main(void)
{
    printf("== looper\n");
    test_free_loop(0, 0);
    test_free_loop(130, 250);
    test_bar_lines();
    test_overdub_undo();
    test_stop_clear_seam();
    printf("== wav\n");
    test_wav();
    printf("== track player\n");
    test_track_player();
    printf("== mixer\n");
    test_mixer();
    test_loopback();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
