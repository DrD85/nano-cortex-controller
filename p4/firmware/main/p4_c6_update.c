// A new firmware for the board's ESP32-C6 (Bluetooth / Wi-Fi, esp_hosted), sent to it over the SDIO link.
//
// The C6's firmware is an esp_hosted "slave" image (built from esp_hosted's slave/ for the esp32c6, the same version
// as the host component here). It is placed into this board's second firmware place, ota_1, with esptool while the
// board runs from ota_0:
//
//     esptool --chip esp32p4 write-flash 0x4A0000 network_adapter.bin
//
// At the next start p4_c6_update_if_staged() finds it there (an ESP image for the C6, not for the P4), takes the
// first sector into memory and erases it in the flash - the update then runs once, whatever happens - and sends the
// image to the C6 in pieces (esp_hosted_slave_ota_begin / write / end). The C6 writes it into its own spare place and
// switches to it only when everything has arrived, so an update that stops halfway leaves its old firmware running.
// Then the board starts again, which also restarts the C6 (esp_hosted resets it at the start).
//
// Nothing of the P4's own firmware, settings or pictures is touched; ota_1 is only the place an update over Wi-Fi
// writes to (it is written whole then).

#include "p4_c6_update.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_update";

#define SECTOR 4096
#define CHUNK 1480   // (as esp_hosted's example: one piece per request)

// The image's size from its header and segments (as esp_hosted's example host_performs_slave_ota).
static size_t image_size(const esp_partition_t *p, const esp_image_header_t *h)
{
    size_t at = sizeof(*h);
    for (int i = 0; i < h->segment_count; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(p, at, &seg, sizeof(seg)) != ESP_OK) return 0;
        at += sizeof(seg) + seg.data_len;
        if (at > p->size) return 0;
    }
    at += (16 - at % 16) % 16;   // padding, then the checksum byte
    at += 1;
    if (h->hash_appended == 1) {
        at += (16 - at % 16) % 16;
        at += 32;                // SHA-256
    }
    return at <= p->size ? at : 0;
}

bool p4_c6_update_if_staged(const char *running_version)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!p || p == running) return false;
    esp_image_header_t h;
    if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;
    if (h.magic != ESP_IMAGE_HEADER_MAGIC || h.chip_id != ESP_CHIP_ID_ESP32C6) return false;   // (a P4 firmware or empty)

    esp_app_desc_t desc;
    char version[33] = "?";
    if (esp_partition_read(p, sizeof(h) + sizeof(esp_image_segment_header_t), &desc, sizeof(desc)) == ESP_OK &&
        desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
        snprintf(version, sizeof(version), "%s", desc.version);
    }
    size_t size = image_size(p, &h);
    ESP_LOGW(TAG, "A firmware for the ESP32-C6 waits in %s: %s, version %s, %u bytes (the C6 runs %s)", p->label,
             desc.magic_word == ESP_APP_DESC_MAGIC_WORD ? desc.project_name : "?", version, (unsigned)size,
             running_version && *running_version ? running_version : "a firmware without a version");
    uint8_t *first = malloc(SECTOR), *chunk = malloc(CHUNK);
    if (!first || !chunk || size < SECTOR || esp_partition_read(p, 0, first, SECTOR) != ESP_OK) {
        ESP_LOGE(TAG, "Not readable - no update");
        free(first);
        free(chunk);
        return false;
    }
    // Once only: the first sector goes out of the flash before anything is sent.
    if (esp_partition_erase_range(p, 0, SECTOR) != ESP_OK) {
        ESP_LOGE(TAG, "Its first sector could not be erased - no update (it would run at every start)");
        free(first);
        free(chunk);
        return false;
    }
    if (running_version && !strcmp(running_version, version)) {
        ESP_LOGW(TAG, "The C6 runs this version already - not sent (removed from %s)", p->label);
        free(first);
        free(chunk);
        return false;
    }

    int64_t started = esp_timer_get_time();
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "The C6 did not start an update (%s) - it keeps its firmware", esp_err_to_name(err));
        free(first);
        free(chunk);
        return false;
    }
    size_t sent = 0;
    while (sent < size && err == ESP_OK) {
        size_t n = size - sent < CHUNK ? size - sent : CHUNK;
        if (sent + n <= SECTOR) memcpy(chunk, first + sent, n);
        else if (sent < SECTOR) {
            size_t a = SECTOR - sent;
            memcpy(chunk, first + sent, a);
            err = esp_partition_read(p, SECTOR, chunk + a, n - a);
        } else err = esp_partition_read(p, sent, chunk, n);
        if (err == ESP_OK) err = esp_hosted_slave_ota_write(chunk, (uint32_t)n);
        if (err == ESP_OK) sent += n;
        if (err == ESP_OK && (sent / CHUNK) % 100 == 0)
            ESP_LOGI(TAG, "%u of %u bytes sent to the C6", (unsigned)sent, (unsigned)size);
    }
    free(first);
    free(chunk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sending stopped at %u of %u bytes (%s) - the C6 keeps its firmware", (unsigned)sent, (unsigned)size,
                 esp_err_to_name(err));
        esp_hosted_slave_ota_end();
        return false;
    }
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "The C6 did not accept the new firmware at the end (%s) - it keeps the old one", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "New C6 firmware sent and checked by the C6: %u bytes in %lld ms", (unsigned)size,
             (esp_timer_get_time() - started) / 1000);
    // From esp_hosted 2.6 on the C6 waits for "activate"; older firmware switches at the end by itself.
    esp_hosted_coprocessor_fwver_t fw = { 0 };
    if (esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK && (fw.major1 > 2 || (fw.major1 == 2 && fw.minor1 > 5))) {
        err = esp_hosted_slave_ota_activate();
        ESP_LOGW(TAG, "Activated on the C6: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "The board starts again (with it the C6)");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return true;
}
