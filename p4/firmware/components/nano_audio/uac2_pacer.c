// Frames per microframe for the Nano's playback (see uac2_pacer.h).
#include "uac2_pacer.h"

#include <string.h>

#define WINDOW_PACKETS 8000   // implicit feedback: average over ~1 s of capture packets

void uac2_pacer_init(uac2_pacer_t *p, uint32_t rate, uint32_t microframes_per_s, int max_frames)
{
    memset(p, 0, sizeof(*p));
    p->nominal_q16 = (uint32_t)(((uint64_t)rate << 16) / microframes_per_s);
    p->rate_q16 = p->nominal_q16;
    p->max_frames = max_frames;
}

static bool plausible(uac2_pacer_t *p, uint32_t v)
{
    uint32_t tol = p->nominal_q16 / 100;
    return v + tol >= p->nominal_q16 && v <= p->nominal_q16 + tol;
}

void uac2_pacer_feedback(uac2_pacer_t *p, uint32_t value_q16)
{
    if (!plausible(p, value_q16)) {
        p->rejected++;
        return;
    }
    p->explicit_fb = true;
    p->rate_q16 = value_q16;
}

void uac2_pacer_capture_packet(uac2_pacer_t *p, int frames)
{
    if (p->explicit_fb || frames < 0) return;
    if (frames <= p->max_frames) {
        if (p->sw - p->sr >= UAC2_PACER_RING) {   // nobody takes them: keep the newest
            p->sr++;
            p->mirror_dropped++;
        }
        p->sizes[p->sw++ % UAC2_PACER_RING] = (uint8_t)frames;
        if (!p->mirroring && p->sw - p->sr >= UAC2_PACER_LAG) p->mirroring = true;
    }
    p->cap_frames += (uint64_t)frames;
    p->cap_packets++;
    if (p->cap_packets >= 1000) {   // enough to say something
        uint32_t v = (uint32_t)((p->cap_frames << 16) / p->cap_packets);
        if (plausible(p, v)) p->rate_q16 = v;
        else p->rejected++;
    }
    if (p->cap_packets >= WINDOW_PACKETS) {   // keep half of the window, so the average can follow slowly
        p->cap_frames /= 2;
        p->cap_packets /= 2;
    }
}

int uac2_pacer_next(uac2_pacer_t *p)
{
    if (p->mirroring) {
        if (p->sw != p->sr) return p->sizes[p->sr++ % UAC2_PACER_RING];
        p->mirror_short++;   // (the capture is late: the averaged rate for this one)
    }
    p->acc += p->rate_q16;
    int n = (int)(p->acc >> 16);
    p->acc &= 0xFFFF;
    if (n > p->max_frames) {   // never more than the endpoint takes; the rest goes into the next microframe
        p->acc += (uint32_t)(n - p->max_frames) << 16;
        n = p->max_frames;
    }
    return n;
}
