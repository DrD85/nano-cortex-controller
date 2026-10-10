#include "footswitches.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "footswitch";

// SX1509 registers (bank B = pins 8-15, bank A = pins 0-7)
#define REG_INPUT_DISABLE_B 0x00
#define REG_PULLUP_B 0x06
#define REG_PULLUP_A 0x07
#define REG_PULLDOWN_B 0x08
#define REG_PULLDOWN_A 0x09
#define REG_DIR_B 0x0E
#define REG_DIR_A 0x0F
#define REG_DATA_B 0x10
#define REG_RESET 0x7D

#define PINS 16
// A press counts as soon as a pin is low in two polls in a row (2-4 ms after the contact closes - a looper needs
// that); the contact's bouncing afterwards is ignored because a release only counts after RELEASE_POLLS high polls.
#define POLL_MS 2
#define PRESS_POLLS 2
#define RELEASE_POLLS 10   // 20 ms
#define LEARN_TIMEOUT_MS 15000
#define STORE_NAMESPACE "nano"
#define STORE_KEY "fsw_pins"

// Default SX1509 pin of footswitch 1-8, as wired in the user's TonexOneController enclosure.
static const int8_t DEFAULT_PINS[FOOTSWITCH_COUNT] = { 11, 10, 0, 1, 2, 3, 8, 9 };

static i2c_master_dev_handle_t s_dev;
static footswitch_cb s_on_press;
static footswitch_learn_cb s_on_learn;
// Pin per footswitch; only the footswitch task changes it (learn and reset requests are handed over).
static int8_t s_pins[FOOTSWITCH_COUNT];
static int s_learn_request = -1;       // from footswitches_learn: -1 = none, 0 = cancel, 1-8 = learn
static volatile bool s_reset;

static int switch_for_pin(int pin)
{
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) if (s_pins[i] == pin) return i + 1;
    return 0;
}

static void pins_load(void)
{
    memcpy(s_pins, DEFAULT_PINS, sizeof(s_pins));
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    int8_t stored[FOOTSWITCH_COUNT];
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, STORE_KEY, stored, &size) == ESP_OK && size == sizeof(stored)) memcpy(s_pins, stored, sizeof(s_pins));
    nvs_close(nvs);
}

static void pins_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_blob(nvs, STORE_KEY, s_pins, sizeof(s_pins)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the footswitch order");
    }
    nvs_close(nvs);
}

static void log_pins(void)
{
    ESP_LOGI(TAG, "Footswitch 1-8 on SX1509 pins %d %d %d %d %d %d %d %d", s_pins[0], s_pins[1], s_pins[2], s_pins[3],
             s_pins[4], s_pins[5], s_pins[6], s_pins[7]);
}

void footswitches_learn(int number)
{
    __atomic_store_n(&s_learn_request, number >= 1 && number <= FOOTSWITCH_COUNT ? number : 0, __ATOMIC_SEQ_CST);
}

void footswitches_reset(void)
{
    s_reset = true;
}

