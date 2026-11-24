// WAV header and sample conversion (see wav.h).
#include "wav.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool wav_parse(const uint8_t *head, size_t n, wav_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (n < 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) return false;
    bool have_fmt = false;
    uint16_t format = 0;
    size_t p = 12;
    while (p + 8 <= n) {
        uint32_t size = rd32(head + p + 4);
        const uint8_t *body = head + p + 8;
        if (!memcmp(head + p, "fmt ", 4)) {
            if (size < 16 || p + 8 + 16 > n) return false;
            format = rd16(body);
            out->channels = rd16(body + 2);
            out->rate = rd32(body + 4);
            out->block_align = rd16(body + 12);
            out->bits = rd16(body + 14);
            if (format == 0xFFFE && size >= 40 && p + 8 + 40 <= n) format = rd16(body + 24);   // EXTENSIBLE
            have_fmt = true;
        } else if (!memcmp(head + p, "data", 4)) {
            if (!have_fmt) return false;
            out->data_offset = (uint32_t)(p + 8);
            out->data_bytes = size;
            break;
        }
        p += 8 + (size_t)size + (size & 1);   // chunks are padded to even sizes
    }
    if (!have_fmt || !out->data_offset) return false;
    if (out->channels < 1 || out->channels > 2 || out->rate < 8000 || out->rate > 192000) return false;
    if (format == 1 && (out->bits == 16 || out->bits == 24 || out->bits == 32)) out->is_float = false;
    else if (format == 3 && out->bits == 32) out->is_float = true;
    else return false;
    if (out->block_align != out->channels * out->bits / 8) return false;
    out->frames = out->data_bytes / out->block_align;
    return out->frames > 0;
}

static inline int16_t from_float(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float s = v * 32767.0f;
    return (int16_t)(s + (s >= 0 ? 0.5f : -0.5f));
}

static inline int16_t sample(const wav_info_t *w, const uint8_t *s)
{
    switch (w->bits) {
    case 16: return (int16_t)rd16(s);
    case 24: {
        int32_t v = (int32_t)(s[0] << 8 | s[1] << 16 | (uint32_t)s[2] << 24) >> 8;   // sign-extended 24 bits
        int32_t r = (v + 128) >> 8;
        return (int16_t)(r > 32767 ? 32767 : r);
    }
    default:
        if (w->is_float) {
            float f;
            memcpy(&f, s, 4);
            return from_float(f);
        }
        return (int16_t)(rd32(s) >> 16);
    }
}

void wav_to_stereo16(const wav_info_t *w, const uint8_t *src, uint32_t frames, int16_t *dst)
{
    int bytes = w->bits / 8;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *f = src + (size_t)i * w->block_align;
        int16_t l = sample(w, f);
        int16_t r = w->channels == 2 ? sample(w, f + bytes) : l;
        dst[2 * i] = l;
        dst[2 * i + 1] = r;
    }
}
