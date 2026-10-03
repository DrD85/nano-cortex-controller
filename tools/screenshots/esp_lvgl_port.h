// Host stub for the screenshot tool: LVGL runs in one thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>
static inline bool lvgl_port_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
static inline void lvgl_port_unlock(void) {}
