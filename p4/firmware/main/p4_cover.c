// P4 board: the cover of the jam track that plays (2026-11-27, the user: "statt der Platte die Coverart").
//
// Looked for as the Jam Player on the Mac does it: a picture named like the track beside it (<name>.jpg / .jpeg),
// else the picture in the file's own tags (an ID3 chunk of the WAV with an APIC frame, the front cover first), else a
// picture of the album's folder, else one of the jam folder (cover / folder / front / album / artwork .jpg / .jpeg).
// Names are relative to the jam folder ("Album/Title", loose "Title", 2026-11-27). JPEG only, baseline (as nearly
// every cover is); a PNG or a progressive JPEG is not shown and the record stays.
// Decoded in software (TJpgDec, main/tjpgd) block by block, each pixel of the middle square added straight into the
// small picture's sums: no large picture is ever held (a few KB of work memory and the sums, 256 KB in PSRAM).
// A task of its own (core 0, lowest priority, a pause every 15 ms) looks every 300 ms whether the track changed; the
// screen copies the result when p4_cover_seq() moved (ui_set_cover_source).
// The decoding part also runs on the computer: tools/test_cover.
#include "p4_cover.h"

#include <stdlib.h>
#include <string.h>

#include "tjpgd/tjpgd.h"

#ifdef P4_COVER_HOST
#define cover_alloc(n) calloc(1, (n))
#define COVER_LOGW(...) ((void)0)
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#define cover_alloc(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define COVER_LOGW(...) ESP_LOGW("p4_cover", __VA_ARGS__)
#endif

#define T P4_COVER_PX
#define WORK_BYTES 8192            // TJpgDec's work memory (R0.03 wants about 3.1 KB)
#define MAX_PIXELS (4096 * 4096)   // larger pictures are not decoded (they would take seconds)
#define PATH_BYTES 320             // the jam folder, "Album/Title" (143 bytes at most) and ".jpeg"; or "Album/artwork.jpeg"

