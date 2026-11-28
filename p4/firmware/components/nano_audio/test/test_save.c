// Saving a looper part while the audio runs (looper.h looper_save_*, loop_wav.h) on the computer:
//   - the copy is the loop as it plays from its next pass on (a take, with an overdub, undone / redone, stopped in
//     the middle of a merge or before its seam was laid) - compared frame by frame with what the looper then plays;
//   - what changes the loop (a take, an overdub, undo / redo, clear) makes a copy that is under way fail, what does not
//     (play / stop, pause, a part switch, the loop playing on) leaves it alone;
//   - a copy taken in pieces between the renders, and one taken by a thread of its own against an audio thread that
//     overdubs, undoes and clears at random: whenever it says "whole", it is the loop exactly;
//   - the WAV header (read back by the board's own reader) and the file names.
// Run: components/nano_audio/test/run.sh
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "loop_parts.h"
#include "loop_wav.h"
#include "looper.h"
#include "wav.h"

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        printf((cond) ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__);         \
        printf("\n"); if (!(cond)) failures++;                                 \
    } while (0)

#define RATE 48000
#define BLOCK 48
#define LAT 300                        // the looper's round trip here (frames)

static float sig(uint32_t t, int which)   // a noise-like signal (amplitude 0.25)
{
    uint32_t x = t * 2654435761u + (uint32_t)which * 40503u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return ((float)(x & 0xffff) / 65535.0f - 0.5f) * 0.5f;
}

static looper_t L;
static uint32_t T;                     // frames processed
static int voice = -1;                 // what is played: -1 silence, else sig(t, 2 * voice + channel)
static float out_all[RATE * 8 * 2];    // what the looper played, frame by frame (for the comparison)
static uint32_t out_n;

static void run(uint32_t frames)
{
    float in[2 * BLOCK], out[2 * BLOCK];
    for (uint32_t k = 0; k < frames; k += BLOCK) {
        for (int i = 0; i < BLOCK; i++) {
            in[2 * i] = voice < 0 ? 0 : sig(T + i, 2 * voice);
            in[2 * i + 1] = voice < 0 ? 0 : sig(T + i, 2 * voice + 1);
        }
        looper_process(&L, in, out, BLOCK);
        if (out_n + BLOCK <= RATE * 8) {
            memcpy(out_all + 2 * out_n, out, sizeof(out));
            out_n += BLOCK;
        }
        T += BLOCK;
    }
}

// What the looper plays over its next two passes (it renders silence in), against the copy: the frame the loop is
// at when a render starts is lp->pos (this test is the audio side). The second pass counts (the first may still lay
// a seam or merge an overdub the copy already shows done). Returns the largest difference in int16 steps.
static int32_t against_next_passes(const int16_t *copy, uint32_t len, uint32_t *compared)
{
    voice = -1;
    uint32_t start = L.pos;
    out_n = 0;
    run(2 * len + BLOCK);
    int32_t worst = 0;
    *compared = 0;
    for (uint32_t k = len; k < 2 * len && k < out_n; k++) {
        uint32_t p = (start + k) % len;
        for (int c = 0; c < 2; c++) {
            int32_t heard = (int32_t)lrintf(out_all[2 * k + c] * 32768.0f);
            int32_t d = abs(heard - copy[2 * p + c]);
            if (d > worst) worst = d;
        }
        (*compared)++;
    }
    return worst;
}

static int16_t *copy_all(looper_save_t *s, bool *whole)
{
    int16_t *c = malloc(sizeof(int16_t) * 2 * s->len);
    uint32_t clipped = 0;
    *whole = true;
    for (uint32_t from = 0; from < s->len; from += 1000) {
        uint32_t n = s->len - from < 1000 ? s->len - from : 1000;
        looper_save_read(&L, s, from, n, c + 2 * from, &clipped);
        *whole = *whole && looper_save_same(&L, s);
    }
    return c;
}

static void check_copy(const char *what)
{
    looper_save_t s;
    bool begun = looper_save_begin(&L, &s), whole = false;
    if (!begun) { CHECK(false, "%s: the copy could not begin (state %d)", what, L.st); return; }
    int16_t *c = copy_all(&s, &whole);
    uint32_t n;
    int32_t worst = against_next_passes(c, s.len, &n);
    CHECK(whole && worst == 0 && n == s.len, "%s: the copy is what plays from the next pass on (%u frames, largest difference %d)", what, n, worst);
    free(c);
}

