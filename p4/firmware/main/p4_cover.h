// P4 board: the cover of the jam track that plays (p4_cover.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define P4_COVER_PX 128   // the cover on the screen: square, RGB565

// The picture in a WAV's tags: an "id3 " / "ID3 " chunk with an APIC frame holding a JPEG (the front cover first).
// Its place in the file, or false.
bool p4_cover_find_apic(FILE *f, long *offset, long *length);
// A JPEG at offset / length of the file into P4_COVER_PX x P4_COVER_PX pixels (RGB565): the middle square of the
// picture, averaged down. False for anything that is not a baseline JPEG it can read.
bool p4_cover_decode(FILE *f, long offset, long length, uint16_t *out);
// The cover of track name (relative to dir: "Album/Title" or "Title") into out: beside it (<name>.jpg / .jpeg), its
// WAV's tags, the album folder's picture, the jam folder's picture (cover / folder / front / album / artwork). Where
// it came from, or NULL.
const char *p4_cover_load(const char *dir, const char *name, uint16_t *out);

// ---- on the board ----
void p4_cover_start(void);
uint32_t p4_cover_seq(void);                  // goes up whenever the cover changed (or went away)
void p4_cover_reload(void);                   // a cover came (over Wi-Fi): look for the current track's again
bool p4_cover_copy(uint16_t *dst, uint32_t *seq);   // the current one into dst (P4_COVER_PX^2 pixels); false: none
