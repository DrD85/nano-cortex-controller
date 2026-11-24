// A new firmware over Wi-Fi (see p4_ota.h).
#include "p4_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4_ota";

#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define KEEP_AFTER_MS 30000

struct p4_ota {
    const esp_partition_t *part;
    esp_ota_handle_t handle;
    bool begun;
    size_t total, got;
    uint8_t head[HEAD_LEN];
    size_t head_len;
};

static char s_new_version[32];

// ---- keeping a new firmware ----

static void keep_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(KEEP_AFTER_MS));
    // the screen's task has to answer too (a firmware that runs but shows nothing is not kept)
    for (int tries = 0; tries < 10; tries++) {
        if (lvgl_port_lock(500)) {
            lvgl_port_unlock();
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "The new firmware works - kept (%s)", esp_err_to_name(err));
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "The screen does not answer: back to the firmware before");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    vTaskDelete(NULL);
}

void p4_ota_start(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &state);
    ESP_LOGI(TAG, "Firmware %s (%s %s) in %s", app->version, app->date, app->time, run ? run->label : "?");
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "New firmware: kept once it has run for %d s", KEEP_AFTER_MS / 1000);
        xTaskCreatePinnedToCore(keep_task, "ota_keep", 3072, NULL, 2, NULL, 1);
    }
}

// ---- receiving ----

static bool refuse(char *why, size_t n, const char *text)
{
    snprintf(why, n, "%s", text);
    return false;
}

// The file's start: an ESP32-P4 image of this project?
static bool check_head(p4_ota_t *o, char *why, size_t n)
{
    const esp_image_header_t *h = (const esp_image_header_t *)o->head;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(o->head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *own = esp_app_get_description();
    if (h->magic != ESP_IMAGE_HEADER_MAGIC) return refuse(why, n, "not a firmware file (use build/nano_controller_p4.bin)");
    if (h->chip_id != ESP_CHIP_ID_ESP32P4) return refuse(why, n, "a firmware for another chip (the small board?)");
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return refuse(why, n, "a firmware file without its description");
    if (strncmp(d->project_name, own->project_name, sizeof(d->project_name)) != 0) {
        snprintf(why, n, "a firmware of another project (%.31s), not %.31s", d->project_name, own->project_name);
        return false;
    }
    snprintf(s_new_version, sizeof(s_new_version), "%.31s", d->version);
    ESP_LOGI(TAG, "Update %s -> %s (%.15s %.15s), %u KB", own->version, d->version, d->date, d->time, (unsigned)(o->total / 1024));
    return true;
}

p4_ota_t *p4_ota_open(size_t total, char *why, size_t why_len)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        refuse(why, why_len, "this board has only one firmware place (flash partitions.csv by cable once)");
        return NULL;
    }
    if (total < HEAD_LEN || total > part->size) {
        snprintf(why, why_len, "%u bytes do not fit (%u KB room)", (unsigned)total, (unsigned)(part->size / 1024));
        return NULL;
    }
    p4_ota_t *o = calloc(1, sizeof(*o));
    if (!o) {
        refuse(why, why_len, "no memory");
        return NULL;
    }
    o->part = part;
    o->total = total;
    s_new_version[0] = 0;
    return o;
}

bool p4_ota_write(p4_ota_t *o, const void *data, size_t len, char *why, size_t why_len)
{
    const uint8_t *p = data;
    if (!o->begun) {   // the start is collected first and checked before anything is written
        size_t take = HEAD_LEN - o->head_len < len ? HEAD_LEN - o->head_len : len;
        memcpy(o->head + o->head_len, p, take);
        o->head_len += take;
        p += take;
        len -= take;
        if (o->head_len < HEAD_LEN) return true;
        if (!check_head(o, why, why_len)) return false;
        // erased bit by bit while writing (no long stop at the start)
        esp_err_t err = esp_ota_begin(o->part, OTA_WITH_SEQUENTIAL_WRITES, &o->handle);
        if (err != ESP_OK) {
            snprintf(why, why_len, "could not start (%s)", esp_err_to_name(err));
            return false;
        }
        o->begun = true;
        if (esp_ota_write(o->handle, o->head, HEAD_LEN) != ESP_OK) return refuse(why, why_len, "could not write");
        o->got = HEAD_LEN;
    }
    if (len && esp_ota_write(o->handle, p, len) != ESP_OK) return refuse(why, why_len, "could not write");
    o->got += len;
    return true;
}

bool p4_ota_finish(p4_ota_t *o, char *why, size_t why_len)
{
    bool ok = false;
    if (!o->begun || o->got != o->total) {
        refuse(why, why_len, "the file is not complete");
    } else {
        esp_err_t err = esp_ota_end(o->handle);   // checks the image and its checksum
        o->begun = false;
        if (err != ESP_OK) snprintf(why, why_len, "the file is damaged (%s)", esp_err_to_name(err));
        else if ((err = esp_ota_set_boot_partition(o->part)) != ESP_OK) snprintf(why, why_len, "not chosen (%s)", esp_err_to_name(err));
        else ok = true;
    }
    if (ok) ESP_LOGI(TAG, "New firmware %s in %s - starts after the restart", s_new_version, o->part->label);
    p4_ota_abort(o);
    return ok;
}

void p4_ota_abort(p4_ota_t *o)
{
    if (!o) return;
    if (o->begun) esp_ota_abort(o->handle);
    free(o);
}

static void restart(void *arg)
{
    ESP_LOGW(TAG, "Restart for the new firmware");
    esp_restart();
}

void p4_ota_restart_soon(int ms)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart, .name = "ota_restart" };
    if (!timer && esp_timer_create(&args, &timer) != ESP_OK) return;
    esp_timer_start_once(timer, (uint64_t)ms * 1000);
}

const char *p4_ota_new_version(void)
{
    return s_new_version;
}
