// Wi-Fi for jam tracks from the Mac (see p4_net.h).
#include "p4_net.h"

#include <ctype.h>
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
#include "p4_audio.h"
#include "p4_ota.h"
#include "wav.h"

static const char *TAG = "p4_net";

#define HOST_NAME "nano-board"
#define AP_SSID "Nano Board"
#define NVS_NS "p4net"
#define RECV_CHUNK (32 * 1024)
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

static esp_err_t tracks(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "application/json");
    char line[128];
    snprintf(line, sizeof(line), "{\"firmware\":\"%.31s\",\"card\":%s,\"measuring\":%d,\"tracks\":[",
             esp_app_get_description()->version, p4_jam_card() ? "true" : "false", p4_jam_measuring());
    httpd_resp_sendstr_chunk(req, line);
    for (int i = 0; i < p4_jam_count(); i++) {
        httpd_resp_sendstr_chunk(req, i ? ",{\"name\":" : "{\"name\":");
        json_string(req, p4_jam_name(i));
        p4_jam_note(i, line, sizeof(line));
        httpd_resp_sendstr_chunk(req, ",\"note\":");
        json_string(req, line);
        httpd_resp_sendstr_chunk(req, "}");
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *why);

// POST /api/trims: the corrections by ear of the Jam Player ("<file><TAB><dB>" per line, at most 16 KB)
static esp_err_t trims(httpd_req_t *req)
{
    touch();
    size_t total = req->content_len;
    if (!p4_jam_card() || total > 16 * 1024) return fail(req, "400 Bad Request", "no card, or too long");
    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (!body) return fail(req, "500 Internal Server Error", "no memory");
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    body[got] = 0;
    p4_jam_transfer(true);
    p4_jam_trims(body);
    free(body);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t done(httpd_req_t *req)
{
    p4_jam_transfer(false);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// "/api/tracks/My%20Track.wav" -> "My Track"; false if the name is not usable as a file on the card.
static bool track_name(const char *uri, char *out, size_t n)
{
    const char *p = uri + strlen("/api/tracks/");
    size_t k = 0;
    for (; *p && *p != '?' && k < n - 1; p++) {
        char c = *p;
        if (c == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
            c = (char)(hex(p[1]) << 4 | hex(p[2]));
            p += 2;
        }
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            (unsigned char)c < 0x20) return false;
        out[k++] = c;
    }
    out[k] = 0;
    if (k > 4 && !strcasecmp(out + k - 4, ".wav")) out[k -= 4] = 0;
    return k > 0 && k < P4_JAM_NAME - 1 && out[0] != '.';
}

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
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_st.receiving, name, sizeof(s_st.receiving));
    s_st.percent = 0;
    taskEXIT_CRITICAL(&s_lock);

    char *buf = heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_SPIRAM);
    FILE *f = buf ? fopen(P4_JAM_DIR "/upload.tmp", "wb") : NULL;
    if (!f) {
        free(buf);
        return fail(req, "500 Internal Server Error", "could not write to the card");
    }
    size_t got = 0;
    bool ok = true, checked = false;
    while (got < total) {
        int n = httpd_req_recv(req, buf, total - got < RECV_CHUNK ? total - got : RECV_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { ok = false; break; }
        if (!checked) {   // the first piece holds the header: only WAV files the board can play
            wav_info_t wi;
            checked = true;
            if (!wav_parse((const uint8_t *)buf, n, &wi)) { ok = false; break; }
        }
        if (fwrite(buf, 1, n, f) != (size_t)n) { ok = false; break; }
        got += n;
        int pct = (int)(got * 100 / total);
        taskENTER_CRITICAL(&s_lock);
        s_st.percent = pct;
        taskEXIT_CRITICAL(&s_lock);
        touch();
    }
    ok = fclose(f) == 0 && ok;
    free(buf);
    if (!ok) {
        remove(P4_JAM_DIR "/upload.tmp");
        return fail(req, "400 Bad Request", checked ? "not a WAV file the board can play, or the transfer broke off" : "transfer broke off");
    }
    char path[sizeof(P4_JAM_DIR) + P4_JAM_NAME + 8];
    snprintf(path, sizeof(path), P4_JAM_DIR "/%s.wav", name);
    remove(path);   // (a new version of a track replaces the old one)
    if (rename(P4_JAM_DIR "/upload.tmp", path) != 0) return fail(req, "500 Internal Server Error", "could not name the file");

    float lufs = header_float(req, "X-Lufs"), play = header_float(req, "X-Play"), peak = header_float(req, "X-Peak");
    if (isfinite(lufs) && isfinite(play) && isfinite(peak) && peak > 0) p4_jam_measured(name, (uint32_t)total, lufs, play, peak);
    float trim = header_float(req, "X-Trim");
    if (isfinite(trim)) p4_jam_set_trim(name, trim);
    taskENTER_CRITICAL(&s_lock);
    s_st.received++;
    s_st.receiving[0] = 0;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "received %s.wav (%u KB)%s", name, (unsigned)(total / 1024), isfinite(play) ? ", measured on the Mac" : "");
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

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 15;
    cfg.core_id = 1;
    cfg.task_priority = 4;   // below the audio and the screen
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server not started");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page },
        { .uri = "/api/tracks", .method = HTTP_GET, .handler = tracks },
        { .uri = "/api/tracks/*", .method = HTTP_PUT, .handler = upload },
        { .uri = "/api/trims", .method = HTTP_POST, .handler = trims },
        { .uri = "/api/done", .method = HTTP_POST, .handler = done },
        { .uri = "/api/firmware", .method = HTTP_PUT, .handler = firmware },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_http, &uris[i]);
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
