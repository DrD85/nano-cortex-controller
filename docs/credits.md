# Nano Cortex Controller – credits

This project stands on the shoulders of:

- [Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor) (MIT) – the Bluetooth protocol as tested
  against the device, the FX model and parameter tables and the effect icon drawings
- [rixrix/deskop-nano-cortex](https://github.com/rixrix/deskop-nano-cortex) (Apache-2.0) – protocol notes on the
  state layout and the preset-change acknowledgement (via the editor)
- [Builty/TonexOneController](https://github.com/Builty/TonexOneController) (Apache-2.0) – the idea of a touch
  screen controller on this Waveshare board, its display and touch setup (pins, timing, touch reset) and the
  SX1509 footswitch wiring
- [ESP-IDF](https://github.com/espressif/esp-idf) (Apache-2.0) by Espressif, including the
  [NimBLE](https://github.com/apache/mynewt-nimble) Bluetooth stack (Apache-2.0)
- [LVGL](https://github.com/lvgl/lvgl) (MIT) – the graphics library, including TinyTTF with
  [stb_truetype](https://github.com/nothings/stb) (MIT / public domain)
- [esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port),
  [esp_lcd_touch](https://components.espressif.com/components/espressif/esp_lcd_touch) and
  [esp_lcd_touch_gt911](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911)
  (Apache-2.0) by Espressif
- [IBM Plex Sans](https://github.com/IBM/plex) (SIL Open Font License 1.1, `main/fonts/OFL.txt`) by IBM – the
  typeface of the screen, as in the desktop editor. `tools/gen_fonts.py` converts it into the bitmap fonts in
  `main/ui_fonts.c` (named `ui_font_*`, as converted fonts must not carry the reserved name); the tuner note is
  drawn from the unchanged TTF
- [Montserrat](https://github.com/JulietaUla/Montserrat) font (SIL Open Font License 1.1) by
  The Montserrat Project Authors – LVGL's built-in fonts, used here for the symbols
- [ESPWebTool](https://github.com/SpacehuhnTech/espwebtool) (MIT) by Spacehuhn – flashing in the browser
- [esptool](https://github.com/espressif/esptool) (GPL-2.0) by Espressif – flashing and release images
- [Pillow](https://github.com/python-pillow/Pillow) (MIT-CMU) – rendering the icons in `tools/`
- [Waveshare](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) – board documentation and demos