static void test_copies(void)
{
    printf("== the copy against what the looper plays\n");
    static int16_t mem[RATE * 3 * 4];
    looper_init(&L, mem, RATE * 3, RATE);
    looper_set_latency(&L, LAT);
    looper_save_t s;
    CHECK(!looper_save_begin(&L, &s), "an empty looper gives no copy");

    voice = 0;
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(RATE / 2);
    CHECK(!looper_save_begin(&L, &s), "... nor one that records its first take");
    run(RATE / 2);
    looper_command(&L, LOOPER_CMD_REC_DUB);   // fixed: 1 s
    run(BLOCK);
    CHECK(!looper_save_begin(&L, &s), "... nor right after the take, while its end and seam are still written");
    voice = -1;
    run(RATE / 10);
    check_copy("a take");

    voice = 1;   // an overdub of the second voice over half the loop
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(RATE / 4);
    CHECK(!looper_save_begin(&L, &s), "no copy while it overdubs");
    run(RATE / 4);
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(BLOCK);
    voice = -1;
    run(RATE / 20);
    check_copy("with an overdub");
    looper_command(&L, LOOPER_CMD_UNDO);
    run(BLOCK);
    check_copy("the overdub undone");
    looper_command(&L, LOOPER_CMD_REDO);
    run(BLOCK);
    check_copy("... and redone");

    // a second overdub, stopped in the render it began in: the merge of the first one stops with the loop (a loop
    // that has faded out runs nothing) - the copy merges it as the looper will
    looper_command(&L, LOOPER_CMD_REC_DUB);
    looper_command(&L, LOOPER_CMD_PLAY_STOP);
    run(RATE / 10);
    CHECK(L.st == LOOPER_STOP && L.fold_on && L.fold_left > 0, "an overdub stopped at once: stopped in the middle of the merge (%u frames left to merge)", L.fold_left);
    looper_save_t mid;
    bool begun = looper_save_begin(&L, &mid), whole;
    int16_t *c = begun ? copy_all(&mid, &whole) : NULL;
    looper_command(&L, LOOPER_CMD_PLAY_STOP);   // from the top: the merge goes on
    run(BLOCK);
    CHECK(!looper_save_same(&L, &mid), "... a copy under way fails once it plays again (the merge writes the loop)");
    uint32_t n;
    int32_t worst = c ? against_next_passes(c, mid.len, &n) : 99999;
    CHECK(begun && mid.fold_on && worst == 0, "... its copy is what plays once it runs again (largest difference %d)", worst);
    free(c);

    // a first take whose seam is not laid yet: fixed and stopped in one render, faded out before the take's tail came
    looper_command(&L, LOOPER_CMD_CLEAR);
    run(RATE / 10);
    voice = 2;
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(RATE / 2);
    looper_command(&L, LOOPER_CMD_REC_DUB);
    looper_command(&L, LOOPER_CMD_PLAY_STOP);
    run(BLOCK);
    voice = -1;
    run(RATE / 10);
    CHECK(L.st == LOOPER_STOP && L.tail_pending && L.tail_done < L.tail, "a take fixed and stopped at once: its seam waits for the next start (%u of %u laid)", L.tail_done, L.tail);
    begun = looper_save_begin(&L, &mid);
    c = begun ? copy_all(&mid, &whole) : NULL;
    looper_command(&L, LOOPER_CMD_PLAY_STOP);
    run(BLOCK);
    CHECK(!looper_save_same(&L, &mid), "... a copy under way fails once it plays again (the seam is laid)");
    worst = c ? against_next_passes(c, mid.len, &n) : 99999;
    CHECK(begun && mid.tail_pending && worst <= 1, "... its copy is what plays once it runs (largest difference %d)", worst);
    free(c);

    // loud: a take and an overdub that add up beyond full scale are clipped in the copy and counted
    looper_command(&L, LOOPER_CMD_CLEAR);
    run(RATE / 10);
    for (int i = 0; i < 2; i++) {
        float lvl = 0.7f;
        float in[2 * BLOCK], out[2 * BLOCK];
        for (int k = 0; k < 2 * BLOCK; k++) in[k] = lvl;
        looper_command(&L, LOOPER_CMD_REC_DUB);
        for (int k = 0; k < RATE / 4 / BLOCK; k++) looper_process(&L, in, out, BLOCK);
    }
    looper_command(&L, LOOPER_CMD_REC_DUB);   // overdub over the full loop, then off
    {
        float in[2 * BLOCK], out[2 * BLOCK];
        for (int k = 0; k < 2 * BLOCK; k++) in[k] = 0.7f;
        for (int k = 0; k < (int)(L.len / BLOCK) + 20; k++) looper_process(&L, in, out, BLOCK);
    }
    looper_command(&L, LOOPER_CMD_REC_DUB);
    voice = -1;
    run(RATE / 5);
    uint32_t clipped = 0;
    begun = looper_save_begin(&L, &mid);
    int16_t *one = malloc(sizeof(int16_t) * 2 * L.len);
    if (begun) looper_save_read(&L, &mid, 0, mid.len, one, &clipped);
    CHECK(begun && clipped > 0 && one[2 * (mid.len / 2)] == 32767, "a loop over full scale: clipped in the copy (%u samples counted)", clipped);
    free(one);
}

