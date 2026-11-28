// Wi-Fi for jam tracks from the Mac (see p4_net.h).
#include "p4_net.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "esp_app_desc.h"
#include "loop_wav.h"
#include "p4_audio.h"
#include "p4_cover.h"
#include "p4_ota.h"
#include "wav.h"

static const char *TAG = "p4_net";

#define HOST_NAME "nano-board"
#define AP_SSID "Nano Board"
#define NVS_NS "p4net"
#define RECV_CHUNK (64 * 1024)
#define IDLE_DONE_US (15 * 1000 * 1000)   // no request for this long: the transfer counts as done
#define JOIN_TRIES 4

extern const char upload_html_start[] asm("_binary_upload_html_start");
extern const char upload_html_end[] asm("_binary_upload_html_end");

static p4_net_status_t s_st;
static bool s_on, s_inited;
static esp_netif_t *s_sta, *s_ap;
static httpd_handle_t s_http;
static esp_timer_handle_t s_idle;
static int s_tries;
static char s_pending_ssid[33];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_state(p4_net_state_t st)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.state = st;
    taskEXIT_CRITICAL(&s_lock);
}

void p4_net_status(p4_net_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
}

bool p4_net_enabled(void) { return s_on; }

// ---- stored network ----

static bool load_network(char ssid[33], char pass[65])
{
    nvs_handle_t nvs;
    ssid[0] = pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t n = 33, m = 65;
    nvs_get_str(nvs, "ssid", ssid, &n);
    nvs_get_str(nvs, "pass", pass, &m);
    nvs_close(nvs);
    return ssid[0] != 0;
}

static void ap_password(char out[12])
{
    nvs_handle_t nvs;
    size_t n = 12;
    out[0] = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        strcpy(out, "nanoboard");
        return;
    }
    if (nvs_get_str(nvs, "appass", out, &n) != ESP_OK || strlen(out) < 8) {   // made once, shown on the screen
        snprintf(out, 12, "%08lu", (unsigned long)(esp_random() % 100000000UL));
        nvs_set_str(nvs, "appass", out);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

void p4_net_set_network(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "pass", password);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "network %s%s", ssid[0] ? "set: " : "forgotten", ssid);
    if (s_on) {   // join it now
        p4_net_enable(false);
        p4_net_enable(true);
    }
}

// From the screen's keyboard: 'W' = the network's name, then 'K' = its password.
void p4_net_text(char kind, const char *text)
{
    if (kind == 'W') strlcpy(s_pending_ssid, text, sizeof(s_pending_ssid));
    else if (kind == 'K') p4_net_set_network(s_pending_ssid, text);
}

// ---- the networks around ----

static p4_net_ap_t s_found[P4_NET_SCAN_MAX];
static int s_found_n;
static volatile bool s_scanning;

bool p4_net_scanning(void) { return s_scanning; }

