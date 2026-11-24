// Browser build: the parts of ESP-IDF the shared code uses (see web/README.md).
#pragma once
#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110

const char *esp_err_to_name(esp_err_t err);

#define ESP_ERROR_CHECK(x) do { esp_err_t err_ = (x); \
    if (err_ != ESP_OK) printf("E %s:%d %s failed: %s\n", __FILE__, __LINE__, #x, esp_err_to_name(err_)); } while (0)