static void test_changes(void)
{
    printf("== what makes a copy under way fail\n");
    static int16_t mem[RATE * 2 * 4];
    looper_init(&L, mem, RATE * 2, RATE);
    looper_set_latency(&L, LAT);
    voice = 0;
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(RATE / 2);
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(BLOCK);
    voice = -1;
    run(RATE / 10);
    looper_save_t s;
    CHECK(looper_save_begin(&L, &s), "a copy begins on a playing loop");
    run(RATE);
    CHECK(looper_save_same(&L, &s), "... the loop playing on changes nothing");
    looper_command(&L, LOOPER_CMD_PLAY_STOP);
    run(RATE / 10);
    looper_command(&L, LOOPER_CMD_PLAY_STOP);
    run(RATE / 10);
    CHECK(looper_save_same(&L, &s) && L.st == LOOPER_PLAY, "... nor stop and play again");
    const struct { looper_cmd_t cmd; const char *what; } CHANGES[] = {
        { LOOPER_CMD_REC_DUB, "an overdub" }, { LOOPER_CMD_UNDO, "undo" }, { LOOPER_CMD_REDO, "redo" }, { LOOPER_CMD_CLEAR, "clear" },
    };
    for (int i = 0; i < 4; i++) {
        if (CHANGES[i].cmd == LOOPER_CMD_CLEAR) CHECK(looper_save_begin(&L, &s), "(a copy begins again)");
        else if (!looper_save_begin(&L, &s)) { run(RATE / 5); CHECK(looper_save_begin(&L, &s), "(a copy begins again)"); }
        looper_command(&L, CHANGES[i].cmd);
        run(BLOCK);
        CHECK(!looper_save_same(&L, &s), "%s makes it fail", CHANGES[i].what);
        if (CHANGES[i].cmd == LOOPER_CMD_REC_DUB) { voice = 1; run(RATE / 4); looper_command(&L, LOOPER_CMD_REC_DUB); voice = -1; run(RATE / 5); }
    }

    printf("== ... with two parts\n");
    static int16_t pm[RATE * 2 * 8], pre[LOOP_PRE * 2];
    static loop_parts_t P;
    loop_parts_init(&P, pm, RATE * 2, RATE, pre);
    float in[2 * BLOCK], out[2 * BLOCK];
    #define RUN_P(frames) for (uint32_t _k = 0; _k < (frames); _k += BLOCK) { for (int i = 0; i < 2 * BLOCK; i++) in[i] = voice < 0 ? 0 : sig(T + i / 2, i & 1); loop_parts_process(&P, in, out, BLOCK); T += BLOCK; }
    voice = 0;
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    RUN_P(RATE / 2);
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    RUN_P(BLOCK);
    voice = -1;
    RUN_P(RATE / 10);
    looper_save_t a;
    CHECK(looper_save_begin(&P.part[0], &a), "a copy of part A begins");
    loop_parts_pause(&P);
    RUN_P(RATE / 5);
    loop_parts_pause(&P);
    RUN_P(RATE / 5);
    CHECK(looper_save_same(&P.part[0], &a), "... pause and go on change nothing");
    loop_parts_choose(&P, 1);   // B (empty) records at A's end
    voice = 1;
    RUN_P(RATE);
    loop_parts_info_t info;
    loop_parts_info(&P, &info);
    CHECK(info.active == 1 && looper_save_same(&P.part[0], &a), "... nor B taking over and recording (A left as it was)");
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);   // B fixed
    voice = -1;
    RUN_P(RATE / 5);
    looper_save_t b;
    CHECK(looper_save_begin(&P.part[1], &b), "a copy of part B begins once B plays");
    loop_parts_choose(&P, 0);   // back to A at B's end, then an overdub on A
    RUN_P(RATE + RATE / 2);
    loop_parts_info(&P, &info);
    CHECK(info.active == 0 && looper_save_same(&P.part[0], &a) && looper_save_same(&P.part[1], &b), "... back to A: both copies still good");
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    voice = 0;
    RUN_P(RATE / 4);
    CHECK(!looper_save_same(&P.part[0], &a) && looper_save_same(&P.part[1], &b), "an overdub on A makes A's copy fail, B's stays");
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    voice = -1;
    RUN_P(RATE / 5);
}

