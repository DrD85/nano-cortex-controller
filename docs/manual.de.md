# Nano Cortex Controller – Handbuch

Alle Funktionen im Detail. Die kurze bebilderte Anleitung steht im [README](../README.de.md); wie die Firmware gebaut wird und wie sie mit dem Nano spricht, steht in [development.de.md](development.de.md).

**Inhalt:** [Der Bildschirm](#der-bildschirm) · [Fußschalter und Kacheln](#fußschalter-und-kacheln) · [Lange drücken](#lange-drücken) · [Eigene Bänke](#eigene-bänke) · [Capture und Cab/IR](#capture-und-cabir) · [FX-Editor](#fx-editor) · [Reverb-Schalter](#reverb-schalter-fußschalter-8) · [Zweiter Effekt auf Pre FX 1](#zweiter-effekt-auf-pre-fx-1-fußschalter-3) · [Stimmgerät](#stimmgerät) · [USB-Audio-Lautstärke](#usb-audio-lautstärke) · [Bluetooth-MIDI](#bluetooth-midi) · [Looper-Modus](#looper-modus-eine-looper-app-auf-dem-telefon) · [Editor über den Controller](#nano-cortex-editor-über-den-controller) · [Fußschalter-Learn](#fußschalter-learn) · [Fußschalter (Verkabelung)](#fußschalter) · [Was wo gespeichert ist](#was-wo-gespeichert-ist)

## Der Bildschirm

<img src="images/guide-main.png" alt="" width="640">

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

## Fußschalter und Kacheln

| Schalter | Preset-Modus | FX-Modus |
|---|---|---|
| 1 | in den FX-Modus wechseln; **gehalten: Looper-Modus** | in den Preset-Modus wechseln; **gehalten: Looper-Modus** |
| 2 | Stimmgerät an/aus | Stimmgerät an/aus |
| 3–8 | die sechs Presets der aktuellen Bank | 3–7: Pre FX 1, Pre FX 2, Post FX 1–3 an/aus |
| 8 | (sechstes Preset) | Reverb: Mix Pos 1 ↔ Pos 2 oder Reverb A ↔ B |

Aktive Kacheln leuchten in voller Farbe, inaktive sind gedimmt. FX-Kacheln tragen die Farben der Effektkategorien.

## Lange drücken

| Wo | Was sich öffnet |
|---|---|
| Kachel 1 | **Looper-Modus** an / aus (wie Fußschalter 1 halten) |
| Kachel 2–8 im Looper-Modus | **Looper-Kachel**: Name, Farbe und Symbol dieses Schalters |
| Kachel 2 | **Learn** für Fußschalter 2 – und mit *Switch 1* in diesem Fenster für Fußschalter 1 |
| Preset-Kachel (3–8, Preset-Modus) | **Bank-Fenster**: Farbe, Symbol und Preset dieses Schalters; `DEFAULT` stellt das Standard-Preset her; `LEARN SWITCH` |
| FX-Kachel (3–7, FX-Modus) | **FX-Editor**: Modell (auf den Kopf tippen), an/aus und alle Parameter |
| Kachel 8 (FX-Modus) | **Reverb-Fenster** mit den Reitern *MIX POS 1 / 2* und *2ND REVERB* |
| Preset-Name | **Umbenennen** mit Bildschirmtastatur (mindestens 4 Zeichen, eindeutig) |

## Eigene Bänke

<img src="images/05-bank-editor.png" alt="" width="440">

16 Bänke mit je sechs Schaltern. Ab Werk liegen in Bank 1 die Presets 1–6, in Bank 2 die Presets 7–12 und so weiter.
Lange auf eine Preset-Kachel drücken, dann beliebiges Preset, eine von zehn Farben und eines von 19 Symbolen wählen
(Clean, Edge, Drive, Solo, Fuzz, Atmospheric, Metal, Boost, Rhythm, Bass, Acoustic, Blues, Live, Favorite,
Fuzz Wave, Guitarist, Rocket, Space, Swell).
Die Bänke speichert der Controller, die Presets selbst bleiben auf dem Nano.

## Capture und Cab/IR

<img src="images/06-capture-library.png" alt="" width="49%"> <img src="images/14-capture-amp.png" alt="" width="49%">

<img src="images/12-cab-settings.png" alt="" width="49%"> <img src="images/11-capture-volume.png" alt="" width="49%">

Auf das Feld CAPTURE oder CAB / IR tippen. **SLOTS** zeigt die 25 Capture-Slots (5 Bänke × 5) bzw. die 5 Cab-Slots,
**LIBRARY** alle auf dem Nano gespeicherten Captures bzw. IRs mit Kategorie-Filtern (AMP = Topteil oder Combo, AMP+CAB, CAB, PEDAL, OTHER).
Ein Library-Eintrag wird nach einer Rückfrage in den aktiven Slot geladen.

**Capture-Lautstärke**: **VOL** im Preset-Feld, −24 dB bis +12 dB (**0 dB** setzt zurück). **Capture-Klang**: lange
auf das CAPTURE-Feld drücken – **GAIN**, **BASS**, **MID** und **TREBLE** des Captures, 0–10. **Cab-Einstellungen**: lange auf das CAB / IR-Feld drücken – **OUTPUT** (−96 dB bis +12 dB),
**HIGH PASS** (20–800 Hz) und **LOW PASS** (1–20 kHz) des aktiven Cabs, beim Öffnen vom Nano gelesen.
Alles gehört zum Preset: Änderungen sind sofort zu hören, **SAVE** behält sie.

## FX-Editor

<img src="images/04-fx-editor.png" alt="" width="49%"> <img src="images/16-fx-model.png" alt="" width="49%">

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

## Reverb-Schalter (Fußschalter 8)

<img src="images/07-reverb.png" alt="" width="440">

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

## Zweiter Effekt auf Pre FX 1 (Fußschalter 3)

<img src="images/13-second-effect.png" alt="" width="440">

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

## Stimmgerät

<img src="images/08-tuner.png" alt="" width="440">

Fußschalter 2 oder die Kachel TUNER. Sieht aus wie der Tuner im Desktop-Editor: große Note in der Stimmfarbe (grün =
gestimmt, orange = daneben), eine Skala mit einem Strich alle 10 Cent und eine leuchtende Nadel, dazu die Abweichung
in Cent. Unten stellt **Reference A4** den Kammerton ein (400–480 Hz, − / + gedrückt halten läuft durch), und
**Mute output** schaltet den Nano beim Stimmen stumm. Bildschirm antippen oder Fußschalter 2 erneut drücken zum Schließen.

## USB-Audio-Lautstärke

<img src="images/09-usb-volume.png" alt="" width="440">

**USB** im Preset-Feld: Lautstärke des Audios, das der Computer über USB abspielt, −40 dB (OFF) bis 0 dB,
wie in der offiziellen App. Der Wert wird jedes Mal frisch vom Nano gelesen.

## Bluetooth-MIDI

<img src="images/10-bluetooth-midi.png" alt="" width="440">

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
| CC 59 | Looper-Modus: Wert 64–127 an, 0–63 aus |

Ein MIDI-Kabeleingang ist auf diesem Board ohne zusätzliche Hardware nicht möglich; dafür einen
Bluetooth-MIDI-Adapter verwenden.

## Looper-Modus (eine Looper-App auf dem Telefon)

<img src="images/18-looper-mode.png" alt="" width="49%"> <img src="images/19-looper-tile.png" alt="" width="49%">

Der Controller kann der Fußcontroller einer Looper-App sein, zum Beispiel **Loopy Pro** auf iPhone oder iPad. Das
Telefon spielt über den Nano: Am USB-C-Anschluss ist der Nano das Audio-Interface des Telefons – die Gitarre geht
in die App, Loops (und Backing Tracks) kommen aus dem Nano. Ihre Lautstärke ist die *USB-Audio-Lautstärke* (siehe
oben). Die Fußschalter erreichen die App per **Bluetooth-MIDI**: Der Controller ist ein Bluetooth-MIDI-Gerät mit
dem Namen **„Nano Cortex Controller“**.

1. In Loopy Pro im Hauptmenü **Bluetooth Devices** öffnen und *Nano Cortex Controller* wählen. Der Controller zeigt
   *Phone connected* und wie oft MIDI ausgetauscht wird (auf einem iPhone alle 11,25 oder 15 ms).
2. **Fußschalter 1 halten** (0,6 s) oder lange auf Kachel 1 drücken: Die Kacheln werden zu den Schaltern des
   Loopers. Jeder Tritt auf Fußschalter 1 verlässt den Modus wieder; FX und Presets bleiben, wie sie waren.
3. In Loopy Pro **MIDI Learn** wählen, antippen, was ein Schalter tun soll (für einen Loop: *Abspielen/Stoppen*
   mit *Aufnahme, wenn leer*), und diesen Fußschalter treten. Jeden Schalter nur einmal belegen: Zwei Belegungen auf
   demselben Loop heben sich gegenseitig auf.

| Fußschalter | Sendet (MIDI-Kanal 16) | Kachel (ab Werk) |
|---|---|---|
| 2 | CC 102 | Pause |
| 3–8 | CC 103–108 | Loop 1–6 |

**Eigene Namen**: Langes Drücken auf eine Kachel im Looper-Modus öffnet ihr Fenster – Name (Tastatur), Farbe und
Symbol, gespeichert auf dem Controller; *Default* stellt die oben gezeigte Kachel wieder her. Kachel 1 zeigt, was der
letzte Tritt gesendet hat (*Sent CC 103*) – hilfreich beim Belegen eines Schalters in der App.

Ab Werk zeigen die Kacheln die Loops in drei Farbpaaren, wie ein zweispaltiges Loopy-Pro-Projekt. Was ein Schalter
tut, bestimmt die App.

Ein Fußschalter sendet beim Treten den Wert 127 und beim Loslassen 0; so funktionieren die Auslöser *Hold*
und *Double Tap* der App. Antippen einer Kachel sendet einen kurzen Tritt. Im Looper-Modus geht ein Tritt 2–4 ms nach
dem Schließen des Kontakts hinaus; über Bluetooth wartet er dann auf den nächsten Austausch mit dem Telefon –
höchstens das beim Verbinden angezeigte Intervall. Bei Loops, die auf dem Takt beginnen und enden (Quantisierung der
App), spielt das keine Rolle; ein frei aufgenommener erster Loop kann um so viel länger oder kürzer werden.

Telefon und Nano Cortex Editor können gleichzeitig verbunden sein. Was die App zurückschickt (Rückmeldung für die
Lämpchen eines Controllers), steht vorerst im seriellen Protokoll.

## Nano Cortex Editor über den Controller

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

## Fußschalter-Learn

Lange auf Kachel 2 drücken (Fußschalter 2; **Switch 1** im Fenster lernt stattdessen Fußschalter 1) oder **LEARN SWITCH**
im Bank-Fenster. Dann innerhalb von 15 Sekunden den
Fußschalter drücken, der diese Funktion bekommen soll. Hatte er eine andere Funktion, werden die beiden getauscht.
**DEFAULT ORDER** stellt die Standard-Reihenfolge wieder her.

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

## Was wo gespeichert ist

| Auf dem Nano | Auf dem Controller |
|---|---|
| Presets, Namen, Captures, Cabs, FX und ihre Werte, Expression-Einstellungen (Pos 1 / Pos 2), USB-Lautstärke | eigene Bänke (Preset, Farbe, Symbol), Fußschalter-Reihenfolge, zweite Reverbs, das Bluetooth-MIDI-Gerät |
