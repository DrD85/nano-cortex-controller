// Tests for the phone-audio path on the computer: resampler with drifting clocks, dropouts, volume, sample format,
// microframe pacing and descriptor reading. Run: components/nano_audio/test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_pipe.h"
#include "uac2_desc.h"
#include "uac2_pacer.h"

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) printf("  ok    ");                                          \
        else { printf("  FAIL  "); failures++; }                               \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0)

#define OUT_RATE 48000
#define CH 8

static double slot_to_float(int32_t s) { return (double)(s >> 8) / 8388607.0; }

// ---- 1. phone at 44.1 kHz whose clock runs `ppm` fast, a gap of gap_ms at 20 s, 60 s in all ----
typedef struct {
    double freq_hz, snr_db, max_step;
    audio_pipe_stats_t st;
    int min_fill, max_fill, gap_silent;
} run_result_t;

static run_result_t run_stream(double ppm, int gap_ms, int seconds, uint32_t in_rate)
{
    static int16_t ring[4096 * 2];
    audio_pipe_t p;
    audio_pipe_init(&p, ring, 4096, in_rate, OUT_RATE);

    const double f0 = 1000.0, amp = 0.5;
    double in_phase = 0, prod_clock = 0;                 // producer samples owed
    const double prod_per_ms = in_rate * (1 + ppm * 1e-6) / 1000.0;
    int16_t block[256 * 2];
    int pending = 0;

    size_t total = (size_t)seconds * OUT_RATE;
    float *mono = malloc(sizeof(float) * total);
    int32_t frame[48 * CH];
    run_result_t res = { 0 };
    res.min_fill = 1 << 30;
    size_t n = 0;
    int gap_from = 20000, gap_to = 20000 + gap_ms;
    for (int ms = 0; ms < seconds * 1000; ms++) {
        // the producer delivers whole I2S blocks of 256 frames
        bool gap = ms >= gap_from && ms < gap_to;
        prod_clock += prod_per_ms;
        while (prod_clock >= 1) {
            prod_clock -= 1;
            int16_t v = (int16_t)lrint(sin(in_phase) * amp * 32767);
            in_phase += 2 * M_PI * f0 / in_rate;
            if (gap) continue;   // the phone sends nothing (but its clock goes on)
            block[2 * pending] = v;
            block[2 * pending + 1] = (int16_t)-v;
            if (++pending == 256) {
                audio_pipe_write(&p, block, 256);
                pending = 0;
            }
        }
        audio_pipe_render(&p, frame, 48, CH, 0, 1);
        for (int i = 0; i < 48; i++) {
            mono[n] = (float)slot_to_float(frame[i * CH]);
            if (frame[i * CH + 1] != 0 && fabs(slot_to_float(frame[i * CH + 1]) + mono[n]) > 1e-4)
                res.max_step = 99;   // right channel must be the inverted left
            for (int c = 2; c < CH; c++)
                if (frame[i * CH + c]) res.max_step = 99;   // other channels must stay silent
            n++;
        }
        audio_pipe_stats_t st;
        audio_pipe_stats(&p, &st);
        if (ms > 2000 && !(ms >= gap_from && ms < gap_to + 500)) {
            if ((int)st.fill < res.min_fill) res.min_fill = st.fill;
            if ((int)st.fill > res.max_fill) res.max_fill = st.fill;
        }
        if (gap_ms && ms == gap_from + gap_ms / 2) res.gap_silent = frame[0] == 0;
    }
    audio_pipe_stats(&p, &res.st);

    // largest step between neighbouring samples (a click would be a big one)
    for (size_t i = 1; i < n; i++) {
        double d = fabs(mono[i] - mono[i - 1]);
        if (d > res.max_step) res.max_step = d;
    }
    // frequency over the last 10 s from interpolated rising zero crossings
    size_t from = n - 10 * OUT_RATE;
    double first = -1, last = -1;
    int crossings = 0;
    for (size_t i = from + 1; i < n; i++) {
        if (mono[i - 1] < 0 && mono[i] >= 0) {
            double t = (i - 1) + mono[i - 1] / (mono[i - 1] - mono[i]);
            if (first < 0) first = t;
            last = t;
            crossings++;
        }
    }
    res.freq_hz = (crossings - 1) / ((last - first) / OUT_RATE);
    // quality: least-squares sine fit per block of 480 samples, residual against signal
    double sig = 0, noise = 0;
    double w = 2 * M_PI * res.freq_hz / OUT_RATE;
    for (size_t b = from; b + 480 <= n; b += 480) {
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        for (int i = 0; i < 480; i++) {
            double s = sin(w * i), c = cos(w * i);
            ss += s * s; sc += s * c; cc += c * c; ys += mono[b + i] * s; yc += mono[b + i] * c;
        }
        double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, bb = (yc * ss - ys * sc) / det;
        for (int i = 0; i < 480; i++) {
            double fit = a * sin(w * i) + bb * cos(w * i), y = mono[b + i];
            sig += fit * fit;
            noise += (y - fit) * (y - fit);
        }
    }
    res.snr_db = 10 * log10(sig / noise);
    free(mono);
    return res;
}