// ---- a copy in pieces between the renders: random commands meanwhile ----

static uint32_t rnd_state = 12345;
static uint32_t rnd(uint32_t n) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 8) % n; }

static void test_interleaved(void)
{
    printf("== copies in pieces between the renders, random commands\n");
    static int16_t mem[RATE * 2 * 4];
    looper_init(&L, mem, RATE * 2, RATE);
    looper_set_latency(&L, LAT);
    voice = 0;
    looper_command(&L, LOOPER_CMD_REC_DUB);
    run(RATE / 3);
    looper_command(&L, LOOPER_CMD_REC_DUB);
    int whole = 0, failed = 0, wrong = 0, missed = 0;
    int16_t *c = malloc(sizeof(int16_t) * 2 * RATE * 2), *now = malloc(sizeof(int16_t) * 2 * RATE * 2);
    for (int round = 0; round < 400; round++) {
        looper_save_t s;
        while (!looper_save_begin(&L, &s)) {   // (a loop there, nothing being written)
            voice = 0;
            if (L.st == LOOPER_EMPTY) { looper_command(&L, LOOPER_CMD_REC_DUB); run(RATE / 4); }
            if (L.st == LOOPER_REC || L.st == LOOPER_DUB) looper_command(&L, LOOPER_CMD_REC_DUB);
            run(4 * BLOCK);
        }
        bool same = true, changed = false;
        uint32_t clipped = 0, e0 = looper_edit_count(&L);
        for (uint32_t from = 0; from < s.len && same; from += 600) {
            uint32_t n = s.len - from < 600 ? s.len - from : 600;
            looper_save_read(&L, &s, from, n, c + 2 * from, &clipped);
            same = looper_save_same(&L, &s);
            if (rnd(40) == 0) {   // now and then a command between two pieces
                static const looper_cmd_t CMDS[] = { LOOPER_CMD_REC_DUB, LOOPER_CMD_PLAY_STOP, LOOPER_CMD_UNDO, LOOPER_CMD_REDO, LOOPER_CMD_CLEAR };
                looper_cmd_t cmd = CMDS[rnd(5)];
                voice = (int)rnd(3);
                looper_command(&L, cmd);
            }
            run(BLOCK);
            changed |= looper_edit_count(&L) != e0;
        }
        if (!same || looper_edit_count(&L) != s.edit) { failed++; if (!changed) missed++; continue; }
        whole++;
        uint32_t cl2 = 0;
        looper_save_t s2;   // a new copy now: the same count says the same loop, so it must be the same
        if (looper_save_begin(&L, &s2) && s2.edit == s.edit) {
            looper_save_read(&L, &s2, 0, s2.len, now, &cl2);
            if (s2.len != s.len || memcmp(c, now, sizeof(int16_t) * 2 * s.len)) wrong++;
        }
        if (rnd(3) == 0) {   // now and then an overdub between two copies
            voice = 0;
            looper_command(&L, LOOPER_CMD_REC_DUB);
            run(RATE / 8);
            looper_command(&L, LOOPER_CMD_REC_DUB);
        }
    }
    free(c);
    free(now);
    CHECK(whole > 100 && failed > 20 && wrong == 0 && missed == 0,
          "%d copies whole and equal to the loop, %d thrown away after a change (%d wrong, %d failed without a change)", whole, failed, wrong, missed);
}

