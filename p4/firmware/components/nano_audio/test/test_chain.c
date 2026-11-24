// Whole chain as on the board: capture packets (6 frames) every microframe, handed over 8 at a time per 1 ms
// transfer, now and then held back for a while (the task late), the render per playback packet (6 frames); the
// mixer with its cushion, two looper parts, the drums. The "guitar" is g(capture frame). The loop must hold one
// unbroken stretch of g.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_mix.h"
#include "loop_parts.h"
#define RATE 48000
#ifndef CAPX
#define CAPX 24   // as CAP_XFERS in uac2_host.c
#endif
static double g(double t) { return 0.4 * sin(2 * M_PI * 196.0 * t / RATE) + 0.2 * sin(2 * M_PI * 523.3 * t / RATE + 0.3) + 0.1 * sin(2 * M_PI * 61.7 * t / RATE); }
static audio_mix_t mix; static loop_parts_t P; static drum_machine_t D;
static int16_t pre[LOOP_PRE * 2];
int main(int argc, char **argv)
{
    int stall_ms = argc > 1 ? atoi(argv[1]) : 10, count_in = argc > 2 ? atoi(argv[2]) : 0, drums = argc > 3 ? atoi(argv[3]) : 1;
    float late = argc > 4 ? atof(argv[4]) : 0.15f;
    uint32_t frames = RATE * 22;
    int16_t *mem = calloc((size_t)frames * 8, sizeof(int16_t));
    audio_mix_init(&mix, NULL, NULL, NULL);
    drum_machine_init(&D, RATE, NULL); drum_machine_set_bpm(&D, 92); drum_machine_set_level_db(&D, -15);
    loop_parts_init(&P, mem, frames, RATE, pre);
    uint32_t lat = 578 - 192 + MIX_CUSHION + LIM_AHEAD;
    looper_set_latency(&P.part[0], lat); looper_set_latency(&P.part[1], lat);
    loop_parts_set_drums(&P, &D);
    uint32_t bar = (uint32_t)(48000.0f * 60.0f / 92 * 4);
    loop_parts_set_bar(&P, bar, bar);
    loop_parts_set_quantize(&P, true); loop_parts_set_count_in(&P, count_in); loop_parts_set_drum_sync(&P, true);
    audio_mix_set_parts(&mix, &P); audio_mix_set_drums(&mix, &D);
    if (drums) loop_parts_drums(&P, true);
    static int32_t cap[6 * 8], out[7 * 8];
    uint64_t ct = 0;           // capture frames produced
    int pend_cap = 0, pend_play = 0;   // transfers waiting for the task
    unsigned seed = 11;
    int stall_left = 0;
    uint32_t ms = 0, press1 = (uint32_t)((1 + late) * bar / 48), press2 = press1 + 2 * bar / 48;
    // capture packets are buffered per transfer (8 x 6 frames)
    static int32_t xfer[64][8 * 6 * 8]; int xw = 0, xr = 0;
    for (ms = 0; ms < 20000; ms++) {
        // the Nano: one capture transfer per ms (8 packets)
        for (int k = 0; k < 8 * 6; k++) { double v = g((double)ct++); for (int c = 0; c < 8; c++) xfer[xw % 64][k * 8 + c] = (c == 2 || c == 3) ? audio_to_slot((float)v) : 0; }
        xw++; pend_cap++; pend_play++;
        if (ms == press1) loop_parts_command(&P, LOOPER_CMD_REC_DUB);
        if (ms == press2) loop_parts_command(&P, LOOPER_CMD_REC_DUB);
        seed = seed * 1103515245u + 12345u;
        if (!stall_left && (seed >> 16) % 700 == 0) stall_left = stall_ms;   // the task late now and then
        if (stall_left) { stall_left--; continue; }
        if (pend_cap > CAPX) { pend_cap = CAPX; }   // (only 6 capture transfers wait at the host: older ones are lost)
        // the task handles what waits, in mixed order
        while (pend_cap || pend_play) {
            seed = seed * 1103515245u + 12345u;
            if (pend_cap && (!pend_play || ((seed >> 16) & 1))) {
                xr = xw - pend_cap;
                for (int p = 0; p < 8; p++) audio_mix_capture(&mix, &xfer[xr % 64][p * 6 * 8], 6, 8, 2, 3);
                pend_cap--;
            } else {
                for (int p = 0; p < 8; p++) audio_mix_render(&mix, out, 6, 8, 0, 1);
                pend_play--;
            }
        }
    }
    loop_parts_info_t info; loop_parts_info(&P, &info);
    looper_t *lp = &P.part[0];
    uint32_t len = info.part[0].len;
    audio_mix_stats_t st; audio_mix_get_stats(&mix, &st);
    // the loop's frame 1000 against g: its offset, then the stretches that do not follow on
    long best = 0; double be = 1e9;
    for (long off = 0; off < (long)ct; off += 1) {
        double e = 0;
        for (int i = 0; i < 200; i++) { e += fabs(lp->base[2 * (1000 + i * 3)] / 32767.0 - g((double)off + 1000 + i * 3)); if (e > be) break; }
        if (e < be) be = e, best = off;
        if (off > 30L * RATE) break;
    }
    int bad = 0; long last_off = best; uint32_t first_bad = 0;
    for (uint32_t i = 300; i + 300 < len; i++) {
        if (fabs(lp->base[2 * i] / 32767.0 - g((double)last_off + i)) > 0.01) {
            if (!bad) first_bad = i;
            bad++;
        }
    }
    int ok = bad == 0 && st.capture_short == 0 && len > 0;
    printf("  %s  ", ok ? "ok  " : "FAIL");
    printf("stall %d ms, count-in %d, drums %d: loop %u frames (state %d); mixer short %u dropped %u; frames off the stretch: %d (first at loop frame %u)\n",
           stall_ms, count_in, drums, len, info.part[0].state, st.capture_short, st.capture_dropped, bad, first_bad);
    return ok ? 0 : 1;
}
