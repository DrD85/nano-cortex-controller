// Two looper parts with the drums (loop_parts.h) on the computer.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loop_parts.h"

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        printf((cond) ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__);         \
        printf("\n"); if (!(cond)) failures++;                                 \
    } while (0)

#define RATE 48000
#define BLOCK 48

static loop_parts_t P;
static float in[BLOCK * 2], out[BLOCK * 2];
static uint32_t t;            // frames processed
static float level_in = 0;    // what the "guitar" plays now

static void run(uint32_t frames, float *peak)
{
    for (uint32_t k = 0; k < frames; k += BLOCK) {
        for (int i = 0; i < BLOCK; i++) in[2 * i] = in[2 * i + 1] = level_in;
        loop_parts_process(&P, in, out, BLOCK);
        if (peak) for (int i = 0; i < 2 * BLOCK; i++) *peak = fmaxf(*peak, fabsf(out[i]));
        t += BLOCK;
    }
}

static double since_bar_test(drum_machine_t *d) { return fmod(d->step_pos, drum_machine_steps_per_bar(d)) * drum_machine_step_frames(d); }

int main(void)
{
    static int16_t mem[RATE * 2 * 8];   // two parts of 2 s
    static int16_t pre[LOOP_PRE * 2];
    loop_parts_init(&P, mem, RATE * 2, RATE, pre);
    loop_parts_info_t info;

    printf("== loop parts\n");
    // part A: 0.5 s at 0.2 (a constant "guitar"), closed
    level_in = 0.2f;
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    run(RATE / 2, NULL);
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    level_in = 0;
    run(BLOCK, NULL);
    loop_parts_info(&P, &info);
    CHECK(info.active == 0 && info.part[0].state == LOOPER_PLAY && info.part[0].len > 0, "part A recorded and plays (%u frames)", info.part[0].len);
    uint32_t len_a = info.part[0].len;

    // tap B: waits for the end of A, then B records (empty)
    loop_parts_choose(&P, 1);
    run(BLOCK, NULL);
    loop_parts_info(&P, &info);
    CHECK(info.active == 0 && info.next == 1, "tap on B: A plays on, B waits for A's end");
    uint32_t pos = info.part[0].pos, to_end = len_a - pos;
    run(to_end - to_end % BLOCK - BLOCK, NULL);
    loop_parts_info(&P, &info);
    CHECK(info.active == 0, "... still A one block before its end");
    run(2 * BLOCK, NULL);
    loop_parts_info(&P, &info);
    CHECK(info.active == 1 && info.part[1].state == LOOPER_REC, "... then B, recording its first take (A's position %u, B %u)", info.part[0].pos, info.part[1].pos);
    CHECK(info.part[1].pos > 0 && info.part[1].pos <= 2 * BLOCK, "B began right at A's end (%u frames into B after 2 blocks)", info.part[1].pos);

    // B: 0.25 s at 0.4
    level_in = 0.4f;
    run(RATE / 4, NULL);
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    level_in = 0;
    float peak = 0;
    run(RATE / 8, &peak);
    loop_parts_info(&P, &info);
    CHECK(info.active == 1 && info.part[1].state == LOOPER_PLAY && fabsf(peak - 0.4f) < 0.02f, "B plays its own loop (level %.2f, want 0.40)", peak);

    // an overdub on B, then back to A in the middle of it
    level_in = 0.1f;
    loop_parts_command(&P, LOOPER_CMD_REC_DUB);
    run(RATE / 16, NULL);
    loop_parts_choose(&P, 0);
    run(RATE / 2, NULL);
    level_in = 0;
    run(BLOCK * 4, NULL);
    loop_parts_info(&P, &info);
    peak = 0;
    run(RATE / 8, &peak);
    CHECK(info.active == 0 && info.part[1].state == LOOPER_PLAY && fabsf(peak - 0.2f) < 0.02f,
          "back to A at B's end: A from the top (level %.2f, want 0.20), B's overdub stopped (%d)", peak, info.part[1].state);
    CHECK(info.part[1].can_undo, "B keeps its overdub (undo possible on B)");

    // undo works on the part that plays only
    loop_parts_command(&P, LOOPER_CMD_UNDO);
    run(BLOCK, NULL);
    loop_parts_info(&P, &info);
    CHECK(info.part[1].can_undo && !info.part[0].can_redo, "undo while A plays leaves B's overdub alone");

    // pause: silence, the place is kept; again: on from there
    loop_parts_info(&P, &info);
    uint32_t at = info.part[0].pos;
    loop_parts_pause(&P);
    peak = 0;
    run(RATE / 4, &peak);
    loop_parts_info(&P, &info);
    CHECK(info.paused && peak == 0 && info.part[0].pos == at, "pause: silence, A held at frame %u", info.part[0].pos);
    loop_parts_pause(&P);
    peak = 0;
    run(BLOCK * 2, &peak);
    loop_parts_info(&P, &info);
    CHECK(!info.paused && peak > 0.15f && info.part[0].pos >= at + BLOCK, "pause again: A goes on from there (frame %u)", info.part[0].pos);

    printf("== drums with the parts\n");
    static drum_machine_t d;
    CHECK(drum_machine_init(&d, RATE, NULL), "kit made");
    static loop_parts_t Q;
    loop_parts_init(&Q, mem, RATE * 2, RATE, pre);
    loop_parts_set_drums(&Q, &d);
    drum_machine_set_bpm(&d, 120);   // a bar is 2 s = 96000 frames; a step 6000
    loop_parts_set_bar(&Q, 96000, 0);   // the drums' bar for the sync, the looper free
    for (int i = 0; i < BLOCK * 2; i++) in[i] = 0;
    #define BOTH() (loop_parts_process(&Q, in, out, BLOCK), drum_machine_render(&d, out, BLOCK))   // as the mixer does
    for (int k = 0; k < 10; k++) BOTH();
    loop_parts_command(&Q, LOOPER_CMD_REC_DUB);
    BOTH();
    CHECK(d.running && d.step_pos > 0 && d.step_pos < 0.01, "the first take starts the drums with it (step %.4f)", d.step_pos);
    // pause holds the drums too
    loop_parts_pause(&Q);
    for (int k = 0; k < 100; k++) BOTH();
    double held = d.step_pos;
    for (int k = 0; k < 100; k++) BOTH();
    CHECK(d.step_pos == held && d.paused, "pause holds the drums at their step (%.2f)", held);
    loop_parts_pause(&Q);
    for (int k = 0; k < 10; k++) BOTH();
    CHECK(d.step_pos > held && !d.paused, "... and they go on with the loop");

    printf("== count-in\n");
    static loop_parts_t C;
    static drum_machine_t dc;
    drum_machine_init(&dc, RATE, NULL);
    drum_machine_set_bpm(&dc, 120);
    loop_parts_init(&C, mem, RATE * 2, RATE, pre);
    loop_parts_set_drums(&C, &dc);
    loop_parts_set_bar(&C, 96000, 0);
    loop_parts_set_count_in(&C, true);
    static float mix[BLOCK * 2];
    #define ALL() (loop_parts_process(&C, in, out, BLOCK), memset(mix, 0, sizeof(mix)), drum_machine_render(&dc, mix, BLOCK))
    loop_parts_command(&C, LOOPER_CMD_REC_DUB);
    float click = 0;
    uint32_t frames = 0;
    for (; frames < RATE; frames += BLOCK) { ALL(); for (int i = 0; i < 2 * BLOCK; i++) click = fmaxf(click, fabsf(mix[i])); }
    loop_parts_info(&C, &info);
    CHECK(info.counting && info.part[0].state == LOOPER_EMPTY && click > 0.05f, "count-in: after 1 s still counting, clicks heard (%.2f), nothing recorded", click);
    for (; frames < 96000 + 2 * BLOCK; frames += BLOCK) ALL();
    loop_parts_info(&C, &info);
    CHECK(!info.counting && info.part[0].state == LOOPER_REC && info.part[0].pos == 2 * BLOCK,
          "... the first take starts exactly one bar later (%u frames into it after %u)", info.part[0].pos, frames - 96000);
    printf("== on the drums' bar lines\n");
    {
        static loop_parts_t B;
        static drum_machine_t db;
        drum_machine_init(&db, RATE, NULL);
        drum_machine_set_bpm(&db, 160);   // a bar: 72000 frames (the test's memory: 96000)
        loop_parts_init(&B, mem, RATE * 2, RATE, pre);
        looper_set_latency(&B.part[0], 300);
        looper_set_latency(&B.part[1], 300);
        loop_parts_set_drums(&B, &db);
        loop_parts_set_bar(&B, 72000, 72000);   // (with the drums the looper rounds to their bars too)
        #define BR() (loop_parts_process(&B, in, out, BLOCK), memset(mix, 0, sizeof(mix)), drum_machine_render(&db, mix, BLOCK))
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0;
        loop_parts_drums(&B, true);
        for (uint32_t f = 0; f < 72000 + 9000; f += BLOCK) BR();   // an eighth of a bar after the 2nd bar line
        loop_parts_command(&B, LOOPER_CMD_REC_DUB);                  // pressed late
        BR();
        loop_parts_info(&B, &info);
        CHECK(info.part[0].state == LOOPER_REC && fabs(info.part[0].pos - since_bar_test(&db)) <= 1,
              "pressed an eighth of a bar late: the take starts back at the bar line (%u frames in, the drums %.0f past it)", info.part[0].pos, since_bar_test(&db));
        for (uint32_t f = 0; f < 72000 - 9000 - 3000; f += BLOCK) BR();   // closed a little early
        loop_parts_command(&B, LOOPER_CMD_REC_DUB);
        for (uint32_t f = 0; f < 6000; f += BLOCK) BR();
        loop_parts_info(&B, &info);
        CHECK(info.part[0].len == 72000, "... and ends on the bar line: one bar (%u frames)", info.part[0].len);
        // early: a quarter of a bar before a bar line -> waits for it
        loop_parts_command(&B, LOOPER_CMD_CLEAR);
        for (uint32_t f = 0; f < RATE; f += BLOCK) BR();
        while (since_bar_test(&db) < 72000 * 3 / 4) BR();
        loop_parts_command(&B, LOOPER_CMD_REC_DUB);
        BR();
        loop_parts_info(&B, &info);
        CHECK(info.part[0].state == LOOPER_EMPTY && info.counting, "pressed a quarter bar early: waits for the bar line");
        for (uint32_t f = 0; f < 72000 / 4 + BLOCK; f += BLOCK) BR();
        loop_parts_info(&B, &info);
        double since = since_bar_test(&db);
        CHECK(info.part[0].state == LOOPER_REC && fabs(info.part[0].pos - since) <= 1, "... and starts on it (%u frames in, the drums %.0f past their bar line)", info.part[0].pos, since);
    }
    printf("== armed: the first note starts the take\n");
    {
        static loop_parts_t A;
        loop_parts_init(&A, mem, RATE * 2, RATE, pre);
        looper_set_latency(&A.part[0], 200);
        loop_parts_set_arm(&A, true);
        loop_parts_set_quantize(&A, false);
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0.0005f;   // the noise
        loop_parts_command(&A, LOOPER_CMD_REC_DUB);
        for (int k = 0; k < 2000; k++) loop_parts_process(&A, in, out, BLOCK);
        loop_parts_info(&A, &info);
        CHECK(info.armed && info.part[0].state == LOOPER_EMPTY, "armed: the noise does not start it");
        for (int i = 0; i < BLOCK; i++) in[2 * i] = in[2 * i + 1] = i >= 20 ? 0.3f : 0.0005f;   // a note at frame 20
        loop_parts_process(&A, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0.3f;
        for (int k = 0; k < 100; k++) loop_parts_process(&A, in, out, BLOCK);
        loop_parts_info(&A, &info);
        int16_t *base = A.part[0].base;
        int onset = -1;
        for (int i = 0; i < 4000 && onset < 0; i++) if (base[2 * i] > 3000) onset = i;
        CHECK(!info.armed && info.part[0].state == LOOPER_REC && onset == ARM_MARGIN,
              "the note starts the take; it lies %d frames into the loop (want %d: 10 ms of attack before it)", onset, ARM_MARGIN);
    }
    printf("== count-in without the drums: four clicks\n");
    {
        static loop_parts_t K;
        static drum_machine_t dk;
        static float kmix[BLOCK * 2];
        drum_machine_init(&dk, RATE, NULL);
        drum_machine_set_bpm(&dk, 120);                  // a beat: 24000 frames
        loop_parts_init(&K, mem, RATE * 2, RATE, pre);
        loop_parts_set_drums(&K, &dk);
        loop_parts_set_bar(&K, 96000, 0);
        loop_parts_set_count_in(&K, true);
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0;
        loop_parts_command(&K, LOOPER_CMD_REC_DUB);
        float beat_peak[4] = { 0 };
        for (uint32_t f = 0; f < 96000; f += BLOCK) {
            loop_parts_process(&K, in, out, BLOCK);
            memset(kmix, 0, sizeof(kmix));
            drum_machine_render(&dk, kmix, BLOCK);
            uint32_t beat = f / 24000, into = f % 24000;
            if (into < 3000) for (int i = 0; i < 2 * BLOCK; i++) beat_peak[beat] = fmaxf(beat_peak[beat], fabsf(kmix[i]));
        }
        CHECK(beat_peak[0] > 0.05f && beat_peak[1] > 0.03f && beat_peak[2] > 0.03f && beat_peak[3] > 0.03f,
              "a click on each of the four beats (%.2f %.2f %.2f %.2f)", beat_peak[0], beat_peak[1], beat_peak[2], beat_peak[3]);
    }
    printf("== undo of a fresh take\n");
    {
        static loop_parts_t U;
        loop_parts_init(&U, mem, RATE * 2, RATE, pre);
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0.2f;     // part A: 0.5 s at 0.2
        loop_parts_command(&U, LOOPER_CMD_REC_DUB);
        for (uint32_t f = 0; f < RATE / 2; f += BLOCK) loop_parts_process(&U, in, out, BLOCK);
        loop_parts_command(&U, LOOPER_CMD_REC_DUB);
        loop_parts_process(&U, in, out, BLOCK);               // (A closed before B is tapped)
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0.4f;     // part B (after A's end): 0.3 s at 0.4
        loop_parts_choose(&U, 1);
        for (uint32_t f = 0; f < RATE; f += BLOCK) loop_parts_process(&U, in, out, BLOCK);
        loop_parts_info(&U, &info);
        CHECK(info.active == 1 && info.part[1].state == LOOPER_REC, "part B records after A's end");
        loop_parts_command(&U, LOOPER_CMD_REC_DUB);
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0;
        for (uint32_t f = 0; f < RATE / 10; f += BLOCK) loop_parts_process(&U, in, out, BLOCK);
        loop_parts_info(&U, &info);
        CHECK(info.active == 1 && info.part[1].state == LOOPER_PLAY && info.part[1].can_undo, "B plays; undo is offered for its fresh take");
        loop_parts_command(&U, LOOPER_CMD_UNDO);
        float peak = 0;
        for (uint32_t f = 0; f < RATE / 10; f += BLOCK) loop_parts_process(&U, in, out, BLOCK);
        for (uint32_t f = 0; f < RATE / 10; f += BLOCK) { loop_parts_process(&U, in, out, BLOCK); for (int i = 0; i < 2 * BLOCK; i++) peak = fmaxf(peak, out[i]); }
        loop_parts_info(&U, &info);
        CHECK(info.part[1].state == LOOPER_EMPTY && info.active == 0 && info.part[0].state == LOOPER_PLAY,
              "undo: B's take is gone and A plays again (B %d, active %d)", info.part[1].state, info.active);
        CHECK(fabsf(peak - 0.2f) < 0.02f, "... A is what is heard (%.3f)", peak);
        // A alone, fresh: undo empties it (nothing else to play)
        static loop_parts_t V;
        loop_parts_init(&V, mem, RATE * 2, RATE, pre);
        for (int i = 0; i < BLOCK * 2; i++) in[i] = 0.2f;
        loop_parts_command(&V, LOOPER_CMD_REC_DUB);
        for (uint32_t f = 0; f < RATE / 2; f += BLOCK) loop_parts_process(&V, in, out, BLOCK);
        loop_parts_command(&V, LOOPER_CMD_REC_DUB);
        for (uint32_t f = 0; f < RATE / 10; f += BLOCK) loop_parts_process(&V, in, out, BLOCK);
        loop_parts_command(&V, LOOPER_CMD_UNDO);
        for (uint32_t f = 0; f < RATE / 10; f += BLOCK) loop_parts_process(&V, in, out, BLOCK);
        loop_parts_info(&V, &info);
        CHECK(info.part[0].state == LOOPER_EMPTY, "undo after a fresh take on A alone: A is empty");
    }
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
