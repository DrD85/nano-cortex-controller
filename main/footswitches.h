// Footswitches on an SX1509 IO expander (I2C address 0x71, as in TonexOneController). All 16 pins are inputs
// with pull-ups; a pressed switch pulls its pin to ground.
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

#define FOOTSWITCH_COUNT 8

// What a footswitch did: pressed, released, or held down for FOOTSWITCH_HOLD_MS (once per press, before the release).
typedef enum { FOOTSWITCH_PRESS, FOOTSWITCH_RELEASE, FOOTSWITCH_HOLD } footswitch_event_t;
#define FOOTSWITCH_HOLD_MS 600

// Called from the footswitch task with the number (1-8) of a switch and what it did.
typedef void (*footswitch_cb)(int number, footswitch_event_t event);

// Called from the footswitch task when a learn ends: number = the learned switch (0 = nothing pressed in time),
// swapped = the switch that got the old pin of the learned one (0 = none).
typedef void (*footswitch_learn_cb)(int number, int swapped);

// Starts polling if an SX1509 is found. Returns false if there is none.
bool footswitches_start(i2c_master_bus_handle_t bus, footswitch_cb on_press, footswitch_learn_cb on_learn);

// Learn: the next pressed SX1509 pin becomes footswitch number (1-8); a switch that had that pin gets the old
// pin of this one. 0 cancels. The assignment is stored on the board.
void footswitches_learn(int number);

// Back to the default order (the wiring of the TonexOneController enclosure).
void footswitches_reset(void);