int p4_net_scan_results(p4_net_ap_t *out, int max)
{
    taskENTER_CRITICAL(&s_lock);
    int n = s_found_n < max ? s_found_n : max;
    memcpy(out, s_found, sizeof(*out) * (size_t)n);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

static int by_signal(const void *a, const void *b) { return ((const p4_net_ap_t *)b)->rssi - ((const p4_net_ap_t *)a)->rssi; }

static bool init_once(void);
static void start_ap(void);

void p4_net_scan(void)
{
    if (s_scanning) return;
    s_scanning = true;
    if (!init_once()) {
        s_scanning = false;
        return;
    }
    bool was_on = s_on;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (!was_on) {   // only to look: the station side, joining nothing (on_event does nothing while s_on is false)
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    } else if (mode == WIFI_MODE_AP) {   // the board's own network stays open meanwhile
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    const wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&cfg, true);
    uint16_t count = 0;
    static wifi_ap_record_t rec[32];
    if (e == ESP_OK) {
        count = sizeof(rec) / sizeof(rec[0]);
        if (esp_wifi_scan_get_ap_records(&count, rec) != ESP_OK) count = 0;
    }
    p4_net_ap_t found[P4_NET_SCAN_MAX];
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (!rec[i].ssid[0]) continue;
        int same = -1;
        for (int k = 0; k < n; k++) if (!strcmp(found[k].ssid, (const char *)rec[i].ssid)) same = k;
        if (same >= 0) {   // one name, several access points: the strongest
            if (rec[i].rssi > found[same].rssi) found[same].rssi = rec[i].rssi;
            continue;
        }
        if (n == P4_NET_SCAN_MAX) continue;
        strlcpy(found[n].ssid, (const char *)rec[i].ssid, sizeof(found[n].ssid));
        found[n].rssi = rec[i].rssi;
        found[n].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    qsort(found, (size_t)n, sizeof(found[0]), by_signal);
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_found, found, sizeof(found[0]) * (size_t)n);
    s_found_n = n;
    taskEXIT_CRITICAL(&s_lock);
    if (!was_on && !s_on) esp_wifi_stop();
    else if (s_on && mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
    ESP_LOGI(TAG, "scan: %d networks (%s)", n, e == ESP_OK ? "ok" : esp_err_to_name(e));
    s_scanning = false;
}

// ---- HTTP ----

static void touch(void)   // a request: the transfer goes on
{
    if (s_idle) {
        esp_timer_stop(s_idle);
        esp_timer_start_once(s_idle, IDLE_DONE_US);
    }
}

static void idle_done(void *arg) { p4_jam_transfer(false); }

static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, upload_html_start, upload_html_end - upload_html_start);
}

static void json_string(httpd_req_t *req, const char *s)
{
    char out[2 * P4_JAM_NAME + 4];
    size_t n = 0;
    out[n++] = '"';
    for (; *s && n < sizeof(out) - 3; s++) {
        if (*s == '"' || *s == '\\') out[n++] = '\\';
        if ((unsigned char)*s >= 0x20) out[n++] = *s;
    }
    out[n++] = '"';
    out[n] = 0;
    httpd_resp_sendstr_chunk(req, out);
}

// GET /api/tracks. "albums": this board takes "Album/Title" names (2026-11-27) - a track's "name" is relative to the
// jam folder, "source" the stamp of the Mac's file it came from ("" = sent before stamps), so the Jam Player sends
// only what is new or changed. "more": tracks on the card the list had no room for (not in "tracks" - sending them
// again would not help).
static esp_err_t tracks(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "application/json");
    char line[160];
    snprintf(line, sizeof(line), "{\"firmware\":\"%.31s\",\"albums\":true,\"card\":%s,\"measuring\":%d,\"more\":%d,\"tracks\":[",
             esp_app_get_description()->version, p4_jam_card() ? "true" : "false", p4_jam_measuring(), p4_jam_left_out());
    httpd_resp_sendstr_chunk(req, line);
    for (int i = 0; i < p4_jam_count(); i++) {
        httpd_resp_sendstr_chunk(req, i ? ",{\"name\":" : "{\"name\":");
        json_string(req, p4_jam_name(i));
        p4_jam_note(i, line, sizeof(line));
        httpd_resp_sendstr_chunk(req, ",\"note\":");
        json_string(req, line);
        char path[P4_JAM_PATH];   // its cover beside it (the Jam Player sends the missing)
        struct stat st;
        snprintf(path, sizeof(path), P4_JAM_DIR "/%s.jpg", p4_jam_name(i));
        httpd_resp_sendstr_chunk(req, stat(path, &st) == 0 ? ",\"cover\":true,\"source\":" : ",\"cover\":false,\"source\":");
        json_string(req, p4_jam_source(i));
        snprintf(line, sizeof(line), ",\"size\":%lu}", (unsigned long)p4_jam_size(i));   // (the Jam Player: a WAV from before stamps changed?)
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *why);

