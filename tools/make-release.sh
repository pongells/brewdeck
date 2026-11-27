#!/usr/bin/env bash
# Package a release of Brewdeck for one board: the prebuilt images, flash.sh, checksums and a short INSTALL.txt, as
# dist/brewdeck-<version>-<chip>.zip. Builds the board's release env (no debug web server) first.
#   tools/make-release.sh v0.1.0          the Sunton ESP32-S3 screen  -> brewdeck-v0.1.0-esp32s3.zip
#   tools/make-release.sh v0.1.0 p4       the Guition ESP32-P4 board  -> brewdeck-v0.1.0-esp32p4.zip (also carries
#                                         the board's Wi-Fi chip updater in c6-update/)
# The image has no Wi-Fi network built in (wifi_config.h from the example): the screen starts on "No Wi-Fi set up".
set -euo pipefail
VER="${1:?usage: tools/make-release.sh <version, e.g. v0.1.0> [s3|p4]}"
BOARD="${2:-s3}"
case "$BOARD" in
  s3) ENV=ui; CHIP=esp32s3 ;;
  p4) ENV=ui-p4; CHIP=esp32p4 ;;
  *) echo "board must be s3 or p4, not $BOARD"; exit 1 ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$ROOT/firmware"
# a release never carries anyone's network: build with the example config, put a local one back afterwards
if [[ -f "$FW/include/wifi_config.h" ]] && ! cmp -s "$FW/include/wifi_config.h" "$FW/include/wifi_config.example.h"; then
  mv "$FW/include/wifi_config.h" "$FW/include/wifi_config.h.local"
  trap 'mv "$FW/include/wifi_config.h.local" "$FW/include/wifi_config.h"' EXIT
fi
cp "$FW/include/wifi_config.example.h" "$FW/include/wifi_config.h"
(cd "$FW" && UI=ui ./build.sh "$ENV")
[[ $BOARD == p4 ]] && (cd "$FW" && ./build.sh p4-c6-update)
# the version built into the image (git describe, firmware/ui_select.py) should be the release's own tag
BUILT="$(sed -n 's/^#define BREWDECK_VERSION "\(.*\)"$/\1/p' "$FW/.pio/build/$ENV/gen/brewdeck_version.h" 2>/dev/null || true)"
if [[ "$BUILT" != "$VER" ]]; then
  echo "The image says version '${BUILT:-unknown}', not $VER (tag the commit first, no local changes)."
  [[ -n "${CI:-}" ]] && { echo "Refusing to package a release with the wrong version."; exit 1; }   # local: a warning
fi
NAME="brewdeck-$VER-$CHIP"
OUT="$ROOT/dist/$NAME"
rm -rf "$OUT" "$ROOT/dist/$NAME.zip"
mkdir -p "$OUT"
sums() { (cd "$1" && if command -v sha256sum >/dev/null; then sha256sum *.bin; else shasum -a 256 *.bin; fi > SHA256SUMS); }
for f in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do cp "$FW/.pio/build/$ENV/$f" "$OUT/"; done
sums "$OUT"
if [[ $BOARD == p4 ]]; then
  mkdir -p "$OUT/c6-update"
  for f in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do cp "$FW/.pio/build/p4-c6-update/$f" "$OUT/c6-update/"; done
  sums "$OUT/c6-update"
fi
cp "$FW/flash.sh" "$OUT/"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.txt" "$OUT/"
chmod +x "$OUT/flash.sh"

if [[ $BOARD == s3 ]]; then
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
License: PolyForm Noncommercial 1.0.0 (LICENSE). Third-party software in the images: THIRD_PARTY_NOTICES.txt.
EOF
else
cat > "$OUT/INSTALL.txt" <<EOF
Brewdeck $VER: unofficial Wi-Fi screen for Gaggiuino machines (not affiliated with the Gaggiuino project).
For the Guition JC4880P443 4.3" ESP32-P4 board (480x800 panel shown in landscape). Full instructions:
https://github.com/pongells/brewdeck#esp32-p4-board

