// The board's loudness against the Jam Player's (JavaScript, test/loudness_check.js): this program writes test
// signals as raw 16-bit files and its own results as JSON into the folder given; the script measures the same files
// with the code from jam-player/index.html and compares.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loudness.h"

typedef struct { const char *name; uint32_t rate; int mono; int seconds; } test_signal_t;

static uint32_t rnd = 12345;
static double noise(void)
{
    rnd = rnd * 1664525u + 1013904223u;
    return ((rnd >> 8) & 0xFFFF) / 32768.0 - 1.0;
}

// music-like: a bass line (80 Hz), chords (noise through a crude low pass), pauses, a loud part
static void make(const test_signal_t *s, int16_t *out, uint32_t frames)
{
    double lp = 0;
    for (uint32_t i = 0; i < frames; i++) {
        double t = (double)i / s->rate;
        double env = (fmod(t, 4.0) < 0.3) ? 0.0 : (t > s->seconds * 0.6 ? 1.0 : 0.4);   // a pause every 4 s, louder at the end
        double nz = noise();
        lp += 0.05 * (nz - lp);
        if (s->name[0] == 'r') lp = 0.3 * (nz - lp);   // "rock": bright, little bass
        double bass = (s->name[0] == 'b' ? 0.5 : s->name[0] == 'r' ? 0.02 : 0.15) * sin(2 * M_PI * 80 * t);
        double l = env * (bass + 0.6 * lp + 0.05 * sin(2 * M_PI * 440 * t));
        double r = s->mono ? l : env * (bass + 0.6 * lp * 0.8 + 0.05 * sin(2 * M_PI * 660 * t));
        out[2 * i] = (int16_t)lrint(fmax(-1, fmin(1, l)) * 32767);
        out[2 * i + 1] = (int16_t)lrint(fmax(-1, fmin(1, r)) * 32767);
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const test_signal_t sigs[] = { { "music44", 44100, 0, 30 }, { "bassy48", 48000, 0, 25 }, { "mono44", 44100, 1, 20 }, { "rock44", 44100, 0, 20 } };
    char path[512];
    snprintf(path, sizeof(path), "%s/board.json", dir);
    FILE *js = fopen(path, "w");
    fprintf(js, "[");
    for (size_t k = 0; k < sizeof(sigs) / sizeof(sigs[0]); k++) {
        const test_signal_t *s = &sigs[k];
        uint32_t frames = s->rate * s->seconds;
        int16_t *pcm = malloc(sizeof(int16_t) * 2 * frames);
        make(s, pcm, frames);
        snprintf(path, sizeof(path), "%s/%s.raw", dir, s->name);
        FILE *f = fopen(path, "wb");
        fwrite(pcm, sizeof(int16_t), 2 * (size_t)frames, f);
        fclose(f);
        uint32_t nb = s->seconds * 10 + 10;
        double *e = calloc(nb, sizeof(double)), *nbs = calloc(nb, sizeof(double));
        loudness_t m;
        loudness_init(&m, s->rate, e, nbs, nb);
        for (uint32_t done = 0; done < frames; done += 4096)   // in chunks, as the board reads
            loudness_feed(&m, pcm + 2 * done, frames - done < 4096 ? frames - done : 4096);
        loudness_result_t r = loudness_result(&m);
        fprintf(js, "%s{\"name\":\"%s\",\"rate\":%u,\"mono\":%d,\"lufs\":%.4f,\"play\":%.4f,\"peak\":%.6f,\"gain\":%.4f}",
                k ? "," : "", s->name, s->rate, s->mono, r.lufs, r.play, r.peak, loudness_gain_db(r, LOUD_TARGET_DB, 0));
        printf("  board  %-8s lufs %7.3f  play %7.3f  peak %.4f\n", s->name, r.lufs, r.play, r.peak);
        free(pcm);
        free(e);
        free(nbs);
    }
    fprintf(js, "]\n");
    fclose(js);
    return 0;
}
