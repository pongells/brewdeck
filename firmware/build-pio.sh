#!/usr/bin/env bash
# Main build (macOS / Linux): pioarduino with the custom sdkconfig in platformio.ini, then flash.
#   ./build-pio.sh ui            build only
#   ./build-pio.sh ui --flash    build, then flash (encrypted or plain, as the chip needs; needs a 16 MB backup)
#   ./build-pio.sh ui-debug ...  the same plus the debug web server (screenshots for tools/screenshot.sh, /heap)
#   UI=ui_minimal ./build-pio.sh ui   another UI folder under src/ (default: ui, Brewdeck's own)
# Env: PORT=/dev/cu.usbserial-XXXX (default: esptool auto-detect)  GAGGI_BACKUP=path to the 16 MB backup
#      FLASH_ENC=encrypted|plain|auto (what you expect; checked against the chip)  NO_BACKUP=1 (plain boards only)
#      DRY_RUN=1 (show the flash decision and command, write nothing)
# --flash asks the chip whether its flash is encrypted and writes accordingly (encrypted: Gaggiuino Gen 3 screen,
# backup required; plain: a Sunton board bought on its own). Never use `pio run -t upload`: it ignores all this.
set -euo pipefail
TARGET="${1:-ui}"; FLASH=0; [[ "${2:-}" == "--flash" ]] && FLASH=1
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [[ ! -f include/wifi_config.h ]]; then
  cp include/wifi_config.example.h include/wifi_config.h
  echo "Created include/wifi_config.h: add your Wi-Fi there, or leave it and choose the network on the screen."
fi

# pioarduino needs Python <= 3.14 (Homebrew's platformio runs on 3.15): run PlatformIO in an isolated uv env
PIO="${PIO:-uvx --python 3.14 --from platformio pio}"
# which UI folder under src/ is built with the core (ui_select.py): Brewdeck's own by default
export BREWDECK_UI="${UI:-ui}"
# the build date and time (__DATE__ / __TIME__, shown in About and on the splash) are fixed when a file compiles.
# PlatformIO (SCons) recompiles by content, not by date, so remove the objects of the files that use them: they always
# recompile and the date is this build's
for f in $(grep -rl --include='*.c' --include='*.cpp' '__DATE__' src); do rm -f ".pio/build/$TARGET/$f.o"; done
$PIO run -e "$TARGET"
BP="$HERE/.pio/build/$TARGET"
echo "Build OK: $BP"
[[ $FLASH == 1 ]] || exit 0

BACKUP="${GAGGI_BACKUP:-$HERE/../backup/gaggiuino-screen-backup.bin}"
BOOT="$(find "$HOME/.platformio/packages" -path "*framework-arduinoespressif32*/tools/partitions/boot_app0.bin" 2>/dev/null | head -1)"
[[ -f "$BOOT" ]] || { echo "boot_app0.bin not found under ~/.platformio/packages"; exit 1; }

