# Nano Cortex Controller in the browser

The controller's firmware – the same screen and the same logic as on the board (`main/main.c`, `main/ui.c`, …) –
compiled to WebAssembly with [Emscripten](https://emscripten.org). Only the hardware is replaced:

| Board | Browser (`web/`) |
|---|---|
| 800×480 display and touch panel (`board.c`) | canvas, mouse or finger (`board_web.c`) |
| SX1509 footswitches (`footswitches.c`) | eight buttons below the screen, keys 1–8 (`board_web.c`) |
| NimBLE link to the Nano (`nano_link.c`) | Web Bluetooth (`nano_link_web.c`, `app.js`) |
| Bluetooth MIDI (`midi_ble.c`) | Web MIDI: the MIDI inputs of the computer (`midi_web.c`, `app.js`) |
| NVS, esp_timer, FreeRTOS queue (ESP-IDF) | localStorage, timers and queues in the page's main loop (`platform.c`, `shim/`) |
| app bridge (`app_link.c`) | – |

The shared code has a few `#ifdef NANO_WEB` places (texts, no console, no tasks). The browser build uses the public
artwork only (drawn icons, no pedal pictures).

## Build

```bash
source ~/emsdk/emsdk_env.sh   # Emscripten SDK
web/build.sh                  # → build-web/site
```

Needs one ESP-IDF build first (LVGL in `managed_components`, `build-public/config/sdkconfig.h` from
`tools/release.sh`). The WebAssembly is embedded in `nano-controller.js` (`-sSINGLE_FILE`), so the page also opens
as a local file. It needs Chrome or Edge (Web Bluetooth) and `https://…`, `http://localhost` or a local file.

## Publish

The folder `build-web/site` is the complete page. It is published in its own repository,
[nano-cortex-controller-web](https://github.com/DrD85/nano-cortex-controller-web), with GitHub Pages:
`web/build.sh ../../nano-cortex-controller-web` (next to this repository) updates the page there; its README,
LICENSE and screenshot stay.
