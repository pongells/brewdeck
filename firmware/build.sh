#!/usr/bin/env bash
# Main build (macOS / Linux): ESP-IDF's idf.py (settings in sdkconfig.base.<chip> + sdkconfig.brewdeck.<chip>), then flash.
#   ./build.sh ui            build only (the Sunton ESP32-S3)
#   ./build.sh ui --flash    build, then flash (encrypted or plain, as the chip needs; needs a 16 MB backup)
#   ./build.sh ui-debug ...  the same plus the debug web server (docs/development.md: screenshots, recorder, ...)
#   ./build.sh ui-p4-debug --ota   build, then send it over Wi-Fi to a screen running a debug build (kept only if it
#                            runs properly for 60 s, else the screen goes back; HOST=name or address of the screen)
#   ./build.sh ui-p4 ...     the Guition ESP32-P4 board (ui-p4-debug: with the debug web server)
#   ./build.sh p4-c6-update --flash   P4 board: update its Wi-Fi chip's firmware (built from firmware/c6, installed by
#                                     src/c6update), then flash ui-p4
#   UI=ui_minimal ./build.sh ui   another UI folder under src/ (default: ui, Brewdeck's own)
#   UI=../my-ui ./build.sh ui     a UI folder outside this repository (any path containing a /)
#   UI=touchtest ./build.sh ui --flash   touch test: shows where touches land (src/touchtest; ui-p4 for the P4)
# Output: build/<env>/ (firmware.bin, bootloader.bin, partitions.bin, boot_app0.bin: what flash.sh takes).
# Env: IDF_PATH=ESP-IDF folder (default ~/esp/esp-idf-<IDF_VERSION>)
#      PORT=/dev/cu.usbserial-XXXX (default: esptool auto-detect)  GAGGI_BACKUP=path to the 16 MB backup
#      FLASH_ENC=encrypted|plain|auto (what you expect; checked against the chip)  NO_BACKUP=1 (plain boards only)
#      DRY_RUN=1 (show the flash decision and command, write nothing)
#      BREWDECK_VERSION=v0.2.9-test (the version the firmware reports; default: git describe)
#      DEFINES="OTA_TEST_FREEZE" (extra defines for every file, space-separated; test builds only)
# --flash runs flash.sh on the build: it asks the chip whether its flash is encrypted and writes accordingly
# (encrypted: Gaggiuino Gen 3 screen, backup required; plain: a Sunton board bought on its own). Never flash with
# `idf.py flash`: it ignores all this.
set -euo pipefail
# the ESP-IDF release Brewdeck is built and tested with (README: Building)
IDF_VERSION=v5.5.6
TARGET="${1:-ui}"; FLASH=0; OTA=0
CALLER="$PWD"
case "${2:-}" in "") ;; --flash) FLASH=1 ;; --ota) OTA=1 ;; *) echo "unknown option '$2' (--flash or --ota)" >&2; exit 2 ;; esac
if (( OTA )) && [[ "$TARGET" != *debug* ]]; then
  echo "--ota needs a debug build (ui-debug, ui-p4-debug): release builds have no web server" >&2; exit 2
fi
[[ "$TARGET" == -* ]] && { echo "first argument is the env (ui, ui-debug, ui-p4, ...), then --flash" >&2; exit 2; }
ONLY=""; DEBUG=OFF
case "$TARGET" in
  ui) BOARD=s3 ;;
  ui-debug) BOARD=s3; DEBUG=ON ;;
  ui-p4) BOARD=p4 ;;
  ui-p4-debug) BOARD=p4; DEBUG=ON ;;
  p4-c6-update) BOARD=p4; ONLY=c6update ;;
  touchtest) echo "the touch test is a UI now: UI=touchtest ./build.sh ui (or ui-p4)" >&2; exit 2 ;;
  *) echo "unknown env '$TARGET' (ui, ui-debug, ui-p4, ui-p4-debug, p4-c6-update)" >&2; exit 2 ;;
esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

