// A take started back at the drums' bar line (no count-in): the loop holds exactly what was played - also where the
// kept input (copied over several renders) meets the live recording. Renders of 5-7 frames as on the board.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loop_parts.h"
#define RATE 48000
#define LAT 578
static loop_parts_t P; static drum_machine_t D;
static int16_t pre[LOOP_PRE * 2];
static double g(double t) { return t < 0 ? 0 : 0.5 * sin(2 * M_PI * 220.5 * t / RATE) + 0.2 * sin(2 * M_PI * 37.3 * t / RATE); }
int main(int argc, char **argv)
{
    float bpm = argc > 1 ? atof(argv[1]) : 92, late = argc > 2 ? atof(argv[2]) : 0.1f;
    int count_in = argc > 3 ? atoi(argv[3]) : 0;
    uint32_t frames = RATE * 22;
    int16_t *mem = calloc((size_t)frames * 8, sizeof(int16_t));
    drum_machine_init(&D, RATE, NULL); drum_machine_set_bpm(&D, bpm);
    loop_parts_init(&P, mem, frames, RATE, pre);
    looper_set_latency(&P.part[0], LAT); looper_set_latency(&P.part[1], LAT);
    loop_parts_set_drums(&P, &D);
    uint32_t bar = (uint32_t)(48000.0f * 60.0f / bpm * 4);
    loop_parts_set_bar(&P, bar, bar);
    loop_parts_set_quantize(&P, true); loop_parts_set_count_in(&P, count_in); loop_parts_set_drum_sync(&P, true);
    loop_parts_drums(&P, true);
    static float in[16], out[16], dm[16];
    const int sizes[] = { 6, 6, 5, 7, 6, 6, 6, 6 };
    size_t total = (size_t)RATE * 30, t = 0; int si = 0, pressed = 0;
    size_t press1 = (size_t)(bar * (1 + late)), press2 = press1 + 2 * bar;
    long start = -1;
    while (t < total) {
        int n = sizes[si++ % 8];
        if (!pressed && t >= press1) { loop_parts_command(&P, LOOPER_CMD_REC_DUB); pressed = 1; }
        if (pressed == 1 && t >= press2) { loop_parts_command(&P, LOOPER_CMD_REC_DUB); pressed = 2; }
        for (int k = 0; k < n; k++) { double v = g((double)(t + k) - LAT); in[2 * k] = (float)v; in[2 * k + 1] = (float)v; }
        loop_parts_process(&P, in, out, n);
        loop_parts_info_t info; loop_parts_info(&P, &info);
        if (start < 0 && info.part[0].state == LOOPER_REC) start = (long)(t + n) - (long)info.part[0].pos;   // render frame of loop frame 0
        memset(dm, 0, sizeof(dm)); drum_machine_render(&D, dm, n);
        t += n;
    }
    loop_parts_info_t info; loop_parts_info(&P, &info);
    looper_t *lp = &P.part[0];
    uint32_t len = info.part[0].len;
    // base[i] should be g(start + i) (what was played at loop frame i); find the best offset and the worst error
    long best = 0; double best_err = 1e9;
    for (long off = -3000; off <= 3000; off++) {
        double e = 0;
        for (uint32_t i = 1000; i < 20000; i += 37) e = fmax(e, fabs(lp->base[2 * i] / 32767.0 - g((double)start + i + off)));
        if (e < best_err) best_err = e, best = off;
    }
    double worst = 0; uint32_t at = 0;
    for (uint32_t i = 300; i + 300 < len; i++) {   // (the seam's crossfade at both ends left out)
        double e = fabs(lp->base[2 * i] / 32767.0 - g((double)start + i + best));
        if (e > worst) worst = e, at = i;
    }
    int ok = best == 0 && worst < 1e-3 && len > 0;
    printf("  %s  bpm %.0f, pressed %.2f bar %s, count-in %d: loop %u frames, offset %ld, worst error %.4f (loop frame %u)\n",
           ok ? "ok  " : "FAIL", bpm, late < 0 ? -late : late, late < 0 ? "early" : "late", count_in, len, best, worst, at);
    return ok ? 0 : 1;
}
