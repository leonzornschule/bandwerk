#!/bin/bash
# Baut Bandwerk als VST3 und AU (Universal: Apple Silicon + Intel) und installiert
# beides in ~/Library/Audio/Plug-Ins. Aufruf im Terminal:  ./build_mac.sh
set -euo pipefail
cd "$(dirname "$0")"

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Die Xcode Command Line Tools fehlen. Installieren mit:  xcode-select --install"
    echo "Danach dieses Skript erneut starten."
    exit 1
fi

# CMake suchen: PATH, CMake.app, Homebrew, pip-Installation im Benutzerordner
find_cmake() {
    for c in "$(command -v cmake 2>/dev/null)" /Applications/CMake.app/Contents/bin/cmake \
             /opt/homebrew/bin/cmake /usr/local/bin/cmake "$HOME"/Library/Python/*/bin/cmake; do
        if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return 0; fi
    done
    return 1
}

CMAKE=$(find_cmake || true)
if [ -z "$CMAKE" ]; then
    echo "CMake fehlt, installiere es ohne Administratorrechte in den Benutzerordner (pip, ca. 50 MB) ..."
    python3 -m pip install --user --quiet cmake
    CMAKE=$(find_cmake || true)
    if [ -z "$CMAKE" ]; then
        echo "CMake konnte nicht installiert werden. Alternativ CMake.app von https://cmake.org/download/ in Programme legen."
        exit 1
    fi
fi
echo "CMake: $CMAKE ($("$CMAKE" --version | head -1))"

JOBS=$(sysctl -n hw.ncpu)
echo "Konfiguriere (laedt beim ersten Mal JUCE von GitHub, ca. 100 MB) ..."
"$CMAKE" -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
echo "Kompiliere mit $JOBS Kernen ..."
"$CMAKE" --build build-mac --config Release -j "$JOBS"

ART="build-mac/Bandwerk_artefacts/Release"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DIR" "$AU_DIR"

rm -rf "$VST3_DIR/Bandwerk.vst3" "$AU_DIR/Bandwerk.component"
cp -R "$ART/VST3/Bandwerk.vst3" "$VST3_DIR/"
cp -R "$ART/AU/Bandwerk.component" "$AU_DIR/"

# lokale Ad-hoc-Signatur, damit macOS die Bundles laedt
codesign --force --deep -s - "$VST3_DIR/Bandwerk.vst3"
codesign --force --deep -s - "$AU_DIR/Bandwerk.component"

# AU-Cache auffrischen und pruefen
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
if auval -v aufx Bwmb Leon >/tmp/bandwerk_auval.txt 2>&1; then
    echo "AU-Validierung: bestanden"
else
    echo "AU-Validierung meldet Probleme, Details in /tmp/bandwerk_auval.txt"
fi

echo
echo "Fertig. Installiert:"
echo "  $VST3_DIR/Bandwerk.vst3"
echo "  $AU_DIR/Bandwerk.component"
echo "In Ableton: Einstellungen > Plug-ins > 'Erneut scannen', dann unter Plug-ins > Leon > Bandwerk."
