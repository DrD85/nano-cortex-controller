// Waveshare ESP32-S3-Touch-LCD-4.3 (dual USB-C version): 800x480 RGB display, GT911 touch,
// CH422G IO expander for display reset, touch reset and backlight.
// Pins and timings follow TonexOneController (platform_ws43.c / main.h, Apache-2.0).
#pragma once

#include "driver/i2c_master.h"
#include "lvgl.h"

// Starts display, touch and LVGL (esp_lvgl_port). Use lvgl_port_lock()/unlock() around LVGL calls.
lv_display_t *board_display_init(void);

// The board's I2C bus (GPIO8/9): touch, CH422G and the external SX1509 footswitch expander.
i2c_master_bus_handle_t board_i2c_bus(void);
