// Browser build: only the types nano_link.h names (Bluetooth runs through Web Bluetooth, web/nano_link_web.c).
#pragma once
#include <stdint.h>
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
struct ble_hs_adv_fields;
struct ble_gap_event;
typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);
