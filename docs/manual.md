# Nano Cortex Controller – manual

Every function in detail. The short illustrated guide is in the [README](../README.md); building the firmware and how it talks to the Nano are in [development.md](development.md).

**Contents:** [The screen](#the-screen) · [Footswitches and tiles](#footswitches-and-tiles) · [Long presses](#long-presses) · [Own banks](#own-banks) · [Capture and Cab/IR](#capture-and-cabir) · [FX editor](#fx-editor) · [Reverb switch](#reverb-switch-footswitch-8) · [Second effect on Pre FX 1](#second-effect-on-pre-fx-1-footswitch-3) · [Tuner](#tuner) · [USB audio volume](#usb-audio-volume) · [Bluetooth MIDI](#bluetooth-midi) · [Looper mode](#looper-mode-a-looper-app-on-a-phone) · [Editor through the controller](#nano-cortex-editor-through-the-controller) · [Footswitch learn](#footswitch-learn) · [Footswitches (wiring)](#footswitches) · [What is stored where](#what-is-stored-where)

## The screen

<img src="images/guide-main.png" alt="" width="640">

- **Tap a tile** = press its footswitch. A tile lights up briefly when its footswitch is pressed.
- **Tiles as in the desktop editor's signal chain**: an active preset or an effect that is on is **filled** in its
  colour; everything else is dark with a **coloured border** and its top row on the dimmed colour – the difference
  shows from across the stage. All
  names use one size.
- **↻** (top left) reads everything from the Nano again: preset names, the current preset and the library.
- The **speaker** symbol next to it sets the capture volume of the current preset; **MIDI** and **USB** follow.
- **Swipe left / right**: next / previous preset.
- **Swipe up – gig view**: the tiles fill the screen with larger names, and a slim bar on top shows the
  connection, preset number, bank, preset name, unsaved changes and the mode (PRESETS / FX). **Swipe down**: back.
- The green dot shows the Bluetooth connection to the Nano; **Save** turns green and an orange dot appears next to
  the name when the preset has unsaved changes. **MIDI** turns green while a Bluetooth MIDI controller is connected.
- The **capture card** shows the capture's type (amp head, combo, amp + cab, cab, pedal …) as in the editor.

## Footswitches and tiles

| Switch | Preset mode | FX mode |
|---|---|---|
| 1 | switch to FX mode; **held: looper mode** | switch to preset mode; **held: looper mode** |
| 2 | tuner on/off | tuner on/off |
| 3–8 | the six presets of the current bank | 3–7: Pre FX 1, Pre FX 2, Post FX 1–3 on/off |
| 8 | (sixth preset) | reverb: mix Pos 1 ↔ Pos 2, or reverb A ↔ B |

Active tiles light up in full colour, inactive ones are dimmed. FX tiles use the effect category colours.
Footswitch 1 acts when you lift your foot (it has a held function); holding it for 0.6 s opens the
[looper mode](#looper-mode-a-looper-app-on-a-phone).

## Long presses

| Where | What opens |
|---|---|
| Tile 1 | **Looper mode** on / off (as holding footswitch 1) |
| Tile 2–8 in looper mode | **Looper tile**: name, colour and symbol of this switch |
| Tile 2 | **Learn** for footswitch 2 – and, with *Switch 1* in that dialog, for footswitch 1 |
| Preset tile (3–8, preset mode) | **Bank editor**: colour, symbol and preset of this switch; `DEFAULT` restores the standard preset; `LEARN SWITCH` |
| FX tile (3–7, FX mode) | **FX editor**: model (tap the header), on/off and all parameters |
| Tile 8 (FX mode) | **Reverb dialog** with the tabs *MIX POS 1 / 2* and *2ND REVERB* |
| Preset name | **Rename** with on-screen keyboard (at least 4 characters, unique) |

## Own banks

<img src="images/05-bank-editor.png" alt="" width="440">

16 banks with six switches each. By default bank 1 holds presets 1–6, bank 2 presets 7–12 and so on.
A long press on a preset tile lets you choose any preset, one of ten colours and one of 19 symbols
(Clean, Edge, Drive, Solo, Fuzz, Atmospheric, Metal, Boost, Rhythm, Bass, Acoustic, Blues, Live, Favorite,
Fuzz Wave, Guitarist, Rocket, Space, Swell).
The banks are stored on the controller, the presets themselves stay on the Nano.

## Capture and Cab/IR

<img src="images/06-capture-library.png" alt="" width="49%"> <img src="images/14-capture-amp.png" alt="" width="49%">

<img src="images/12-cab-settings.png" alt="" width="49%"> <img src="images/11-capture-volume.png" alt="" width="49%">

Tap the CAPTURE or CAB / IR card. **SLOTS** lists the 25 capture slots (5 banks × 5) or 5 cab slots;
**LIBRARY** shows all captures or IRs stored on the Nano with category filters (AMP = head or combo, AMP+CAB, CAB, PEDAL, OTHER).
A library item is loaded into the active slot after a confirmation.

**Capture volume**: **VOL** in the preset card, −24 dB to +12 dB (**0 dB** resets). **Capture tone**: a long press
on the CAPTURE card – **GAIN**, **BASS**, **MID** and **TREBLE** of the capture, 0–10. **Cab settings**: a long press on the CAB / IR card – **OUTPUT** (−96 dB to +12 dB),
**HIGH PASS** (20–800 Hz) and **LOW PASS** (1–20 kHz) of the active cab, read from the Nano when the dialog opens.
All of them are part of the preset: changes are heard at once, **SAVE** keeps them.

## FX editor

<img src="images/04-fx-editor.png" alt="" width="49%"> <img src="images/16-fx-model.png" alt="" width="49%">

The header shows the effect's symbol – filled in its colour when the effect is on, outlined when it is off – with
the slot and the model. Tap it to choose another model from a list grouped by type (Drive, EQ & utility, Wah &
filter …); the large switch on the right turns the effect on or off.

The parameters are bars in two columns, filled in the effect colour up to the value. **Slide sideways** on a bar to
change it: it follows your finger from its current value, so a touch or a scroll never makes a value jump. Up and
down scrolls the list. Settings with two or three options (*Off | On*, *Sync* …) are switched with a tap, longer
lists (*Sync Note* …) open a list.

Changes are sent to the Nano while you move a control. The Nano only reports parameter values of effects that
are switched on – switch an effect on to see and edit its values. **SAVE** in the preset card stores the preset
on the Nano.

**FX presets** (the bar above the parameters): *Original* and four named settings per effect model, stored on the
controller and usable in every preset and slot with that model; empty places show **+**.

- **Hold** a place: the keyboard opens and the current settings are saved under the name you type. An empty name
  deletes the place.
- **Tap** a place: its settings are loaded at once.
- **Original** goes back to the settings the effect had when you opened the editor. Right after choosing a new
  model, these are the model's defaults.

Loaded settings count as changes to the Nano preset: **SAVE** keeps them there.

## Reverb switch (footswitch 8)

<img src="images/07-reverb.png" alt="" width="440">

- **MIX POS 1 / 2**: footswitch 8 sets the reverb's Mix to Pos 1 or Pos 2. These are the heel and toe values of
  the preset's expression setting for the reverb ("Post FX 3 Amount"). Moving a slider plays that mix;
  **SAVE** writes both values into the preset (and creates the expression setting if there is none).
- **2ND REVERB**: choose a second reverb (B) for this preset. Footswitch 8 then switches between the preset's
  reverb (A) and B; the Nano has one reverb slot, so the controller swaps the model and sends the stored values
  (takes about half a second). Tile 8 shows reverb B and lights up while it runs; tile 7 shows the reverb in the slot. **EDIT B** loads B and opens the FX editor to set it up. Reverb B is stored on the
  controller, per preset. If you save the preset while B is active, B becomes the preset's reverb.

## Second effect on Pre FX 1 (footswitch 3)

<img src="images/13-second-effect.png" alt="" width="440">

Long press on the Pre FX 1 tile, then **+B** (next to the model in the FX editor): choose a second effect (B) for
this preset, for example the Envelope Filter as an auto-wah next to a drive. **A | B** then shows which one runs:
tap the other one to swap at once, tap the running one to choose another second effect or *None*. Then footswitch 3
in FX mode:

- **short press**: the effect in the slot on / off (it acts when you lift your foot)
- **hold** (0.6 s): swap A ↔ B – the effect stays on or off, as it was

The tile shows *Pre FX 1 A* or *Pre FX 1 B* and, below it, the other effect (⇄ name). The Nano has one model per slot, so the controller swaps the model and
sends the stored values of the other effect: this takes about half a second with a short gap in the sound.
For a gapless change put the second effect into a free slot instead and switch it on and off.
B is edited in the FX editor while it runs and is stored on the controller, per preset (MIDI: CC 58). The controller
also keeps the settings of A, read whenever A is on. Only if A was never on since B was set up, the first swap
switches A on for a moment to read them.

## Tuner

<img src="images/08-tuner.png" alt="" width="440">

Footswitch 2 or the TUNER tile. Looks like the desktop editor's tuner: a large note in the tuning colour (green = in
tune, orange = off), a scale with a tick every 10 cents and a glowing needle, the deviation in cents. At the bottom,
**Reference A4** sets the reference pitch (400-480 Hz, hold − / + to run) and **Mute output** silences the Nano while
tuning. Tap the screen or press footswitch 2 again to close it.

## USB audio volume

<img src="images/09-usb-volume.png" alt="" width="440">

**USB** in the preset card: volume of the audio played from the computer over USB, −40 dB (OFF) to 0 dB,
as in the official app. The value is read from the Nano each time.

## Bluetooth MIDI

<img src="images/10-bluetooth-midi.png" alt="" width="440">

**MIDI** in the preset card opens the device list. It shows Bluetooth MIDI devices nearby – for example a
CME WIDI adapter on the MIDI port of a Morningstar MC6, or a Bluetooth MIDI
footswitch. Tap one to connect it; the controller remembers it and connects again by itself whenever it is
switched on. **FORGET DEVICE** disconnects and forgets it. The Nano stays connected at the same time.

The controller understands the same messages as the Nano's own MIDI over USB, on all MIDI channels, so banks made
for the Nano (for example with the MC6 export of the Nano Cortex Editor) work over Bluetooth too:

| Message | Action |
|---|---|
| Program Change 0–63 | preset 1–64 |
| CC 37–41 | FX slot 1–5 (Pre FX 1, Pre FX 2, Post FX 1–3): value 64–127 on, 0–63 off |
| CC 1 | expression: reverb mix from Pos 1 (0) to Pos 2 (127) |
| CC 50–57, value 64–127 | press footswitch 1–8 (mode, tuner, presets or FX, reverb switch) |
| CC 58, value 64–127 | hold footswitch 3: Pre FX 1 swaps to its second effect and back |
| CC 59 | looper mode: value 64–127 on, 0–63 off |

A wired MIDI input is not possible on this board without extra hardware; use a Bluetooth MIDI adapter instead.

## Looper mode (a looper app on a phone)

<img src="images/18-looper-mode.png" alt="" width="49%"> <img src="images/19-looper-tile.png" alt="" width="49%">

The controller can be the foot controller of a looper app, for example **Loopy Pro** on an iPhone or iPad. The
phone plays through the Nano: connected to its USB-C port, the Nano is the phone's audio interface, so the guitar
goes into the app and the loops (and backing tracks) come out of the Nano. Their level is the *USB audio volume*
(see above). The footswitches reach the app over **Bluetooth MIDI**: the controller is a Bluetooth MIDI device with
the name **"Nano Cortex Controller"**.

1. In Loopy Pro open the main menu > **Bluetooth Devices** and choose *Nano Cortex Controller*. The controller
   shows *Phone connected* and how often MIDI is exchanged (every 11.25 or 15 ms on an iPhone).
2. **Hold footswitch 1** (0.6 s) or long press tile 1: the tiles become the looper's switches. Any press of
   footswitch 1 leaves the mode again; FX and presets are as you left them.
3. In Loopy Pro choose **MIDI Learn**, tap what a switch should do (for a loop: *Play/Stop* with *Record if
   empty*) and press that footswitch. Bind every switch only once: two bindings on the same loop cancel each other.

| Footswitch | Sends (MIDI channel 16) | Tile (as delivered) |
|---|---|---|
| 2 | CC 102 | Pause |
| 3–8 | CC 103–108 | Loop 1–6 |

**Your own names**: a long press on a tile in looper mode opens its dialog – name (keyboard), colour and symbol,
stored on the controller; *Default* brings back the tile shown above. Tile 1 shows what the last press sent
(*Sent CC 103*), which helps when you bind a switch in the app.

By default the tiles show the loops in three colour pairs, as a two-column Loopy Pro project does. What a switch
does is up to the app.

A footswitch sends the value 127 when you press it and 0 when you lift your foot, so the app's own *hold* and
*double tap* triggers work. Tapping a tile on the screen sends a short press. In looper mode a press goes out
2–4 ms after the contact closes; over Bluetooth it then waits for the next exchange with the phone – at most the
interval shown when the phone connects. With loops that start and end on the beat (the app's quantisation) this
does not matter; a freely recorded first loop can be that much longer or shorter.

The phone and the Nano Cortex Editor can be connected at the same time. What the app sends back (feedback meant
for a controller's lights) is written to the serial log for now.

## Nano Cortex Editor through the controller

The Nano accepts only one Bluetooth connection. When the controller is connected to the Nano, it offers itself as
**"Nano Cortex Controller"** with the same Bluetooth service as the Nano, so the
[Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor) connects to the controller instead:
**app ↔ controller ↔ Nano**. The Mac app picks it automatically; in the browser choose "Nano Cortex Controller".
Without the controller (or before it has connected) the app connects to the Nano directly, as before.

- Everything the app sends goes on to the Nano; replies to the app's requests go back to the app, replies to the
  controller's own requests stay on the controller, and messages the Nano sends on its own reach both.
- Changes made in the app appear on the controller a moment later; changes made on the controller (footswitches,
  touch, MIDI) make the app read the preset again, as after a change on the pedal.
- While the app is connected, **APP** is shown in the status line.

## Footswitch learn

Long press on tile 2 (footswitch 2; **Switch 1** in the dialog learns footswitch 1 instead), or **LEARN SWITCH** in the
bank editor. Then press the footswitch that should have
this function within 15 seconds. If it had another function, the two are swapped. **DEFAULT ORDER** restores
the standard order.

## Footswitches

The footswitches are read by an **SX1509** I/O expander (for example the SparkFun breakout), as in
[TonexOneController](https://github.com/Builty/TonexOneController).

- Connect the SX1509 to the board's **I2C** bus: SDA = GPIO 8, SCL = GPIO 9, 3.3 V and GND
  (see Waveshare's wiki for the connector of your board).
- Set the SX1509 address to **0x71** (or 0x70). 0x3E/0x3F are taken by the board's own I/O expander.
- Wire each footswitch (momentary, normally open) between an SX1509 I/O pin and **GND**. Internal pull-ups are
  used, no resistors needed.
- Default order: switch 1–8 on SX1509 pins **11, 10, 0, 1, 2, 3, 8, 9**. Any other pins work too – use
  footswitch learn to assign them.

Without an SX1509 the controller works with the touch screen only.

## What is stored where

| On the Nano | On the controller |
|---|---|
| presets, names, captures, cabs, FX and their values, expression settings (Pos 1 / Pos 2), USB volume | own banks (preset, colour, symbol), footswitch order, second reverbs, the Bluetooth MIDI device |
