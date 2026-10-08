// Browser build: the ESP-IDF services the shared code uses (timers, queues, settings, logging), on top of
// Emscripten. Everything runs in the page's main thread; web_platform_run() is called from the main loop and
// after every message from JavaScript.
#include <emscripten.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#ifndef PROJECT_VER
#define PROJECT_VER "web"
#endif

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    default: return "ESP_FAIL";
    }
}

// ---- time and logging ----

int64_t esp_timer_get_time(void)
{
    return (int64_t)(emscripten_get_now() * 1000.0);
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)emscripten_get_now();
}

void esp_log_buffer_hex(const char *tag, const void *buffer, size_t len)
{
    const uint8_t *d = buffer;
    for (size_t i = 0; i < len; i += 16) {
        char line[16 * 3 + 1] = "";
        for (size_t j = i; j < len && j < i + 16; j++) sprintf(line + 3 * (j - i), "%02x ", d[j]);
        printf("I %s: %s\n", tag, line);
    }
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

void esp_restart(void)
{
    EM_ASM(location.reload());
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = { .version = PROJECT_VER, .project_name = "nano_controller" };
    return &desc;
}

// ---- memory (one heap) ----

void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
void heap_caps_free(void *ptr) { free(ptr); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 0; }

// ---- timers ----

struct esp_timer {
    esp_timer_create_args_t args;
    bool active;
    int64_t due_us, period_us;
    struct esp_timer *next;
};
static struct esp_timer *s_timers;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    struct esp_timer *t = calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->args = *args;
    t->next = s_timers;
    s_timers = t;
    *out = t;
    return ESP_OK;
}

// As in ESP-IDF: a running timer is not restarted.
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    if (t->active) return ESP_ERR_INVALID_STATE;
    t->active = true;
    t->period_us = 0;
    t->due_us = esp_timer_get_time() + (int64_t)timeout_us;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us)
{
    if (t->active) return ESP_ERR_INVALID_STATE;
    t->active = true;
    t->period_us = (int64_t)period_us;
    t->due_us = esp_timer_get_time() + (int64_t)period_us;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t->active) return ESP_ERR_INVALID_STATE;
    t->active = false;
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    return t->active;
}

// Runs the timers that are due, earliest first. A callback may start or stop timers.
static void run_timers(void)
{
    for (int guard = 0; guard < 64; guard++) {
        int64_t now = esp_timer_get_time();
        struct esp_timer *first = NULL;
        for (struct esp_timer *t = s_timers; t; t = t->next) {
            if (t->active && t->due_us <= now && (!first || t->due_us < first->due_us)) first = t;
        }
        if (!first) return;
        if (first->period_us) first->due_us += first->period_us;
        else first->active = false;
        first->args.callback(first->args.arg);
    }
}

// ---- queues (never block) ----

struct web_queue {
    size_t item_size, capacity, head, count;
    uint8_t *items;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct web_queue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->item_size = item_size;
    q->capacity = length < 256 ? 256 : length;   // nothing else empties it while the page is busy: some room
    q->items = malloc(q->capacity * item_size);
    if (!q->items) {
        free(q);
        return NULL;
    }
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->count == q->capacity) return pdFALSE;
    memcpy(q->items + ((q->head + q->count) % q->capacity) * q->item_size, item, q->item_size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->count) return pdFALSE;
    memcpy(item, q->items + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return pdTRUE;
}

// ---- settings in localStorage ("nano-controller:<namespace>:<key>" = hex) ----

#define NVS_NAMESPACES 8
static char s_namespaces[NVS_NAMESPACES][16];

EM_JS(int, js_store_get, (const char *key, uint8_t *out, int max), {
    let hex = null;
    try { hex = localStorage.getItem(UTF8ToString(key)); } catch (e) {}
    if (hex === null) return -1;
    const n = hex.length >> 1;
    for (let i = 0; i < n && i < max; i++) HEAPU8[out + i] = parseInt(hex.substr(2 * i, 2), 16);
    return n;
});

EM_JS(void, js_store_set, (const char *key, const uint8_t *data, int len), {
    let hex = "";
    for (let i = 0; i < len; i++) hex += HEAPU8[data + i].toString(16).padStart(2, "0");
    try { localStorage.setItem(UTF8ToString(key), hex); } catch (e) { console.warn("Settings not stored", e); }
});

EM_JS(void, js_store_remove, (const char *key), {
    try { localStorage.removeItem(UTF8ToString(key)); } catch (e) {}
});

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    for (int i = 0; i < NVS_NAMESPACES; i++) {
        if (!s_namespaces[i][0]) strncpy(s_namespaces[i], name, sizeof(s_namespaces[i]) - 1);
        if (!strcmp(s_namespaces[i], name)) {
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }

static void store_key(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    snprintf(out, size, "nano-controller:%s:%s", handle >= 1 && handle <= NVS_NAMESPACES ? s_namespaces[handle - 1] : "?", key);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    char k[64];
    store_key(handle, key, k, sizeof(k));
    int n = js_store_get(k, NULL, 0);
    if (n < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) {
        *length = (size_t)n;
        return ESP_OK;
    }
    if (*length < (size_t)n) return ESP_ERR_NVS_INVALID_LENGTH;
    js_store_get(k, out, n);
    *length = (size_t)n;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    char k[64];
    store_key(handle, key, k, sizeof(k));
    js_store_set(k, value, (int)length);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    char k[64];
    store_key(handle, key, k, sizeof(k));
    if (js_store_get(k, NULL, 0) < 0) return ESP_ERR_NVS_NOT_FOUND;
    js_store_remove(k);
    return ESP_OK;
}

// ---- main loop ----

void web_app_pump(void);   // main.c

// Due timers and the queued events of the app (messages from the Nano, commands from the screen).
EMSCRIPTEN_KEEPALIVE void web_platform_run(void)
{
    run_timers();
    web_app_pump();
}
