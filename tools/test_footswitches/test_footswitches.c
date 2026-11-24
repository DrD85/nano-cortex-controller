// Test of main/footswitches.c on the computer: the real file, compiled against stand-ins for ESP-IDF (stubs/) and
// fed with pin levels millisecond by millisecond - a simulated SX1509, the eight pins of the P4 board's header P3,
// a clock and a small settings store. footswitches.c keeps its state in statics, so every scenario is a run of its
// own:   ./run.sh   (all scenarios, as the P4 board builds the file and as the 4.3" board builds it)
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stub.h"
#include FOOTSWITCHES_C   // the file under test, e.g. "../../main/footswitches.c" (run.sh sets it)

// ---- checks ----

static int s_checks, s_failed;
#define CHECK(condition, ...) \
    do { \
        s_checks++; \
        if (!(condition)) { \
            s_failed++; \
            printf("  FAILED line %d: ", __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

// ---- clock and task ----

static TickType_t s_now, s_end;
static jmp_buf s_done;
static void (*s_script)(TickType_t now);   // what happens to the pins, called for every millisecond
static TaskFunction_t s_task;
static int s_tasks;

void vTaskDelay(TickType_t ticks)
{
    for (TickType_t i = 0; i < ticks; i++) {
        s_now++;
        if (s_script) s_script(s_now);
    }
    if (s_now >= s_end) longjmp(s_done, 1);
}

TickType_t xTaskGetTickCount(void)
{
    return s_now;
}

int xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *arg, int priority, void *handle)
{
    s_task = function;
    s_tasks++;
    return 1;
}

// Runs the footswitch task until the clock shows `until` (once per scenario: a second call would start it anew).
static void run(TickType_t until)
{
    s_end = until;
    if (!s_task) return;
    if (!setjmp(s_done)) s_task(NULL);
}

// ---- log ----

static char s_log[32768];

void stub_log(char level, const char *tag, const char *format, ...)
{
    size_t used = strlen(s_log);
    if (used > sizeof(s_log) - 512) return;
    used += (size_t)snprintf(s_log + used, sizeof(s_log) - used, "%c %6u %s: ", level, (unsigned)s_now, tag);
    va_list args;
    va_start(args, format);
    used += (size_t)vsnprintf(s_log + used, sizeof(s_log) - used, format, args);
    va_end(args);
    snprintf(s_log + used, sizeof(s_log) - used, "\n");
}

static bool logged(const char *text)
{
    return strstr(s_log, text) != NULL;
}

// ---- settings store ----

typedef struct {
    char key[16];
    uint8_t data[32];
    size_t size;
} entry_t;
static entry_t s_store[8];
static int s_entries;

static entry_t *store_find(const char *key)
{
    for (int i = 0; i < s_entries; i++) if (!strcmp(s_store[i].key, key)) return &s_store[i];
    return NULL;
}

static void store_seed(const char *key, const int8_t pins[FOOTSWITCH_COUNT])
{
    entry_t *e = &s_store[s_entries++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    memcpy(e->data, pins, FOOTSWITCH_COUNT);
    e->size = FOOTSWITCH_COUNT;
}

static bool store_is(const char *key, const int8_t pins[FOOTSWITCH_COUNT])
{
    entry_t *e = store_find(key);
    return e && e->size == FOOTSWITCH_COUNT && !memcmp(e->data, pins, FOOTSWITCH_COUNT);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (strcmp(name, "nano")) return ESP_FAIL;
    if (mode == NVS_READONLY && !s_entries) return ESP_FAIL;   // as the real one: no such namespace yet
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    entry_t *e = store_find(key);
    if (!e || *size < e->size) return ESP_FAIL;
    memcpy(value, e->data, e->size);
    *size = e->size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    entry_t *e = store_find(key);
    if (!e) {
        e = &s_store[s_entries++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    memcpy(e->data, value, size);
    e->size = size;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
}

// ---- SX1509 ----

static int s_sx_address;             // 0 = no chip on the bus
static uint8_t s_sx_reg[0x80];
static uint16_t s_sx_pins = 0xFFFF;  // bit set = released

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    return s_sx_address && address == s_sx_address ? ESP_OK : ESP_FAIL;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config, i2c_master_dev_handle_t *device)
{
    *device = (i2c_master_dev_handle_t)s_sx_reg;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t size, int timeout_ms)
{
    if (!s_sx_address || size != 2) return ESP_FAIL;
    s_sx_reg[data[0] & 0x7F] = data[1];
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *out, size_t out_size, uint8_t *in, size_t in_size, int timeout_ms)
{
    if (!s_sx_address || out_size != 1) return ESP_FAIL;
    if (out[0] == 0x10 && in_size == 2) {   // the pins: bank B, then bank A
        in[0] = (uint8_t)(s_sx_pins >> 8);
        in[1] = (uint8_t)s_sx_pins;
    } else {
        for (size_t i = 0; i < in_size; i++) in[i] = s_sx_reg[(out[0] + i) & 0x7F];
    }
    return ESP_OK;
}

static void sx(int pin, bool pressed)
{
    if (pressed) s_sx_pins &= (uint16_t)~(1u << pin);
    else s_sx_pins |= (uint16_t)(1u << pin);
}

// ---- GPIO ----

static uint64_t s_gpio_low;      // pins a switch pulls to ground
static uint64_t s_gpio_inputs;   // pins set up as inputs with pull-up
static int s_gpio_configs, s_gpio_bad_reads;

esp_err_t gpio_config(const gpio_config_t *config)
{
    s_gpio_configs++;
    if (config->mode == GPIO_MODE_INPUT && config->pull_up_en == GPIO_PULLUP_ENABLE) s_gpio_inputs |= config->pin_bit_mask;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio)
{
    if (!((s_gpio_inputs >> gpio) & 1)) s_gpio_bad_reads++;   // an open pin without pull-up reads anything
    return (s_gpio_low >> gpio) & 1 ? 0 : 1;
}

static void sw(int gpio, bool pressed)
{
    if (pressed) s_gpio_low |= 1ULL << gpio;
    else s_gpio_low &= ~(1ULL << gpio);
}

// ---- what the footswitches report ----

typedef struct {
    TickType_t at;
    int number;
    footswitch_event_t event;
} event_t;
static event_t s_events[128];
static int s_event_count;
static struct { TickType_t at; int number, swapped; } s_learned[8];
static int s_learn_count;

static void on_press(int number, footswitch_event_t event)
{
    if (s_event_count < (int)(sizeof(s_events) / sizeof(s_events[0]))) s_events[s_event_count++] = (event_t){ s_now, number, event };
}

static void on_learn(int number, int swapped)
{
    if (s_learn_count < 8) {
        s_learned[s_learn_count].at = s_now;
        s_learned[s_learn_count].number = number;
        s_learned[s_learn_count].swapped = swapped;
        s_learn_count++;
    }
}

static int count(int number, footswitch_event_t event)
{
    int n = 0;
    for (int i = 0; i < s_event_count; i++) if (s_events[i].number == number && s_events[i].event == event) n++;
    return n;
}

static const event_t *first(int number, footswitch_event_t event)
{
    for (int i = 0; i < s_event_count; i++) if (s_events[i].number == number && s_events[i].event == event) return &s_events[i];
    return NULL;
}

static int presses(void)
{
    int n = 0;
    for (int i = 0; i < s_event_count; i++) if (s_events[i].event == FOOTSWITCH_PRESS) n++;
    return n;
}

static bool start(void)
{
    return footswitches_start(NULL, on_press, on_learn);
}

// A press of `ms` at `at` is reported once, 2-4 ms after the contact closed; its release about 20 ms after it opened.
static void check_tap(int number, TickType_t at, TickType_t ms)
{
    const event_t *p = first(number, FOOTSWITCH_PRESS), *r = first(number, FOOTSWITCH_RELEASE);
    CHECK(count(number, FOOTSWITCH_PRESS) == 1, "switch %d: %d presses instead of 1", number, count(number, FOOTSWITCH_PRESS));
    CHECK(p && p->at >= at + 2 && p->at <= at + 4, "switch %d: press at %u, contact at %u", number, p ? (unsigned)p->at : 0, (unsigned)at);
    CHECK(count(number, FOOTSWITCH_RELEASE) == 1, "switch %d: %d releases instead of 1", number, count(number, FOOTSWITCH_RELEASE));
    CHECK(r && r->at >= at + ms + 18 && r->at <= at + ms + 22, "switch %d: release at %u, contact open at %u", number,
          r ? (unsigned)r->at : 0, (unsigned)(at + ms));
}

// ---- scenarios: an SX1509 on the bus (both boards) ----

static void script_sx(TickType_t t)
{
    if (t == 100) sx(11, true);
    if (t == 150) sx(11, false);
    if (t == 300) sx(9, true);
    if (t == 350) sx(9, false);
    if (t == 500) sw(2, true);        // a header pin: nobody reads it while an SX1509 is there
    if (t == 560) sw(2, false);
    if (t == 1500) s_sx_reg[0x06] = s_sx_reg[0x07] = 0;   // the chip was reset: its pull-ups are gone
}

static void sc_sx1509(void)
{
    static const int8_t reversed[] = { 7, 6, 5, 4, 3, 2, 1, 0 };
    s_sx_address = 0x71;
    store_seed("fsw_gpio", reversed);   // an order of the header's pins must not reach the SX1509
    CHECK(start(), "start with an SX1509 returns true");
    CHECK(s_tasks == 1, "one task, got %d", s_tasks);
    CHECK(logged("SX1509 found at 0x71 - footswitches active"), "log names the chip");
    CHECK(logged("Footswitch 1-8 on SX1509 pins 11 10 0 1 2 3 8 9"), "log shows the default order");
    CHECK(s_gpio_configs == 0, "no header pin is set up while an SX1509 is there");
    CHECK(s_sx_reg[0x06] == 0xFF && s_sx_reg[0x07] == 0xFF && s_sx_reg[0x0E] == 0xFF && s_sx_reg[0x0F] == 0xFF, "pull-ups on, all inputs");
    s_script = script_sx;
    run(2700);
    CHECK(s_event_count == 4, "4 events, got %d", s_event_count);
    check_tap(1, 100, 50);
    check_tap(8, 300, 50);
    CHECK(s_gpio_bad_reads == 0 && presses() == 2, "the header pin did nothing");
    CHECK(logged("SX1509 lost its setup"), "the lost setup is noticed");
    CHECK(s_sx_reg[0x06] == 0xFF && s_sx_reg[0x07] == 0xFF, "and set up again");
}

static void sc_sx1509_0x70(void)
{
    s_sx_address = 0x70;
    CHECK(start(), "start with an SX1509 at 0x70 returns true");
    CHECK(logged("SX1509 found at 0x70"), "log names the address");
}

static void script_sx_learn(TickType_t t)
{
    if (t == 50) footswitches_learn(1);
    if (t == 100) sx(5, true);
    if (t == 150) sx(5, false);
    if (t == 400) sx(5, true);
    if (t == 450) sx(5, false);
}

static void sc_sx1509_learn(void)
{
    static const int8_t learned[] = { 5, 10, 0, 1, 2, 3, 8, 9 };
    s_sx_address = 0x71;
    CHECK(start(), "start");
    s_script = script_sx_learn;
    run(700);
    CHECK(s_learn_count == 1 && s_learned[0].number == 1 && s_learned[0].swapped == 0, "switch 1 learned, nothing swapped");
    CHECK(logged("Footswitch 1 learned: SX1509 pin 5"), "log names the pin");
    CHECK(store_is("fsw_pins", learned), "stored under fsw_pins");
    CHECK(!store_find("fsw_gpio"), "the header's entry is not written");
    CHECK(count(1, FOOTSWITCH_PRESS) == 1 && first(1, FOOTSWITCH_PRESS)->at >= 400, "the learning press is not a press, the next one is");
}

#if !NANO_P4
// ---- the 4.3" board: nothing without an SX1509 ----

static void sc_none(void)
{
    CHECK(!start(), "start without an SX1509 returns false");
    CHECK(s_tasks == 0, "no task");
    CHECK(logged("No SX1509 at 0x71/0x70 - footswitches off"), "log says so");
    CHECK(s_gpio_configs == 0, "no pin touched");
}
#endif

#if NANO_P4
// ---- scenarios: switches on the P4 board's header P3 ----

static const int GPIOS[FOOTSWITCH_COUNT] = { 2, 3, 4, 5, 28, 29, 30, 31 };
static const int8_t ORDER_DEFAULT[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const int8_t ORDER_SX1509[] = { 11, 10, 0, 1, 2, 3, 8, 9 };

static void script_each(TickType_t t)
{
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) {
        if (t == (TickType_t)(100 + 200 * i)) sw(GPIOS[i], true);
        if (t == (TickType_t)(150 + 200 * i)) sw(GPIOS[i], false);
    }
}

static void sc_header_each(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) mask |= 1ULL << GPIOS[i];
    CHECK(start(), "start without an SX1509 returns true");
    CHECK(s_tasks == 1, "one task, got %d", s_tasks);
    CHECK(s_gpio_inputs == mask, "GPIO 2-5 and 28-31 are inputs with pull-up, nothing else (%llx)", (unsigned long long)s_gpio_inputs);
    CHECK(logged("footswitches on the header P3"), "log says where the switches are");
    CHECK(logged("Footswitch 1-8 on GPIOs 2 3 4 5 28 29 30 31"), "log shows the order");
    s_script = script_each;
    run(1800);
    CHECK(s_event_count == 16, "16 events, got %d", s_event_count);
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) check_tap(i + 1, (TickType_t)(100 + 200 * i), 50);
    CHECK(s_gpio_bad_reads == 0, "only pins with pull-up are read");
    CHECK(!logged("SX1509 lost") && !logged("does not answer"), "no SX1509 messages");
    CHECK(!store_find("fsw_gpio") && !store_find("fsw_pins"), "nothing is stored without learning");
}

// A contact that bounces for 8 ms when it closes and when it opens (2 ms each way, so the polls see every flank),
// and one that chatters every millisecond.
static void script_bounce(TickType_t t)
{
    if (t >= 100 && t < 108) sw(2, ((t - 100) / 2) % 2 == 0);
    if (t == 108) sw(2, true);
    if (t >= 300 && t < 308) sw(2, ((t - 300) / 2) % 2 != 0);
    if (t == 308) sw(2, false);
    if (t >= 501 && t < 506) sw(31, (t - 501) % 2 == 0);
    if (t == 506) sw(31, true);
    if (t >= 701 && t < 706) sw(31, (t - 701) % 2 != 0);
    if (t == 706) sw(31, false);
}

static void sc_header_bounce(void)
{
    CHECK(start(), "start");
    s_script = script_bounce;
    run(900);
    CHECK(s_event_count == 4, "4 events, got %d", s_event_count);
    CHECK(count(1, FOOTSWITCH_PRESS) == 1 && count(1, FOOTSWITCH_RELEASE) == 1, "switch 1: one press, one release");
    CHECK(count(8, FOOTSWITCH_PRESS) == 1 && count(8, FOOTSWITCH_RELEASE) == 1, "switch 8: one press, one release");
    const event_t *p = first(1, FOOTSWITCH_PRESS), *r = first(1, FOOTSWITCH_RELEASE);
    CHECK(p && p->at <= 112, "switch 1: press %u, at most 4 ms after the bouncing ended", p ? (unsigned)p->at : 0);
    CHECK(r && r->at >= 308 + 18, "switch 1: release %u only after 20 ms of quiet", r ? (unsigned)r->at : 0);
}

// Interference: lows shorter than two polls in a row are not presses.
static void script_spike(TickType_t t)
{
    sw(3, t == 101 || t == 205 || t == 309);   // 1 ms, between two polls
    sw(28, t == 400 || t == 401);              // 2 ms: one poll sees it
    sw(30, t == 600 || t == 601 || t == 604 || t == 605 || t == 608 || t == 609);   // every other poll
}

static void sc_header_spikes(void)
{
    CHECK(start(), "start");
    s_script = script_spike;
    run(800);
    CHECK(s_event_count == 0, "no events, got %d", s_event_count);
}

static void script_hold(TickType_t t)
{
    if (t == 100) sw(4, true);
    if (t == 1100) sw(4, false);
    if (t == 1300) sw(5, true);
    if (t == 1500) sw(5, false);
}

static void sc_header_hold(void)
{
    CHECK(start(), "start");
    s_script = script_hold;
    run(1700);
    const event_t *p = first(3, FOOTSWITCH_PRESS), *h = first(3, FOOTSWITCH_HOLD), *r = first(3, FOOTSWITCH_RELEASE);
    CHECK(count(3, FOOTSWITCH_PRESS) == 1 && count(3, FOOTSWITCH_HOLD) == 1 && count(3, FOOTSWITCH_RELEASE) == 1, "switch 3: press, one hold, release");
    CHECK(p && h && r && p->at < h->at && h->at < r->at, "in that order");
    CHECK(p && h && h->at - p->at >= FOOTSWITCH_HOLD_MS && h->at - p->at <= FOOTSWITCH_HOLD_MS + 4, "held after %u ms", p && h ? (unsigned)(h->at - p->at) : 0);
    CHECK(count(4, FOOTSWITCH_PRESS) == 1 && count(4, FOOTSWITCH_HOLD) == 0 && count(4, FOOTSWITCH_RELEASE) == 1, "switch 4 (200 ms): no hold");
    CHECK(s_event_count == 5, "5 events, got %d", s_event_count);
}

// A switch that is down while the board starts is not a press; its next press is.
static void script_start_held(TickType_t t)
{
    if (t == 500) sw(3, false);
    if (t == 700) sw(3, true);
    if (t == 760) sw(3, false);
}

static void sc_header_down_at_start(void)
{
    sw(3, true);
    CHECK(start(), "start");
    s_script = script_start_held;
    run(1000);
    const event_t *p = first(2, FOOTSWITCH_PRESS);
    CHECK(count(2, FOOTSWITCH_PRESS) == 1 && p && p->at >= 702 && p->at <= 704, "one press, the one at 700 (got %d, first at %u)",
          count(2, FOOTSWITCH_PRESS), p ? (unsigned)p->at : 0);
    CHECK(count(2, FOOTSWITCH_HOLD) == 0, "no hold from the start");
    CHECK(presses() == 1, "no other switch");
}

static void script_two(TickType_t t)
{
    if (t == 100) { sw(2, true); sw(31, true); }
    if (t == 160) { sw(2, false); sw(31, false); }
}

static void sc_header_two_at_once(void)
{
    CHECK(start(), "start");
    s_script = script_two;
    run(400);
    CHECK(s_event_count == 4, "4 events, got %d", s_event_count);
    check_tap(1, 100, 60);
    check_tap(8, 100, 60);
}

static void script_learn(TickType_t t)
{
    if (t == 60) footswitches_learn(3);
    if (t == 100) sw(2, true);   // GPIO2 (switch 1 so far) becomes switch 3
    if (t == 150) sw(2, false);
    if (t == 400) sw(2, true);
    if (t == 450) sw(2, false);
    if (t == 600) sw(4, true);   // switch 3's old pin is switch 1 now
    if (t == 650) sw(4, false);
}

static void sc_header_learn(void)
{
    static const int8_t learned[] = { 2, 1, 0, 3, 4, 5, 6, 7 };
    CHECK(start(), "start");
    s_script = script_learn;
    run(900);
    CHECK(s_learn_count == 1 && s_learned[0].number == 3 && s_learned[0].swapped == 1, "switch 3 learned, switch 1 took its pin");
    CHECK(logged("Footswitch 3 learned: GPIO 2"), "log names the GPIO");
    CHECK(logged("Footswitch 1-8 on GPIOs 4 3 2 5 28 29 30 31"), "log shows the new order");
    CHECK(store_is("fsw_gpio", learned), "stored under fsw_gpio");
    CHECK(!store_find("fsw_pins"), "the SX1509's entry is not written");
    const event_t *p3 = first(3, FOOTSWITCH_PRESS), *p1 = first(1, FOOTSWITCH_PRESS);
    CHECK(count(3, FOOTSWITCH_PRESS) == 1 && p3 && p3->at >= 402, "GPIO2 is switch 3; the learning press was not a press");
    CHECK(count(1, FOOTSWITCH_PRESS) == 1 && p1 && p1->at >= 602, "GPIO4 is switch 1");
    CHECK(presses() == 2, "2 presses, got %d", presses());
}

static void script_learn_timeout(TickType_t t)
{
    if (t == 60) footswitches_learn(2);
}

static void sc_header_learn_timeout(void)
{
    CHECK(start(), "start");
    s_script = script_learn_timeout;
    run(15300);
    CHECK(s_learn_count == 1 && s_learned[0].number == 0 && s_learned[0].swapped == 0, "learn ends with nothing");
    CHECK(s_learn_count == 1 && s_learned[0].at >= 15060 && s_learned[0].at <= 15070, "after 15 s (%u)", (unsigned)s_learned[0].at);
    CHECK(s_event_count == 0 && !store_find("fsw_gpio"), "nothing pressed, nothing stored");
}

static void script_learn_cancel(TickType_t t)
{
    if (t == 60) footswitches_learn(2);
    if (t == 100) footswitches_learn(0);
    if (t == 200) sw(2, true);
    if (t == 250) sw(2, false);
}

static void sc_header_learn_cancel(void)
{
    CHECK(start(), "start");
    s_script = script_learn_cancel;
    run(500);
    CHECK(s_learn_count == 0, "no learn result after a cancel");
    check_tap(1, 200, 50);
    CHECK(!store_find("fsw_gpio"), "nothing stored");
}

static void script_stored(TickType_t t)
{
    if (t == 100) sw(31, true);
    if (t == 150) sw(31, false);
    if (t == 300) sw(2, true);
    if (t == 350) sw(2, false);
}

static void sc_header_stored_order(void)
{
    static const int8_t reversed[] = { 7, 6, 5, 4, 3, 2, 1, 0 };
    store_seed("fsw_gpio", reversed);
    CHECK(start(), "start");
    CHECK(logged("Footswitch 1-8 on GPIOs 31 30 29 28 5 4 3 2"), "the stored order is used");
    s_script = script_stored;
    run(600);
    check_tap(1, 100, 50);
    check_tap(8, 300, 50);
}

static void script_reset(TickType_t t)
{
    if (t == 60) footswitches_reset();
    if (t == 100) sw(2, true);
    if (t == 150) sw(2, false);
}

static void sc_header_reset(void)
{
    static const int8_t learned[] = { 2, 1, 0, 3, 4, 5, 6, 7 };
    store_seed("fsw_gpio", learned);
    CHECK(start(), "start");
    CHECK(logged("Footswitch 1-8 on GPIOs 4 3 2 5 28 29 30 31"), "starts with the stored order");
    s_script = script_reset;
    run(400);
    CHECK(store_is("fsw_gpio", ORDER_DEFAULT), "reset stores the header's default order");
    check_tap(1, 100, 50);
}

static void script_gpio4(TickType_t t)
{
    if (t == 100) sw(4, true);
    if (t == 150) sw(4, false);
}

// An order with pins the header does not have (here: the SX1509's default) is not used.
static void sc_header_bad_stored_order(void)
{
    store_seed("fsw_gpio", ORDER_SX1509);
    CHECK(start(), "start");
    CHECK(logged("Footswitch 1-8 on GPIOs 2 3 4 5 28 29 30 31"), "default order instead of the unusable one");
    s_script = script_gpio4;
    run(400);
    check_tap(3, 100, 50);
}

// What was learned on an SX1509 stays its own: the header starts with its default order and leaves that entry alone.
static void sc_header_ignores_sx1509_order(void)
{
    static const int8_t sx_learned[] = { 2, 10, 0, 1, 11, 3, 8, 9 };
    store_seed("fsw_pins", sx_learned);
    CHECK(start(), "start");
    s_script = script_gpio4;
    run(400);
    check_tap(3, 100, 50);
    CHECK(store_is("fsw_pins", sx_learned), "the SX1509's order is untouched");
}
#endif

// ---- main ----

static const struct {
    const char *name;
    void (*run)(void);
} SCENARIOS[] = {
    { "sx1509", sc_sx1509 },
    { "sx1509-0x70", sc_sx1509_0x70 },
    { "sx1509-learn", sc_sx1509_learn },
#if NANO_P4
    { "header-each", sc_header_each },
    { "header-bounce", sc_header_bounce },
    { "header-spikes", sc_header_spikes },
    { "header-hold", sc_header_hold },
    { "header-down-at-start", sc_header_down_at_start },
    { "header-two-at-once", sc_header_two_at_once },
    { "header-learn", sc_header_learn },
    { "header-learn-timeout", sc_header_learn_timeout },
    { "header-learn-cancel", sc_header_learn_cancel },
    { "header-stored-order", sc_header_stored_order },
    { "header-reset", sc_header_reset },
    { "header-bad-stored-order", sc_header_bad_stored_order },
    { "header-ignores-sx1509-order", sc_header_ignores_sx1509_order },
#else
    { "none", sc_none },
#endif
};

int main(int argc, char **argv)
{
    const int n = (int)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0]));
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (int i = 0; i < n; i++) puts(SCENARIOS[i].name);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(argv[1], SCENARIOS[i].name)) continue;
        SCENARIOS[i].run();
        if (s_failed) {
            printf("FAILED  %s (%d of %d checks)\n%s", argv[1], s_failed, s_checks, s_log);
            for (int e = 0; e < s_event_count; e++) printf("  event at %u: switch %d %s\n", (unsigned)s_events[e].at, s_events[e].number,
                                                           s_events[e].event == FOOTSWITCH_PRESS ? "press" : s_events[e].event == FOOTSWITCH_HOLD ? "hold" : "release");
            return 1;
        }
        printf("ok      %-28s %2d checks\n", argv[1], s_checks);
        return 0;
    }
    printf("no scenario \"%s\"\n", argv[1]);
    return 2;
}
