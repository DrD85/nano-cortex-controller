// The covers' decoding on the computer (main/p4_cover.c): run.sh makes the pictures and WAVs, this checks them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p4_cover.h"

static int fails;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

static void rgb(uint16_t p, int *r, int *g, int *b) { *r = (p >> 11) << 3; *g = ((p >> 5) & 63) << 2; *b = (p & 31) << 3; }
static int near(uint16_t p, int r, int g, int b)
{
    int pr, pg, pb;
    rgb(p, &pr, &pg, &pb);
    return abs(pr - r) < 40 && abs(pg - g) < 40 && abs(pb - b) < 40;
}
static uint16_t at(const uint16_t *px, int x, int y) { return px[y * P4_COVER_PX + x]; }

static int decode_file(const char *path, uint16_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    int ok = p4_cover_decode(f, 0, n, out);
    fclose(f);
    return ok;
}

// quarters: red top left, green top right, blue bottom left, white bottom right (of the middle square)
static void quarters(const uint16_t *px, const char *what)
{
    char t[160];
    int q = P4_COVER_PX / 4;
    snprintf(t, sizeof(t), "%s: red top left", what); check(near(at(px, q, q), 255, 0, 0), t);
    snprintf(t, sizeof(t), "%s: green top right", what); check(near(at(px, 3 * q, q), 0, 255, 0), t);
    snprintf(t, sizeof(t), "%s: blue bottom left", what); check(near(at(px, q, 3 * q), 0, 0, 255), t);
    snprintf(t, sizeof(t), "%s: white bottom right", what); check(near(at(px, 3 * q, 3 * q), 255, 255, 255), t);
}

int main(int argc, char **argv)
{
    const char *dir = argv[1];
    char p[512];
    static uint16_t px[P4_COVER_PX * P4_COVER_PX];
    snprintf(p, sizeof(p), "%s/square.jpg", dir);
    check(decode_file(p, px), "square 1000 x 1000 baseline JPEG decodes");
    quarters(px, "square");
    snprintf(p, sizeof(p), "%s/wide.jpg", dir);   // 1200 x 600: grey bars left and right of the square
    check(decode_file(p, px), "wide 1200 x 600 decodes");
    quarters(px, "wide (middle square only)");
    snprintf(p, sizeof(p), "%s/small.jpg", dir);
    check(decode_file(p, px), "small 50 x 50 decodes (made larger)");
    quarters(px, "small");
    snprintf(p, sizeof(p), "%s/odd.jpg", dir);   // 4:4:4, odd size
    check(decode_file(p, px), "odd 333 x 333, 4:4:4 decodes");
    quarters(px, "odd");
    snprintf(p, sizeof(p), "%s/progressive.jpg", dir);
    check(!decode_file(p, px), "progressive JPEG refused (not shown)");
    snprintf(p, sizeof(p), "%s/cover.png", dir);
    check(!decode_file(p, px), "PNG refused (not shown)");
    // WAVs with tags
    const char *wavs[][2] = { { "tagged-v3.wav", "ID3v2.3, UTF-16 description, other picture before the front cover" },
                              { "tagged-v4.wav", "ID3v2.4, Latin-1 description" } };
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, wavs[i][0]);
        FILE *f = fopen(p, "rb");
        long o = 0, l = 0;
        int found = f && p4_cover_find_apic(f, &o, &l);
        char t[200];
        snprintf(t, sizeof(t), "%s: APIC found", wavs[i][1]);
        check(found, t);
        snprintf(t, sizeof(t), "%s: front cover decodes", wavs[i][1]);
        check(found && p4_cover_decode(f, o, l, px), t);
        quarters(px, wavs[i][0]);
        if (f) fclose(f);
    }
    snprintf(p, sizeof(p), "%s/plain.wav", dir);
    FILE *f = fopen(p, "rb");
    long o, l;
    check(f && !p4_cover_find_apic(f, &o, &l), "WAV without tags: no cover");
    if (f) fclose(f);
    snprintf(p, sizeof(p), "%s/png-tag.wav", dir);
    f = fopen(p, "rb");
    check(f && !p4_cover_find_apic(f, &o, &l), "WAV whose tag holds a PNG: no cover");
    if (f) fclose(f);
    // where a track's cover comes from (p4_cover_load): beside it, its tags, the album's folder, the jam folder
    char jam[512];
    snprintf(jam, sizeof(jam), "%s/jam", dir);
    static const struct { const char *name, *from; int r, g, b; } places[] = {
        { "Loose", "the folder's picture", 0, 0, 255 },
        { "Blues/Slow", "the album's picture", 0, 255, 0 },        // folder.jpg before artwork.jpeg, before the jam folder's
        { "Blues/Own", "the picture beside it", 255, 0, 0 },
        { "Blues/Tagged", "its tags", -1, 0, 0 },                  // (quarters) before the album's picture
        { "Bare/Song", "the folder's picture", 0, 0, 255 },        // an album without a picture: the jam folder's
        { "Prog/Song", "the album's picture", 255, 0, 255 },       // a progressive cover.jpg passed over for front.jpg
        { "\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84"
          "\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84\xC3\x84/"
          "\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C"
          "\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C"
          "\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C",
          "the picture beside it", 255, 0, 0 },                    // the longest name (60-byte album, 78-byte title)
    };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        char t[200];
        memset(px, 0, sizeof(px));
        const char *from = p4_cover_load(jam, places[i].name, px);
        snprintf(t, sizeof(t), "%.40s: cover from %s", places[i].name, places[i].from);
        check(from && !strcmp(from, places[i].from), t);
        if (places[i].r < 0) quarters(px, places[i].name);
        else {
            snprintf(t, sizeof(t), "%.40s: its colour", places[i].name);
            check(near(at(px, P4_COVER_PX / 2, P4_COVER_PX / 2), places[i].r, places[i].g, places[i].b), t);
        }
    }
    snprintf(p, sizeof(p), "%s/cover.jpg", jam);
    remove(p);
    check(p4_cover_load(jam, "Loose", px) == NULL, "no picture anywhere: none");
    printf("%s (%d failed)\n", fails ? "FAILED" : "all fine", fails);
    return fails != 0;
}
