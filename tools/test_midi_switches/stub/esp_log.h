#pragma once
#include <stdio.h>
#define ESP_LOGI(t, f, ...) printf("I %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) printf("W %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGE(t, f, ...) printf("E %s: " f "\n", t, ##__VA_ARGS__)
