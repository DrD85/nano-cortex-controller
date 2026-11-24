// WAV files for the jam player: reads the header (RIFF / WAVE, PCM 16 / 24 / 32 bit or 32-bit float, mono or
// stereo, any sample rate) and converts sample data to 16-bit stereo. Plain C, tested on the computer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t rate;
    uint16_t channels;          // 1 or 2
    uint16_t bits;              // 16, 24 or 32
    bool is_float;              // 32-bit float
    uint16_t block_align;       // bytes per frame
    uint32_t data_offset;       // where the samples start in the file
    uint32_t data_bytes;
    uint32_t frames;
} wav_info_t;

// head: the first bytes of the file (4 KB is usually enough; a longer header is refused). false = not usable.
bool wav_parse(const uint8_t *head, size_t n, wav_info_t *out);

// frames of the file's format -> interleaved 16-bit stereo (mono is copied to both sides).
void wav_to_stereo16(const wav_info_t *w, const uint8_t *src, uint32_t frames, int16_t *dst);
