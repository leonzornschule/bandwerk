# Bandwerk

Multiband-Kompressor/Expander nach dem Vorbild von FabFilter Pro-MB (Version 1.3x, eine Version 2 gibt es bisher nicht). Formate: VST3 und Audio Unit für macOS, Universal Binary für Apple Silicon und Intel. Der Code ist eigenständig geschrieben, nur Bedienkonzept und Funktionsumfang orientieren sich an Pro-MB.

## Bauen und installieren

Voraussetzungen sind die Xcode Command Line Tools (`xcode-select --install`) und CMake (`brew install cmake` oder CMake.app von cmake.org). Am einfachsten im Finder mit Rechtsklick auf `Bandwerk bauen.command` > Öffnen (beim ersten Mal fragt macOS nach, weil die Datei aus dem Internet kommt). Oder im Terminal im entpackten Ordner:

```
chmod +x build_mac.sh
./build_mac.sh
```

Das Skript lädt beim ersten Lauf JUCE 8.0.10 von GitHub, kompiliert beide Formate (etwa 3 bis 6 Minuten), kopiert sie nach `~/Library/Audio/Plug-Ins/VST3` und `~/Library/Audio/Plug-Ins/Components`, signiert sie ad hoc und lässt `auval` über die AU laufen. In Ableton danach unter Einstellungen > Plug-ins neu scannen. Das Plugin erscheint unter dem Hersteller "Leon".

Wer lieber nichts lokal installiert: Der Ordner `.github/workflows` enthält einen Workflow, der auf GitHub (macOS-Runner) baut und ein Zip mit beiden Bundles als Artefakt ablegt. Nach dem Download aus dem Browser muss die Quarantäne entfernt werden: `xattr -dr com.apple.quarantine Bandwerk.vst3 Bandwerk.component`.

## Bedienung

Doppelklick ins Spektrum erzeugt ein Band an dieser Stelle, die Klickhöhe wird zum Threshold. Ein Band lässt sich am Körper verschieben (waagerecht: Frequenz, senkrecht: Threshold), an den Kanten in der Breite ändern, mit dem Mausrad in der Range verstellen und per Doppelklick löschen. Shift verfeinert alle Mausbewegungen. Kanten rasten an den Kanten anderer Bänder ein, so entsteht eine klassische Frequenzweiche. Ein Band, dessen untere Kante ganz links liegt, wirkt als Tiefband (Low Shelf), entsprechend ganz rechts als Hochband.

Die Richtung der Regelung ergibt sich aus Mode und dem Vorzeichen von Range, wie bei Pro-MB:

| Mode | Range negativ | Range positiv |
|---|---|---|
| Compress | Downward Compression (Ratio inf = Limiter) | Upward Compression |
| Expand | Downward Expansion (Ratio inf = Gate) | Upward Expansion |

Pro Band gibt es Threshold, Range (-60 bis +36 dB), Ratio, Knee (bis 72 dB), Attack, Release, Lookahead (bis 20 ms), Gain, Flankensteilheit 6 bis 48 dB/oct, Stereo/Mid/Side/Left/Right, Stereo-Link, Solo, Bypass, externen Sidechain (EXT SC) und einen frei wählbaren Auslösebereich (FREE SC mit SC LOW/HIGH). Global: Processing Mode, Lookahead-Schalter, Mix 0 bis 200 % und Output.

Der Lookahead der Bänder wirkt nur, wenn oben LOOKAHEAD eingeschaltet ist. Das Plugin meldet dann 20 ms Latenz, die Ableton ausgleicht. Ohne den Schalter arbeitet Dynamic Phase und Minimum Phase latenzfrei.

Externer Sidechain in Ableton: Im Gerät den Sidechain-Bereich aufklappen (kleines Dreieck links oben am Plugin-Gerät), Quelle wählen, dann im Band EXT SC aktivieren. Für einen De-Esser reicht dagegen ein Band zwischen etwa 5 und 10 kHz mit internem Sidechain. Mit FREE SC kann ein Band auf einen anderen Bereich reagieren, zum Beispiel 200 bis 500 Hz absenken, wenn Zischlaute auftreten.

## Die drei Processing Modes

Dynamic Phase ist der Standard. Jedes Band ist ein Paar Butterworth-Shelving-Filter, deren Gain dem Kompressor folgt. Solange ein Band nicht regelt, ist das Filter mathematisch eine Identität: keine Latenz, keine Phasendrehung, Ausgang gleich Eingang bis auf die letzte Stelle. Zwei angrenzende Bänder mit gleicher Absenkung summieren sich exakt flach.

