# Feedback Guard (AU / VST3)

Rückkopplungs-Unterdrückung als Plugin. Ersetzt `feedback_guard_2ch.py` / die GUI-Version samt Python,
BlackHole und zweitem Programm. Das Plugin läuft direkt in Reaper in der Effektkette **vor EverClean**.

* Eine Instanz verarbeitet 1–8 Kanäle, jeder Kanal wird **unabhängig** analysiert (eigene Filter, eigene Anzeige).
* **Null Latenz.** Die Analyse läuft seitlich, das Signal geht nur durch die Filter.
* Keine Datei-Zugriffe, keine Speicher-Allokation und keine Ausgaben im Audio-Thread.
* Die Einstellungen werden mit dem Reaper-Projekt gespeichert. Gesetzte Filter nicht: sie werden beim Laden neu gelernt.

## 1. Installation

### Weg A: Fertig bauen lassen bei GitHub (kein Xcode nötig)

1. Kostenloses Konto auf github.com, neues **privates** Repository anlegen.
2. Den Inhalt dieses Ordners hochladen (Button „Add file → Upload files“, den ganzen Ordnerinhalt hineinziehen,
   dabei muss der versteckte Ordner `.github` mit dabei sein).
3. Reiter **Actions → Build macOS (AU + VST3) → Run workflow**. Nach ca. 10 Minuten steht unter dem Lauf
   ein Download **FeedbackGuard-macOS**.
4. ZIP entpacken. `Feedback Guard.component` nach `~/Library/Audio/Plug-Ins/Components/`,
   `Feedback Guard.vst3` nach `~/Library/Audio/Plug-Ins/VST3/` kopieren.
5. Im Terminal einmal die Download-Sperre entfernen:
   ```bash
   xattr -cr ~/Library/Audio/Plug-Ins/Components/"Feedback Guard.component" ~/Library/Audio/Plug-Ins/VST3/"Feedback Guard.vst3"
   ```

### Weg B: Selbst bauen auf dem Mac

```bash
xcode-select --install          # einmalig, falls noch nicht vorhanden
brew install cmake              # einmalig
cd FeedbackGuard
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j 8
```

Beim ersten Mal lädt CMake JUCE 8.0.6 herunter (Internet nötig). Das Plugin wird automatisch nach
`~/Library/Audio/Plug-Ins/Components/` (AU) und `~/Library/Audio/Plug-Ins/VST3/` kopiert.

### Prüfen

```bash
auval -v aufx Fbgd Fgrd         # muss mit "AU VALIDATION SUCCEEDED" enden
```

In Reaper: *Einstellungen → Plug-ins → AU → „Re-scan“* (bzw. VST → Clear cache/re-scan). Das Plugin heißt
**Feedback Guard** (Hersteller „Feedback Guard“).

## 2. Einbindung in Reaper

Das Ziel ist, dass Python und BlackHole entfallen. Die M32 ist in Reaper Ein- **und** Ausgabegerät
(dasselbe Gerät, damit gibt es keinen Takt-Drift zwischen Ein- und Ausgang).

1. *Einstellungen → Audio → Device:* Eingang und Ausgang beide **M32**, 48 kHz, Puffer **128** (bei Aussetzern 256).
2. Track 1: Eingang **M32 USB In 1** (Handmics), FX-Kette: **Feedback Guard → EverClean**, Ausgang **M32 USB Out 1** (Rückweg).
3. Track 2: Eingang **M32 USB In 2** (Headsets), gleiche Kette, Ausgang **M32 USB Out 2**.
4. Track-Eingang auf *Monitoring: Input* (Record-Arm + Monitor), damit das Signal live durchläuft.
5. Pultseitig bleibt das Routing wie bisher (Busse → USB Out, USB In → Rückweg-Kanäle).

Jeder Track hat seine eigene Plugin-Instanz mit eigenen Einstellungen. Alternativ geht auch ein Stereo-Track
mit Handmics links und Headsets rechts, dann analysiert eine Instanz beide Seiten getrennt.

## 3. Bedienung

Oben: **Aktiv** (aus = Signal läuft unverändert durch), **Nur beobachten** (erkennt und zeigt an, filtert aber nicht),
**Notbremse** (kurze Pegelabsenkung bei extrem schnellem Aufschaukeln), **Alle Filter zurücksetzen**.

Jeder Kanal zeigt Spektrum, die gesetzten Filter (rot, mit Frequenz und Absenkung), eine Alarm-LED, Pegel und Notbremse.
Unten steht ein Protokoll mit Uhrzeit, Kanal, Frequenz und Art der Erkennung.

