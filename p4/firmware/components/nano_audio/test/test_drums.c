// The drum machine on the computer: its sounds against the Duo DAW's own (a file of float samples written by
// test/drumkit_ref.swift from the DAW's DrumKit.swift, when given), the grooves' lanes, the Lo-Fi kit, the timing of
// the hits, swing, levels.
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
    for (int p = 0; p < DRUM_PADS; p++) CHECK(d.computed.gain[p] > 0.03f && d.computed.gain[p] < 4.0f, "pad %d level %.2f", p, d.computed.gain[p]);
    CHECK(DRUM_GROOVE_COUNT >= 50, "%d grooves", DRUM_GROOVE_COUNT);
    // every lane as long as its groove, of ". o x X" only; the drum menu shows ten styles of at most fifteen grooves
    int bad = 0, styles = 0, most = 0;
    const char *style[DRUM_GROOVE_COUNT];
    for (int g = 0; g < DRUM_GROOVE_COUNT; g++) {
        const drum_groove_t *gr = &DRUM_GROOVES[g];
        for (int p = 0; p < DRUM_PADS; p++)
            if (gr->lane[p] && (strlen(gr->lane[p]) != (size_t)gr->steps_per_bar * gr->bars || strspn(gr->lane[p], ".oxX") != strlen(gr->lane[p])))
                bad++, printf("        %s: pad %d\n", gr->name, p);
        int s = 0, n = 0;
        while (s < styles && strcmp(style[s], gr->style)) s++;
        if (s == styles) style[styles++] = gr->style;
        for (int h = 0; h < DRUM_GROOVE_COUNT; h++) n += !strcmp(DRUM_GROOVES[h].style, gr->style);
        if (n > most) most = n;
    }
    CHECK(!bad, "every lane as long as its groove");
    CHECK(styles <= 10 && most <= 15, "%d styles, at most %d grooves in one (the drum menu shows 10 of 15)", styles, most);

    printf("== Lo-Fi kit\n");
    // chosen before it is made: the computed kit plays; made: its own sounds
    drum_machine_set_kit(&d, DRUM_KIT_LOFI);
    drum_machine_render(&d, s, 48);
    drum_machine_hit_now(&d, DRUM_KICK, 0, 1);
    bool computed = false, lofi = false;
    for (int v = 0; v < DRUM_VOICES; v++) computed |= d.voice[v].on && d.voice[v].data == d.computed.sound[DRUM_KICK];
    CHECK(drum_machine_make_lofi(&d, NULL) && drum_machine_make_lofi(&d, NULL), "made (once)");
    drum_machine_hit_now(&d, DRUM_KICK, 0, 1);
    for (int v = 0; v < DRUM_VOICES; v++) lofi |= d.voice[v].on && d.voice[v].data == d.lofi.sound[DRUM_KICK];
    CHECK(computed && lofi, "chosen before it was made the computed kick played, then the Lo-Fi one");
    size_t bytes = 0;
    for (int p = 0; p < DRUM_PADS; p++) {
        bytes += sizeof(int16_t) * 2 * d.lofi.length[p];
        // the sampler's low pass: what lies above 10 kHz (two high passes), against the sound (the computed kit's snare,
        // hats and cymbals: -2 to -10 dB)
        double all = 0, top = 0;
        float z[4][2] = { { 0 } };
        const double w = 2 * M_PI * 10000 / RATE, al = sin(w) / (2 * 0.7071), c = cos(w), a0 = 1 + al;
        const float b0 = (float)((1 + c) / 2 / a0), b1 = -2 * b0, a1 = (float)(-2 * c / a0), a2 = (float)((1 - al) / a0);
        for (uint32_t i = 0; i < d.lofi.length[p]; i++) {
            float x = d.lofi.sound[p][2 * i] / 32768.0f, y = x;
            for (int k = 0; k < 2; k++) {   // (transposed direct form II)
                float o = b0 * y + z[k][0];
                z[k][0] = b1 * y - a1 * o + z[k][1];
                z[k][1] = b0 * y - a2 * o;
                y = o;
            }
            all += x * (double)x, top += y * (double)y;
        }
        double db = 10 * log10(fmax(top, 1e-30) / fmax(all, 1e-30));
        CHECK(d.lofi.gain[p] > 0.03f && d.lofi.gain[p] < 4.0f && db < -18, "pad %d: %4.0f ms, level %.2f, %.0f dB above 10 kHz",
              p, d.lofi.length[p] * 1000.0 / RATE, d.lofi.gain[p], db);
    }
    CHECK(bytes < 1200 * 1024, "%zu KB in all", bytes / 1024);
    drum_machine_set_kit(&d, DRUM_KIT_COMPUTED);
    for (int k = 0; k < RATE; k += 48) drum_machine_render(&d, s, 48);   // (the two kicks ring out)

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