You need esptool v5 (brew install esptool, or pip install esptool) and bash (macOS, Linux). Use the board's USB-C
socket that carries data (on our board the top one, marked D4D5; the other one only powers it).
Fit a 2.4 GHz antenna to the small IPEX Gen3 (MHF3) socket on the module, with the board unpowered: without one its
Wi-Fi is weak (about -83 dBm on our board, -60 dBm with a 3 cm flexible antenna).

1. Check the board (writes nothing):
     esptool --port <port> get-security-info
2. Back up its whole flash, in this folder; it is the only way back to Guition's firmware. Keep a copy:
     mkdir -p backup
     esptool --port <port> --baud 460800 read-flash 0 0x1000000 backup/p4-guition-original.bin
     (the file must be exactly 16777216 bytes)
3. Flash (DRY_RUN=1 shows what it would do):
     GAGGI_BACKUP=backup/p4-guition-original.bin PORT=<port> ./flash.sh

The screen starts on "No Wi-Fi set up": choose your network on the screen, then it looks for the machine
("gaggiuino.local"; another name or IP under Menu > Wi-Fi). On the network the screen is "brewdeck-p4".

Once, after step 3: update the board's Wi-Fi chip (ESP32-C6) to the ESP-Hosted version Brewdeck uses. Boards ship
with older C6 firmware (with very old versions the first Wi-Fi join drops at every start). c6-update/ carries the
C6 image and sends it over the board's own link to the C6 (no network needed; the C6 boots it only when it arrived
complete, else it keeps its current firmware; progress on the serial port, 115200 baud):
     GAGGI_BACKUP=backup/p4-guition-original.bin PORT=<port> ./flash.sh c6-update
Then flash Brewdeck again (step 3).

Later releases install over Wi-Fi: Menu > About shows a newer one with an Install button (the screen checks once a
day; if a new version does not run properly, it goes back to the old one by itself).
Ports: /dev/cu.usbmodem... on macOS, /dev/ttyACM0 or similar on Linux.
License: PolyForm Noncommercial 1.0.0 (LICENSE). Third-party software in the images: THIRD_PARTY_NOTICES.txt.
EOF
fi
(cd "$ROOT/dist" && zip -qr "$NAME.zip" "$NAME")
echo "Release package: $ROOT/dist/$NAME.zip"

# updates over Wi-Fi (ESP32-P4 only for now): the app image + its signed description, published next to the zip.
# UPDATE_SIGNING_KEY = the private key's PEM text (the release workflow's secret) or a path to it.
if [[ $BOARD == p4 ]]; then
  if [[ -z "${UPDATE_SIGNING_KEY:-}" ]]; then
    [[ -n "${CI:-}" ]] && { echo "UPDATE_SIGNING_KEY is not set: refusing to release without the update files."; exit 1; }
    echo "UPDATE_SIGNING_KEY not set: no update files (screens cannot update to this build over Wi-Fi)."
  else
    if [[ -f "$UPDATE_SIGNING_KEY" ]]; then
      "$ROOT/tools/sign-update.sh" "$VER" "$CHIP" "$FW/.pio/build/$ENV/firmware.bin" "$UPDATE_SIGNING_KEY" "$ROOT/dist"
    else   # the key's text: a temporary file, removed whatever happens (the EXIT trap above restores wifi_config.h)
      KEYF="$(mktemp)"
      printf '%s\n' "$UPDATE_SIGNING_KEY" > "$KEYF"
      ok=0
      "$ROOT/tools/sign-update.sh" "$VER" "$CHIP" "$FW/.pio/build/$ENV/firmware.bin" "$KEYF" "$ROOT/dist" || ok=$?
      rm -f "$KEYF"
      [[ $ok == 0 ]] || exit "$ok"
    fi
  fi
fi
