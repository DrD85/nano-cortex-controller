// Switches of a MIDI controller (Bluetooth MIDI, or Web MIDI in the browser build) learned as footswitches: the
// footswitch learn takes the next press that comes over MIDI as well as the next pressed footswitch. A learned switch
// then is that footswitch in every way - its press, its release and its hold (FOOTSWITCH_HOLD_MS), so held functions
// (looper mode, scenes, A / B) work from it too.
//
// What counts as a press: a control change with a value of 64-127 (below 64 is its release), a note on (note off or
// velocity 0 is its release) or a program change (a press without a release). Channel and number must match. CC 1 is
// the expression pedal and is never learned. A learned message comes before every fixed MIDI mapping (CC 50-57, the
// Chocolate's PCs ...), which still works for everything not learned.
//
// Threads: midi_switches_take is called from the MIDI callback (the Bluetooth host task); presses, releases and holds
// reach on_event from one task of their own (in the browser: from its main loop), never from two at once.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "footswitches.h"

// on_learn: number = the footswitch that got the message (0 = nothing pressed in time), other = the footswitch that
// had this message before (0 = none). The table is stored on the board ("nano" / "midi_sw").
void midi_switches_start(footswitch_cb on_event, footswitch_learn_cb on_learn);

// From the MIDI callback: true when the message was a learned switch's (or was learned now) - nothing else may act on
// it then.
bool midi_switches_take(uint8_t status, uint8_t data1, uint8_t data2);

// 1-8: the next press over MIDI becomes footswitch number (15 s), 0 = cancel.
void midi_switches_learn(int number);

// Forgets every learned message.
void midi_switches_reset(void);

// "CC 20 on channel 1" for footswitch number; false (and "") when it has none.
bool midi_switches_describe(int number, char *text, size_t size);
