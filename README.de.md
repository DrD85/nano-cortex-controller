# Nano Cortex Controller (inoffiziell)

![Nano Cortex Controller](docs/images/00-overview.png)

Ein Touchscreen- und Fußschalter-Controller für den **Neural DSP Nano Cortex**, verbunden per Bluetooth.
Er läuft auf einem **Waveshare ESP32-S3-Touch-LCD-4.3** und macht daraus einen Bodencontroller im Stil des
Quad Cortex: acht farbige Kacheln für acht Fußschalter, ein vollständiger FX-Editor, die Capture- und
IR-Library, ein Stimmgerät und eigene Preset-Bänke. Er ist das Hardware-Gegenstück zum
[Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor).

> **Inoffizielles Community-Projekt.** Nicht verbunden mit, unterstützt oder betreut von Neural DSP.
> „Nano Cortex“, „Quad Cortex“ und „Neural DSP“ sind Marken von Neural DSP Technologies.
> Der Controller schreibt auf dein Gerät. Nutzung auf eigene Gefahr – sichere deine Presets vorher
> (zum Beispiel mit der offiziellen Cortex-Cloud-App).

**English:** [README](README.md)

**Ohne Board ausprobieren:** Dieselbe Firmware läuft auch im Browser (Chrome oder Edge) – mit Bildschirm und acht
Fußschalter-Tasten, per Web Bluetooth mit deinem Nano Cortex verbunden:
[Nano Cortex Controller Web](https://github.com/DrD85/nano-cortex-controller-web) (wie sie gebaut wird: [web/](web/README.md)).

![Preset-Modus](docs/images/01-preset-mode.png)

## Funktionen

- **Presets**: sechs Presets pro Bank auf den Fußschaltern 3–8, 16 eigene Bänke mit Farbe und Symbol pro
  Schalter, vorheriges/nächstes Preset per Wischen, Umbenennen und Speichern auf dem Nano, folgt
  Preset-Wechseln am Pedal
- **FX-Modus**: Fußschalter 3–7 schalten Pre FX 1–2 und Post FX 1–3, Kacheln in den Farben der Effektkategorien
- **Zweiter Effekt auf Pre FX 1**: Fußschalter 3 kurz = an/aus, gehalten = Wechsel zu einem zweiten Effekt (z. B. Auto-Wah)
- **FX-Editor** (lange auf eine FX-Kachel drücken): Modell wählen und alle Parameter live einstellen, mit vier
  benannten **FX-Presets** pro Effektmodell
- **Capture und Cab/IR**: einen der 25 Capture-Slots oder 5 Cab-Slots wählen oder jedes Capture bzw. IR aus der
  Library des Nano (Werk und eigene) in den aktiven Slot laden
- **Capture-Lautstärke** (VOL), **Capture-Klang** (lange auf das Capture-Feld drücken: Gain, Bass, Mid, Treble) und
  **Cab-Einstellungen** (lange auf das Cab-Feld drücken: Output, High Pass, Low Pass)
- **Reverb-Schalter** (Fußschalter 8): schaltet den Reverb-Mix zwischen Pos 1 und Pos 2 der Expression-Einstellung
  des Presets um – oder zwischen dem Reverb des Presets und einem **zweiten Reverb** mit eigenen Einstellungen
- **Stimmgerät** mit großer Notenanzeige und Nadel (Fußschalter 2)
- **USB-Audio-Lautstärke** des Nano (Wiedergabe vom Computer), wie in der offiziellen App
- **App-Brücke**: der Nano Cortex Editor (Mac-App oder Browser) kann sich über den Controller verbinden, während
  der Controller mit dem Nano verbunden ist – App und Controller arbeiten gleichzeitig
- **Bluetooth-MIDI**: einen MIDI-Controller kabellos verbinden – zum Beispiel ein Morningstar MC6 mit einem
  WIDI-Adapter – mit den MIDI-Befehlen des Nano selbst (Program Change, CC 37–41, CC 1)
- **Fußschalter-Learn**: jeden Fußschalter jeder Funktion zuordnen
- **Gig-Ansicht** (nach oben wischen): große Kacheln und eine schmale Statusleiste (Preset, Bank, Modus,
  ungespeicherte Änderungen), gut lesbar aus der Entfernung; gefüllt = an, umrandet = aus, wie im Desktop-Editor
- Bis zu 8 Fußschalter an einem SX1509-I/O-Expander (optional – der Touchscreen funktioniert auch allein)

## Bildschirmfotos

| | |
|---|---|
| ![FX-Modus](docs/images/02-fx-mode.png) | ![Gig-Ansicht (nach oben wischen)](docs/images/03-fullscreen.png) |
| FX-Modus | Gig-Ansicht (nach oben wischen) |
| ![FX-Editor](docs/images/04-fx-editor.png) | ![Eigene Bänke: Farbe, Symbol und Preset](docs/images/05-bank-editor.png) |
| FX-Editor | Eigene Bänke: Farbe, Symbol und Preset |
| ![Capture-Library](docs/images/06-capture-library.png) | ![Reverb-Schalter: zweites Reverb](docs/images/07-reverb.png) |
| Capture-Library | Reverb-Schalter: zweites Reverb |
| ![Stimmgerät](docs/images/08-tuner.png) | ![USB-Audio-Lautstärke](docs/images/09-usb-volume.png) |
| Stimmgerät | USB-Audio-Lautstärke |
| ![Bluetooth-MIDI](docs/images/10-bluetooth-midi.png) | ![Capture-Lautstärke](docs/images/11-capture-volume.png) |
| Bluetooth-MIDI | Capture-Lautstärke |
| ![Cab-Einstellungen](docs/images/12-cab-settings.png) | ![Zweiter Effekt auf Pre FX 1](docs/images/13-second-effect.png) |
| Cab-Einstellungen (lange auf das Cab-Feld drücken) | Zweiter Effekt auf Pre FX 1 (A \| B im FX-Editor) |
| ![Capture-Klang](docs/images/14-capture-amp.png) | ![Startbildschirm](docs/images/15-start-screen.png) |
| Capture-Klang (lange auf das Capture-Feld drücken) | Startbildschirm beim Verbinden |
| ![Modell wählen](docs/images/16-fx-model.png) | ![FX-Editor, Effekt aus](docs/images/17-fx-off.png) |
| Modell wählen (auf den Kopf des FX-Editors tippen) | FX-Editor bei ausgeschaltetem Effekt |

Die Bilder zeigen Beispiel-Presets. Sie werden aus dem UI-Code der Firmware gezeichnet
(`tools/screenshots/make_screenshots.sh`).

## Was du brauchst

| Teil | Hinweis |
|---|---|
| [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) | 800 × 480 Touchscreen, Version mit zwei USB-C-Buchsen (**USB** und **UART**) |
| Neural DSP Nano Cortex | Bluetooth an; kein Pairing nötig |
| Optional: SX1509-Platine + bis zu 8 Taster | siehe [Fußschalter](#fußschalter) |

## Firmware installieren (im Browser, ohne Werkzeuge)

Die Firmware liegt auf der Seite [Releases](../../releases). Jedes Release hat zwei Dateien:

| Datei | Adresse | Wofür |
|---|---|---|
| `nano-controller-<version>-full.bin` | `0x0` | **Erstinstallation** (Bootloader, Partitionstabelle und App). Löscht gespeicherte Bänke und Einstellungen. |
| `nano-controller-<version>-update.bin` | `0x10000` | **Updates**: nur die App – deine Bänke, Symbole, Fußschalter-Reihenfolge und zweiten Reverbs bleiben erhalten |

1. **[esptool.spacehuhn.com](https://esptool.spacehuhn.com)** in **Chrome** oder **Edge** auf einem Computer öffnen
   (Safari und Firefox können kein Web Serial).
2. Das Board über die USB-C-Buchse **USB** anschließen (nicht UART).
3. **Connect** klicken und den Anschluss des Boards wählen (meist „USB JTAG/serial debug unit“).
   Erscheint kein Anschluss: **BOOT** gedrückt halten, **RESET** kurz drücken, **BOOT** loslassen und erneut versuchen.
4. Die `.bin`-Datei hinzufügen und die Adresse eintragen: `0x0` für die `-full.bin`, `0x10000` für die `-update.bin`.
5. **Program** klicken und warten, bis das Flashen fertig ist.
6. **RESET** am Board drücken (oder das Kabel kurz abziehen).

Die Prüfsummen beider Dateien stehen in `nano-controller-<version>-sha256.txt`.

## Erster Start

1. Den Nano Cortex einschalten.
2. Die **Cortex-Cloud-App** und den **Nano Cortex Editor** schließen – der Nano erlaubt immer nur eine
   Bluetooth-Verbindung.
3. Das Board mit Strom versorgen. Es sucht den Nano („SEARCHING FOR THE NANO…“), verbindet sich, liest alle
   Preset-Namen und zeigt das aktuelle Preset. Findet es den Nano nicht innerhalb einer Minute, den Nano einmal
   aus- und wieder einschalten.

## Bedienung

### Der Bildschirm

```
┌──────────────────────────────────────────────────────────┐
│ ↻  VOL  MIDI  USB     ● PRESET 7 · BANK 2          Save  │  obere Leiste: Symbole, Status, Save
│  ◀               Revv NF53 Clean •                    ▶  │  Pfeile = Bank −/+, lange auf den Namen: umbenennen
├────────────────────────────┬─────────────────────────────┤
│ CAPTURE  Revv D20 clean    │ CAB / IR  810 Amped VT      │  antippen: Slot oder Library wählen
│                            │                             │  lange drücken: Gain, Bass, Mid, Treble / Cab-Einstellungen
├──────────┬──────────┬──────┴───┬──────────────────────────┤
│ 1 MODE   │ 2 TUNER  │ 3        │ 4                        │  acht Kacheln = acht Fußschalter
├──────────┼──────────┼──────────┼──────────────────────────┤
│ 5        │ 6        │ 7        │ 8                        │
└──────────┴──────────┴──────────┴──────────────────────────┘
```

- **Kachel antippen** = ihren Fußschalter drücken. Eine Kachel leuchtet kurz auf, wenn ihr Fußschalter gedrückt wird.
- **Kacheln wie in der Signal Chain des Desktop-Editors**: das aktive Preset oder ein eingeschalteter Effekt ist in
  seiner Farbe **gefüllt**, alles andere dunkel mit **farbigem Rand** und der gedimmten Farbe hinter der oberen Zeile –
  auch von weiter weg gut zu unterscheiden.
  Alle Namen haben dieselbe Größe.
- **↻** (oben links) liest alles neu vom Nano: Preset-Namen, aktuelles Preset und die Library.
- Das **Lautsprecher**-Symbol daneben stellt die Capture-Lautstärke des aktuellen Presets ein, danach **MIDI** und **USB**.
- **Nach links / rechts wischen**: nächstes / vorheriges Preset.
- **Nach oben wischen – Gig-Ansicht**: die Kacheln füllen den Bildschirm mit größeren Namen, eine schmale Leiste
  oben zeigt Verbindung, Preset-Nummer, Bank, Preset-Name, ungespeicherte Änderungen und den Modus (PRESETS / FX).
  **Nach unten wischen**: zurück.
- Der grüne Punkt zeigt die Bluetooth-Verbindung zum Nano; **Save** wird grün und neben dem Namen erscheint ein
  oranger Punkt, wenn das Preset ungespeicherte Änderungen hat. **MIDI** wird grün, solange ein Bluetooth-MIDI-Controller verbunden ist.
- Die **Capture-Karte** zeigt wie im Editor den Typ des Captures (Amp-Head, Combo, Amp + Cab, Cab, Pedal …).

### Fußschalter und Kacheln

| Schalter | Preset-Modus | FX-Modus |
|---|---|---|
| 1 | in den FX-Modus wechseln | in den Preset-Modus wechseln |
| 2 | Stimmgerät an/aus | Stimmgerät an/aus |
| 3–8 | die sechs Presets der aktuellen Bank | 3–7: Pre FX 1, Pre FX 2, Post FX 1–3 an/aus |
| 8 | (sechstes Preset) | Reverb: Mix Pos 1 ↔ Pos 2 oder Reverb A ↔ B |

Aktive Kacheln leuchten in voller Farbe, inaktive sind gedimmt. FX-Kacheln tragen die Farben der Effektkategorien.

### Lange drücken

| Wo | Was sich öffnet |
|---|---|
| Kachel 1 oder 2 | **Learn** für diesen Fußschalter (siehe unten) |
| Preset-Kachel (3–8, Preset-Modus) | **Bank-Fenster**: Farbe, Symbol und Preset dieses Schalters; `DEFAULT` stellt das Standard-Preset her; `LEARN SWITCH` |
| FX-Kachel (3–7, FX-Modus) | **FX-Editor**: Modell (auf den Kopf tippen), an/aus und alle Parameter |
| Kachel 8 (FX-Modus) | **Reverb-Fenster** mit den Reitern *MIX POS 1 / 2* und *2ND REVERB* |
| Preset-Name | **Umbenennen** mit Bildschirmtastatur (mindestens 4 Zeichen, eindeutig) |

### Eigene Bänke

16 Bänke mit je sechs Schaltern. Ab Werk liegen in Bank 1 die Presets 1–6, in Bank 2 die Presets 7–12 und so weiter.
Lange auf eine Preset-Kachel drücken, dann beliebiges Preset, eine von zehn Farben und eines von 19 Symbolen wählen
(Clean, Edge, Drive, Solo, Fuzz, Atmospheric, Metal, Boost, Rhythm, Bass, Acoustic, Blues, Live, Favorite,
Fuzz Wave, Guitarist, Rocket, Space, Swell).
Die Bänke speichert der Controller, die Presets selbst bleiben auf dem Nano.

### Capture und Cab/IR

Auf das Feld CAPTURE oder CAB / IR tippen. **SLOTS** zeigt die 25 Capture-Slots (5 Bänke × 5) bzw. die 5 Cab-Slots,
**LIBRARY** alle auf dem Nano gespeicherten Captures bzw. IRs mit Kategorie-Filtern (AMP = Topteil oder Combo, AMP+CAB, CAB, PEDAL, OTHER).
Ein Library-Eintrag wird nach einer Rückfrage in den aktiven Slot geladen.

**Capture-Lautstärke**: **VOL** im Preset-Feld, −24 dB bis +12 dB (**0 dB** setzt zurück). **Capture-Klang**: lange
auf das CAPTURE-Feld drücken – **GAIN**, **BASS**, **MID** und **TREBLE** des Captures, 0–10. **Cab-Einstellungen**: lange auf das CAB / IR-Feld drücken – **OUTPUT** (−96 dB bis +12 dB),
**HIGH PASS** (20–800 Hz) und **LOW PASS** (1–20 kHz) des aktiven Cabs, beim Öffnen vom Nano gelesen.
Alles gehört zum Preset: Änderungen sind sofort zu hören, **SAVE** behält sie.

### FX-Editor

Der Kopf zeigt das Symbol des Effekts – in seiner Farbe gefüllt, wenn er an ist, nur umrandet, wenn er aus ist –
dazu Slot und Modell. Antippen öffnet die Modellauswahl, nach Art gruppiert (Drive, EQ & Utility, Wah & Filter …);
der große Schalter rechts schaltet den Effekt an oder aus.

Die Parameter sind Balken in zwei Spalten, bis zum Wert in der Effektfarbe gefüllt. **Seitlich wischen** auf einem
Balken ändert ihn: Er folgt dem Finger ab seinem aktuellen Wert, Antippen oder Scrollen lässt also keinen Wert
springen. Hoch und runter scrollt die Liste. Einstellungen mit zwei oder drei Möglichkeiten (*Off | On*, *Sync* …)
werden angetippt, längere Listen (*Sync Note* …) öffnen eine Auswahl.

Änderungen gehen schon beim Bewegen eines Reglers an den Nano. Der Nano meldet Parameterwerte nur von
eingeschalteten Effekten – einen Effekt also einschalten, um seine Werte zu sehen und zu ändern. **SAVE** im
Preset-Feld speichert das Preset auf dem Nano.

**FX-Presets** (die Leiste über den Reglern): *Original* und vier benannte Einstellungen pro Effektmodell,
gespeichert auf dem Controller und in jedem Preset und Slot mit diesem Modell nutzbar; leere Plätze zeigen **+**.

- **Halten** auf einem Platz: Die Tastatur öffnet sich, und die aktuellen Einstellungen werden unter dem
  eingegebenen Namen gespeichert. Ein leerer Name löscht den Platz.
- **Antippen** lädt die Einstellungen sofort.
- **Original** holt die Einstellungen zurück, die der Effekt beim Öffnen des Editors hatte. Direkt nach der Wahl
  eines neuen Modells sind das seine Grundeinstellungen.

Geladene Einstellungen zählen als Änderung am Nano-Preset: **SAVE** behält sie dort.

### Reverb-Schalter (Fußschalter 8)

- **MIX POS 1 / 2**: Fußschalter 8 setzt den Mix des Reverbs auf Pos 1 oder Pos 2. Das sind die Fersen- und
  Spitzenwerte der Expression-Einstellung des Presets für das Reverb („Post FX 3 Amount“). Beim Bewegen eines
  Reglers hörst du den Mix; **SAVE** schreibt beide Werte ins Preset (und legt die Expression-Einstellung an,
  falls es noch keine gibt).
- **2ND REVERB**: ein zweites Reverb (B) für dieses Preset wählen. Fußschalter 8 wechselt dann zwischen dem
  Reverb des Presets (A) und B. Der Nano hat nur einen Reverb-Platz, deshalb tauscht der Controller das Modell aus
  und schickt die gespeicherten Werte (dauert etwa eine halbe Sekunde). Kachel 8 zeigt Reverb B und leuchtet,
  solange es läuft; Kachel 7 zeigt das Reverb, das gerade im Slot ist. **EDIT B** lädt B und öffnet den FX-Editor
  zum Einstellen. Reverb B speichert der Controller, pro Preset. Wird das Preset gespeichert, während B läuft,
  wird B zum Reverb des Presets.

### Zweiter Effekt auf Pre FX 1 (Fußschalter 3)

Lange auf die Pre-FX-1-Kachel drücken, dann **+B** (neben dem Modell im FX-Editor): einen zweiten Effekt (B) für
dieses Preset wählen, zum Beispiel das Envelope Filter als Auto-Wah neben einem Drive. **A | B** zeigt dann, welcher
läuft: den anderen antippen tauscht sofort, den laufenden antippen wählt einen anderen zweiten Effekt oder *None*.
Dann Fußschalter 3 im FX-Modus:

- **kurz drücken**: den Effekt im Slot an / aus (schaltet beim Loslassen)
- **halten** (0,6 s): A ↔ B wechseln – der Effekt bleibt an oder aus, wie er war

Die Kachel zeigt *Pre FX 1 A* oder *Pre FX 1 B* und darunter den anderen Effekt (⇄ Name). Der Nano hat pro Slot ein Modell, deshalb tauscht der Controller
das Modell und schickt die gespeicherten Werte des anderen Effekts: das dauert etwa eine halbe Sekunde mit einem
kurzen Aussetzer. Ohne Aussetzer geht es, wenn der zweite Effekt in einem freien Slot liegt und nur an- und
ausgeschaltet wird. B wird im FX-Editor eingestellt, während es läuft, und auf dem Controller pro Preset gespeichert
(MIDI: CC 58). Die Einstellungen von A merkt sich der Controller ebenfalls, gelesen immer wenn A an ist. Nur wenn A
seit dem Einrichten von B nie an war, schaltet der erste Wechsel A kurz an, um sie zu lesen.

### Stimmgerät

Fußschalter 2 oder die Kachel TUNER. Sieht aus wie der Tuner im Desktop-Editor: große Note in der Stimmfarbe (grün =
gestimmt, orange = daneben), eine Skala mit einem Strich alle 10 Cent und eine leuchtende Nadel, dazu die Abweichung
in Cent. Unten stellt **Reference A4** den Kammerton ein (400–480 Hz, − / + gedrückt halten läuft durch), und
**Mute output** schaltet den Nano beim Stimmen stumm. Bildschirm antippen oder Fußschalter 2 erneut drücken zum Schließen.

### USB-Audio-Lautstärke

**USB** im Preset-Feld: Lautstärke des Audios, das der Computer über USB abspielt, −40 dB (OFF) bis 0 dB,
wie in der offiziellen App. Der Wert wird jedes Mal frisch vom Nano gelesen.

### Bluetooth-MIDI

**MIDI** im Preset-Feld öffnet die Geräteliste. Sie zeigt Bluetooth-MIDI-Geräte in der Nähe – zum Beispiel einen
CME-WIDI-Adapter an der MIDI-Buchse eines Morningstar MC6 oder einen
Bluetooth-MIDI-Fußschalter. Antippen verbindet; der Controller merkt sich das Gerät und verbindet sich wieder von
selbst, sobald es eingeschaltet ist. **FORGET DEVICE** trennt und vergisst es. Der Nano bleibt dabei verbunden.

Der Controller versteht dieselben Befehle wie der Nano selbst über USB-MIDI, auf allen MIDI-Kanälen. Bänke für den
Nano (zum Beispiel aus dem MC6-Export des Nano Cortex Editors) funktionieren also auch über Bluetooth:

| Befehl | Wirkung |
|---|---|
| Program Change 0–63 | Preset 1–64 |
| CC 37–41 | FX-Slot 1–5 (Pre FX 1, Pre FX 2, Post FX 1–3): Wert 64–127 an, 0–63 aus |
| CC 1 | Expression: Reverb-Mix von Pos 1 (0) bis Pos 2 (127) |
| CC 50–57, Wert 64–127 | Fußschalter 1–8 drücken (Modus, Stimmgerät, Presets oder FX, Reverb-Schalter) |
| CC 58, Wert 64–127 | Fußschalter 3 halten: Pre FX 1 wechselt zum zweiten Effekt und zurück |

Ein MIDI-Kabeleingang ist auf diesem Board ohne zusätzliche Hardware nicht möglich; dafür einen
Bluetooth-MIDI-Adapter verwenden.

### Nano Cortex Editor über den Controller

Der Nano erlaubt nur eine Bluetooth-Verbindung. Ist der Controller mit dem Nano verbunden, bietet er sich selbst als
**„Nano Cortex Controller“** mit demselben Bluetooth-Dienst wie der Nano an. Der
[Nano Cortex Editor](https://github.com/DrD85/nano-cortex-editor) verbindet sich dann mit dem Controller:
**App ↔ Controller ↔ Nano**. Die Mac-App wählt ihn automatisch; im Browser „Nano Cortex Controller“ auswählen.
Ohne Controller (oder bevor er verbunden ist) verbindet sich die App wie bisher direkt mit dem Nano.

- Alles, was die App schickt, geht an den Nano weiter; Antworten auf Anfragen der App gehen an die App, Antworten
  auf Anfragen des Controllers bleiben im Controller, Meldungen, die der Nano von sich aus schickt, gehen an beide.
- Änderungen in der App erscheinen kurz danach am Controller; Änderungen am Controller (Fußschalter, Touch, MIDI)
  lassen die App das Preset neu lesen, wie nach einem Wechsel am Pedal.
- Solange die App verbunden ist, steht **APP** in der Statuszeile.

### Fußschalter-Learn

Lange auf Kachel 1 oder 2 drücken oder **LEARN SWITCH** im Bank-Fenster. Dann innerhalb von 15 Sekunden den
Fußschalter drücken, der diese Funktion bekommen soll. Hatte er eine andere Funktion, werden die beiden getauscht.
**DEFAULT ORDER** stellt die Standard-Reihenfolge wieder her.

### Was wo gespeichert ist

| Auf dem Nano | Auf dem Controller |
|---|---|
| Presets, Namen, Captures, Cabs, FX und ihre Werte, Expression-Einstellungen (Pos 1 / Pos 2), USB-Lautstärke | eigene Bänke (Preset, Farbe, Symbol), Fußschalter-Reihenfolge, zweite Reverbs, das Bluetooth-MIDI-Gerät |

## Fußschalter

Die Fußschalter liest ein **SX1509**-I/O-Expander (zum Beispiel die SparkFun-Platine), wie beim
[TonexOneController](https://github.com/Builty/TonexOneController).

- Den SX1509 an den **I2C**-Bus des Boards anschließen: SDA = GPIO 8, SCL = GPIO 9, 3,3 V und GND
  (den passenden Stecker deines Boards zeigt das Waveshare-Wiki).
- Die Adresse des SX1509 auf **0x71** (oder 0x70) stellen. 0x3E/0x3F belegt der I/O-Expander des Boards selbst.
- Jeden Fußschalter (Taster, Schließer) zwischen einen I/O-Pin des SX1509 und **GND** legen. Die internen
  Pull-ups werden genutzt, Widerstände sind nicht nötig.
- Standard-Reihenfolge: Schalter 1–8 an den SX1509-Pins **11, 10, 0, 1, 2, 3, 8, 9**. Andere Pins gehen auch –
  einfach per Fußschalter-Learn zuordnen.

Ohne SX1509 funktioniert der Controller nur mit dem Touchscreen.

## Serielle Konsole

Am **USB**-Anschluss zeigt jeder serielle Monitor mit 115200 Baud (zum Beispiel `idf.py monitor --no-reset`)
ein Protokoll aller Nachrichten und nimmt Befehle an: `n`/`p` nächstes/vorheriges Preset, eine Zahl (1–64)
wählt ein Preset, `a`–`e` schalten FX-Slot 1–5, `m` Modus, `t` Stimmgerät, `x` Reverb-Schalter,
`s` Preset neu lesen, `r` alles neu lesen, `l` alle Preset-Namen, `h` Hilfe.

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

## Danksagung

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

## Lizenz

[MIT](LICENSE) für den Code dieses Projekts. Die oben genannten Komponenten behalten ihre eigenen Lizenzen.