static uint32_t rd_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t rd_sync(const uint8_t *p) { return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

// ---- the picture in the tags ----

// The ID3 tag at start (size bytes of the chunk): the APIC frame's JPEG, the front cover (type 3) before any other.
static bool id3_apic(FILE *f, long start, uint32_t size, long *offset, long *length)
{
    uint8_t h[10];
    if (fseek(f, start, SEEK_SET) || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return false;
    int ver = h[3];
    if (ver != 3 && ver != 4) return false;   // (ID3v2.2 has three-letter frames; not in WAVs seen so far)
    if (h[5] & 0x80) return false;            // unsynchronised as a whole: not handled
    long at = start + 10, end = start + 10 + (long)rd_sync(h + 6);
    if (end > start + (long)size) end = start + (long)size;
    if (h[5] & 0x40) {   // an extended header: its size counts itself in v2.4, not in v2.3
        uint8_t e[4];
        if (fread(e, 1, 4, f) != 4) return false;
        at += ver == 4 ? (long)rd_sync(e) : 4 + (long)rd_be32(e);
    }
    long best = -1, best_len = 0;
    while (at + 10 <= end) {
        uint8_t fh[10];
        if (fseek(f, at, SEEK_SET) || fread(fh, 1, 10, f) != 10 || !fh[0]) break;   // (zeros: the padding)
        long fs = ver == 4 ? (long)rd_sync(fh + 4) : (long)rd_be32(fh + 4), data = at + 10;
        if (fs <= 0 || data + fs > end) break;
        at = data + fs;
        if (memcmp(fh, "APIC", 4)) continue;
        uint8_t fl = fh[9];
        if (ver == 3 ? (fl & 0xC0) : (fl & 0x0E)) continue;   // compressed, encrypted or unsynchronised
        if (fl & (ver == 3 ? 0x20 : 0x40)) { data++; fs--; }   // a group byte
        if (ver == 4 && (fl & 0x01)) { data += 4; fs -= 4; }   // the data length
        uint8_t b[512];
        long n = fs < (long)sizeof(b) ? fs : (long)sizeof(b);
        if (n < 4 || fseek(f, data, SEEK_SET) || fread(b, 1, (size_t)n, f) != (size_t)n) continue;
        int enc = b[0], i = 1;
        while (i < n && b[i]) i++;   // the MIME type (Latin-1, 0-ended)
        i++;
        if (i >= n) continue;
        int type = b[i++];
        if (enc == 1 || enc == 2) {   // the description: UTF-16 ends in two zero bytes
            while (i + 1 < n && (b[i] || b[i + 1])) i += 2;
            i += 2;
        } else {
            while (i < n && b[i]) i++;
            i++;
        }
        if (i + 2 > n || b[i] != 0xFF || b[i + 1] != 0xD8) continue;   // not a JPEG (a PNG, or more text than read)
        if (best < 0 || type == 3) {
            best = data + i;
            best_len = fs - i;
            if (type == 3) break;
        }
    }
    if (best < 0) return false;
    *offset = best;
    *length = best_len;
    return true;
}

bool p4_cover_find_apic(FILE *f, long *offset, long *length)
{
    uint8_t h[12];
    if (fseek(f, 0, SEEK_END)) return false;
    long end = ftell(f);
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    long at = 12;
    while (at + 8 <= end) {   // the chunks, past the (large) data chunk too: the tags are mostly at the end
        uint8_t c[8];
        if (fseek(f, at, SEEK_SET) || fread(c, 1, 8, f) != 8) break;
        uint32_t size = rd_le32(c + 4);
        if (!memcmp(c, "id3 ", 4) || !memcmp(c, "ID3 ", 4)) return id3_apic(f, at + 8, size, offset, length);
        at += 8 + (long)size + (size & 1);
    }
    return false;
}

// ---- decoding ----

typedef struct {
    FILE *f;
    long left;
    int side, x0, y0;            // the middle square of the picture
    uint32_t *sum;               // T * T * 4: blue, green, red, count
    void (*pause)(void);
} cover_dev_t;

static size_t cover_in(JDEC *jd, uint8_t *buf, size_t n)
{
    cover_dev_t *d = jd->device;
    if ((long)n > d->left) n = (size_t)d->left;
    if (buf) n = fread(buf, 1, n, d->f);
    else if (fseek(d->f, (long)n, SEEK_CUR)) return 0;
    d->left -= (long)n;
    return n;
}

static int cover_out(JDEC *jd, void *bitmap, JRECT *r)
{
    cover_dev_t *d = jd->device;
    const uint8_t *px = bitmap;   // B, G, R per pixel (LVGL's TJpgDec)
    int w = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++) {
        int sy = y - d->y0;
        if (sy < 0 || sy >= d->side) { px += 3 * w; continue; }
        uint32_t *row = d->sum + (size_t)(sy * T / d->side) * T * 4;
        for (int x = r->left; x <= r->right; x++, px += 3) {
            int sx = x - d->x0;
            if (sx < 0 || sx >= d->side) continue;
            uint32_t *s = row + (sx * T / d->side) * 4;
            s[0] += px[0];
            s[1] += px[1];
            s[2] += px[2];
            s[3]++;
        }
    }
    if (d->pause) d->pause();
    return 1;
}

static void (*s_pause)(void);   // (the board's: let lower tasks run now and then)

bool p4_cover_decode(FILE *f, long offset, long length, uint16_t *out)
{
    if (length <= 0 || fseek(f, offset, SEEK_SET)) return false;
    void *work = cover_alloc(WORK_BYTES);
    uint32_t *sum = cover_alloc((size_t)T * T * 4 * sizeof(uint32_t));
    bool ok = false;
    if (work && sum) {
        JDEC jd;
        cover_dev_t d = { .f = f, .left = length, .sum = sum, .pause = s_pause };
        if (jd_prepare(&jd, cover_in, work, WORK_BYTES, &d) == JDR_OK && jd.width && jd.height
            && (uint32_t)jd.width * jd.height <= MAX_PIXELS) {
            d.side = jd.width < jd.height ? jd.width : jd.height;
            d.x0 = (jd.width - d.side) / 2;
            d.y0 = (jd.height - d.side) / 2;
            ok = jd_decomp(&jd, cover_out, 0) == JDR_OK;
        }
    }
    if (ok) {
        // (a picture smaller than T leaves rows and columns without a pixel: they repeat the one above / before;
        //  row 0 and column 0 always get some)
        for (int y = 0; y < T; y++) {
            bool any = false;
            for (int x = 0; x < T && !any; x++) any = sum[(y * T + x) * 4 + 3] != 0;
            if (!any) {
                memcpy(out + y * T, out + (y - 1) * T, T * sizeof(uint16_t));
                continue;
            }
            for (int x = 0; x < T; x++) {
                const uint32_t *s = sum + (y * T + x) * 4;
                if (!s[3]) {
                    out[y * T + x] = out[y * T + x - 1];
                    continue;
                }
                uint32_t b = s[0] / s[3], g = s[1] / s[3], r = s[2] / s[3];
                out[y * T + x] = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
            }
        }
    }
    free(work);
    free(sum);
    return ok;
}

// ---- where it is looked for ----

static bool try_file(const char *path, uint16_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = !fseek(f, 0, SEEK_END);
    long len = ok ? ftell(f) : 0;
    ok = ok && len > 0 && len < 16 * 1024 * 1024 && p4_cover_decode(f, 0, len, out);
    fclose(f);
    if (!ok) COVER_LOGW("%s: not a JPEG it can read (baseline only)", path);
    return ok;
}

static const char *const EXT[] = { "jpg", "jpeg" };

// A folder's picture (cover / folder / front / album / artwork, .jpg / .jpeg), folder = dir or dir/<album>. (path:
// the caller's buffer of PATH_BYTES - one for all, the cover task's stack is small)
static bool folder_picture(const char *dir, const char *album, int album_len, char *path, uint16_t *out)
{
    static const char *const FOLDER[] = { "cover", "folder", "front", "album", "artwork" };
    for (int k = 0; k < 5; k++)
        for (int e = 0; e < 2; e++) {
            if (album_len) snprintf(path, PATH_BYTES, "%s/%.*s/%s.%s", dir, album_len, album, FOLDER[k], EXT[e]);
            else snprintf(path, PATH_BYTES, "%s/%s.%s", dir, FOLDER[k], EXT[e]);
            if (try_file(path, out)) return true;
        }
    return false;
}

const char *p4_cover_load(const char *dir, const char *name, uint16_t *out)
{
    char path[PATH_BYTES];
    for (int e = 0; e < 2; e++) {
        snprintf(path, sizeof(path), "%s/%s.%s", dir, name, EXT[e]);
        if (try_file(path, out)) return "the picture beside it";
    }
    snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
    FILE *f = fopen(path, "rb");
    if (f) {
        long at, len;
        bool ok = p4_cover_find_apic(f, &at, &len) && p4_cover_decode(f, at, len, out);
        fclose(f);
        if (ok) return "its tags";
    }
    const char *slash = strchr(name, '/');   // a track of an album: its folder's picture before the jam folder's
    if (slash && folder_picture(dir, name, (int)(slash - name), path, out)) return "the album's picture";
    if (folder_picture(dir, "", 0, path, out)) return "the folder's picture";
    return NULL;
}

// ---- on the board ----
#ifndef P4_COVER_HOST
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p4_audio.h"

static const char *TAG = "p4_cover";
static SemaphoreHandle_t s_mx;
static uint16_t *s_px;   // the cover shown (PSRAM)
static bool s_has;
static volatile uint32_t s_seq;
static volatile uint32_t s_reload;   // goes up when a cover came over Wi-Fi: look again

static void pause_now_and_then(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last > 15000) {
        vTaskDelay(1);
        last = esp_timer_get_time();
    }
}

