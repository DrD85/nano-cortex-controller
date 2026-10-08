// Browser build of the board: the 800x480 screen is a canvas, the touch panel the mouse or finger on it, the
// footswitches are buttons below it (web/index.html). The app bridge does not exist in the browser.
#include "board.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "app_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "footswitches.h"
#include "nvs.h"

#define W 800
#define H 480

void app_main(void);           // main.c
void web_platform_run(void);   // platform.c

static uint16_t *s_draw;   // LVGL draws the whole screen here (RGB565)
static uint32_t *s_rgba;   // the canvas pixels
static int s_x1 = W, s_y1 = H, s_x2 = -1, s_y2 = -1;   // changed area since the last copy to the canvas

// Copies the changed area to the canvas (Module.ctx, set by app.js).
EM_JS(void, js_blit, (const uint32_t *rgba, int x, int y, int w, int h), {
    if (!Module.ctx) return;
    if (!Module.image || Module.image.data.buffer !== HEAPU8.buffer || Module.imagePtr !== rgba) {
        Module.image = new ImageData(new Uint8ClampedArray(HEAPU8.buffer, rgba, 800 * 480 * 4), 800, 480);
        Module.imagePtr = rgba;
    }
    Module.ctx.putImageData(Module.image, 0, 0, x, y, w, h);
});

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)px_map;   // direct mode: the area is in place in s_draw
    for (int y = area->y1; y <= area->y2; y++) {
        const uint16_t *src = s_draw + y * W;
        uint32_t *dst = s_rgba + y * W;
        for (int x = area->x1; x <= area->x2; x++) {
            uint16_t c = src[x];
            uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            dst[x] = 0xFF000000u | b << 16 | g << 8 | r;   // RGBA bytes in memory
        }
    }
    if (area->x1 < s_x1) s_x1 = area->x1;
    if (area->y1 < s_y1) s_y1 = area->y1;
    if (area->x2 > s_x2) s_x2 = area->x2;
    if (area->y2 > s_y2) s_y2 = area->y2;
    if (lv_display_flush_is_last(disp) && s_x2 >= s_x1) {
        js_blit(s_rgba, s_x1, s_y1, s_x2 - s_x1 + 1, s_y2 - s_y1 + 1);
        s_x1 = W;
        s_y1 = H;
        s_x2 = s_y2 = -1;
    }
    lv_display_flush_ready(disp);
}

// ---- touch: mouse or finger on the canvas ----
// Presses and releases are queued, so a quick tap between two reads is not lost.

#define POINTER_EVENTS 32
static struct { int16_t x, y; bool pressed; } s_pointer[POINTER_EVENTS], s_last;
static int s_pointer_head, s_pointer_count;

EMSCRIPTEN_KEEPALIVE void web_pointer(int x, int y, int pressed)
{
    if (x < 0) x = 0;
    if (x >= W) x = W - 1;
    if (y < 0) y = 0;
    if (y >= H) y = H - 1;
    if (s_pointer_count && !pressed == !s_pointer[(s_pointer_head + s_pointer_count - 1) % POINTER_EVENTS].pressed) {
        // a move: update the newest entry instead of queueing
        int last = (s_pointer_head + s_pointer_count - 1) % POINTER_EVENTS;
        s_pointer[last].x = (int16_t)x;
        s_pointer[last].y = (int16_t)y;
        return;
    }
    if (s_pointer_count == POINTER_EVENTS) return;
    int i = (s_pointer_head + s_pointer_count++) % POINTER_EVENTS;
    s_pointer[i].x = (int16_t)x;
    s_pointer[i].y = (int16_t)y;
    s_pointer[i].pressed = pressed != 0;
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_pointer_count) {
        s_last = s_pointer[s_pointer_head];
        s_pointer_head = (s_pointer_head + 1) % POINTER_EVENTS;
        s_pointer_count--;
        data->continue_reading = s_pointer_count > 0 && s_pointer[s_pointer_head].pressed != s_last.pressed;
    }
    data->point.x = s_last.x;
    data->point.y = s_last.y;
    data->state = s_last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_cb(void)
{
    return (uint32_t)emscripten_get_now();
}

