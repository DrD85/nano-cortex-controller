## Nano Cortex Controller 1.0.0

First release: a touch screen and footswitch controller for the Neural DSP Nano Cortex over Bluetooth,
running on the Waveshare ESP32-S3-Touch-LCD-4.3.

**Highlights**
- Eight Quad-Cortex-style tiles for eight footswitches: preset mode (6 presets per bank, 16 own banks with colour
  and symbol) and FX mode (Pre FX 1–2, Post FX 1–3 on/off)
- FX editor with model selection and all parameters, live
- Capture and Cab/IR slots and the Nano's capture and IR library
- Reverb switch on footswitch 8: mix Pos 1 ↔ Pos 2, or a second reverb per preset
- Tuner with large note display, USB audio volume, footswitch learn, full-screen tiles, rename and save

**Install** (Chrome or Edge, [esptool.spacehuhn.com](https://esptool.spacehuhn.com), board on the **USB** port):
- first installation: `nano-controller-1.0.0-full.bin` at **0x0**
- update: `nano-controller-1.0.0-update.bin` at **0x10000** (keeps your banks and settings)

Step-by-step guide and wiring of the footswitches: see the [README](../../blob/main/README.md)
([Deutsch](../../blob/main/README.de.md)).

Unofficial community project – not affiliated with Neural DSP.

---

**Deutsch:** Erste Version des Touchscreen- und Fußschalter-Controllers für den Nano Cortex auf dem Waveshare
ESP32-S3-Touch-LCD-4.3. Erstinstallation: `…-full.bin` an **0x0**; Update: `…-update.bin` an **0x10000**
(behält Bänke und Einstellungen). Anleitung in der [README auf Deutsch](../../blob/main/README.de.md).
