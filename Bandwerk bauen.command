#!/bin/bash
# Doppelklick im Finder startet den Build im Terminal und schreibt alles zusaetzlich in build.log
cd "$(dirname "$0")"
chmod +x build_mac.sh
./build_mac.sh 2>&1 | tee build.log
echo
read -n 1 -s -r -p "Fertig. Taste druecken zum Schliessen."