// A text body of lines (at most most bytes) into PSRAM, 0-ended; NULL after the refusal has been answered - also
// when the transfer broke off: a cut last line would be taken (a move to "Blues/So" of "Blues/Song", "-1" of "-12.5").
static char *text_body(httpd_req_t *req, size_t most)
{
    size_t total = req->content_len;
    if (!p4_jam_card() || total > most) {
        fail(req, "400 Bad Request", "no card, or too long");
        return NULL;
    }
    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (!body) {
        fail(req, "500 Internal Server Error", "no memory");
        return NULL;
    }
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    if (got < total) {
        free(body);
        fail(req, "400 Bad Request", "transfer broke off");
        return NULL;
    }
    body[got] = 0;
    return body;
}

// POST /api/trims: the corrections by ear of the Jam Player ("<file><TAB><dB>" per line, at most 16 KB)
static esp_err_t trims(httpd_req_t *req)
{
    touch();
    char *body = text_body(req, 16 * 1024);
    if (!body) return ESP_OK;
    p4_jam_transfer(true);
    p4_jam_trims(body);
    free(body);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/move: loose tracks into their albums ("<from>.wav<TAB><to>.wav" per line; the Jam Player's folders became
// albums, 2026-11-27): the WAV, its cover and its keys go along, nothing is sent again. A target that is there already
// is left alone. Playing stops as for an upload; the folder is read again at /api/done.
static esp_err_t move(httpd_req_t *req)
{
    touch();
    char *body = text_body(req, 96 * 1024);
    if (!body) return ESP_OK;
    p4_jam_transfer(true);
    int moved = p4_jam_move(body);
    free(body);
    char reply[48];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"moved\":%d}", moved);
    return httpd_resp_sendstr(req, reply);
}

// POST /api/sources: the Mac's stamps for tracks on the card already ("<file>.wav<TAB><stamp>" per line: sent before
// stamps existed, the same song). Playing goes on.
static esp_err_t sources(httpd_req_t *req)
{
    touch();
    char *body = text_body(req, 64 * 1024);
    if (!body) return ESP_OK;
    p4_jam_sources(body);
    free(body);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t done(httpd_req_t *req)
{
    p4_jam_transfer(false);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// "/api/tracks/Blues/Slow%20Blues.wav" -> "Blues/Slow Blues", "/api/tracks/My%20Track.wav" -> "My Track" (prefix
// "/api/tracks/", ext ".wav"). The album and the title are decoded each on its own (an encoded '/' is refused), then
// the name is made as the card keeps it and checked (p4_jam_clean_name: one folder level, no part starting with '.' -
// nothing outside the jam folder). False if the name is not usable as a file on the card.
static bool file_name(const char *uri, const char *prefix, const char *ext, char *out, size_t n)
{
    const char *p = uri + strlen(prefix);
    size_t k = 0;
    int folders = 0;
    for (; *p && *p != '?' && k < n - 1; p++) {
        char c = *p;
        if (c == '/') {   // between the album and the title
            if (++folders > 1 || !k) return false;
            out[k++] = c;
            continue;
        }
        if (c == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
            c = (char)(hex(p[1]) << 4 | hex(p[2]));
            p += 2;
        }
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            (unsigned char)c < 0x20 || c == 0x7F) return false;
        out[k++] = c;
    }
    if (*p && *p != '?') return false;   // (longer than a name can be)
    out[k] = 0;
    size_t e = strlen(ext);
    if (k > e && !strcasecmp(out + k - e, ext)) out[k -= e] = 0;
    return p4_jam_clean_name(out);
}

// (cut at a whole character: the screen shows it)
static void copy_utf8(char *dst, const char *src, size_t n)
{
    if (strlcpy(dst, src, n) < n) return;
    size_t end = n - 1, start = end;
    while (start && ((unsigned char)dst[start - 1] & 0xC0) == 0x80) start--;
    if (start && (unsigned char)dst[start - 1] >= 0xC0) {   // the last character's first byte: is all of it there?
        unsigned char lead = (unsigned char)dst[start - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (end - (start - 1) < need) dst[start - 1] = 0;
    }
}
static bool track_name(const char *uri, char *out, size_t n) { return file_name(uri, "/api/tracks/", ".wav", out, n); }

static float header_float(httpd_req_t *req, const char *name)
{
    char v[24];
    if (httpd_req_get_hdr_value_str(req, name, v, sizeof(v)) != ESP_OK) return NAN;
    return strtof(v, NULL);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *why)
{
    ESP_LOGW(TAG, "upload refused: %s", why);
    httpd_resp_set_status(req, status);
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", why);
    taskENTER_CRITICAL(&s_lock);
    s_st.receiving[0] = 0;
    taskEXIT_CRITICAL(&s_lock);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t upload(httpd_req_t *req)
{
    touch();
    char name[P4_JAM_NAME];
    if (!p4_jam_card()) return fail(req, "503 Service Unavailable", "no microSD card in the board");
    if (!track_name(req->uri, name, sizeof(name))) return fail(req, "400 Bad Request", "unusable file name");
    size_t total = req->content_len;
    uint64_t card_total = 0, card_free = 0;
    esp_vfs_fat_info("/sdcard", &card_total, &card_free);
    if (total < 44 || total + 1024 * 1024 > card_free) return fail(req, "507 Insufficient Storage", "not enough room on the card");

    p4_jam_transfer(true);   // playing and measuring stop: a file may be replaced
    const char *title = strchr(name, '/');   // (the screen shows the title alone)
    taskENTER_CRITICAL(&s_lock);
    copy_utf8(s_st.receiving, title ? title + 1 : name, sizeof(s_st.receiving));
    s_st.percent = 0;
    taskEXIT_CRITICAL(&s_lock);

    char *buf = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_SPIRAM);
    FILE *f = buf ? fopen(P4_JAM_DIR "/upload.tmp", "wb") : NULL;
    if (!f) {
        free(buf);
        return fail(req, "500 Internal Server Error", "could not write to the card");
    }
    // (2026-11-27: a receive gives one TCP segment, 1-6 KB; written to the card piece by piece that was many small
    //  writes on the SD host the Wi-Fi shares. Now the buffer is filled first and goes to the card in one write.)
    size_t got = 0, fill = 0;
    bool ok = true, checked = false;
    int64_t t_recv = 0, t_write = 0, t0 = esp_timer_get_time();
    while (got < total) {
        int64_t a = esp_timer_get_time();
        int n = httpd_req_recv(req, buf + fill, total - got < RECV_CHUNK - fill ? total - got : RECV_CHUNK - fill);
        t_recv += esp_timer_get_time() - a;
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { ok = false; break; }
        if (!checked && fill + n >= 64) {   // the first piece holds the header: only WAV files the board can play
            wav_info_t wi;
            checked = true;
            if (!wav_parse((const uint8_t *)buf, fill + n, &wi)) { ok = false; break; }
        }
        fill += n;
        got += n;
        if (fill == RECV_CHUNK || got == total) {
            a = esp_timer_get_time();
            if (fwrite(buf, 1, fill, f) != fill) { ok = false; break; }
            t_write += esp_timer_get_time() - a;
            fill = 0;
        }
        int pct = (int)(got * 100 / total);
        taskENTER_CRITICAL(&s_lock);
        s_st.percent = pct;
        taskEXIT_CRITICAL(&s_lock);
        touch();
    }
    if (got) ESP_LOGI(TAG, "%u KB in %u ms (%u KB/s): receiving %u ms, writing %u ms", (unsigned)(got / 1024),
                      (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)(got * 1000 / (esp_timer_get_time() - t0 + 1)),
                      (unsigned)(t_recv / 1000), (unsigned)(t_write / 1000));
    ok = fclose(f) == 0 && ok;
    free(buf);
    if (!ok) {
        remove(P4_JAM_DIR "/upload.tmp");
        return fail(req, "400 Bad Request", checked ? "not a WAV file the board can play, or the transfer broke off" : "transfer broke off");
    }
    char path[P4_JAM_PATH];
    snprintf(path, sizeof(path), P4_JAM_DIR "/%s.wav", name);
    p4_jam_make_album(name);
    remove(path);   // (a new version of a track replaces the old one: FAT's rename would not)
    if (rename(P4_JAM_DIR "/upload.tmp", path) != 0) {
        remove(P4_JAM_DIR "/upload.tmp");
        return fail(req, "500 Internal Server Error", "could not name the file");
    }
    p4_jam_remove_loose(name);   // into an album: the loose copy of it from before albums would be a second one

    float lufs = header_float(req, "X-Lufs"), play = header_float(req, "X-Play"), peak = header_float(req, "X-Peak");
    p4_jam_measured(name, (uint32_t)total, lufs, play, peak);   // (none: "not measured", the old one must not stay)
    float trim = header_float(req, "X-Trim");
    if (isfinite(trim)) p4_jam_set_trim(name, trim);
    char stamp[P4_JAM_SOURCE + 8];   // the stamp of the Mac's file it was made from: the next send skips it while that stays
    if (httpd_req_get_hdr_value_str(req, "X-Source", stamp, sizeof(stamp)) != ESP_OK) stamp[0] = 0;
    p4_jam_set_source(name, stamp);
    taskENTER_CRITICAL(&s_lock);
    s_st.received++;
    s_st.receiving[0] = 0;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "received %s.wav (%u KB)%s", name, (unsigned)(total / 1024), isfinite(play) ? ", measured on the Mac" : "");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// PUT /api/covers/<name>.jpg (<name> as for the tracks: "Album/Title" or "Title"): the cover of a track from the Jam
// Player (a small baseline JPEG it made, 2026-11-27), kept beside the track as <name>.jpg - where p4_cover.c looks
// first. Playing goes on.
#define COVER_MAX (2 * 1024 * 1024)
static esp_err_t cover_upload(httpd_req_t *req)
{
    touch();
    char name[P4_JAM_NAME];
    if (!p4_jam_card()) return fail(req, "503 Service Unavailable", "no microSD card in the board");
    if (!file_name(req->uri, "/api/covers/", ".jpg", name, sizeof(name))) return fail(req, "400 Bad Request", "unusable file name");
    size_t total = req->content_len;
    if (total < 4 || total > COVER_MAX) return fail(req, "400 Bad Request", "not a cover (2 MB at most)");
    char *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) return fail(req, "500 Internal Server Error", "no memory");
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    if (got < total || (uint8_t)buf[0] != 0xFF || (uint8_t)buf[1] != 0xD8) {
        free(buf);
        return fail(req, "400 Bad Request", got < total ? "transfer broke off" : "not a JPEG");
    }
    FILE *f = fopen(P4_JAM_DIR "/cover.tmp", "wb");
    bool ok = f && fwrite(buf, 1, total, f) == total;
    ok = (f && fclose(f) == 0) && ok;
    free(buf);
    char path[P4_JAM_PATH];
    snprintf(path, sizeof(path), P4_JAM_DIR "/%s.jpg", name);
    if (ok) {
        p4_jam_make_album(name);
        remove(path);   // (a new cover replaces the old one)
        ok = rename(P4_JAM_DIR "/cover.tmp", path) == 0;
    }
    if (!ok) {
        remove(P4_JAM_DIR "/cover.tmp");
        return fail(req, "500 Internal Server Error", "could not write to the card");
    }
    p4_cover_reload();
    ESP_LOGI(TAG, "received the cover of %s (%u KB)", name, (unsigned)(total / 1024));
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// PUT /api/firmware: a new firmware for the board (build/nano_controller_p4.bin, see p4_ota.h). The board restarts
// with it after the reply; if it does not work, the next start goes back to the one before.
static esp_err_t firmware(httpd_req_t *req)
{
    touch();
    char why[96];
    p4_ota_t *ota = p4_ota_open(req->content_len, why, sizeof(why));
    if (!ota) return fail(req, "400 Bad Request", why);
    p4_jam_transfer(true);   // no jam track, no measuring meanwhile (the flash is busy)
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_st.receiving, "Firmware", sizeof(s_st.receiving));
    s_st.percent = 0;
    taskEXIT_CRITICAL(&s_lock);
    char *buf = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        p4_ota_abort(ota);
        return fail(req, "500 Internal Server Error", "no memory");
    }
    size_t total = req->content_len, got = 0;
    bool ok = true;
    while (ok && got < total) {
        int n = httpd_req_recv(req, buf, total - got < RECV_CHUNK ? total - got : RECV_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            snprintf(why, sizeof(why), "transfer broke off");
            ok = false;
            break;
        }
        ok = p4_ota_write(ota, buf, n, why, sizeof(why));
        got += n;
        taskENTER_CRITICAL(&s_lock);
        s_st.percent = (int)(got * 100 / total);
        taskEXIT_CRITICAL(&s_lock);
        touch();
    }
    free(buf);
    if (!ok) {
        p4_ota_abort(ota);
        return fail(req, "400 Bad Request", why);
    }
    if (!p4_ota_finish(ota, why, sizeof(why))) return fail(req, "400 Bad Request", why);
    taskENTER_CRITICAL(&s_lock);
    s_st.receiving[0] = 0;
    taskEXIT_CRITICAL(&s_lock);
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":true,\"version\":\"%.31s\",\"restart\":true}", p4_ota_new_version());
    httpd_resp_sendstr(req, body);
    p4_ota_restart_soon(1500);   // after the reply has gone out
    return ESP_OK;
}