static void test_streams(void)
{
    const double natural = 0.5 * 2 * M_PI * 1000.0 / OUT_RATE;   // largest step of the clean sine
    const double ppms[] = { 0, 300, -300, 1500 };
    for (int k = 0; k < 4; k++) {
        double ppm = ppms[k];
        run_result_t r = run_stream(ppm, 0, 60, 44100);
        double want = 1000.0 * (1 + ppm * 1e-6);
        printf("44.1 kHz phone, clock %+.0f ppm: %.3f Hz, %.1f dB, fill %d..%d, correction %d ppm\n", ppm, r.freq_hz,
               r.snr_db, r.min_fill, r.max_fill, (int)r.st.ppm);
        CHECK(fabs(r.freq_hz - want) < 0.02, "pitch follows the phone's clock (%.3f, want %.3f)", r.freq_hz, want);
        CHECK(r.st.underruns == 0 && r.st.dropped == 0, "no dropouts (%u underruns, %u dropped)", r.st.underruns,
              r.st.dropped);
        CHECK(r.snr_db > 60, "clean (%.1f dB)", r.snr_db);
        CHECK(r.max_step < natural * 1.15, "no clicks (largest step %.4f, sine %.4f)", r.max_step, natural);
        CHECK(r.min_fill > 300 && r.max_fill < 2000, "buffer stays around 25 ms (%d..%d frames)", r.min_fill,
              r.max_fill);
    }
    run_result_t r = run_stream(200, 0, 30, 48000);
    printf("48 kHz phone, clock +200 ppm: %.3f Hz, %.1f dB\n", r.freq_hz, r.snr_db);
    CHECK(fabs(r.freq_hz - 1000.2) < 0.02 && r.snr_db > 60 && r.st.underruns == 0, "48 kHz input passes too");

    r = run_stream(100, 150, 40, 44100);
    printf("dropout of 150 ms at 20 s: %u underrun(s), largest step %.4f\n", r.st.underruns, r.max_step);
    CHECK(r.st.underruns == 1, "the dropout is noticed once");
    CHECK(r.gap_silent, "silence during the dropout");
    CHECK(r.max_step < natural * 1.15, "fade out and in without a click (largest step %.4f)", r.max_step);
    CHECK(fabs(r.freq_hz - 1000.1) < 0.02, "back in tune after the dropout (%.3f Hz)", r.freq_hz);
}

