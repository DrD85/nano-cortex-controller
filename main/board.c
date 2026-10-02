#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define LCD_H_RES 800
#define LCD_V_RES 480
#define LCD_PIXEL_CLOCK_HZ (15 * 1000 * 1000)

#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_9
#define TOUCH_INT GPIO_NUM_4

// CH422G: commands are sent as I2C addresses.
#define CH422G_MODE 0x24     // 0x01 = EXIO pins are outputs
#define CH422G_IO_OUT 0x38   // output levels of EXIO0-7
#define EXIO_TOUCH_RST (1 << 1)
#define EXIO_BACKLIGHT (1 << 2)
#define EXIO_LCD_RST (1 << 3)
// EXIO5 (USB_SEL) stays low: high switches GPIO19/20 from the native USB port to CAN.

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_ch422g_mode, s_ch422g_out;

static esp_err_t ch422g_write(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, 50);
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C bus");
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH422G_MODE,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_ch422g_mode), TAG, "CH422G mode");
    dev.device_address = CH422G_IO_OUT;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_ch422g_out), TAG, "CH422G output");
    return ESP_OK;
}

// Resets the GT911. It takes I2C address 0x5D when INT is low while reset is released.
static void reset_touch(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << TOUCH_INT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    ch422g_write(s_ch422g_out, EXIO_LCD_RST);    // touch in reset, backlight off
    ch422g_write(s_ch422g_mode, 0x01);           // EXIO pins as outputs
    ch422g_write(s_ch422g_out, EXIO_LCD_RST);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ch422g_write(s_ch422g_out, EXIO_LCD_RST | EXIO_TOUCH_RST);
    vTaskDelay(pdMS_TO_TICKS(200));
    io.mode = GPIO_MODE_INPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
}

static esp_lcd_panel_handle_t init_panel(void)
{
    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .bounce_buffer_size_px = 10 * LCD_H_RES,
        .dma_burst_size = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        // B3-B7, G2-G7, R3-R7
        .data_gpio_nums = { 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 },
        .flags.fb_in_psram = 1,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

static esp_lcd_touch_handle_t init_touch(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_config, &io) != ESP_OK) return NULL;

    const esp_lcd_touch_config_t config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
    };
    for (int attempt = 0; attempt < 5; attempt++) {
        esp_lcd_touch_handle_t touch = NULL;
        if (esp_lcd_touch_new_i2c_gt911(io, &config, &touch) == ESP_OK) return touch;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return NULL;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

lv_display_t *board_display_init(void)
{
    ESP_ERROR_CHECK(init_i2c());
    reset_touch();
    esp_lcd_panel_handle_t panel = init_panel();
    esp_lcd_touch_handle_t touch = init_touch();

    // LVGL draws on core 1; core 0 runs Bluetooth and the display's bounce-buffer interrupt.
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_stack = 8192;
    port_config.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    const lvgl_port_display_cfg_t display_config = {
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .direct_mode = true },
    };
    const lvgl_port_display_rgb_cfg_t rgb_config = {
        .flags = { .bb_mode = true, .avoid_tearing = true },
    };
    lv_display_t *display = lvgl_port_add_disp_rgb(&display_config, &rgb_config);

    if (touch) {
        const lvgl_port_touch_cfg_t touch_config = { .disp = display, .handle = touch };
        lvgl_port_add_touch(&touch_config);
    } else {
        ESP_LOGW(TAG, "Touch controller not found");
    }

    ch422g_write(s_ch422g_out, EXIO_LCD_RST | EXIO_TOUCH_RST | EXIO_BACKLIGHT);
    ESP_LOGI(TAG, "Display ready");
    return display;
}
