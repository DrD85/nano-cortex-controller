// Bluetooth LE MIDI client: a second Bluetooth connection, next to the Nano, to a MIDI controller
// (for example a Morningstar MC6 with a WIDI adapter). Uses the scanning of nano_link.
// The chosen device is stored and connected again automatically.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIDI_MAX_DEVICES 8

// A channel message (status 0x80-0xEF, data bytes). Called from the NimBLE host task.
typedef void (*midi_message_cb)(uint8_t status, uint8_t data1, uint8_t data2);

// Device list or connection changed. Called from the NimBLE host task.
typedef void (*midi_change_cb)(void);

typedef struct {
    char name[32];
    int8_t rssi;
    bool remembered;
} midi_device_t;

// Call after nano_link_start().
void midi_ble_start(midi_message_cb on_message, midi_change_cb on_change);

// Searching while the MIDI dialog is open (otherwise only the stored device is looked for).
void midi_ble_search(bool on);

// False while the Nano connects and loads its presets: no MIDI scanning or connecting meanwhile
// (an existing MIDI connection stays). True at start.
void midi_ble_allow(bool allowed);

// Devices found with the MIDI service, newest search. Returns the count.
int midi_ble_devices(midi_device_t *out, int max);

// Connect device [index] of midi_ble_devices() and remember it. False if it is not possible now.
bool midi_ble_connect(int index);

// Disconnect and forget the stored device.
void midi_ble_forget(void);

// True while connected; name of the connected (or stored) device in name.
bool midi_ble_status(char *name, size_t size);