| Regler | Bedeutung | Standard |
|---|---|---|
| Empfindlichkeit | Wie deutlich ein Ton aus dem Spektrum ragen muss. Höher = reagiert früher, mehr Fehlalarm-Risiko. | 50 % |
| Max. Absenkung | Tiefste Absenkung pro Filter. Ein neues Filter startet bei 6–12 dB und wird bei erneutem Aufschaukeln schrittweise vertieft. | 18 dB |
| Haltezeit | Nach dieser Zeit ohne erneutes Feedback blendet sich ein Filter über ca. 5 s aus. Ganz rechts (600 s) = dauerhaft. | 60 s |
| Max. Filter | Maximale Zahl gleichzeitiger Filter je Kanal. Sind alle belegt, wird das älteste ersetzt. | 8 |
| Untere / Obere Grenze | Nur in diesem Frequenzbereich wird gesucht. | 150 Hz / 10 kHz |
| Notbremse | Tiefe der kurzen Pegelabsenkung. | 6 dB |
| Reaktion | Vorsichtig / Normal / Schnell: wie lange ein Ton auffällig bleiben muss, bevor gehandelt wird. | Normal |

**Empfohlenes Vorgehen am Pult:** Beim Soundcheck erst auf *Nur beobachten* stellen und normal sprechen/singen lassen.
Erscheinen im Protokoll Einträge, obwohl kein Feedback war, Empfindlichkeit senken oder Reaktion auf *Vorsichtig* stellen.
Danach auf *Aktiv* umschalten und das Mikrofon absichtlich Richtung Monitor halten (Gain vorsichtig anheben).

## 4. Wie die Erkennung funktioniert

Die alte Python-Version hat jeden lautesten Peak als Feedback gewertet und Sprache dadurch dauernd gefiltert.
Der neue Ansatz verlangt, dass ein Ton mehrere unabhängige Merkmale erfüllt:

1. **Auflösung:** FFT mit 4096 Punkten (Python: 256 Samples, aufgefüllt), Frequenz per parabolischer Interpolation (±1–2 Hz).
2. **Schmalbandig:** Der Peak muss deutlich über seiner Umgebung liegen.
3. **Tracking:** Der Ton muss über mehrere Analyse-Schritte an derselben Frequenz bleiben (Jitter < 1 Bin).
4. **Er wächst allein.** Steigt der Pegel des restlichen Spektrums mit (Silbenanfang, Atem), ist es Sprache.
5. **Kein Oberton:** Gehört der Peak zu einer Obertonreihe (Stimme, Instrument), wird er ignoriert.

Drei Regeln lösen aus: *schnelles Aufschaukeln* (≥ 9–15 dB/s), *wachsender Ton* (≥ 2–4 dB/s) und *stehender Ton*
(hält ≥ 0,4 s, sehr deutlich, kein Oberton). Die Filter sind schmale Absenkungen (Q = 30), die weich einblenden.

## 5. Ehrliche Einschätzung / Grenzen

* **Getestet** ist die Engine mit synthetischen Signalen (Sprachnachbildung + simulierte Rückkopplungsschleife, siehe
  `Tests/test_engine.cpp`): 120 s Sprache ohne Fehlalarm, Feedback von 3 bis 25 dB/s wurde jeweils innerhalb von
  0,3–0,7 s gefangen, die Filterfrequenz sitzt auf ±1 Hz. **Nicht getestet** ist das Plugin in einem echten Raum mit
  echten Mikrofonen und Lautsprechern. Das heißt: erst im Soundcheck mit *Nur beobachten* erproben, nicht im ersten Live-Einsatz.
* Bei sehr **tiefen Stimmen** (Grundton unter ca. 100 Hz) liegen die Obertöne so dicht, dass Einzeltöne im Hochton nicht
  mehr von Obertönen zu unterscheiden sind. Dort wird vorsichtshalber *nicht* gefiltert; echtes Feedback wird dann erst
  in der nächsten Sprechpause erkannt.
* Gesang mit langem, reinem Ton wird durch den Obertontest geschützt, ein reiner Sinuston (Pfeifen, Flöte) nicht.
  Er würde nach ca. 0,4 s als „stehender Ton“ gefiltert und nach der Haltezeit wieder freigegeben.
* Das Plugin sitzt im Signalweg. Fällt Reaper oder der Mac aus, ist der Kanal weg. Ein Pult-Bypass (Original-Mikrokanäle
  direkt auf LR) sollte weiterhin vorbereitet sein.
* Der Wert 1,2 % CPU pro Kanal wurde auf einem Linux-Server gemessen, auf dem Mac liegt er in derselben Größenordnung.
* Das Plugin ist unsigniert (ad-hoc). Auf fremden Rechnern verlangt macOS ggf. eine Freigabe unter
  *Systemeinstellungen → Datenschutz & Sicherheit*.

## 6. Dateien

```
CMakeLists.txt            Projekt (lädt JUCE 8.0.6 automatisch)
Source/FeedbackEngine.h   Erkennung und Filter (reines C++, ohne JUCE)
Source/PluginProcessor.*  Plugin, Parameter, Kanalverwaltung
Source/PluginEditor.*     Oberfläche
Tests/test_engine.cpp     Engine-Test:  g++ -O2 -std=c++17 -ISource Tests/test_engine.cpp -o fg_test && ./fg_test
.github/workflows/        automatischer Bau auf einem Mac bei GitHub
```

JUCE steht unter AGPLv3 bzw. kommerzieller Lizenz. Für den privaten bzw. internen Einsatz ist das unproblematisch;
wer das Plugin weitergibt, muss den Quelltext mit weitergeben oder eine JUCE-Lizenz erwerben.
