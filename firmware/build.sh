#!/usr/bin/env bash
# Main build (macOS / Linux): pioarduino with the custom sdkconfig in platformio.ini, then flash.
#   ./build.sh ui            build only
#   ./build.sh ui --flash    build, then flash (encrypted or plain, as the chip needs; needs a 16 MB backup)
#   ./build.sh ui-debug ...  the same plus the debug web server (docs/development.md: screenshots, recorder, ...)
#   ./build.sh ui-p4 ...     the Guition ESP32-P4 board (ui-p4-debug: with the debug web server)
#   ./build.sh p4-c6-update --flash   P4 board: update its Wi-Fi chip's firmware (src/c6update), then flash ui-p4
#   ./build.sh touchtest --flash      S3: raw touch test (src/touchtest)
#   UI=ui_minimal ./build.sh ui   another UI folder under src/ (default: ui, Brewdeck's own)
#   UI=../my-ui ./build.sh ui     a UI folder outside this repository (any path containing a /)
# Env: PORT=/dev/cu.usbserial-XXXX (default: esptool auto-detect)  GAGGI_BACKUP=path to the 16 MB backup
#      FLASH_ENC=encrypted|plain|auto (what you expect; checked against the chip)  NO_BACKUP=1 (plain boards only)
#      DRY_RUN=1 (show the flash decision and command, write nothing)
# --flash runs flash.sh on the build: it asks the chip whether its flash is encrypted and writes accordingly
# (encrypted: Gaggiuino Gen 3 screen, backup required; plain: a Sunton board bought on its own). Never use
# `pio run -t upload`: it ignores all this.
set -euo pipefail
TARGET="${1:-ui}"; FLASH=0
CALLER="$PWD"
case "${2:-}" in "") ;; --flash) FLASH=1 ;; *) echo "unknown option '$2' (only --flash)" >&2; exit 2 ;; esac
[[ "$TARGET" == -* ]] && { echo "first argument is the env (ui, ui-debug, ui-p4, ...), then --flash" >&2; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [[ ! -f include/wifi_config.h ]]; then
  cp include/wifi_config.example.h include/wifi_config.h
  echo "Created include/wifi_config.h: add your Wi-Fi there, or leave it and choose the network on the screen."
fi

# pioarduino needs Python <= 3.14 (Homebrew's platformio runs on 3.15): run PlatformIO in an isolated uv env
PIO="${PIO:-uvx --python 3.14 --from platformio pio}"
# which UI folder under src/ is built with the core (ui_select.py): Brewdeck's own by default
UI="${UI:-ui}"
if [[ "${UI%/}" == */* ]]; then   # a path, not a folder name under src/: resolve it from where build.sh was called
  UI="$(cd "$CALLER" && cd "$UI" && pwd)" || { echo "no UI folder '${UI}'" >&2; exit 2; }
fi
export BREWDECK_UI="$UI"
# the build date and time (__DATE__ / __TIME__, shown in About and on the splash) are fixed when a file compiles.
# PlatformIO (SCons) recompiles by content, not by date, so remove the objects of the files that use them: they always
# recompile and the date is this build's
for f in $(grep -rl --include='*.c' --include='*.cpp' '__DATE__' src); do rm -f ".pio/build/$TARGET/$f.o"; done
$PIO run -e "$TARGET"
BP="$HERE/.pio/build/$TARGET"
echo "Build OK: $BP"

# boot_app0.bin (the OTA data partition's start state) comes with the Arduino core: next to the build's images
BOOT="$(find "$HOME/.platformio/packages" -path "*framework-arduinoespressif32*/tools/partitions/boot_app0.bin" 2>/dev/null | head -1)"
[[ -f "$BOOT" ]] || { echo "boot_app0.bin not found under ~/.platformio/packages"; exit 1; }
cp "$BOOT" "$BP/boot_app0.bin"
[[ $FLASH == 1 ]] || exit 0
# the same flash script a release ships: asks the chip, needs the backup, never guesses
# each board has its own backup of its original flash
case "$TARGET" in
  ui-p4*|p4-*) DEF_BACKUP="$HERE/../backup/p4-guition-original.bin" ;;
  *) DEF_BACKUP="$HERE/../backup/gaggiuino-screen-backup.bin" ;;
esac
GAGGI_BACKUP="${GAGGI_BACKUP:-$DEF_BACKUP}" exec "$HERE/flash.sh" "$BP"