// ---- 2. sample format, silence at the start, volume ----
static void test_format_and_gain(void)
{
    static int16_t ring[1024 * 2];
    audio_pipe_t p;
    audio_pipe_init(&p, ring, 1024, 48000, 48000);
    int32_t out[48 * CH];
    audio_pipe_render(&p, out, 48, CH, 2, 3);
    int nonzero = 0;
    for (int i = 0; i < 48 * CH; i++) nonzero += out[i] != 0;
    CHECK(nonzero == 0, "nothing in the buffer: pure silence");

    int16_t dc[960 * 2];
    for (int i = 0; i < 960 * 2; i++) dc[i] = 16384;   // 0.5
    audio_pipe_write(&p, dc, 960);
    for (int k = 0; k < 10; k++) audio_pipe_render(&p, out, 48, CH, 2, 3);   // past the fade-in
    CHECK(out[47 * CH + 2] == 0x40000000, "0.5 is 0x40000000 in a 4-byte slot (got 0x%08X)", (unsigned)out[47 * CH + 2]);
    CHECK(out[47 * CH + 0] == 0 && out[47 * CH + 4] == 0, "phone only on the chosen channels");

    audio_pipe_set_gain_db(&p, -6.0206f);
    audio_pipe_write(&p, dc, 960);
    double prev = 0.5, worst = 0;
    for (int k = 0; k < 20; k++) {
        audio_pipe_render(&p, out, 48, CH, 2, 3);
        for (int i = 0; i < 48; i++) {
            double v = slot_to_float(out[i * CH + 2]);
            if (fabs(v - prev) > worst) worst = fabs(v - prev);
            prev = v;
        }
    }
    CHECK(fabs(prev - 0.25) < 0.001, "-6 dB halves the level (%.4f)", prev);
    CHECK(worst < 0.001, "volume change is ramped (largest step %.5f)", worst);

    audio_pipe_request_rate(&p, 44100);   // the phone switched to 44.1 kHz (asked from the writer's side)
    audio_pipe_render(&p, out, 48, CH, 2, 3);
    CHECK(p.in_rate == 44100 && fabs(p.step - 44100.0 / 48000) < 1e-9, "a new input rate is taken at the next render");
}

// ---- 3. microframe pacing ----
static void test_pacer(void)
{
    uac2_pacer_t p;
    uac2_pacer_init(&p, 48000, 8000, 7);
    long sum = 0;
    int lo = 99, hi = 0;
    for (int i = 0; i < 8000; i++) {
        int n = uac2_pacer_next(&p);
        sum += n;
        if (n < lo) lo = n;
        if (n > hi) hi = n;
    }
    CHECK(sum == 48000 && lo == 6 && hi == 6, "no feedback: 6 frames per microframe (%ld in 1 s)", sum);

    uac2_pacer_init(&p, 48000, 8000, 7);
    uac2_pacer_feedback(&p, (uint32_t)lrint(6.0012 * 65536));   // the Nano runs 200 ppm fast
    sum = 0;
    for (int i = 0; i < 80000; i++) sum += uac2_pacer_next(&p);
    CHECK(labs(sum - 480096) <= 1, "explicit feedback 6.0012: %ld frames in 10 s (want 480096)", sum);
    uac2_pacer_feedback(&p, 9 << 16);
    CHECK(p.rejected == 1 && p.rate_q16 == (uint32_t)lrint(6.0012 * 65536), "an implausible feedback value is ignored");

    uac2_pacer_init(&p, 48000, 8000, 7);
    // implicit: the capture stream carries 6,6,6,...,7 (one extra frame every 1000 packets = +167 ppm)
    for (int i = 0; i < 16000; i++) uac2_pacer_capture_packet(&p, i % 1000 == 999 ? 7 : 6);
    sum = 0;
    for (int i = 0; i < 80000; i++) sum += uac2_pacer_next(&p);
    CHECK(labs(sum - 480080) <= 2, "implicit feedback from capture: %ld frames in 10 s (want 480080)", sum);

    // Packet by packet: five minutes as on the board - transfers of 8 microframes ending in either order, now and
    // then three together (a busy moment), the Nano's clock 60 ppm fast and later 40 ppm slow. The frames sent
    // follow the frames received exactly, whatever the order.
    uac2_pacer_init(&p, 48000, 8000, 7);
    unsigned seed = 99;
    double owed = 0;
    long received = 0, sent = 0, dlo = 1 << 30, dhi = -(1 << 30);
    int queued_play = 0, queued_cap = 0;
    for (int ms = 0; ms < 300000; ms++) {
        double per = ms < 150000 ? 6.00036 : 5.99976;
        seed = seed * 1103515245u + 12345u;
        queued_cap++, queued_play++;
        bool late = (seed >> 20) % 300 == 0 && ms > 10;   // the client task was busy: handled later together
        bool cap_first = (seed >> 16) & 1;
        if (late) continue;
        for (int side = 0; side < 2; side++) {
            if ((side == 0) == cap_first) {
                for (; queued_cap; queued_cap--)
                    for (int k = 0; k < 8; k++) { owed += per; int n = (int)owed; owed -= n; uac2_pacer_capture_packet(&p, n); received += n; }
            } else {
                for (; queued_play; queued_play--)
                    for (int k = 0; k < 8; k++) sent += uac2_pacer_next(&p);
            }
        }
        if (ms > 1000) { long d = received - sent; if (d < dlo) dlo = d; if (d > dhi) dhi = d; }
    }
    CHECK(p.mirroring && p.mirror_short == 0 && dhi - dlo <= 120, "packet by packet: received - sent stays within %ld .. %ld frames in 5 min (%u short)", dlo, dhi, p.mirror_short);
}