# the password a screen asks for firmware pushed over Wi-Fi (debug builds); made once, git-ignored, never printed
if [[ ! -f include/ota_token.h ]]; then
  printf '#pragma once\n#define OTA_TOKEN "%s"\n' "$(LC_ALL=C tr -dc 'a-zA-Z0-9' < /dev/urandom | head -c 32)" \
    > include/ota_token.h
  echo "Created include/ota_token.h (the password for --ota; a screen accepts it after one flash by USB)"
fi
if [[ ! -f include/wifi_config.h ]]; then
  cp include/wifi_config.example.h include/wifi_config.h
  echo "Created include/wifi_config.h: add your Wi-Fi there, or leave it and choose the network on the screen."
fi

# which UI folder is built with the core: a name under src/ (Brewdeck's own by default) or a path
UI="${UI:-ui}"
if [[ "${UI%/}" == */* ]]; then   # a path, not a folder name under src/: resolve it from where build.sh was called
  UI="$(cd "$CALLER" && cd "$UI" && pwd)" || { echo "no UI folder '${UI}'" >&2; exit 2; }
  UI_DIR="$UI"
else
  UI="${UI%/}"
  case "$UI" in core|board|c6update|.*) echo "UI=$UI is not a UI folder" >&2; exit 2 ;; esac
  [[ -d "src/$UI" ]] || { echo "no UI folder src/$UI (or give a path)" >&2; exit 2; }
  UI_DIR="$HERE/src/$UI"
fi
BP="$HERE/build/$TARGET"

# ESP-IDF: the release named above (tools/install-idf.sh installs it)
USER_PATH="$PATH"   # flashing takes the user's esptool first (below)
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-$IDF_VERSION}"
[[ -f "$IDF_PATH/export.sh" ]] || { echo "ESP-IDF $IDF_VERSION not found in $IDF_PATH: run tools/install-idf.sh (or set IDF_PATH)" >&2; exit 1; }
set +u   # export.sh reads variables that may be unset
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null || { echo "ESP-IDF's environment did not load: run $IDF_PATH/export.sh" >&2; exit 1; }
set -u
HAVE="$(idf.py --version | sed -n 's/^ESP-IDF \(v[^ ]*\).*/\1/p')"
[[ "$HAVE" == "$IDF_VERSION" ]] || { echo "ESP-IDF $HAVE in $IDF_PATH, Brewdeck is built with $IDF_VERSION" >&2; exit 1; }
if [[ -n "$ONLY" ]]; then   # the C6 updater carries the C6's firmware (firmware/c6): build that first
  for d in c6/sdkconfig.defaults*; do
    [[ -f c6/build/sdkconfig && "$d" -nt c6/build/sdkconfig ]] && { rm -f c6/build/sdkconfig; echo "$d changed: the C6 settings are made again"; break; }
  done
  (cd c6 && idf.py -B build -DSDKCONFIG=build/sdkconfig build)
  cp c6/build/brewdeck_c6.bin c6/build/firmware.bin   # src/c6update finds it by this name
fi
# the build date and time (__DATE__ / __TIME__, shown in About and on the splash) are fixed when a file compiles:
# drop the objects of the files that use them, so the date is this build's
for f in $(grep -rl --include='*.c' --include='*.cpp' '__DATE__' src "$UI_DIR"); do
  [[ -d "$BP/esp-idf/src" ]] && find "$BP/esp-idf/src" -name "$(basename "$f").obj" -delete
done
# ESP-IDF applies the defaults (sdkconfig.base.<chip>, sdkconfig.brewdeck.<chip>) only to settings missing from the
# build's sdkconfig, so a changed defaults file starts that file again
for d in sdkconfig.base.* sdkconfig.brewdeck.*; do
  [[ -f "$BP/sdkconfig" && "$d" -nt "$BP/sdkconfig" ]] && { rm -f "$BP/sdkconfig"; echo "$d changed: the settings are made again"; break; }
done
idf.py -B "$BP" -DSDKCONFIG="$BP/sdkconfig" -DBREWDECK_BOARD="$BOARD" -DBREWDECK_UI="$UI" -DBREWDECK_DEBUG="$DEBUG" \
  -DBREWDECK_ONLY="$ONLY" -DBREWDECK_DEFINES="$(echo ${DEFINES:-} | tr ' ' ';')" build
# the names flash.sh and the release take; boot_app0.bin = the OTA data partition's start state (boot app0)
cp "$BP/brewdeck.bin" "$BP/firmware.bin"
cp "$BP/bootloader/bootloader.bin" "$BP/bootloader.bin"
cp "$BP/partition_table/partition-table.bin" "$BP/partitions.bin"
cp "$BP/ota_data_initial.bin" "$BP/boot_app0.bin"
echo "Build OK: $BP"

if (( OTA )); then   # over Wi-Fi to the debug web server (src/core/ota.h)
  case "$TARGET" in ui-p4*) DEF_HOST=brewdeck-p4.local ;; *) DEF_HOST=brewdeck.local ;; esac
  HOST="${HOST:-$DEF_HOST}"
  TOKEN="$(sed -n 's/^#define OTA_TOKEN "\(.*\)"/\1/p' include/ota_token.h)"
  heap() { curl -4 -s -m 5 "http://$HOST/heap" 2>/dev/null || true; }   # no answer while it restarts: not an error
  BEFORE="$(heap | sed -n 's/^firmware: //p')"
  [[ -n "$BEFORE" ]] || { echo "no answer from $HOST (a debug build with OTA, flashed once by USB?)" >&2; exit 1; }
  echo "Screen runs: $BEFORE"
  [[ "$BEFORE" == *"on trial"* ]] && { echo "Its firmware is still on its 60 s trial: try again in a minute" >&2; exit 1; }
  echo "Sending $(wc -c < "$BP/firmware.bin" | tr -d ' ') bytes to $HOST ..."
  # a refusal comes before the image is read, then the screen closes: curl reports a reset, the answer is still there
  ANS="$(curl -4 -s -m 300 -H "Expect:" -H "X-OTA-Token: $TOKEN" -H "Content-Type: application/octet-stream" \
         --data-binary @"$BP/firmware.bin" "http://$HOST/ota" || true)"
  [[ -n "$ANS" ]] || { echo "no answer from $HOST: the upload failed; nothing changed" >&2; exit 1; }
  echo "Screen: $ANS"
  [[ "$ANS" == installed* ]] || exit 1
  echo "Waiting for it to restart and pass its 60 s trial ..."
  for i in $(seq 1 60); do   # "(kept)" with the build from before = it went back
    sleep 3
    NOW="$(heap | sed -n 's/^firmware: //p')"
    case "$NOW" in
      *"(kept)"*) [[ "${NOW%, *}" == "${BEFORE%, *}" ]] && { echo "Went back to the previous firmware: $NOW (see /log/prev)" >&2; exit 1; }
                  echo "Kept: $NOW"; exit 0 ;;
      *"on trial"*) [[ -z "${SEEN:-}" ]] && { echo "Running on trial: $NOW"; SEEN=1; } ;;
    esac
  done
  echo "No answer after 3 min; check http://$HOST/heap and the log" >&2; exit 1
fi
[[ $FLASH == 1 ]] || exit 0
# the same flash script a release ships: asks the chip, needs the backup, never guesses
# each board has its own backup of its original flash
case "$TARGET" in
  ui-p4*|p4-*) DEF_BACKUP="$HERE/../backup/p4-guition-original.bin" ;;
  *) DEF_BACKUP="$HERE/../backup/gaggiuino-screen-backup.bin" ;;
esac
# an esptool of your own (v5, brew install esptool) before ESP-IDF's (4.x): flash.sh takes the first one on PATH
if PATH="$USER_PATH" command -v esptool >/dev/null || PATH="$USER_PATH" command -v esptool.py >/dev/null; then
  export PATH="$USER_PATH"
fi
GAGGI_BACKUP="${GAGGI_BACKUP:-$DEF_BACKUP}" exec "$HERE/flash.sh" "$BP"
