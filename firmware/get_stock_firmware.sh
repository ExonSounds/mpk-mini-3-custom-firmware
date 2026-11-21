#!/usr/bin/env bash
# Fetch Akai's official MPK mini 3 updater v1.26 and extract the stock firmware images into firmware/stock_v1.26/.
# They are needed to build images (the bootloader region is copied from AT.bin) and to restore stock firmware.
# Akai's firmware is not part of this repository; this script gets it from Akai's own download server.
#
# usage: firmware/get_stock_firmware.sh [MPKmini3_Updater_v1.26.app.zip]   (uses a local copy if given)
set -euo pipefail

URL="https://cdn.inmusicbrands.com/akai/mpk3mini/1_26/MPKmini3_Updater_v1.26.app.zip"
DEST="$(cd "$(dirname "$0")" && pwd)/stock_v1.26"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ $# -ge 1 ]; then
    cp "$1" "$TMP/updater.zip"
else
    echo "downloading $URL"
    curl -fL --proto '=https' -o "$TMP/updater.zip" "$URL"
fi

unzip -q -j "$TMP/updater.zip" \
    'MPKmini3_Updater_v1.26.app/Contents/Resources/AT.bin' \
    'MPKmini3_Updater_v1.26.app/Contents/Resources/AT.xml' \
    'MPKmini3_Updater_v1.26.app/Contents/Resources/STM.bin' \
    'MPKmini3_Updater_v1.26.app/Contents/Resources/STM.xml' -d "$TMP/out"

(cd "$TMP/out" && sha1sum -c --quiet - <<'EOF'
2217791fc4756209ea7af4ac90dda4cf23719ca5  AT.bin
a77df789690fa1fc4b71734fd6e8fc68f9a01a06  STM.bin
EOF
)

mkdir -p "$DEST"
cp "$TMP/out/"* "$DEST/"
echo "stock firmware v1.26 in $DEST"
