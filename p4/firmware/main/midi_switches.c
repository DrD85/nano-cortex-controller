#include "midi_switches.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#ifndef NANO_WEB
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

static const char *TAG = "midi_switch";

#define LEARN_TIMEOUT_US (15 * 1000 * 1000)
#define HOLD_US (FOOTSWITCH_HOLD_MS * 1000LL)
#define STORE_NAMESPACE "nano"
#define STORE_KEY "midi_sw"

// Per footswitch the learned message: status << 8 | number, with the status of a CC (0xB0), a note (0x90, also for
// its note off) or a program change (0xC0) and its channel; 0 = none. Read by the MIDI callback, changed by it (learn)
// and by midi_switches_reset.
static _Atomic uint16_t s_keys[FOOTSWITCH_COUNT];
static _Atomic int s_learn;   // footswitch waiting for a MIDI press (1-8), 0 = none; whoever exchanges it to 0 ends it

static footswitch_cb s_on_event;
static footswitch_learn_cb s_on_learn;

typedef enum { EV_DOWN, EV_UP, EV_TAP, EV_LEARNED, EV_SAVE, EV_WAKE } ev_kind_t;
typedef struct { uint8_t kind, number, other; } ev_t;

// ---- the worker: the task of its own (board) or the browser's main loop - the only one that calls on_event ----

static bool s_pressed[FOOTSWITCH_COUNT + 1];
static int64_t s_hold_at[FOOTSWITCH_COUNT + 1];   // when the press becomes a hold (0 = not waiting for one)
static int64_t s_learn_until;                     // 0 = not learning

static void save(void)
{
    uint16_t keys[FOOTSWITCH_COUNT];
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) keys[i] = atomic_load(&s_keys[i]);
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Could not store the learned MIDI switches");
        return;
    }
    esp_err_t err = nvs_set_blob(nvs, STORE_KEY, keys, sizeof(keys));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "Could not store the learned MIDI switches: %s", esp_err_to_name(err));
}

static void work(ev_t ev)
{
    int n = ev.number;
    int64_t now = esp_timer_get_time();
    switch (ev.kind) {
    case EV_DOWN:
        if (s_pressed[n]) break;   // (a controller may repeat its 127)
        s_pressed[n] = true;
        s_hold_at[n] = now + HOLD_US;
        ESP_LOGI(TAG, "Footswitch %d (MIDI)", n);
        if (s_on_event) s_on_event(n, FOOTSWITCH_PRESS);
        break;
    case EV_UP:
        if (!s_pressed[n]) break;
        s_pressed[n] = false;
        s_hold_at[n] = 0;
        if (s_on_event) s_on_event(n, FOOTSWITCH_RELEASE);
        break;
    case EV_TAP:   // a program change: pressed and let go at once
        if (s_pressed[n]) break;
        ESP_LOGI(TAG, "Footswitch %d (MIDI program change)", n);
        if (s_on_event) {
            s_on_event(n, FOOTSWITCH_PRESS);
            s_on_event(n, FOOTSWITCH_RELEASE);
        }
        break;
    case EV_LEARNED:
        s_learn_until = 0;
        save();
        if (s_on_learn) s_on_learn(n, ev.other);
        break;
    case EV_SAVE:
        save();
        break;
    case EV_WAKE:   // learn started or cancelled
        s_learn_until = atomic_load(&s_learn) ? now + LEARN_TIMEOUT_US : 0;
        break;
    }
}

// Holds and the learn's end that are due; returns the microseconds to the next one (-1 = none).
static int64_t tick(void)
{
    int64_t now = esp_timer_get_time(), next = -1;
    for (int n = 1; n <= FOOTSWITCH_COUNT; n++) {
        if (!s_hold_at[n]) continue;
        if (now >= s_hold_at[n]) {
            s_hold_at[n] = 0;
            ESP_LOGI(TAG, "Footswitch %d held (MIDI)", n);
            if (s_on_event) s_on_event(n, FOOTSWITCH_HOLD);
        } else if (next < 0 || s_hold_at[n] - now < next) {
            next = s_hold_at[n] - now;
        }
    }
    if (s_learn_until && now >= s_learn_until) {
        s_learn_until = 0;
        if (atomic_exchange(&s_learn, 0) && s_on_learn) s_on_learn(0, 0);
    } else if (s_learn_until && (next < 0 || s_learn_until - now < next)) {
        next = s_learn_until - now;
    }
    return next;
}

#ifndef NANO_WEB
static QueueHandle_t s_queue;

static void post(ev_t ev)
{
    if (!s_queue || xQueueSend(s_queue, &ev, 0) != pdTRUE) ESP_LOGW(TAG, "Queue full - MIDI switch event lost");
}