// ---- the looper's loops on the card (p4_loops.c, 2026-11-28): listed, fetched and deleted by the Jam Player ----

#define LOOP_SEND (16 * 1024)      // bytes read from the card and sent at a time
#define LOOP_PATH (sizeof(P4_LOOPS_DIR) + LOOP_WAV_NAME_MAX + 2)

static esp_err_t loop_refuse(httpd_req_t *req, const char *status, const char *why)
{
    httpd_resp_set_status(req, status);
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", why);
    return httpd_resp_sendstr(req, body);
}

// "/api/loops/Loop%200007%20A%2C%20120%20BPM%2C%204%20bars.wav" -> the path of "Loop 0007 A, 120 BPM, 4 bars.wav";
// false: not a loop's name (loop_wav_name_ok: no path, nothing hidden - nothing outside the loops folder).
static bool loop_path(const char *uri, char *path)
{
    char name[LOOP_WAV_NAME_MAX];
    const char *p = uri + strlen("/api/loops/");
    size_t k = 0;
    for (; *p && *p != '?' && k < sizeof(name) - 1; p++) {
        char c = *p;
        if (c == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
            c = (char)(hex(p[1]) << 4 | hex(p[2]));
            p += 2;
        }
        name[k++] = c;
    }
    if (*p && *p != '?') return false;
    name[k] = 0;
    if (!loop_wav_name_ok(name)) return false;
    snprintf(path, LOOP_PATH, P4_LOOPS_DIR "/%s", name);
    return true;
}