// ---- 4. descriptor reading (a made-up UAC 2.0 device shaped like the Nano: 8 ch, 24 in 4, EP 0x03 / 0x82) ----
static void test_descriptor(void)
{
    const uint8_t d[] = {
        9, 2, 0, 0, 5, 1, 0, 0xC0, 0,                              // configuration
        8, 0x0B, 0, 3, 1, 0, 0x20, 0,                              // interface association
        9, 4, 0, 0, 0, 1, 1, 0x20, 0,                              // AudioControl
        9, 0x24, 1, 0, 2, 8, 0, 0, 0,                              // AC header
        8, 0x24, 0x0A, 0x29, 3, 7, 0, 0,                           // clock source 0x29
        9, 4, 1, 0, 0, 1, 2, 0x20, 0,                              // playback, alt 0 (no endpoint)
        9, 4, 1, 1, 1, 1, 2, 0x20, 0,                              // playback, alt 1
        16, 0x24, 1, 2, 0, 1, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0,        // AS general: 8 channels
        6, 0x24, 2, 1, 4, 24,                                      // format: 4-byte slots, 24 bits
        7, 5, 0x03, 0x05, 224, 0, 1,                               // EP 0x03 iso async, 224, every microframe
        8, 0x25, 1, 0, 0, 0, 0, 0,                                 // class endpoint
        9, 4, 2, 0, 0, 1, 2, 0x20, 0,                              // capture, alt 0
        9, 4, 2, 1, 1, 1, 2, 0x20, 0,                              // capture, alt 1
        16, 0x24, 1, 3, 0, 1, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0,
        6, 0x24, 2, 1, 4, 24,
        7, 5, 0x82, 0x25, 224, 0, 1,                               // EP 0x82 iso async, implicit feedback data
        8, 0x25, 1, 0, 0, 0, 0, 0,
        9, 4, 5, 0, 1, 3, 0, 0, 0,                                 // HID
        7, 5, 0x81, 3, 64, 0, 1,
    };
    uac2_stream_t s;
    bool ok = uac2_find_stream(d, sizeof(d), 0x03, &s);
    CHECK(ok && s.interface == 1 && s.alt == 1 && s.mps == 224 && s.interval == 1 && s.sync == 1 && s.usage == 0,
          "playback: interface %d alt %d, %d bytes, interval %d, sync %d", s.interface, s.alt, s.mps, s.interval, s.sync);
    CHECK(s.channels == 8 && s.subslot == 4 && s.bits == 24 && s.fb_ep == 0, "playback format: %d ch, %d bytes, %d bits",
          s.channels, s.subslot, s.bits);
    CHECK(s.ac_interface == 0 && s.clock_id == 0x29, "clock source 0x%02X on interface %d", s.clock_id, s.ac_interface);
    ok = uac2_find_stream(d, sizeof(d), 0x82, &s);
    CHECK(ok && s.interface == 2 && s.usage == 2, "capture: interface %d, usage %d (implicit feedback)", s.interface,
          s.usage);
    CHECK(!uac2_find_stream(d, sizeof(d), 0x04, &s), "an endpoint that is not audio is not found");
    CHECK(!uac2_find_stream(d, 40, 0x03, &s), "a cut-off descriptor is read safely");
}

int main(void)
{
    printf("== streams\n");
    test_streams();
    printf("== format and volume\n");
    test_format_and_gain();
    printf("== pacing\n");
    test_pacer();
    printf("== descriptor\n");
    test_descriptor();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
