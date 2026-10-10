# Nano Cortex Controller – Danksagung

Dieses Projekt baut auf:

- [Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor) (MIT) – das am Gerät erprobte
  Bluetooth-Protokoll, die Tabellen der FX-Modelle und Parameter und die Zeichnungen der Effekt-Symbole
- [rixrix/deskop-nano-cortex](https://github.com/rixrix/deskop-nano-cortex) (Apache-2.0) – Protokoll-Notizen zum
  Aufbau des Zustands und zur Bestätigung von Preset-Wechseln (über den Editor)
- [Builty/TonexOneController](https://github.com/Builty/TonexOneController) (Apache-2.0) – die Idee eines
  Touchscreen-Controllers auf diesem Waveshare-Board, dessen Display- und Touch-Einrichtung (Pins, Timing,
  Touch-Reset) und die Verdrahtung der Fußschalter am SX1509
- [ESP-IDF](https://github.com/espressif/esp-idf) (Apache-2.0) von Espressif, einschließlich des
  Bluetooth-Stacks [NimBLE](https://github.com/apache/mynewt-nimble) (Apache-2.0)
- [LVGL](https://github.com/lvgl/lvgl) (MIT) – die Grafikbibliothek, einschließlich TinyTTF mit
  [stb_truetype](https://github.com/nothings/stb) (MIT / Public Domain)
- [esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port),
  [esp_lcd_touch](https://components.espressif.com/components/espressif/esp_lcd_touch) und
  [esp_lcd_touch_gt911](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911)
  (Apache-2.0) von Espressif
- Schrift [IBM Plex Sans](https://github.com/IBM/plex) (SIL Open Font License 1.1, `main/fonts/OFL.txt`) von IBM –
  die Schrift des Bildschirms, wie im Desktop-Editor. `tools/gen_fonts.py` wandelt sie in die Bitmap-Schriften in
  `main/ui_fonts.c` um (Namen `ui_font_*`, da umgewandelte Schriften den reservierten Namen nicht tragen dürfen);
  die Note im Stimmgerät kommt aus der unveränderten TTF
- Schrift [Montserrat](https://github.com/JulietaUla/Montserrat) (SIL Open Font License 1.1) von
  The Montserrat Project Authors – die eingebauten LVGL-Schriften, hier für die Symbole
- [ESPWebTool](https://github.com/SpacehuhnTech/espwebtool) (MIT) von Spacehuhn – Flashen im Browser
- [esptool](https://github.com/espressif/esptool) (GPL-2.0) von Espressif – Flashen und Release-Images
- [Pillow](https://github.com/python-pillow/Pillow) (MIT-CMU) – Zeichnen der Symbole in `tools/`
- [Waveshare](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) – Dokumentation und Demos des Boards
