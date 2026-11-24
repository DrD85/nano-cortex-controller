#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

uint32_t esp_log_timestamp(void);
void esp_log_buffer_hex(const char *tag, const void *buffer, size_t len);

#define ESP_LOGE(tag, fmt, ...) printf("E (%lu) %s: " fmt "\n", (unsigned long)esp_log_timestamp(), tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%lu) %s: " fmt "\n", (unsigned long)esp_log_timestamp(), tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I (%lu) %s: " fmt "\n", (unsigned long)esp_log_timestamp(), tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
#define ESP_LOG_BUFFER_HEX(tag, buffer, len) esp_log_buffer_hex(tag, buffer, len)
