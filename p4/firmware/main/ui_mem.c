// LVGL's memory. The screen is thousands of small objects; with the C library's malloc all of them went into
// internal RAM until it was full (18 KB were left after the start) - the RAM Bluetooth lives on. Here they take
// internal RAM only while plenty of it is free and PSRAM after that, so the reserve stays for the radio.
// (On the computer - screenshots, browser version - this is plain malloc.)
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"

#define UI_INTERNAL_RESERVE (96 * 1024)   // internal RAM that stays free of the screen's objects

static void *ui_alloc(size_t size)
{
    void *p = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > UI_INTERNAL_RESERVE + size) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

static void *ui_realloc(void *p, size_t size)
{
    void *q = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // (a block in internal RAM moves over)
    return q ? q : heap_caps_realloc(p, size, MALLOC_CAP_DEFAULT);
}

static void ui_free(void *p)
{
    heap_caps_free(p);
}
#else
#define ui_alloc malloc
#define ui_realloc realloc
#define ui_free free
#endif

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return ui_alloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return ui_realloc(p, new_size);
}

void lv_free_core(void *p)
{
    ui_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif
