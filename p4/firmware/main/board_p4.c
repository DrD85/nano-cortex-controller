// Waveshare ESP32-P4-WIFI6-Touch-LCD-7B: 7" 1024 x 600 MIPI-DSI display (EK79007, 2 lanes), GT911 touch on the
// I2C bus (GPIO7 SDA / GPIO8 SCL, shared with the audio codec and the SX1509 footswitch board), backlight on GPIO32,
// LCD reset on GPIO33. Pins and timings from the Waveshare schematic and BSP 1.0.4 (PLAN.md).
//
// STATUS: builds; not yet run on hardware. To check on the board first: the LDO channel / voltage of the DSI PHY,
// the backlight's polarity, the GT911's address.
#include "board.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "src/core/lv_global.h"
#include "src/misc/lv_event_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define LCD_H_RES 1024
#define LCD_V_RES 600
#define I2C_SDA GPIO_NUM_7
#define I2C_SCL GPIO_NUM_8
#define LCD_BACKLIGHT GPIO_NUM_32
#define LCD_RESET GPIO_NUM_33
#define DSI_PHY_LDO_CHANNEL 3       // as on Espressif's P4 boards (to check)
#define DSI_PHY_LDO_MV 2500
#define DSI_LANES 2
#define DSI_LANE_MBPS 1000          // Waveshare BSP

static i2c_master_bus_handle_t s_i2c;

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
    return i2c_new_master_bus(&bus, &s_i2c);
}