Linear Phase teilt das Signal mit linearphasigen FIR-Filtern (8192 Taps bei 44,1/48 kHz, 16384 bei 88,2/96 kHz) auf. Latenz 85 ms bei 48 kHz, plus Lookahead. Pre-Ringing ist bei schnellen Attack-Zeiten hörbar, wie bei jedem linearphasigen Multiband.

Minimum Phase ist eine klassische Linkwitz-Riley-Weiche (24 dB/oct bis zur Einstellung 24, darüber 48 dB/oct), aus allen Bandkanten zu einem Baum mit Allpass-Kompensation zusammengesetzt. Der Betragsfrequenzgang bleibt flach, die Phase dreht sich an den Kanten auch ohne Regelung. Dieser Modus reagiert am direktesten auf schnelle Transienten.

## Messergebnisse

Gemessen mit dem Testprogramm in `Tests/` (Linux-Build, 48 kHz, gleiche DSP wie auf dem Mac):

| Prüfung | Ergebnis |
|---|---|
| Transparenz ohne Regelung | Dynamic und Linear bitgenau, Minimum flach auf 0,0000 dB |
| Band 500 Hz bis 2 kHz, Gain -12 dB, Messung gegen Anzeige | max. 0,06 dB Abweichung |
| Zwei angrenzende Bänder je -6 dB | Dynamic -6,00 bis -6,01 dB, Linear -6,00, Minimum -6,00 |
| Kennlinie 4:1, Threshold -20 dB, Knee 0 | max. 0,34 dB Abweichung |
| Attack 5 / 30 ms (63 %) | 5,0 / 30,0 ms |
| Release 100 / 500 ms (63 %) | 136 / 534 ms |
| Upward, Expander, Gate, Range, M/S, Link, Sidechain, Lookahead | alle Sollwerte innerhalb 0,6 dB |
| Speichern/Laden aller 130 Parameter | keine Abweichung |
| Zufallsautomation, Blockgrößen 1 bis 2048, 44,1/96 kHz, Mono/Stereo | keine NaN, danach neutral innerhalb 0,12 dB |
| CPU, 6 Bänder, 48 dB/oct, Lookahead, Puffer 128 | Dynamic 9 %, Linear 15 %, Minimum 6 % eines Kerns (Cloud-Server, ein M-Prozessor liegt meist darunter) |

## Unterschiede zu Pro-MB und bekannte Grenzen

Release ist programmabhängig: Nach länger anhaltender Gain-Reduktion klingt die zweite Hälfte langsamer ab (bis zum Dreifachen der eingestellten Zeit). Deshalb liegen die gemessenen Release-Zeiten etwas über dem Sollwert.

In Linear Phase und Minimum Phase erreicht ein schmales Band in der Mitte nicht die volle Absenkung, weil sich die Flanken der Weiche überlappen. Ein Band über zwei Oktaven mit 24 dB/oct und -12 dB senkt in der Mitte um 9,5 dB ab. Dynamic Phase erreicht die eingestellten -12 dB.

In Dynamic Phase reagieren tiefe Bänder bei Attack-Zeiten unter etwa 3 ms langsamer als in Minimum Phase, weil die Shelving-Filter selbst eine Einschwingzeit haben. Für harte Transientenkontrolle im Bassbereich ist Minimum Phase die bessere Wahl.

In Linear Phase addieren sich überlappende Bänder linear statt multiplikativ. Zwei Bänder, die denselben Bereich je um 6 dB absenken, ergeben dort mehr als 12 dB. In Dynamic und Minimum Phase multiplizieren sie sich korrekt.

Es fehlen: Oversampling im Linear-Phase-Modus, Analyzer-Freeze, MIDI Learn, eigene Undo/A-B-Funktion (Ableton übernimmt Undo), skalierbares Fenster und eine Presetverwaltung (Abletons Geräte-Presets funktionieren). Pro Band ist der Gain-Bereich auf -80 bis +48 dB begrenzt.

## Projektaufbau

`Source/DSP/Filters.h` enthält die Filter (TPT-SVF, Butterworth-Shelves, Linkwitz-Riley), `Dynamics.h` Gain-Computer und Hüllkurve, `Engine.cpp` die drei Signalwege, `Params.h` alle Parameter. Die Oberfläche liegt in `Source/UI` und `PluginEditor.cpp`. Das Messprogramm baut man mit `-DBANDWERK_TESTS=ON`.

JUCE steht unter AGPLv3 bzw. der JUCE-Lizenz. Für den privaten Gebrauch ist das ohne Weiteres möglich. Wer das Plugin weitergibt, muss den Quellcode unter der AGPL mitgeben oder eine JUCE-Lizenz haben.