// A pin was pressed while learning: it becomes footswitch number; a switch that had it gets the old pin.
static void learn_pin(int number, int pin)
{
    int other = switch_for_pin(pin);
    if (other && other != number) s_pins[other - 1] = s_pins[number - 1];
    s_pins[number - 1] = (int8_t)pin;
    pins_save();
    ESP_LOGI(TAG, "Footswitch %d learned: SX1509 pin %d", number, pin);
    log_pins();
    if (s_on_learn) s_on_learn(number, other != number ? other : 0);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t read_pins(uint16_t *pins)
{
    uint8_t reg = REG_DATA_B, data[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, data, sizeof(data), 50);
    if (err == ESP_OK) *pins = (uint16_t)(data[0] << 8 | data[1]);   // B, then A (auto-increment)
    return err;
}

static void footswitch_task(void *arg)
{
    uint16_t stable = 0xFFFF;          // debounced pins (bit set = released)
    uint8_t low[PINS] = { 0 }, high[PINS] = { 0 };   // polls in a row with the pin low / high
    int learn = 0;                     // switch waiting for a press, 0 = none
    TickType_t learn_start = 0;
    TickType_t down_since[FOOTSWITCH_COUNT + 1] = { 0 };
    bool down[FOOTSWITCH_COUNT + 1] = { false };   // pressed and not held long enough yet
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (s_reset) {
            s_reset = false;
            memcpy(s_pins, DEFAULT_PINS, sizeof(s_pins));
            pins_save();
            log_pins();
        }
        int request = __atomic_exchange_n(&s_learn_request, -1, __ATOMIC_SEQ_CST);
        if (request >= 0) {
            learn = request;
            learn_start = xTaskGetTickCount();
        }
        if (learn && xTaskGetTickCount() - learn_start > pdMS_TO_TICKS(LEARN_TIMEOUT_MS)) {
            learn = 0;
            if (s_on_learn) s_on_learn(0, 0);
        }
        for (int number = 1; number <= FOOTSWITCH_COUNT; number++) {
            if (down[number] && xTaskGetTickCount() - down_since[number] >= pdMS_TO_TICKS(FOOTSWITCH_HOLD_MS)) {
                down[number] = false;
                ESP_LOGI(TAG, "Footswitch %d held", number);
                if (s_on_press) s_on_press(number, FOOTSWITCH_HOLD);
            }
        }
        uint16_t pins;
        if (read_pins(&pins) != ESP_OK) continue;
        uint16_t pressed = 0, released = 0;
        for (int pin = 0; pin < PINS; pin++) {
            bool is_low = !(pins & (1u << pin));
            low[pin] = is_low && low[pin] < 255 ? low[pin] + 1 : is_low ? 255 : 0;
            high[pin] = !is_low && high[pin] < 255 ? high[pin] + 1 : !is_low ? 255 : 0;
            if ((stable & (1u << pin)) && low[pin] >= PRESS_POLLS) pressed |= 1u << pin;
            else if (!(stable & (1u << pin)) && high[pin] >= RELEASE_POLLS) released |= 1u << pin;
        }
        if (!pressed && !released) continue;
        stable = (uint16_t)((stable & ~pressed) | released);
        for (int pin = 0; pin < PINS; pin++) {
            if (released & (1u << pin)) {
                int number = switch_for_pin(pin);
                if (number) {
                    down[number] = false;
                    if (s_on_press) s_on_press(number, FOOTSWITCH_RELEASE);
                }
            }
            if (!(pressed & (1u << pin))) continue;
            if (learn) {
                learn_pin(learn, pin);
                learn = 0;
                continue;
            }
            int number = switch_for_pin(pin);
            if (!number) {
                ESP_LOGI(TAG, "SX1509 pin %d pressed (no footswitch assigned)", pin);
                continue;
            }
            ESP_LOGI(TAG, "Footswitch %d (SX1509 pin %d)", number, pin);
            down[number] = true;
            down_since[number] = xTaskGetTickCount();
            if (s_on_press) s_on_press(number, FOOTSWITCH_PRESS);
        }
    }
}

bool footswitches_start(i2c_master_bus_handle_t bus, footswitch_cb on_press, footswitch_learn_cb on_learn)
{
    s_on_press = on_press;
    s_on_learn = on_learn;
    pins_load();
    // 0x71 is TonexOneController's address; 0x3E/0x3F would clash with the board's CH422G.
    static const uint8_t addresses[] = { 0x71, 0x70 };
    for (size_t i = 0; i < sizeof(addresses); i++) {
        if (i2c_master_probe(bus, addresses[i], 50) != ESP_OK) continue;
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &config, &s_dev) != ESP_OK) return false;

        // Software reset, then all pins as inputs with pull-ups.
        write_reg(REG_RESET, 0x12);
        write_reg(REG_RESET, 0x34);
        write_reg(REG_INPUT_DISABLE_B, 0x00);
        write_reg(REG_INPUT_DISABLE_B + 1, 0x00);
        write_reg(REG_PULLDOWN_B, 0x00);
        write_reg(REG_PULLDOWN_A, 0x00);
        write_reg(REG_PULLUP_B, 0xFF);
        write_reg(REG_PULLUP_A, 0xFF);
        write_reg(REG_DIR_B, 0xFF);
        write_reg(REG_DIR_A, 0xFF);

        ESP_LOGI(TAG, "SX1509 found at 0x%02X - footswitches active", addresses[i]);
        log_pins();
        xTaskCreate(footswitch_task, "footswitch", 3072, NULL, 5, NULL);
        return true;
    }
    ESP_LOGW(TAG, "No SX1509 at 0x71/0x70 - footswitches off");
    return false;
}
