# Nano Cortex Controller – Entwicklung

Die Firmware bauen, der Aufbau des Projekts und wie der Controller mit dem Nano Cortex spricht. Bedienung: [README](../README.de.md) und [Handbuch](manual.de.md).

## Selbst bauen

Voraussetzung: [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v6.0.2**. Die Komponenten (LVGL,
Display-Port, Touch-Treiber) lädt der Komponenten-Manager beim ersten Bauen.

```bash
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor --no-reset
```

Den Monitor mit `--no-reset` öffnen, sonst kann der Reset über die native USB-Buchse das Board im Flash-Modus
lassen. Den Anschlussnamen deines Systems verwenden (`ls /dev/cu.*` auf dem Mac).

`tools/release.sh` baut die Release-Dateien (`release/…-full.bin` und `…-update.bin`) so, wie sie veröffentlicht werden.

### Aufbau des Projekts

| Pfad | Inhalt |
|---|---|
| `main/main.c` | App-Task: Ereignisse von Bluetooth, Touchscreen, Fußschaltern und Konsole; Modi, Bänke, FX-Editor, Reverb-Schalter |
| `main/nano_link.c` | Bluetooth-LE-Central (NimBLE): suchen, verbinden, MTU 517, Benachrichtigungen, Zusammensetzen langer Nachrichten, Sende-Warteschlange |
| `main/nano_state.c` | Protobuf-Auswertung des Nano-Zustands und alle Anfrage-Nachrichten |
| `main/library.c` | Capture-/IR-Library (lesen, sortieren, in einen Slot laden) |
| `main/midi_ble.c` | Bluetooth-LE-MIDI-Client: Geräteliste, Verbindung, BLE-MIDI-Pakete |
| `main/app_link.c` | App-Brücke: Nano-Dienst (A002 / C304 / C305) für den Editor, Pakete wie vom Nano |
| `main/phone_midi.c` | Bluetooth-MIDI-Gerät für eine Looper-App auf dem Telefon (Looper-Modus) |
| `main/ui.c` | LVGL-Oberfläche |
| `main/board.c` | Display (RGB 800 × 480), GT911-Touch, CH422G-I/O-Expander, LVGL-Port |
| `main/footswitches.c` | SX1509 abfragen, entprellen, Learn |
| `main/fx_models.c`, `main/fx_icons.c`, `main/preset_icons.c` | erzeugte Tabellen und Symbole (siehe `tools/`) |
| `tools/gen_tables.py` | FX-Modelle und Parameter (`tools/editor_models.json`, aus dem Editor exportiert) und Effekt-Symbole |
| `tools/preset_icons.py` | die Preset-Symbole (eigene Zeichnungen), gezeichnet von `tools/svg_render.py` |
| `tools/gen_pedals.py` | optionale Pedalbilder für einen eigenen Build (siehe unten) |
| `tools/screenshots/` | zeichnet die Bildschirmfotos in `docs/images` am Computer aus `main/ui.c` mit Beispieldaten |

### Eigene Grafiken (optional, nur für eigene Builds)

`main/fx_icons.c` enthält gezeichnete Effekt-Symbole, `main/fx_pedals.c` keine Pedalbilder. Wer ein eigenes
Symbol-Set oder Pedalbilder hat, kann sie mit `tools/gen_tables.py` und `tools/gen_pedals.py` in
`main/private/fx_icons_private.c` und `main/private/fx_pedals_private.c` umwandeln: Der Build nimmt sie automatisch,
Git ignoriert sie, und in der veröffentlichten Firmware (`-DNANO_PUBLIC=1`) sind sie nie enthalten. Grafiken, an
denen du keine Rechte hast, bitte nicht veröffentlichen.

## Serielle Konsole

Am **USB**-Anschluss zeigt jeder serielle Monitor mit 115200 Baud (zum Beispiel `idf.py monitor --no-reset`)
ein Protokoll aller Nachrichten und nimmt Befehle an: `n`/`p` nächstes/vorheriges Preset, eine Zahl (1–64)
wählt ein Preset, `a`–`e` schalten FX-Slot 1–5, `m` Modus, `o` Looper-Modus, `$` Szenen / Presets, `t` Stimmgerät, `x` Reverb-Schalter,
`s` Preset neu lesen, `r` alles neu lesen, `l` alle Preset-Namen, `h` Hilfe.

## So funktioniert es

Der Nano Cortex bietet einen Bluetooth-LE-Dienst `A002` mit einer Schreib-Characteristic `C304` und einer
Benachrichtigungs-Characteristic `C305`. Jede Nachricht ist ein Protobuf-Payload im Rahmen
`[Länge] C0 [Payload] [32-Bit-Nachrichtentyp, Little Endian]`; lange Antworten kommen in mehreren Paketen.
Der Controller ist Bluetooth-Central: Er verbindet sich ohne Pairing, fordert eine MTU von 517 an, liest einmal
den vollständigen Zustand (alle Preset-Namen) und danach nach jedem Wechsel das aktuelle Preset.

Verwendete Nachrichtentypen (Anfrage → Antwort): 1 → 2 Zustand, 3 Speichern, 26 Capture-Lautstärke und Amp-Regler, 28 Capture-/Cab-Slot,
29 → 30 Preset-Wechsel, 31 FX an/aus, 60 → 61 und 62 Expression-Einstellungen, 65 → 66 und 67 → 68 Einstellungen,
76 → 77 Library, 78 → 79 / 80 → 81 IR / Capture laden, 94 Cab-Einstellung, 95 → 96 Cab-Einstellungen, 99 FX-Parameter, 111 → 112 Umbenennen,
115 ungespeicherte Änderungen, 127 / 128 Stimmgerät, 136 FX-Modell, 137 → 138 FX-Parameterwerte.
Der Aufbau der Nachrichten steht in den Kommentaren von `main/nano_state.c`, `main/library.c` und im
[Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor).

Für die App-Brücke ist der Controller zusätzlich Bluetooth-Peripheriegerät mit dem Dienst des Nano. Antworten tragen
keine Kennung; deshalb merkt sich der Controller zu jeder Anfrage mit bekanntem Antworttyp den Absender (Controller
oder App). Der Nano antwortet der Reihe nach, jede Antwort geht an den Absender der ältesten offenen Anfrage ihres
Typs. Lange Antworten zerlegt der Controller für die App genau wie der Nano:
`[Länge low] [0x40 erstes | 0x80 letztes | Länge high] Daten`, bis zu 510 Bytes pro Paket.

Bluetooth-MIDI läuft über eine zweite Verbindung neben dem Nano: Der Controller sucht den BLE-MIDI-Dienst
(`03B80E5A-EDE8-4B33-A751-6CE34EC4C700`), abonniert dessen Characteristic und zerlegt die BLE-MIDI-Pakete
(Zeitstempel, Running Status) in Kanal-Nachrichten.

Wird das Preset am Pedal gewechselt, schickt der Nano die Nachricht 29 und wartet auf die Bestätigung 30
(`06 C0 20 01 1E 00 00 00`); der Controller bestätigt und liest das neue Preset.
