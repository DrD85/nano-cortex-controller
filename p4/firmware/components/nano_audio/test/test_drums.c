// The drum machine on the computer: its sounds against the Duo DAW's own (a file of float samples written by
// test/drumkit_ref.swift from the DAW's DrumKit.swift, when given), the timing of the hits, swing, levels.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drum_machine.h"

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        printf((cond) ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__);         \
        printf("\n"); if (!(cond)) failures++;                                 \
    } while (0)

#define RATE 48000

static int find_groove(const char *name)
{
    for (int i = 0; i < DRUM_GROOVE_COUNT; i++)
        if (!strcmp(DRUM_GROOVES[i].name, name)) return i;
    return -1;
}

int main(int argc, char **argv)
{
    printf("== drum kit\n");
    static float s[RATE * 5 * 2];
    if (argc > 1) {   // the DAW's sounds: per pad "<frames>" then frames*2 floats, in pad order
        FILE *f = fopen(argv[1], "rb");
        if (!f) { printf("  FAIL  %s not readable\n", argv[1]); return 1; }
        static const char *const NAMES[DRUM_PADS] = { "kick", "snare", "clap", "hat", "open hat", "low tom", "high tom", "ride", "crash" };
        static float ref[RATE * 5 * 2];
        for (int p = 0; p < DRUM_PADS; p++) {
            int32_t n = 0;
            if (fread(&n, 4, 1, f) != 1 || n <= 0 || n > RATE * 5 || fread(ref, sizeof(float), 2 * (size_t)n, f) != 2 * (size_t)n) {
                printf("  FAIL  reference of %s unreadable\n", NAMES[p]);
                failures++;
                break;
            }
            uint32_t m = drum_kit_sound(p, RATE, s, RATE * 5);
            double worst = 0, err = 0, sig = 0;
            for (int i = 0; i < 2 * n && i < 2 * (int)m; i++) {
                worst = fmax(worst, fabs(s[i] - ref[i]));
                err += (s[i] - ref[i]) * (double)(s[i] - ref[i]);
                sig += ref[i] * (double)ref[i];
            }
            double db = 10 * log10(fmax(err, 1e-30) / fmax(sig, 1e-30));   // the difference against the sound
            CHECK((int)m == n && db < -60, "%-8s as the Duo DAW's (%d frames, difference %.0f dB under the sound, largest %.5f)", NAMES[p], n, db, worst);
        }
        fclose(f);
    }

    static drum_machine_t d;
    CHECK(drum_machine_init(&d, RATE, NULL), "kit made");
    for (int p = 0; p < DRUM_PADS; p++) CHECK(d.gain[p] > 0.03f && d.gain[p] < 4.0f, "pad %d level %.2f", p, d.gain[p]);
    CHECK(DRUM_GROOVE_COUNT >= 50, "%d grooves", DRUM_GROOVE_COUNT);

    printf("== drum machine\n");
    static float out[RATE * 4 * 2];
    // Rock at 120 BPM: a step is 6000 frames; the kick on steps 0, 8, 10 - the first hit at frame 0
    int rock = find_groove("Rock");
    drum_machine_set_groove(&d, rock);
    drum_machine_set_bpm(&d, 120);
    drum_machine_set_level_db(&d, 0);
    drum_machine_start(&d);
    memset(out, 0, sizeof(out));
    for (int k = 0; k < RATE * 2; k += 48) drum_machine_render(&d, out + 2 * k, 48);
    // the hat lane "x.x.x.x." - a hit every other step: louder right after frames 12000, 24000, 36000 than before
    int rises = 0;
    for (int h = 1; h <= 3; h++) {
        double before = 0, after = 0;
        for (int i = h * 12000 - 600; i < h * 12000; i++) before = fmax(before, fabs(out[2 * i]));
        for (int i = h * 12000; i < h * 12000 + 200; i++) after = fmax(after, fabs(out[2 * i]));
        if (after > before * 1.5) rises++;
    }
    CHECK(rises == 3 && fabs(out[0]) + fabs(out[2 * 40]) > 0, "Rock at 120 BPM: hits at frames 0, 12000, 24000, 36000 (6000 frames a step)");
    double peak = 0;
    for (int i = 0; i < RATE * 2 * 2; i++) peak = fmax(peak, fabs(out[i]));
    CHECK(peak > 0.2 && peak < 2.5, "Rock: peak %.2f at 0 dB (the mixer's limiter holds it; the default level is lower)", peak);

    // Shuffle (swing 0.5): the off steps half a step late
    drum_machine_stop(&d);
    memset(out, 0, sizeof(out));
    for (int k = 0; k < RATE; k += 48) drum_machine_render(&d, out + 2 * k, 48);   // the old sounds ring out
    drum_machine_set_groove(&d, find_groove("Shuffle"));
    drum_machine_start(&d);
    memset(out, 0, sizeof(out));
    for (int k = 0; k < RATE * 2; k += 48) drum_machine_render(&d, out + 2 * k, 48);
    drum_machine_info_t info;
    drum_machine_info(&d, &info);
    CHECK(info.running && info.groove == find_groove("Shuffle"), "Shuffle running");
    // the hat lane "x.xxx.xxx.xxx.xx": hits on steps 0, 2, 3 ... step 3 (odd) comes at 3.5 steps = 21000 frames
    int found = 0;
    for (int i = 20990; i < 21010; i++) if (fabsf(out[2 * i]) > 0.0f && fabsf(out[2 * (i - 1)]) == 0.0f) found = i;
    double before = 0, after = 0;
    for (int i = 20000; i < 21000; i++) before = fmax(before, fabs(out[2 * i]));
    for (int i = 21000; i < 21200; i++) after = fmax(after, fabs(out[2 * i]));
    CHECK(after > before * 1.5, "Shuffle: the hat on step 3 comes half a step late (frame 21000: %.3f before, %.3f after, %d)", before, after, found);

    // a new groove takes over at the next bar line (Shuffle has played 2 s = one bar; half a bar more first)
    for (int k = 0; k < RATE; k += 48) drum_machine_render(&d, out, 48);
    drum_machine_set_groove(&d, find_groove("Punk"));
    for (int k = 0; k < RATE / 2; k += 48) drum_machine_render(&d, out, 48);
    drum_machine_info(&d, &info);
    CHECK(info.groove == find_groove("Shuffle"), "a new groove waits for the bar line (still Shuffle after 1.5 s)");
    for (int k = 0; k < RATE; k += 48) drum_machine_render(&d, out, 48);
    drum_machine_info(&d, &info);
    CHECK(info.groove == find_groove("Punk"), "... and plays after it (Punk)");
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
