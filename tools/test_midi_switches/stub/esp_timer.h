#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef void *esp_timer_handle_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t h);
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
