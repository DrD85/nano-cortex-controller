// Browser build: no tasks. Queues are plain buffers that the main loop empties (web/platform.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configASSERT(x) do { if (!(x)) printf("assert failed: %s\n", #x); } while (0)