// ---- a copy by a thread of its own, against an audio thread ----

static loop_parts_t TP;
static atomic_int t_stop, t_hold, t_held;   // hold: the audio thread waits between two renders (held = it does)
static atomic_uint t_renders;

static void *audio_thread(void *arg)
{
    (void)arg;
    float in[2 * BLOCK], out[2 * BLOCK];
    uint32_t t = 0, r = 777;
    while (!atomic_load(&t_stop)) {
        if (atomic_load(&t_hold)) { atomic_store(&t_held, 1); while (atomic_load(&t_hold) && !atomic_load(&t_stop)) {} atomic_store(&t_held, 0); }
        for (int i = 0; i < BLOCK; i++) in[2 * i] = sig(t + i, (int)(r % 4)), in[2 * i + 1] = sig(t + i, 5);
        loop_parts_process(&TP, in, out, BLOCK);
        t += BLOCK;
        atomic_fetch_add(&t_renders, 1);
        r = r * 1103515245u + 12345u;
        uint32_t x = (r >> 8) % 4000;   // now and then a command, as the footswitches would
        static const looper_cmd_t CMDS[] = { LOOPER_CMD_REC_DUB, LOOPER_CMD_PLAY_STOP, LOOPER_CMD_UNDO, LOOPER_CMD_REDO, LOOPER_CMD_CLEAR };
        if (x < 5) loop_parts_command(&TP, CMDS[x]);
        else if (x < 7) loop_parts_choose(&TP, (int)(x - 5));
        else if (x == 7) loop_parts_pause(&TP);
        // (a part that is empty gets a take soon: the copies need loops)
        if (x == 8 && TP.part[TP.active].st == LOOPER_EMPTY) loop_parts_command(&TP, LOOPER_CMD_REC_DUB);
        if (x == 9 && TP.part[TP.active].st == LOOPER_REC) loop_parts_command(&TP, LOOPER_CMD_REC_DUB);
        if ((t / BLOCK) % 64 == 0) usleep(50);   // (about real time's pace in bursts: other threads get the core)
    }
    return NULL;
}

static void test_threads(void)
{
    printf("== a copy by a thread of its own against a running audio thread\n");
    static int16_t mem[RATE * 2 * 8], pre[LOOP_PRE * 2];
    loop_parts_init(&TP, mem, RATE * 2, RATE, pre);
    pthread_t th;
    pthread_create(&th, NULL, audio_thread, NULL);
    int whole = 0, failed = 0, wrong = 0, checked = 0;
    int16_t *c = malloc(sizeof(int16_t) * 2 * RATE * 2), *now = malloc(sizeof(int16_t) * 2 * RATE * 2);
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < 3) {
        for (int k = 0; k < 2; k++) {
            looper_t *lp = &TP.part[k];
            looper_save_t s;
            if (!looper_save_begin(lp, &s)) continue;
            bool same = true;
            uint32_t clipped = 0;
            for (uint32_t from = 0; from < s.len && same; from += 2048) {
                uint32_t n = s.len - from < 2048 ? s.len - from : 2048;
                looper_save_read(lp, &s, from, n, c + 2 * from, &clipped);
                same = looper_save_same(lp, &s);
            }
            if (!same) { failed++; continue; }
            whole++;
            // hold the audio between two renders: if the loop is still the one copied, the copy must be it exactly
            atomic_store(&t_hold, 1);
            while (!atomic_load(&t_held)) {}
            looper_save_t s2;   // (a new copy of the held loop: the same count must be the same loop)
            if (looper_save_begin(lp, &s2) && s2.edit == s.edit) {
                looper_save_read(lp, &s2, 0, s2.len, now, &clipped);
                checked++;
                if (s2.len != s.len || memcmp(c, now, sizeof(int16_t) * 2 * s.len)) wrong++;
            }
            atomic_store(&t_hold, 0);
        }
    }
    atomic_store(&t_stop, 1);
    pthread_join(th, NULL);
    free(c);
    free(now);
    CHECK(whole > 20 && wrong == 0 && checked > 10,
          "%d copies whole (%d checked against the held loop, %d wrong), %d thrown away after a change; %u renders meanwhile",
          whole, checked, wrong, failed, atomic_load(&t_renders));
}

