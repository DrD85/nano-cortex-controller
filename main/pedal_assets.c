#include "pedal_assets.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) && !defined(NANO_WEB)
#include "esp_flash.h"
#include "esp_log.h"
#include "spi_flash_mmap.h"
#define ON_BOARD 1
#endif

// The file: "NPB1", count, version, size of the file; `count` entries; then the pictures.
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t count, version;
    uint32_t size;
} head_t;
typedef struct __attribute__((packed)) {
    uint32_t type, offset;
    uint16_t w, h;
} entry_t;
#define MAX_PICTURES 160

static lv_image_dsc_t *s_images;
static uint32_t *s_types;
static int s_count;

static bool head_ok(const head_t *head)
{
    return !memcmp(head->magic, "NPB1", 4) && head->version == 1 && head->count > 0 && head->count <= MAX_PICTURES &&
           head->size >= sizeof(head_t) + (size_t)head->count * sizeof(entry_t) && head->size <= PEDALBOARD_MAX;
}

bool pedal_assets_load(const uint8_t *data, size_t size)
{
    head_t head;
    if (!data || size < sizeof(head)) return false;
    memcpy(&head, data, sizeof(head));
    if (!head_ok(&head) || head.size > size) return false;
    lv_image_dsc_t *images = calloc(head.count, sizeof(*images));
    uint32_t *types = calloc(head.count, sizeof(*types));
    if (!images || !types) {
        free(images);
        free(types);
        return false;
    }
    int count = 0;
    for (int i = 0; i < head.count; i++) {
        entry_t e;
        memcpy(&e, data + sizeof(head) + (size_t)i * sizeof(e), sizeof(e));
        size_t bytes = (size_t)e.w * e.h * 3;   // RGB565, then the alpha values
        if (!e.type || !e.w || !e.h || e.w > 800 || e.h > 480 || e.offset > head.size || bytes > head.size - e.offset) continue;
        types[count] = e.type;
        images[count].header.magic = LV_IMAGE_HEADER_MAGIC;
        images[count].header.cf = LV_COLOR_FORMAT_RGB565A8;
        images[count].header.w = e.w;
        images[count].header.h = e.h;
        images[count].header.stride = (uint32_t)e.w * 2;
        images[count].data_size = (uint32_t)bytes;
        images[count].data = data + e.offset;
        count++;
    }
    free(s_images);
    free(s_types);
    s_images = images;
    s_types = types;
    s_count = count;
    return count > 0;
}

void pedal_assets_init(void)
{
#ifdef ON_BOARD
    static const char *TAG = "pedals";
    head_t head;
    if (esp_flash_read(NULL, &head, PEDALBOARD_OFFSET, sizeof(head)) != ESP_OK || !head_ok(&head)) {
        ESP_LOGI(TAG, "No pedal pictures in the flash memory - the pedal view draws its pedals");
        return;
    }
    const void *map = NULL;
    spi_flash_mmap_handle_t handle;
    size_t size = (head.size + 0xFFFFu) & ~(size_t)0xFFFFu;   // whole 64 KB pages
    esp_err_t err = spi_flash_mmap(PEDALBOARD_OFFSET, size, SPI_FLASH_MMAP_DATA, &map, &handle);
    if (err != ESP_OK || !map) {
        ESP_LOGW(TAG, "Pedal pictures found, but they could not be mapped (0x%X)", (unsigned)err);
        return;
    }
    if (pedal_assets_load(map, head.size)) ESP_LOGI(TAG, "Pedal pictures: %d (%u KB, drawn from the flash memory)", s_count, (unsigned)(head.size / 1024));
    else spi_flash_munmap(handle);
#endif
}

const lv_image_dsc_t *pedal_asset(uint32_t type)
{
    for (int i = 0; type && i < s_count; i++) {
        if (s_types[i] == type) return &s_images[i];
    }
    return NULL;
}