// GET /api/loops: {"card":true,"loops":[{"name":"Loop 0007 A, 120 BPM, 4 bars.wav","size":1536512}, ...]} - the WAVs
// in /sdcard/loops (a file being written is save.tmp until it is whole: not in the list), in the card's order (the
// names sort by their number).
static esp_err_t loops(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "application/json");
    bool card = p4_jam_card();
    httpd_resp_sendstr_chunk(req, card ? "{\"card\":true,\"loops\":[" : "{\"card\":false,\"loops\":[");
    DIR *d = card ? opendir(P4_LOOPS_DIR) : NULL;
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        char path[LOOP_PATH], line[40];
        struct stat st;
        if (!loop_wav_name_ok(e->d_name)) continue;
        snprintf(path, sizeof(path), P4_LOOPS_DIR "/%.*s", LOOP_WAV_NAME_MAX - 1, e->d_name);   // (checked: shorter)
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        httpd_resp_sendstr_chunk(req, n++ ? ",{\"name\":" : "{\"name\":");
        json_string(req, e->d_name);
        snprintf(line, sizeof(line), ",\"size\":%lu}", (unsigned long)st.st_size);
        httpd_resp_sendstr_chunk(req, line);
    }
    if (d) closedir(d);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// GET /api/loops/<name>: the WAV, in pieces (its bytes in X-Size: a piecewise answer has no length). Playing goes on.