static void worker_task(void *arg)
{
    for (;;) {
        int64_t wait = tick();
        TickType_t ticks = wait < 0 ? portMAX_DELAY : pdMS_TO_TICKS(wait / 1000) + 1;
        ev_t ev;
        if (xQueueReceive(s_queue, &ev, ticks) == pdTRUE) work(ev);
    }
}
#else
// Browser: everything comes on the main loop; a timer does the holds and the learn's end.
static esp_timer_handle_t s_timer;

static void schedule(void)
{
    int64_t wait = tick();
    esp_timer_stop(s_timer);
    if (wait >= 0) esp_timer_start_once(s_timer, (uint64_t)wait + 1000);
}

static void timer_cb(void *arg)
{
    (void)arg;
    schedule();
}

static void post(ev_t ev)
{
    work(ev);
    schedule();
}
#endif

// ---- interface ----

void midi_switches_start(footswitch_cb on_event, footswitch_learn_cb on_learn)
{
    s_on_event = on_event;
    s_on_learn = on_learn;
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t keys[FOOTSWITCH_COUNT];
        size_t size = sizeof(keys);
        if (nvs_get_blob(nvs, STORE_KEY, keys, &size) == ESP_OK && size == sizeof(keys)) {
            for (int i = 0; i < FOOTSWITCH_COUNT; i++) atomic_store(&s_keys[i], keys[i] >> 8 >= 0x80 ? keys[i] : 0);
        }
        nvs_close(nvs);
    }
    for (int n = 1; n <= FOOTSWITCH_COUNT; n++) {
        char text[32];
        if (midi_switches_describe(n, text, sizeof(text))) ESP_LOGI(TAG, "Footswitch %d: MIDI %s", n, text);
    }
#ifndef NANO_WEB
    s_queue = xQueueCreate(16, sizeof(ev_t));
    xTaskCreate(worker_task, "midi_switch", 3072, NULL, 5, NULL);
#else
    const esp_timer_create_args_t timer = { .callback = timer_cb, .name = "midi_switch" };
    esp_timer_create(&timer, &s_timer);
#endif
}

bool midi_switches_take(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint8_t type = status & 0xF0, channel = status & 0x0F;
    uint16_t key;
    bool press, tap = false;
    if (type == 0xB0) {
        if (data1 == 1) return false;   // the expression pedal
        key = (uint16_t)(status << 8 | data1);
        press = data2 >= 64;
    } else if (type == 0x90 || type == 0x80) {
        key = (uint16_t)((0x90 | channel) << 8 | data1);
        press = type == 0x90 && data2 > 0;
    } else if (type == 0xC0) {
        key = (uint16_t)(status << 8 | data1);
        press = tap = true;
    } else {
        return false;
    }
    if (press && atomic_load(&s_learn)) {
        int number = atomic_exchange(&s_learn, 0);
        if (number >= 1 && number <= FOOTSWITCH_COUNT) {
            int other = 0;
            for (int i = 0; i < FOOTSWITCH_COUNT; i++) {
                if (i != number - 1 && atomic_load(&s_keys[i]) == key) {
                    atomic_store(&s_keys[i], 0);
                    other = i + 1;
                }
            }
            atomic_store(&s_keys[number - 1], key);
            post((ev_t){ .kind = EV_LEARNED, .number = (uint8_t)number, .other = (uint8_t)other });
            return true;
        }
    }
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) {
        if (atomic_load(&s_keys[i]) != key) continue;
        post((ev_t){ .kind = tap ? EV_TAP : press ? EV_DOWN : EV_UP, .number = (uint8_t)(i + 1) });
        return true;
    }
    return false;
}

void midi_switches_learn(int number)
{
    atomic_store(&s_learn, number >= 1 && number <= FOOTSWITCH_COUNT ? number : 0);
    post((ev_t){ .kind = EV_WAKE });
}

void midi_switches_reset(void)
{
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) atomic_store(&s_keys[i], 0);
    post((ev_t){ .kind = EV_SAVE });
}

bool midi_switches_describe(int number, char *text, size_t size)
{
    if (size) text[0] = 0;
    if (number < 1 || number > FOOTSWITCH_COUNT) return false;
    uint16_t key = atomic_load(&s_keys[number - 1]);
    if (!key) return false;
    uint8_t status = (uint8_t)(key >> 8), value = (uint8_t)key;
    const char *kind = (status & 0xF0) == 0xB0 ? "CC" : (status & 0xF0) == 0x90 ? "note" : "PC";
    snprintf(text, size, "%s %u on channel %u", kind, value, (status & 0x0F) + 1u);
    return true;
}
