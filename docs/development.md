# Nano Cortex Controller – development

Building the firmware, the layout of the project and how the controller talks to the Nano Cortex. Using the controller: [README](../README.md) and [manual](manual.md).

## Build from source

Requirements: [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v6.0.2**. The components (LVGL, display
port, touch driver) are downloaded by the component manager on the first build.

```bash
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor --no-reset
```

Open the monitor with `--no-reset`; otherwise the reset over the native USB port can leave the board in
download mode. Use the port name of your system (`ls /dev/cu.*` on a Mac).

`tools/release.sh` builds the release files (`release/…-full.bin` and `…-update.bin`) as published.

### Project layout

| Path | Contents |
|---|---|
| `main/main.c` | app task: events from Bluetooth, touch screen, footswitches and console; modes, banks, FX editor, reverb switch |
| `main/nano_link.c` | Bluetooth LE central (NimBLE): scan, connect, MTU 517, notifications, reassembly of long messages, write queue |
| `main/nano_state.c` | protobuf parsing of the Nano's state and all request messages |
| `main/library.c` | capture / IR library (reading, sorting, loading into a slot) |
| `main/midi_ble.c` | Bluetooth LE MIDI client: device list, connection, BLE MIDI packets |
| `main/app_link.c` | app bridge: Nano service (A002 / C304 / C305) for the editor, packets as the Nano sends them |
| `main/phone_midi.c` | Bluetooth MIDI device for a looper app on a phone (looper mode) |
| `main/ui.c` | LVGL user interface |
| `main/board.c` | display (RGB 800 × 480), GT911 touch, CH422G I/O expander, LVGL port |
| `main/footswitches.c` | SX1509 polling, debouncing, learn |
| `main/fx_models.c`, `main/fx_icons.c`, `main/preset_icons.c` | generated tables and icons (see `tools/`) |
| `tools/gen_tables.py` | FX models and parameters (`tools/editor_models.json`, exported from the editor) and effect icons |
| `tools/preset_icons.py` | the preset symbols (own drawings), rendered by `tools/svg_render.py` |
| `tools/gen_pedals.py` | optional pedal pictures for an own build (see below) |
| `tools/screenshots/` | renders the screenshots in `docs/images` on the computer from `main/ui.c` with example data |

### Own artwork (optional, own builds only)

`main/fx_icons.c` contains drawn effect icons and `main/fx_pedals.c` no pedal pictures. If you have your own icon
set or pedal pictures, `tools/gen_tables.py` and `tools/gen_pedals.py` can turn them into
`main/private/fx_icons_private.c` and `main/private/fx_pedals_private.c`: the build uses them automatically, they
are ignored by git and never part of the published firmware (`-DNANO_PUBLIC=1`). Do not publish artwork you have
no rights to.

## Serial console

With the board on the **USB** port, any serial monitor at 115200 baud (for example `idf.py monitor --no-reset`)
shows a log of all messages and accepts commands: `n`/`p` next/previous preset, a number (1–64) selects a preset,
`a`–`e` switch FX slots 1–5, `m` mode, `o` looper mode, `t` tuner, `x` reverb switch, `s` read the preset again, `r` read everything again,
`l` list the preset names, `h` help.

## How it works

The Nano Cortex offers a Bluetooth LE service `A002` with a write characteristic `C304` and a notify
characteristic `C305`. Every message is a protobuf payload framed as
`[length] C0 [payload] [32-bit little-endian message type]`; long replies are split into several packets.
The controller is a Bluetooth central: it connects without pairing, requests an MTU of 517 and reads the full
state (all preset names) once, then the current preset after every change.

Message types used (request → reply): 1 → 2 state, 3 save, 28 capture/cab slot, 29 → 30 preset change,
26 capture volume and amp knobs, 31 FX on/off, 60 → 61 and 62 expression settings, 65 → 66 and 67 → 68 settings, 76 → 77 library,
78 → 79 / 80 → 81 load IR / capture, 94 cab setting, 95 → 96 cab settings, 99 FX parameter, 111 → 112 rename, 115 unsaved changes,
127 / 128 tuner, 136 FX model, 137 → 138 FX parameter values. The message layouts are documented in the
comments of `main/nano_state.c`, `main/library.c` and in the
[Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor).

For the app bridge the controller is also a Bluetooth peripheral with the Nano's service. Replies carry no request
id, so every request with a known reply type is noted with its sender (controller or app); the Nano answers in
order, and each reply goes to the sender of the oldest open request of its type. Long replies are split for the app
exactly as the Nano does: `[length low] [0x40 first | 0x80 last | length high] data`, up to 510 bytes per packet.

Bluetooth MIDI runs on a second connection next to the Nano: the controller looks for the BLE MIDI service
(`03B80E5A-EDE8-4B33-A751-6CE34EC4C700`), subscribes to its characteristic and decodes the BLE MIDI packets
(timestamps, running status) into channel messages.

When the preset is changed on the pedal, the Nano sends message 29 and waits for the acknowledgement 30
(`06 C0 20 01 1E 00 00 00`); the controller answers it and reads the new preset.