static void cover_task(void *arg)
{
    uint16_t *work = heap_caps_malloc(T * T * 2, MALLOC_CAP_SPIRAM);
    char shown[P4_JAM_NAME] = "\x01";
    int shown_count = -1;
    uint32_t shown_reload = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(300));
        int cur;
        track_player_info_t ti;
        p4_jam_info(&cur, &ti);
        char name[P4_JAM_NAME] = "";
        int count = p4_jam_count();
        if (cur >= 0 && p4_jam_card()) snprintf(name, sizeof(name), "%s", p4_jam_name(cur));
        uint32_t reload = s_reload;
        if (!strcmp(name, shown) && count == shown_count && reload == shown_reload) continue;   // (a new list or a cover)
        snprintf(shown, sizeof(shown), "%s", name);
        shown_count = count;
        shown_reload = reload;
        int64_t t0 = esp_timer_get_time();
        const char *from = name[0] && work ? p4_cover_load(P4_JAM_DIR, name, work) : NULL;
        if (name[0]) ESP_LOGI(TAG, "%s: %s%s (%d ms)", name, from ? "cover from " : "no cover", from ? from : "",
                              (int)((esp_timer_get_time() - t0) / 1000));
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (from) memcpy(s_px, work, T * T * 2);
        s_has = from != NULL;
        s_seq++;
        xSemaphoreGive(s_mx);
    }
}

void p4_cover_start(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_px = heap_caps_malloc(T * T * 2, MALLOC_CAP_SPIRAM);
    s_pause = pause_now_and_then;
    if (!s_mx || !s_px || xTaskCreatePinnedToCore(cover_task, "cover", 6144, NULL, 1, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "no memory for the covers");
}

uint32_t p4_cover_seq(void) { return s_seq; }
void p4_cover_reload(void) { s_reload++; }

bool p4_cover_copy(uint16_t *dst, uint32_t *seq)
{
    if (!s_mx) return false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool has = s_has;
    if (has) memcpy(dst, s_px, T * T * 2);
    if (seq) *seq = s_seq;
    xSemaphoreGive(s_mx);
    return has;
}
#endif