static void backlight(bool on)
{
    const gpio_config_t io = { .pin_bit_mask = 1ULL << LCD_BACKLIGHT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    // The backlight is on while the pin is LOW: Waveshare's BSP (1.0.4 and 3.0.1) drives it by PWM with
    // output_invert, 100 % = low. (Brightness by LEDC later: Settings > Display - inverted as well.)
    gpio_set_level(LCD_BACKLIGHT, !on);
}

// Brightness: PWM on the backlight pin, inverted like the pin itself (low = light). At 100 % there is no PWM at all
// (the pin simply stays low); it starts with the first lower value. 20 kHz: above what one can hear.
static int s_brightness = 100;
static bool s_pwm;

int board_brightness(void)
{
    return s_brightness;
}

void board_set_brightness(int percent, bool keep)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    if (!s_pwm && percent < 100) {
        const ledc_timer_config_t timer = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                            .timer_num = LEDC_TIMER_1, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK };
        const ledc_channel_config_t channel = { .gpio_num = LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
                                                .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1, .duty = 1023,
                                                .hpoint = 0, .flags.output_invert = 1 };
        if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) {
            ESP_LOGW(TAG, "Brightness: no PWM on the backlight pin - it stays at 100 %%");
            backlight(true);
            return;
        }
        s_pwm = true;
    }
    if (s_pwm) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (uint32_t)(1023 * percent / 100));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    }
    if (keep) {
        nvs_handle_t nvs;
        if (nvs_open("p4", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, "bright", (uint8_t)percent);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
}

static esp_lcd_panel_handle_t init_panel(void)
{
    // The DSI PHY is powered from one of the P4's internal LDOs.
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t ldo_config = { .chan_id = DSI_PHY_LDO_CHANNEL, .voltage_mv = DSI_PHY_LDO_MV };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &ldo));

    esp_lcd_dsi_bus_handle_t bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = 0,   // default
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &bus));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dbi_config, &io));

    // EK79007_1024_600_PANEL_60HZ_CONFIG of the driver, written out: IDF 6 renamed pixel_format to in_color_format.
    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 52,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,   // LVGL draws into the driver's two frame buffers (no tearing)
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 160,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 1,
            .vsync_back_porch = 23,
            .vsync_front_porch = 12,
        },
    };
    ek79007_vendor_config_t vendor = {
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi_config, .lane_num = DSI_LANES },
    };
    const esp_lcd_panel_dev_config_t dev_config = {
        .reset_gpio_num = LCD_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(io, &dev_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

// Bring-up: every new touch goes to the log twice - where the touch chip saw the finger (its own coordinates,
// before any mirroring) and what the screen made of it (the position and the object under it).
static void touch_seen(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *count,
                       uint8_t max)
{
    static int64_t last;
    if (!*count) return;
    int64_t now = esp_timer_get_time();
    if (now - last > 300000) ESP_LOGI(TAG, "Touch chip: finger at %u, %u", x[0], y[0]);
    last = now;
}

static void touch_pressed(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_t *obj = lv_indev_get_active_obj();
    lv_area_t a = { 0 };
    if (obj) lv_obj_get_coords(obj, &a);
    ESP_LOGI(TAG, "Screen: pressed at %d, %d on an object from %d, %d to %d, %d", (int)p.x, (int)p.y, (int)a.x1,
             (int)a.y1, (int)a.x2, (int)a.y2);
}

// Bring-up, found on 2026-11-18: LVGL keeps a list of the events being handled (one entry per nested event, each on
// the stack of the function that sends it). Between two rounds of its task nothing is being handled, so the list has
// to be empty there. On the board it was not - an object deleted next then walked into a stale entry and the
// firmware crashed (lv_event_mark_deleted). This watch runs between the rounds: it reports a left-over entry and
// empties the list, which is the state it has to be in.
static void event_list_watch(lv_timer_t *timer)
{
    lv_event_t *e = LV_GLOBAL_DEFAULT()->event_header;
    if (!e) return;
    LV_GLOBAL_DEFAULT()->event_header = NULL;
    static int reports;
    if (reports++ >= 20) return;
    uint32_t w[8] = { 0 };
    uintptr_t a = (uintptr_t)e;
    if ((a >= 0x4ff00000 && a < 0x4ffc0000) || (a >= 0x48000000 && a < 0x4a000000)) memcpy(w, e, sizeof(w));
    int here;
    ESP_LOGE(TAG, "Event list not empty between rounds: entry %p (this task's stack is near %p): target %08x %08x, "
             "code %u, user %08x, param %08x, prev %08x", e, &here, (unsigned)w[0], (unsigned)w[1], (unsigned)w[2],
             (unsigned)w[3], (unsigned)w[4], (unsigned)w[5]);
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
        .rst_gpio_num = -1,   // the 7B has no touch reset / interrupt line to the P4
        .int_gpio_num = -1,
        // The touch panel counts from the corner opposite to the display's first pixel (as Espressif's own table says
        // for this EK79007 / GT911 panel at "rotation 0"). Seen on the board 2026-11-18: taps at the top left arrived
        // at the bottom right.
        .flags = { .mirror_x = 1, .mirror_y = 1 },
        .process_coordinates = touch_seen,
    };
    for (int attempt = 0; attempt < 5; attempt++) {
        esp_lcd_touch_handle_t touch = NULL;
        if (esp_lcd_touch_new_i2c_gt911(io, &config, &touch) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        // What the touch chip itself is set to (its own memory, written by the maker of the panel): the size it
        // reports positions in, how many fingers, and its switches (bit 7 Y reversed, bit 6 X reversed, bit 3 X/Y
        // swapped, bits 0-1 how its INT line signals).
        uint8_t c[7] = { 0 }, status = 0;
        if (esp_lcd_panel_io_rx_param(io, 0x8047, c, sizeof(c)) == ESP_OK &&
            esp_lcd_panel_io_rx_param(io, 0x814E, &status, 1) == ESP_OK)
            ESP_LOGI(TAG, "Touch chip: version %d, set to %d x %d, %d fingers, switches 0x%02X, status 0x%02X", c[0],
                     c[1] | c[2] << 8, c[3] | c[4] << 8, c[5] & 0x0F, c[6], status);
        return touch;
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
    backlight(false);
    esp_lcd_panel_handle_t panel = init_panel();
    esp_lcd_touch_handle_t touch = init_touch();

    // LVGL draws on core 1; core 0 runs USB, the footswitches and the app.
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_stack = 16384;   // 8192 left 1.3 KB at the most on this chip (System page, 2026-11-18)
    port_config.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    const lvgl_port_display_cfg_t display_config = {
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_spiram = true, .direct_mode = true },
    };
    const lvgl_port_display_dsi_cfg_t dsi_config = { .flags = { .avoid_tearing = true } };
    lv_display_t *display = lvgl_port_add_disp_dsi(&display_config, &dsi_config);

    if (touch) {
        const lvgl_port_touch_cfg_t touch_config = { .disp = display, .handle = touch };
        lv_indev_t *indev = lvgl_port_add_touch(&touch_config);
        if (indev && lvgl_port_lock(0)) {
            lv_indev_add_event_cb(indev, touch_pressed, LV_EVENT_PRESSED, NULL);
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGW(TAG, "Touch controller not found");
    }
    if (lvgl_port_lock(0)) {
        lv_timer_create(event_list_watch, 1, NULL);
        lvgl_port_unlock();
    }
    backlight(true);
    nvs_handle_t nvs;   // the brightness chosen in Settings > Display
    if (nvs_open("p4", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t percent = 100;
        if (nvs_get_u8(nvs, "bright", &percent) == ESP_OK && percent < 100) board_set_brightness(percent, false);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Display ready (1024 x 600, MIPI-DSI), brightness %d %%", s_brightness);
    return display;
}