static esp_err_t loop_get(httpd_req_t *req)
{
    touch();
    char path[LOOP_PATH];
    if (!p4_jam_card()) return loop_refuse(req, "503 Service Unavailable", "no microSD card in the board");
    if (!loop_path(req->uri, path)) return loop_refuse(req, "400 Bad Request", "not a loop's name");
    struct stat st;
    FILE *f = stat(path, &st) == 0 ? fopen(path, "rb") : NULL;
    if (!f) return loop_refuse(req, "404 Not Found", "no such loop");
    char *buf = heap_caps_malloc(LOOP_SEND, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return loop_refuse(req, "500 Internal Server Error", "no memory");
    }
    char size[16];   // (kept until the headers have gone out with the first piece)
    snprintf(size, sizeof(size), "%lu", (unsigned long)st.st_size);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "X-Size", size);
    esp_err_t err = ESP_OK;
    size_t got, sent = 0;
    while ((got = fread(buf, 1, LOOP_SEND, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, got)) != ESP_OK) break;
        sent += got;
        touch();
    }
    fclose(f);
    free(buf);
    if (err != ESP_OK) {   // (the Mac went away: the server closes the connection)
        ESP_LOGW(TAG, "%s: sending broke off after %u KB", path, (unsigned)(sent / 1024));
        return err;
    }
    ESP_LOGI(TAG, "sent %s (%u KB)", path, (unsigned)(sent / 1024));
    return httpd_resp_send_chunk(req, NULL, 0);
}

