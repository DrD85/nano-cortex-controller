#pragma once
#include "FreeRTOS.h"
typedef struct web_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
// Never blocks: wait is ignored (an empty queue returns pdFALSE at once).
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