# esptool: one on PATH (e.g. brew install esptool, the latest) first, else PlatformIO's bundled copy
if command -v esptool >/dev/null; then ESP=(esptool)
elif [[ -f "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" ]]; then ESP=("$HOME/.platformio/penv/bin/python" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py")
else echo "esptool not found (try: brew install esptool)"; exit 1; fi
VER="$("${ESP[@]}" version 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)"
if [[ "${VER%%.*}" -ge 5 ]] 2>/dev/null; then W=write-flash; FM=--flash-mode; FF=--flash-freq; FS=--flash-size; SI=get-security-info
else W=write_flash; FM=--flash_mode; FF=--flash_freq; FS=--flash_size; SI=get_security_info; fi
echo "Using esptool $VER: ${ESP[*]}  (serial ports: $(ls /dev/cu.usb* /dev/cu.wch* 2>/dev/null | tr '\n' ' '))"

# ${a[@]+"${a[@]}"}: macOS bash 3.2 with set -u calls an empty "${a[@]}" unbound
PORTARG=(); [[ -n "${PORT:-}" ]] && PORTARG=(--port "$PORT")

# Encrypted or plain write: asked from the chip itself, never guessed. A Gaggiuino Gen 3 screen has flash encryption
# on (development mode): it needs --encrypt (a plain write would not boot) and a full backup of its original flash,
# the only way back. A Sunton board bought on its own has it off: it needs a plain write (an encrypted one would not
# boot). The firmware never turns encryption on, so a plain board stays plain.
# FLASH_ENC=encrypted|plain: say what you expect (the chip must agree); DRY_RUN=1: show the decision, write nothing;
# NO_BACKUP=1: flash a PLAIN board without a backup of its original flash (never accepted for an encrypted one).
INFO="$("${ESP[@]}" ${PORTARG[@]+"${PORTARG[@]}"} $SI 2>&1)" || { echo "$INFO" | tail -5; echo "Could not read the chip's security info. Refusing to flash."; exit 1; }
ENC="$(echo "$INFO" | sed -n 's/^Flash Encryption: *\([A-Za-z]*\).*/\1/p' | head -1)"
FLAGS="$(echo "$INFO" | sed -n 's/^Flags: *\(0x[0-9a-fA-F]*\).*/\1/p' | head -1)"
CRYPT="$(echo "$INFO" | sed -n 's/^SPI Boot Crypt Count[^:]*: *\(0x[0-9a-fA-F]*\).*/\1/p' | head -1)"
case "$ENC" in
  Enabled) MODE_ENC=encrypted ;;
  Disabled) MODE_ENC=plain ;;
  *) echo "$INFO" | tail -12; echo "Flash encryption state not found in the chip's answer. Refusing to flash."; exit 1 ;;
esac
if [[ -n "$FLAGS" ]] && (( (FLAGS >> 2) & 1 )); then   # SECURE_DOWNLOAD_ENABLE: the ROM refuses writes from here
  echo "This chip is in secure download mode (locked, e.g. a Gaggiuino screen after its partition migration): it"
  echo "cannot take other firmware over USB. Nothing was written."; exit 1
fi
if [[ -n "${FLASH_ENC:-}" && "${FLASH_ENC}" != auto && "${FLASH_ENC}" != "$MODE_ENC" ]]; then
  echo "FLASH_ENC=$FLASH_ENC, but this chip's flash encryption is $ENC ($MODE_ENC write needed). Refusing to flash."; exit 1
fi
echo "Flash encryption: $ENC${CRYPT:+ (SPI_BOOT_CRYPT_CNT $CRYPT)} -> $MODE_ENC write"
HAVE_BACKUP=0; [[ -f "$BACKUP" && $(wc -c < "$BACKUP") -eq 16777216 ]] && HAVE_BACKUP=1
if [[ $HAVE_BACKUP == 0 ]]; then
  if [[ $MODE_ENC == encrypted ]]; then
    echo "Backup missing or not 16 MB: $BACKUP (set GAGGI_BACKUP). An encrypted screen is never flashed without it"
    echo "(see the README, \"Backup and restore\"). Refusing to flash."; exit 1
  elif [[ "${NO_BACKUP:-0}" != 1 ]]; then
    echo "No backup of this board's original flash: $BACKUP. Make one first (README, \"Backup and restore\"), or run"
    echo "with NO_BACKUP=1 to flash this plain board without it. Refusing to flash."; exit 1
  fi
  echo "No backup (NO_BACKUP=1, plain board): flashing anyway."
fi
ENCARG=(); [[ $MODE_ENC == encrypted ]] && ENCARG=(--encrypt)
CMD=("${ESP[@]}" ${PORTARG[@]+"${PORTARG[@]}"} --baud 460800 $W ${ENCARG[@]+"${ENCARG[@]}"} $FM dio $FF 80m $FS 16MB
     0x0 "$BP/bootloader.bin" 0x8000 "$BP/partitions.bin" 0xe000 "$BOOT" 0x10000 "$BP/firmware.bin")
if [[ "${DRY_RUN:-0}" == 1 ]]; then echo "DRY_RUN=1, would run: ${CMD[*]}"; exit 0; fi
"${CMD[@]}"
echo "Flashed ($MODE_ENC write). The screen restarts by itself; if the picture looks wrong, unplug and replug USB."