// DELETE /api/loops/<name>
static esp_err_t loop_delete(httpd_req_t *req)
{
    touch();
    char path[LOOP_PATH];
    if (!p4_jam_card()) return loop_refuse(req, "503 Service Unavailable", "no microSD card in the board");
    if (!loop_path(req->uri, path)) return loop_refuse(req, "400 Bad Request", "not a loop's name");
    if (remove(path) != 0) return loop_refuse(req, "404 Not Found", "no such loop");
    ESP_LOGI(TAG, "deleted %s", path);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/time: "<unix seconds> <minutes east of UTC>" - the Mac's clock; loops saved after it carry the date in
// their names (the board has no clock of its own; nothing is stored).
static esp_err_t set_time(httpd_req_t *req)
{
    touch();
    char body[48];
    size_t total = req->content_len, got = 0;
    if (total >= sizeof(body)) return loop_refuse(req, "400 Bad Request", "too long");
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    body[got] = 0;
    long long t;
    int minutes;
    if (got < total || sscanf(body, "%lld %d", &t, &minutes) != 2) return loop_refuse(req, "400 Bad Request", "not a time");
    p4_loops_set_time(t, minutes);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void http_start(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page },
        { .uri = "/api/tracks", .method = HTTP_GET, .handler = tracks },
        { .uri = "/api/tracks/*", .method = HTTP_PUT, .handler = upload },
        { .uri = "/api/covers/*", .method = HTTP_PUT, .handler = cover_upload },
        { .uri = "/api/trims", .method = HTTP_POST, .handler = trims },
        { .uri = "/api/move", .method = HTTP_POST, .handler = move },
        { .uri = "/api/sources", .method = HTTP_POST, .handler = sources },
        { .uri = "/api/done", .method = HTTP_POST, .handler = done },
        { .uri = "/api/firmware", .method = HTTP_PUT, .handler = firmware },
        { .uri = "/api/loops", .method = HTTP_GET, .handler = loops },
        { .uri = "/api/loops/*", .method = HTTP_GET, .handler = loop_get },
        { .uri = "/api/loops/*", .method = HTTP_DELETE, .handler = loop_delete },
        { .uri = "/api/time", .method = HTTP_POST, .handler = set_time },
    };
    const size_t count = sizeof(uris) / sizeof(uris[0]);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = count;   // (the default holds 8: a handler past them would be refused, its URI answered 404)
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 15;
    cfg.core_id = 1;
    cfg.task_priority = 4;   // below the audio and the screen
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server not started");
        return;
    }
    for (size_t i = 0; i < count; i++)
        if (httpd_register_uri_handler(s_http, &uris[i]) != ESP_OK) ESP_LOGE(TAG, "%s: not served", uris[i].uri);
}

