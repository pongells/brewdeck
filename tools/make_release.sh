#!/usr/bin/env bash
# Package a release of Brewdeck for the Sunton ESP32-S3 screen: the prebuilt images, flash.sh, checksums and a short
# INSTALL.txt, as dist/brewdeck-<version>-esp32s3.zip. Builds the release env (`ui`, no debug web server) first.
#   tools/make_release.sh v0.1.0
# The image has no Wi-Fi network built in (wifi_config.h from the example): the screen starts on "No Wi-Fi set up".
set -euo pipefail
VER="${1:?usage: tools/make_release.sh <version, e.g. v0.1.0>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$ROOT/firmware"
# a release never carries anyone's network: build with the example config, put a local one back afterwards
if [[ -f "$FW/include/wifi_config.h" ]] && ! cmp -s "$FW/include/wifi_config.h" "$FW/include/wifi_config.example.h"; then
  mv "$FW/include/wifi_config.h" "$FW/include/wifi_config.h.local"
  trap 'mv "$FW/include/wifi_config.h.local" "$FW/include/wifi_config.h"' EXIT
fi
cp "$FW/include/wifi_config.example.h" "$FW/include/wifi_config.h"
(cd "$FW" && UI=ui ./build-pio.sh ui)
NAME="brewdeck-$VER-esp32s3"
OUT="$ROOT/dist/$NAME"
rm -rf "$OUT" "$ROOT/dist/$NAME.zip"
mkdir -p "$OUT"
for f in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do cp "$FW/.pio/build/ui/$f" "$OUT/"; done
cp "$FW/flash.sh" "$OUT/"
chmod +x "$OUT/flash.sh"
(cd "$OUT" && if command -v sha256sum >/dev/null; then sha256sum *.bin; else shasum -a 256 *.bin; fi > SHA256SUMS)
cat > "$OUT/INSTALL.txt" <<EOF
Brewdeck $VER: unofficial Wi-Fi screen for Gaggiuino machines (not affiliated with the Gaggiuino project).
For a Sunton ESP32-8048S043-style 4.3" ESP32-S3 screen. Full instructions and warnings:
https://github.com/pongells/brewdeck#hardware

Brewdeck needs its OWN screen: never flash the screen fitted to your machine (on Gen 3 it also runs the machine's
Wi-Fi and web UI). You need esptool v5 (brew install esptool, or pip install esptool) and bash (macOS, Linux).

1. Check the screen (writes nothing). "Secure download mode" means it is locked and cannot take Brewdeck:
     esptool --port <port> get-security-info
2. Back up its whole flash, in this folder; it is the only way back to the original firmware. Keep a copy:
     mkdir -p backup
     esptool --port <port> --baud 460800 read-flash 0 0x1000000 backup/gaggiuino-screen-backup.bin
     (the file must be exactly 16777216 bytes)
3. Flash (asks the chip whether its flash is encrypted and writes accordingly; DRY_RUN=1 shows what it would do):
     PORT=<port> ./flash.sh
   A plain Sunton board can be flashed without a backup: NO_BACKUP=1 PORT=<port> ./flash.sh

The screen starts on "No Wi-Fi set up": choose your network on the screen, then it looks for the machine
("gaggiuino.local"; another name or IP under Menu > Wi-Fi).
Ports: /dev/cu.usbserial-... on macOS, /dev/ttyUSB0 or similar on Linux.
License: PolyForm Noncommercial 1.0.0 (https://github.com/pongells/brewdeck/blob/main/LICENSE).
EOF
(cd "$ROOT/dist" && zip -qr "$NAME.zip" "$NAME")
echo "Release package: $ROOT/dist/$NAME.zip"