lv_display_t *board_display_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    s_draw = calloc(W * H, sizeof(uint16_t));
    s_rgba = calloc(W * H, sizeof(uint32_t));
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_draw, NULL, W * H * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, pointer_read);
    lv_indev_set_display(touch, disp);
    return disp;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return NULL;
}

// ---- footswitches: the eight buttons below the screen (and the keys 1-8) ----
// Learning works as on the board: the next button pressed becomes the footswitch (stored in the browser).

#define STORE_NAMESPACE "footsw"
#define STORE_KEY "map"
#define LEARN_TIMEOUT_MS 15000

static footswitch_cb s_on_press;
static footswitch_learn_cb s_on_learn;
static uint8_t s_map[FOOTSWITCH_COUNT];   // footswitch number of each button
static int s_learn;
static bool s_swallow[FOOTSWITCH_COUNT];  // the press that was learned: no release / hold for it
static esp_timer_handle_t s_learn_timer;

static void map_store(void)
{
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, STORE_KEY, s_map, sizeof(s_map));
    nvs_close(nvs);
}

static void learn_timeout(void *arg)
{
    (void)arg;
    if (!s_learn) return;
    s_learn = 0;
    if (s_on_learn) s_on_learn(0, 0);
}

bool footswitches_start(i2c_master_bus_handle_t bus, footswitch_cb on_press, footswitch_learn_cb on_learn)
{
    (void)bus;
    s_on_press = on_press;
    s_on_learn = on_learn;
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) s_map[i] = (uint8_t)(i + 1);
    nvs_handle_t nvs;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t stored[FOOTSWITCH_COUNT];
        size_t size = sizeof(stored);
        if (nvs_get_blob(nvs, STORE_KEY, stored, &size) == ESP_OK && size == sizeof(stored)) memcpy(s_map, stored, sizeof(s_map));
        nvs_close(nvs);
    }
    const esp_timer_create_args_t timer = { .callback = learn_timeout, .name = "learn" };
    esp_timer_create(&timer, &s_learn_timer);
    return true;
}

void footswitches_learn(int number)
{
    s_learn = number >= 1 && number <= FOOTSWITCH_COUNT ? number : 0;
    esp_timer_stop(s_learn_timer);
    if (s_learn) esp_timer_start_once(s_learn_timer, LEARN_TIMEOUT_MS * 1000);
}

void footswitches_reset(void)
{
    for (int i = 0; i < FOOTSWITCH_COUNT; i++) s_map[i] = (uint8_t)(i + 1);
    map_store();
}

// button 1-8, event = footswitch_event_t (the page measures the hold time)
EMSCRIPTEN_KEEPALIVE void web_footswitch(int button, int event)
{
    if (button < 1 || button > FOOTSWITCH_COUNT) return;
    int b = button - 1;
    if (event == FOOTSWITCH_PRESS && s_learn) {
        int number = s_learn, old = s_map[b];
        s_learn = 0;
        esp_timer_stop(s_learn_timer);
        for (int i = 0; i < FOOTSWITCH_COUNT; i++) {
            if (s_map[i] == number) s_map[i] = (uint8_t)old;   // the button that was this footswitch takes the old number
        }
        s_map[b] = (uint8_t)number;
        map_store();
        s_swallow[b] = true;
        ESP_LOGI("footswitch", "Footswitch %d learned: button %d", number, button);
        if (s_on_learn) s_on_learn(number, old != number ? old : 0);
    } else if (s_swallow[b]) {
        if (event == FOOTSWITCH_RELEASE) s_swallow[b] = false;
    } else if (s_on_press) {
        s_on_press(s_map[b], (footswitch_event_t)event);
    }
    web_platform_run();
}

// ---- app bridge: not in the browser ----

void app_link_register(void) {}
void app_link_start(app_write_cb on_write, app_state_cb on_state) { (void)on_write; (void)on_state; }
void app_link_enable(bool enabled) { (void)enabled; }
bool app_link_connected(void) { return false; }
void app_link_send_message(const uint8_t *message, size_t len) { (void)message; (void)len; }
void app_link_send(uint32_t type, const uint8_t *payload, size_t len) { (void)type; (void)payload; (void)len; }

// ---- main loop ----

static void frame(void)
{
    web_platform_run();
    lv_timer_handler();
}

int main(void)
{
    app_main();
    emscripten_set_main_loop(frame, 0, false);
    return 0;
}
