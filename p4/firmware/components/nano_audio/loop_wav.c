// A looper part as a WAV file (see loop_wav.h).
#include "loop_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void putf(uint8_t *p, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}

void loop_wav_header(uint8_t *h, uint32_t frames, uint32_t rate, float bpm, uint32_t beats, uint32_t beats_per_bar)
{
    uint32_t data = frames * 4;
    memset(h, 0, LOOP_WAV_HEAD);
    memcpy(h, "RIFF", 4);
    put32(h + 4, LOOP_WAV_HEAD - 8 + data);
    memcpy(h + 8, "WAVE", 4);
    uint8_t *p = h + 12;
    memcpy(p, "fmt ", 4);
    put32(p + 4, 16);
    put16(p + 8, 1);                // PCM
    put16(p + 10, 2);               // stereo
    put32(p + 12, rate);
    put32(p + 16, rate * 4);
    put16(p + 20, 4);
    put16(p + 22, 16);
    p += 24;
    if (bpm > 0 && beats > 0) {     // (the layout libsndfile reads: flags, root note, two unknown, beats, meter, tempo)
        memcpy(p, "acid", 4);
        put32(p + 4, 24);
        put32(p + 8, 0x04);         // a loop (not a one-shot), stretched to the song's tempo; no root note
        put16(p + 12, 60);
        put16(p + 14, 0x8000);
        putf(p + 16, 0);
        put32(p + 20, beats);
        put16(p + 24, 4);           // the meter: beats_per_bar / 4
        put16(p + 26, beats_per_bar ? beats_per_bar : 4);
        putf(p + 28, bpm);
        p += 32;
    }
    uint8_t *data_head = h + LOOP_WAV_HEAD - 8;
    memcpy(p, "JUNK", 4);
    put32(p + 4, (uint32_t)(data_head - p - 8));
    memcpy(data_head, "data", 4);
    put32(data_head + 4, data);
}

size_t loop_wav_name(char *out, size_t n, const loop_name_t *l)
{
    char what[48];
    if (l->bars) {
        float r = roundf(l->bpm);
        if (fabsf(l->bpm - r) < 0.05f) snprintf(what, sizeof(what), "%.0f BPM, %u %s", (double)r, l->bars, l->bars == 1 ? "bar" : "bars");
        else snprintf(what, sizeof(what), "%.1f BPM, %u %s", (double)l->bpm, l->bars, l->bars == 1 ? "bar" : "bars");
    } else {
        snprintf(what, sizeof(what), "%.1f s", (double)l->seconds);
    }
    char when[24] = "";
    if (l->dated)
        snprintf(when, sizeof(when), ", %04d-%02d-%02d %02d.%02d", l->year % 10000, l->month % 100, l->day % 100, l->hour % 100, l->minute % 100);
    int k = snprintf(out, n, "Loop %04u %c, %s%s.wav", l->number, l->part ? 'B' : 'A', what, when);
    return k < 0 ? 0 : (size_t)k < n ? (size_t)k : n - 1;
}

unsigned loop_wav_number(const char *name)
{
    if (strncmp(name, "Loop ", 5) || name[5] < '0' || name[5] > '9') return 0;
    unsigned long v = strtoul(name + 5, NULL, 10);
    return v > 999999999UL ? 0 : (unsigned)v;
}

bool loop_wav_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n < 10 || n >= LOOP_WAV_NAME_MAX || !loop_wav_number(name) || strcasecmp(name + n - 4, ".wav") || strstr(name, ".."))
        return false;
    for (const char *c = name; *c; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr(" ,.-()", *c);
        if (!ok) return false;
    }
    return true;
}
