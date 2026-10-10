// App bridge: the controller offers the Nano's Bluetooth service (A002 with C304 / C305) under the name
// "Nano Cortex Controller", so the Nano Cortex Editor (Mac app or browser) can connect to the controller while the
// controller is connected to the Nano: app <-> controller <-> Nano. Without the controller the app connects to the
// Nano directly, as before.
//
// Messages from the app go on to the Nano; replies to them come back to the app (see nano_link). Messages the Nano
// sends on its own go to both. The controller is offered only while it is connected to the Nano.
//
// The same advertisement also offers Bluetooth MIDI to a phone (phone_midi, looper control): a connection that comes
// in is the app's once it subscribes to C305, the phone's once it subscribes to the MIDI characteristic.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A framed message arrived from the app (type = its message type). Called from the NimBLE host task.
typedef void (*app_write_cb)(uint32_t type);
// App connected / disconnected. Called from the NimBLE host task.
typedef void (*app_state_cb)(bool connected);

// Registers the GATT service. Called by nano_link before the Bluetooth host starts.
void app_link_register(void);

void app_link_start(app_write_cb on_write, app_state_cb on_state);

// Offer the controller to the app (only while the Nano is connected); false also disconnects the app.
void app_link_enable(bool enabled);

// Offer Bluetooth MIDI to a phone (phone_midi); false only stops offering it - a connected phone stays.
void app_link_allow_phone(bool allowed);

bool app_link_connected(void);

// Sends a message (payload + u32 type) to the app, split into packets as the Nano does.
void app_link_send_message(const uint8_t *message, size_t len);

// Sends payload with the message type to the app (for example a preset change the controller made).
void app_link_send(uint32_t type, const uint8_t *payload, size_t len);
