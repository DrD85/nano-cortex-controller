// Bluetooth LE MIDI for a phone or tablet (looper control): the controller is a Bluetooth MIDI device that an
// iPhone or iPad connects to - in Loopy Pro: main menu > Bluetooth Devices. The footswitches of the looper mode
// go out as control changes; what the app sends back (feedback meant for a controller's LEDs) comes in through on_message.
//
// The service lives in the controller's GATT server next to the app bridge (app_link), which also advertises it
// and tells this module when the phone subscribed or left.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_ble.h"

// Phone connected (it listens to the MIDI characteristic) or gone. Called from the NimBLE host task.
typedef void (*phone_state_cb)(bool connected);

// Registers the GATT service. Called by nano_link before the Bluetooth host starts.
void phone_midi_register(void);

// on_message: a channel message from the phone. Called from the NimBLE host task.
void phone_midi_start(midi_message_cb on_message, phone_state_cb on_state);

bool phone_midi_connected(void);

// Connection interval in ms (0 = not connected): a message waits for the next connection event, so this is the
// longest it is on its way.
float phone_midi_interval_ms(void);

// Sends one channel message at once. May be called from any task. False if no phone is listening.
bool phone_midi_send(uint8_t status, uint8_t data1, uint8_t data2);

#ifndef NANO_WEB
// ---- for app_link, which owns the advertising and the incoming connections ----
#include "host/ble_hs.h"

extern const ble_uuid128_t PHONE_MIDI_SERVICE_UUID;
uint16_t phone_midi_conn(void);                           // BLE_HS_CONN_HANDLE_NONE if there is none
bool phone_midi_subscribe(uint16_t conn, uint16_t attr_handle, bool notify);   // true if it was the MIDI characteristic
void phone_midi_disconnected(uint16_t conn);
void phone_midi_conn_updated(uint16_t conn, int status);
#endif
