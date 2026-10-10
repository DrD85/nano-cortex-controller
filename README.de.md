# Nano Cortex Controller (inoffiziell)

Ein Touchscreen- und Fußschalter-Controller für den **Neural DSP Nano Cortex**. Er läuft auf einem
**Waveshare ESP32-S3-Touch-LCD-4.3**, verbindet sich per Bluetooth und arbeitet wie ein kleiner Bodencontroller im
Stil des Quad Cortex: acht Kacheln für acht Fußschalter, eigene Preset-Bänke, Szenen, ein FX-Editor, die Capture-
und IR-Library, die Belegung des Expression-Pedals, ein Stimmgerät und ein Looper-Modus. Er ist das Hardware-Gegenstück zum
[Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor).

![Nano Cortex Controller](docs/images/00-overview.png)

**English:** [README](README.md) · **Noch kein Board?** Dieselbe Firmware läuft im Browser:
[Nano Cortex Controller Web](https://github.com/DrD85/nano-cortex-controller-web)

> **Inoffizielles Community-Projekt.** Nicht verbunden mit, unterstützt oder betreut von Neural DSP.
> „Nano Cortex“, „Quad Cortex“ und „Neural DSP“ sind Marken von Neural DSP Technologies.
> Der Controller schreibt auf dein Gerät. Nutzung auf eigene Gefahr – sichere deine Presets vorher
> (zum Beispiel mit der offiziellen Cortex-Cloud-App).

## Was er kann

- **Presets und Bänke** – sechs Presets pro Bank auf den Fußschaltern 3–8, 16 Bänke mit eigenen Farben und Symbolen
- **Szenen** – sechs pro Preset: ein Fußschalter schaltet mehrere Effekte auf einmal, ohne die Lücke eines Preset-Wechsels
- **FX-Modus** – die fünf FX-Slots mit dem Fuß schalten, dazu ein Reverb-Schalter und ein zweiter Effekt auf Pre FX 1
- **FX-Editor** – Modelle wählen und jeden Parameter am Touchscreen einstellen, mit benannten FX-Presets
- **Capture und Cab/IR** – Slots wählen oder alles aus der Library des Nano laden; Capture-Klang, Lautstärke, Cab
- **Expression-Pedal** – pro Preset festlegen, was das Pedal des Nano bewegt (ein Wah, einen Mix, den Pegel …) und wie weit
- **Stimmgerät**, **Gig-Ansicht** mit großen Kacheln, **Bluetooth-MIDI** für einen weiteren Controller
- **Looper-Modus** – die Fußschalter steuern eine Looper-App auf dem Telefon (zum Beispiel Loopy Pro)
- Der [Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor) kann sich **über** den Controller verbinden

## Loslegen

**1. Was du brauchst**

| Teil | Hinweise |
|---|---|
| [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) | Touchscreen mit 800 × 480, Version mit EINEM USB-C-Anschluss (**USB**) |
| Neural DSP Nano Cortex | Bluetooth an; kein Koppeln nötig |
| Optional: SX1509-Breakout + bis zu 8 Fußtaster | [Verkabelung](docs/manual.de.md#fußschalter) – der Touchscreen funktioniert auch allein |

**2. Firmware installieren** – im Browser, ohne Werkzeuge. Die Dateien stehen auf der Seite [Releases](../../releases).

1. **[esptool.spacehuhn.com](https://esptool.spacehuhn.com)** in **Chrome** oder **Edge** öffnen und das Board am
   USB-C-Anschluss **USB** anschließen.
2. **Connect** klicken und das Board wählen (meist „USB JTAG/serial debug unit“). Kein Anschluss zu sehen? **BOOT**
   halten, **RESET** drücken und loslassen, **BOOT** loslassen, noch einmal versuchen.
3. Datei und Adresse eintragen, dann **Program** klicken:

   | Datei | Adresse | Wofür |
   |---|---|---|
   | `nano-controller-<version>-full.bin` | `0x0` | die **Erstinstallation** (löscht gespeicherte Bänke und Einstellungen) |
   | `nano-controller-<version>-update.bin` | `0x10000` | **Updates** – Bänke und Einstellungen bleiben |

4. **RESET** am Board drücken.

**3. Erster Start** – Nano einschalten, die Cortex-Cloud-App und den Nano Cortex Editor schließen (der Nano nimmt
nur eine Bluetooth-Verbindung an) und das Board mit Strom versorgen. Es findet den Nano, liest die Presets und zeigt
das aktuelle. Findet es den Nano nicht innerhalb einer Minute, den Nano aus- und wieder einschalten.

## Bedienung

### Der Hauptbildschirm

![Hauptbildschirm mit Nummern](docs/images/guide-main.png)

1. **↻** alles neu lesen · **Lautsprecher** Capture-Lautstärke · **MIDI**-Geräte · **USB**-Audio-Lautstärke
2. Grüner Punkt = mit dem Nano verbunden; Preset und Bank
3. **Pedal**: was das [Expression-Pedal](docs/manual.de.md#expression-pedal) bewegt · **Ebenen**: [Szenen](#szenen) auf
   den Kacheln 3–8, nochmal tippen: die Presets der Bank · **Diskette**: Preset auf dem Nano speichern – grün bei
   ungespeicherten Änderungen
4. Vorherige / nächste **Bank**
5. Preset-Name – **halten** zum Umbenennen
6. Capture und Cab – **tippen** zum Auswählen, **halten** für Klang / Cab-Einstellungen
7. Acht Kacheln = acht Fußschalter. **Tippen** auf eine Kachel = ihren Schalter treten

Eine Kachel ist **gefüllt**, wenn sie aktiv ist (das geladene Preset, ein eingeschalteter Effekt), und dunkel mit
ihrer Farbe am oberen Rand, wenn nicht. **Nach links / rechts wischen**: nächstes / vorheriges Preset.

### Presets und FX

<table>
<tr>
<td width="50%"><img src="docs/images/01-preset-mode.png" alt="Preset-Modus"></td>
<td width="50%"><img src="docs/images/02-fx-mode.png" alt="FX-Modus"></td>
</tr>
<tr>
<td>

**Preset-Modus** – die Kacheln 3–8 sind die sechs Presets der Bank. Eine Kachel **halten**, um dem Schalter ein
anderes Preset, eine Farbe und ein Symbol zu geben.

</td>
<td>

**FX-Modus** – die Kacheln 3–7 schalten Pre FX 1–2 und Post FX 1–3, Kachel 8 ist der Reverb-Schalter. Eine Kachel
**halten** öffnet ihren Editor.

</td>
</tr>
</table>

**Fußschalter 1** wechselt zwischen den beiden Modi, **Fußschalter 2** ist das Stimmgerät.

### Szenen

<table>
<tr>
<td width="50%"><img src="docs/images/20-scene-mode.png" alt="Scene-Modus"></td>
<td width="50%"><img src="docs/images/21-scene.png" alt="Szenen-Fenster"></td>
</tr>
<tr>
<td>

**Szenen** (der Ebenen-Knopf neben Save) – die Kacheln 3–8 werden zu sechs Szenen des aktuellen Presets. Eine Szene legt fest, welche
seiner fünf Effekte an sind – und auf Wunsch, wie sie eingestellt sind. Ihr Fußschalter ändert das alles auf einmal,
ohne die Lücke eines Preset-Wechsels. Die Quadrate auf einer Kachel zeigen die fünf Effekte in ihren Farben: gefüllt = an.

</td>
<td>

Eine Kachel **halten**, um eine Szene einzurichten: Name, Farbe und die Effekte, die an sind – ein Effekt wird durch
Antippen geschaltet. Eine leere Szene beginnt mit dem, was gerade an ist. Szenen liegen auf dem Controller, pro Preset.

</td>
</tr>
</table>

Nochmal auf den Szenen-Knopf tippen bringt die Presets der Bank zurück – oder **Fußschalter 2 halten**, um mit dem Fuß
zu wechseln. Fußschalter 1 wechselt zwischen FX und dem, was du davon gewählt hast.

**Andere Einstellungen pro Szene** (mehr Delay im Solo): den Effekt im [FX-Editor](#fx-editor) öffnen, einstellen und
auf den Szenen-Knopf am Ende der FX-Preset-Leiste tippen, dann auf die Szene, die diese Einstellungen bekommen soll.

### Gig-Ansicht

<table>
<tr>
<td width="50%"><img src="docs/images/03-fullscreen.png" alt="Gig-Ansicht"></td>
<td>

**Nach oben wischen**: Die Kacheln füllen den Bildschirm mit großen Namen, eine schmale Leiste zeigt Verbindung,
Preset, Bank, ungespeicherte Änderungen und den Modus. **Nach unten wischen**: zurück.

</td>
</tr>
</table>

### FX-Editor

**Eine FX-Kachel halten** öffnet ihn.

![FX-Editor mit Nummern](docs/images/guide-fx-editor.png)

1. Zurück
2. **Tippen**, um ein anderes Modell zu wählen. Das Symbol ist gefüllt, wenn der Effekt an ist
3. Effekt an / aus
4. **FX-Presets**: Tippen = laden, **halten** = aktuelle Einstellungen unter einem Namen speichern
5. **Szene**: diese Einstellungen für eine [Szene](#szenen) des Presets – sie setzt sie bei jedem Tritt auf ihren Schalter
6. Auf einem Balken **seitlich wischen** ändert den Wert; hoch und runter scrollt

Werte sind sichtbar, solange der Effekt an ist. *Off | On* wird angetippt. Mehr: [Handbuch](docs/manual.de.md#fx-editor).

### Capture, Cab und Stimmgerät

<table>
<tr>
<td width="33%"><img src="docs/images/06-capture-library.png" alt="Capture-Library"></td>
<td width="33%"><img src="docs/images/14-capture-amp.png" alt="Capture-Klang"></td>
<td width="33%"><img src="docs/images/08-tuner.png" alt="Stimmgerät"></td>
</tr>
<tr>
<td>

**Tippen** auf das Capture- oder Cab-Feld: die Slots des Nano oder seine ganze Library mit Filtern.

</td>
<td>

**Halten** auf dem Capture-Feld für Gain, Bass, Mid und Treble – auf dem Cab-Feld für Output, High und Low Pass.

</td>
<td>

**Fußschalter 2** öffnet das Stimmgerät, mit Kammerton und Stummschaltung. Tippen schließt es.

</td>
</tr>
</table>

### Looper-Modus

<table>
<tr>
<td width="50%"><img src="docs/images/18-looper-mode.png" alt="Looper-Modus"></td>
<td>

**Fußschalter 1 halten**: Die Kacheln werden zu den Schaltern einer Looper-App auf dem Telefon, zum Beispiel
Loopy Pro, das über den Nano spielt. Jeder Tritt auf Fußschalter 1 geht zurück.

- In der App das Bluetooth-Gerät *Nano Cortex Controller* verbinden und die Schalter per MIDI Learn belegen
- Eine Kachel **halten**, um Name, Farbe und Symbol zu ändern

Einrichtung Schritt für Schritt: [Handbuch](docs/manual.de.md#looper-modus-eine-looper-app-auf-dem-telefon).

</td>
</tr>
</table>

### Touch und Fußschalter auf einen Blick

| Aktion | Wo | Was passiert |
|---|---|---|
| **Tippen** | eine Kachel | dasselbe wie ihr Fußschalter |
| | der **Szenen**-Knopf (obere Leiste) | Kacheln 3–8: Szenen des Presets ↔ Presets der Bank |
| | der **Pedal**-Knopf (obere Leiste) | was das Expression-Pedal des Nano in diesem Preset bewegt |
| **Halten** | eine Preset-Kachel | Bank-Fenster: Preset, Farbe und Symbol dieses Schalters |
| | eine Szenen-Kachel | die Szene: Name, Farbe und welche Effekte an sind |
| | eine FX-Kachel | FX-Editor |
| | Kachel 8 im FX-Modus | Reverb-Schalter: Mix-Positionen oder ein zweites Reverb |
| | Kachel 1 | Looper-Modus an / aus |
| | eine Looper-Kachel | ihr Name, ihre Farbe und ihr Symbol |
| | Kachel 2 | lernen, welcher Fußschalter welcher ist |
| | der Preset-Name | Preset umbenennen |
| | Capture- / Cab-Feld | Capture-Klang / Cab-Einstellungen |
| **Wischen** links / rechts | überall | nächstes / vorheriges Preset |
| **Wischen** hoch / runter | überall | Gig-Ansicht an / aus |
| **Fußschalter 1** | | kurz: Presets oder Szenen ↔ FX · gehalten: Looper-Modus |
| **Fußschalter 2** | | kurz: Stimmgerät · gehalten: Szenen ↔ Presets |
| **Fußschalter 3** gehalten | FX-Modus | Pre FX 1 wechselt zum zweiten Effekt |

## Mehr

- **[Handbuch](docs/manual.de.md)** – alle Funktionen im Detail: Bänke, Szenen, Expression-Pedal, Capture und Cab, FX-Presets, Reverb-Schalter,
  zweiter Effekt, [Bluetooth-MIDI](docs/manual.de.md#bluetooth-midi) mit allen Befehlen, Looper-Modus, der Editor über
  den Controller, [Verkabelung der Fußschalter](docs/manual.de.md#fußschalter), was wo gespeichert ist
- **[Entwicklung](docs/development.de.md)** – selbst bauen, Aufbau des Projekts, serielle Konsole, wie der Controller
  mit dem Nano spricht
- **[Web-Version](web/README.md)** – wie die Browser-Fassung gebaut wird

Die Bildschirmfotos zeigen Beispiel-Presets und werden aus dem UI-Code der Firmware gerendert
(`tools/screenshots/make_screenshots.sh`).

## Danksagung

Aufgebaut auf dem [Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor) (Protokoll, FX-Tabellen),
[rixrix/deskop-nano-cortex](https://github.com/rixrix/deskop-nano-cortex) (Protokoll-Notizen),
[Builty/TonexOneController](https://github.com/Builty/TonexOneController) (die Idee, Display- und
Fußschalter-Aufbau), [ESP-IDF](https://github.com/espressif/esp-idf) mit NimBLE,
[LVGL](https://github.com/lvgl/lvgl) und [IBM Plex Sans](https://github.com/IBM/plex).
Die vollständige Liste mit Lizenzen: [docs/credits.de.md](docs/credits.de.md).

## Lizenz

[MIT](LICENSE) für den Code dieses Projekts. Die oben genannten Komponenten behalten ihre eigenen Lizenzen.
