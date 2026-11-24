// What footswitches.c needs from ESP-IDF, for the test on the computer (test_footswitches.c holds the other side:
// a simulated SX1509, GPIO levels, a clock and a small key / value store).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)

void stub_log(char level, const char *tag, const char *format, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGI(tag, ...) stub_log('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) stub_log('W', tag, __VA_ARGS__)

// FreeRTOS: one tick is one millisecond.
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void (*TaskFunction_t)(void *);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
int xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *arg, int priority, void *handle);

// Settings store
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

// I2C
typedef struct stub_i2c_bus *i2c_master_bus_handle_t;
typedef struct stub_i2c_dev *i2c_master_dev_handle_t;
typedef enum { I2C_ADDR_BIT_LEN_7 } i2c_addr_bit_len_t;
typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config, i2c_master_dev_handle_t *device);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t size, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *out, size_t out_size, uint8_t *in, size_t in_size, int timeout_ms);

// GPIO
typedef int gpio_num_t;
enum { GPIO_NUM_2 = 2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_28 = 28, GPIO_NUM_29, GPIO_NUM_30, GPIO_NUM_31 };
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
int gpio_get_level(gpio_num_t gpio);
