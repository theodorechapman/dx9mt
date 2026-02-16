#!/bin/bash
set -e

WINEPREFIX="$(cd "$(dirname "$0")" && pwd)/wineprefix"
WINE=/opt/homebrew/bin/wine
GOG_DIR="$WINEPREFIX/drive_c/Games/GOG Fallout New Vegas"
INSTALL_DIR='C:\Games\GOG Fallout New Vegas\Fallout New Vegas'

export WINEPREFIX

echo "=== GOG Fallout: New Vegas Installer ==="
echo "Wine prefix: $WINEPREFIX"
echo "Install dir: $INSTALL_DIR"
echo ""

cd "$GOG_DIR"
"$WINE" "setup_fallout_new_vegas_1.4.0.525(a)_(55068).exe" /DIR="$INSTALL_DIR"

echo ""
echo "=== Installer finished ==="
echo "Game should be at: $GOG_DIR/Fallout New Vegas/"
ls "$GOG_DIR/Fallout New Vegas/" 2>/dev/null && echo "OK" || echo "WARNING: install dir not found"