// ---- Wi-Fi ----

static void start_ap(void)
{
    char pass[12];
    ap_password(pass);
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) {
        set_state(P4_NET_FAILED);
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_st.ssid, AP_SSID, sizeof(s_st.ssid));
    strlcpy(s_st.ap_pass, pass, sizeof(s_st.ap_pass));
    strlcpy(s_st.addr, "192.168.4.1", sizeof(s_st.addr));
    s_st.state = P4_NET_AP;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "own network \"%s\" open (192.168.4.1)", AP_SSID);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (!s_on) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_st.state == P4_NET_AP) return;
        if (++s_tries < JOIN_TRIES) {
            set_state(P4_NET_JOINING);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "network \"%s\" not joined: the board opens its own", s_st.ssid);
            start_ap();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        taskENTER_CRITICAL(&s_lock);
        snprintf(s_st.addr, sizeof(s_st.addr), IPSTR, IP2STR(&ev->ip_info.ip));
        s_st.state = P4_NET_STATION;
        taskEXIT_CRITICAL(&s_lock);
        s_tries = 0;
        ESP_LOGI(TAG, "joined \"%s\": http://%s.local / http://%s", s_st.ssid, HOST_NAME, s_st.addr);
    }
}

static bool init_once(void)
{
    if (s_inited) return true;
    if (esp_netif_init() != ESP_OK) return false;
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {   // (the C6 answers through esp_hosted)
        ESP_LOGE(TAG, "Wi-Fi (ESP32-C6) not reachable");
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t t = { .callback = idle_done, .name = "net_idle" };
    esp_timer_create(&t, &s_idle);
    s_inited = true;
    return true;
}

void p4_net_enable(bool on)
{
    if (on == s_on) return;
    if (!on) {
        s_on = false;
        if (s_http) httpd_stop(s_http);
        s_http = NULL;
        mdns_free();
        esp_wifi_stop();
        if (s_idle) esp_timer_stop(s_idle);
        p4_jam_transfer(false);
        memset(&s_st, 0, sizeof(s_st));
        ESP_LOGI(TAG, "Wi-Fi off");
        return;
    }
    s_on = true;
    set_state(P4_NET_STARTING);
    if (!init_once()) {
        set_state(P4_NET_FAILED);
        return;
    }
    s_st.received = 0;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOST_NAME);
        mdns_instance_name_set("Nano Board");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    http_start();
    char ssid[33], pass[65];
    if (load_network(ssid, pass)) {
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        taskENTER_CRITICAL(&s_lock);
        strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
        s_st.state = P4_NET_JOINING;
        taskEXIT_CRITICAL(&s_lock);
        s_tries = 0;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        if (esp_wifi_start() != ESP_OK) start_ap();
        ESP_LOGI(TAG, "joining \"%s\"", ssid);
    } else {
        start_ap();
    }
}