// ---- the file ----

static void test_wav(void)
{
    printf("== the WAV file\n");
    uint8_t h[LOOP_WAV_HEAD];
    wav_info_t wi;
    loop_wav_header(h, 96000, RATE, 0, 0, 4);
    CHECK(wav_parse(h, sizeof(h), &wi) && wi.rate == RATE && wi.channels == 2 && wi.bits == 16 && !wi.is_float &&
          wi.data_offset == LOOP_WAV_HEAD && wi.frames == 96000, "header without a tempo: the board's reader takes it (samples at %u, %u frames)", wi.data_offset, wi.frames);
    uint32_t riff = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    CHECK(riff == LOOP_WAV_HEAD - 8 + 96000 * 4, "... RIFF size %u", riff);
    loop_wav_header(h, 192000, RATE, 120, 16, 4);
    bool acid = false;
    float tempo = 0;
    uint32_t beats = 0;
    for (size_t p = 12; p + 8 <= sizeof(h);) {
        uint32_t size = h[p + 4] | h[p + 5] << 8 | h[p + 6] << 16 | (uint32_t)h[p + 7] << 24;
        if (!memcmp(h + p, "acid", 4) && size == 24) {
            acid = true;
            memcpy(&beats, h + p + 20, 4);
            memcpy(&tempo, h + p + 28, 4);
        }
        p += 8 + size;
    }
    CHECK(wav_parse(h, sizeof(h), &wi) && wi.data_offset == LOOP_WAV_HEAD && wi.frames == 192000 && acid && tempo == 120 && beats == 16,
          "header with tempo: an acid chunk (%.1f BPM, %u beats), samples still at %u", tempo, beats, wi.data_offset);

    printf("== the file names\n");
    char n[LOOP_WAV_NAME_MAX];
    loop_name_t l = { .number = 7, .part = 0, .bars = 4, .bpm = 120.0004f, .seconds = 8 };
    loop_wav_name(n, sizeof(n), &l);
    CHECK(!strcmp(n, "Loop 0007 A, 120 BPM, 4 bars.wav"), "\"%s\"", n);
    l = (loop_name_t){ .number = 12, .part = 1, .bars = 1, .bpm = 92.5f };
    loop_wav_name(n, sizeof(n), &l);
    CHECK(!strcmp(n, "Loop 0012 B, 92.5 BPM, 1 bar.wav"), "\"%s\"", n);
    l = (loop_name_t){ .number = 3, .part = 1, .seconds = 8.34f, .dated = true, .year = 2026, .month = 11, .day = 28, .hour = 9, .minute = 5 };
    loop_wav_name(n, sizeof(n), &l);
    CHECK(!strcmp(n, "Loop 0003 B, 8.3 s, 2026-11-28 09.05.wav"), "\"%s\"", n);
    CHECK(loop_wav_number(n) == 3 && loop_wav_name_ok(n), "... number %u, a name the board serves", loop_wav_number(n));
    l = (loop_name_t){ .number = 123456, .part = 0, .bars = 99, .bpm = 300, .dated = true, .year = 2026, .month = 12, .day = 31, .hour = 23, .minute = 59 };
    size_t k = loop_wav_name(n, sizeof(n), &l);
    CHECK(k < LOOP_WAV_NAME_MAX && loop_wav_name_ok(n) && loop_wav_number(n) == 123456, "the longest: \"%s\" (%zu bytes)", n, k);
    static const char *const BAD[] = { "../Loop 0001 A.wav", "Loop 0001 A/../x.wav", ".Loop 0001.wav", "Loop x.wav", "Loop 0001 A.mp3",
                                       "loop 0001 A.wav", "Loop 0001 A\x01.wav", "Loop 0001 A..wav", "Loop 0001 A?.wav", "Loop 0001 A:.wav",
                                       "Loop 0001 A, 2026-11-28 \xC3\xA4.wav", "x" };
    int refused = 0;
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) refused += !loop_wav_name_ok(BAD[i]);
    CHECK(refused == (int)(sizeof(BAD) / sizeof(BAD[0])), "%d of %zu names that are no loop's refused", refused, sizeof(BAD) / sizeof(BAD[0]));
}

int main(void)
{
    test_copies();
    test_changes();
    test_interleaved();
    test_threads();
    test_wav();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
