// Loudness of a jam track (see loudness.h) - the Jam Player's trackLoudness, step for step.
#include "loudness.h"

#include <math.h>
#include <string.h>

static double lufs_of(double e) { return e > 0 ? -0.691 + 10 * log10(e) : -INFINITY; }

static void k_weighting(double rate, double s[5], double h[5])
{
    double k = tan(M_PI * 1681.974450955533 / rate);
    double q = 0.7071752369554196;
    double vh = pow(10, 3.999843853973347 / 20), vb = pow(vh, 0.4996667741545416);
    double a0 = 1 + k / q + k * k;
    s[0] = (vh + vb * k / q + k * k) / a0;
    s[1] = 2 * (k * k - vh) / a0;
    s[2] = (vh - vb * k / q + k * k) / a0;
    s[3] = 2 * (k * k - 1) / a0;
    s[4] = (1 - k / q + k * k) / a0;
    k = tan(M_PI * 38.13547087602444 / rate);
    q = 0.5003270373238773;
    a0 = 1 + k / q + k * k;
    h[0] = 1;
    h[1] = -2;
    h[2] = 1;
    h[3] = 2 * (k * k - 1) / a0;
    h[4] = (1 - k / q + k * k) / a0;
}

static void high_pass(double rate, double hz, double c[5])
{
    double w = 2 * M_PI * hz / rate, cs = cos(w), alpha = sin(w) / M_SQRT2;
    double a0 = 1 + alpha;
    c[0] = (1 + cs) / 2 / a0;
    c[1] = -(1 + cs) / a0;
    c[2] = (1 + cs) / 2 / a0;
    c[3] = -2 * cs / a0;
    c[4] = (1 - alpha) / a0;
}

void loudness_init(loudness_t *m, uint32_t rate, double *energy, double *no_bass, uint32_t max_blocks)
{
    memset(m, 0, sizeof(*m));
    m->hop = (uint32_t)lround(rate / 10.0);
    m->energy = energy;
    m->no_bass = no_bass;
    m->max_blocks = max_blocks;
    k_weighting(rate, m->s, m->h);
    high_pass(rate, LOUD_BASS_HZ, m->c);
}

void loudness_feed(loudness_t *m, const int16_t *stereo, uint32_t frames)
{
    const double *s = m->s, *h = m->h, *c = m->c;
    for (uint32_t i = 0; i < frames && m->count < m->max_blocks; i++) {
        for (int ch = 0; ch < 2; ch++) {
            double x = stereo[2 * i + ch] / 32768.0;
            double a = x < 0 ? -x : x;
            if (a > m->peak) m->peak = (float)a;
            double *st = m->st[ch];   // (transposed direct form II, as there)
            double y = s[0] * x + st[0];
            st[0] = s[1] * x - s[3] * y + st[1];
            st[1] = s[2] * x - s[4] * y;
            double z = h[0] * y + st[2];
            st[2] = h[1] * y - h[3] * z + st[3];
            st[3] = h[2] * y - h[4] * z;
            m->sum += z * z;
            double w = c[0] * z + st[4];
            st[4] = c[1] * z - c[3] * w + st[5];
            st[5] = c[2] * z - c[4] * w;
            m->cut += w * w;
        }
        if (++m->fill == m->hop) {
            m->energy[m->count] = m->sum / m->hop;
            m->no_bass[m->count] = m->cut / m->hop;
            m->count++;
            m->fill = 0;
            m->sum = m->cut = 0;
        }
    }
}

// Gated as there: windows of up to four blocks (the first ones shorter), -70 LUFS, then 10 LU below the mean.
static double gated(const double *blocks, uint32_t n)
{
    double sum_kept = 0;
    uint32_t kept = 0;
    for (int pass = 0; pass < 2; pass++) {
        double gate = pass == 0 ? -70 : lufs_of(sum_kept / kept) - 10;
        double sum = 0;
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t from = i >= 3 ? i - 3 : 0;
            double e = 0;
            for (uint32_t j = from; j <= i; j++) e += blocks[j];
            e /= (i - from + 1);
            if (lufs_of(e) > gate) {
                sum += e;
                k++;
            }
        }
        if (!k) return -INFINITY;
        sum_kept = sum;
        kept = k;
    }
    return lufs_of(sum_kept / kept);
}

loudness_result_t loudness_result(const loudness_t *m)
{
    double lufs = gated(m->energy, m->count);
    double play = gated(m->no_bass, m->count) + LOUD_BASS_USUAL_DB;
    if (play < lufs - LOUD_BASS_MOST_DB) play = lufs - LOUD_BASS_MOST_DB;
    loudness_result_t r = { (float)lufs, (float)play, m->peak };
    return r;
}

float loudness_gain_db(loudness_result_t r, float target, float trim)
{
    if (!isfinite(r.play)) return trim;
    float wanted = target + trim - r.play;
    float most = LOUD_CEILING_DB - (r.peak > 0 ? 20 * log10f(r.peak) : 0);
    return wanted < most ? wanted : most;
}
