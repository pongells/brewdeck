#!/usr/bin/env bash
# Flash Brewdeck's images onto the screen (macOS / Linux). Used by build.sh --flash after a build, and shipped in
# each release zip next to the prebuilt images.
#   ./flash.sh [<folder>]   folder with bootloader.bin, partitions.bin, boot_app0.bin, firmware.bin (default: this
#                           script's folder, as in a release zip)
# Env: PORT=/dev/cu.usbserial-XXXX (default: esptool auto-detect)
#      GAGGI_BACKUP=path to the 16 MB backup of the screen's original flash (default: backup/gaggiuino-screen-backup.bin
#      next to the images)
#      FLASH_ENC=encrypted|plain|auto (what you expect; checked against the chip)  NO_BACKUP=1 (plain boards only)
#      DRY_RUN=1 (show the flash decision and command, write nothing)
# It asks the chip whether its flash is encrypted and writes accordingly (encrypted: Gaggiuino Gen 3 screen, backup
# required; plain: a Sunton board bought on its own, or the Guition ESP32-P4 board), refuses a locked chip, refuses
# images built for another chip (S3 / P4, read from bootloader.bin), and never guesses.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="$(cd "${1:-$HERE}" && pwd)"
for f in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do
  [[ -f "$IMG/$f" ]] || { echo "$IMG/$f not found. Refusing to flash."; exit 1; }
done
# a release ships SHA256SUMS: a damaged or changed download is not flashed
if [[ -f "$IMG/SHA256SUMS" ]]; then
  if command -v sha256sum >/dev/null; then SUM=(sha256sum -c); else SUM=(shasum -a 256 -c); fi
  (cd "$IMG" && "${SUM[@]}" --quiet SHA256SUMS) || { echo "Checksums do not match (SHA256SUMS). Refusing to flash."; exit 1; }
  echo "Checksums OK."
fi
BACKUP="${GAGGI_BACKUP:-$IMG/backup/gaggiuino-screen-backup.bin}"
# which chip the images are for (chip id in the bootloader's image header, bytes 12-13) and where its bootloader goes
IMG_CHIP="$(od -An -tu1 -j12 -N2 "$IMG/bootloader.bin" | awk 'NF {print $1 + 256 * $2; exit}')"
case "$IMG_CHIP" in
  9) IMG_NAME=ESP32-S3; BOOT_AT=0x0 ;;
  18) IMG_NAME=ESP32-P4; BOOT_AT=0x2000 ;;
  *) echo "bootloader.bin is for chip id $IMG_CHIP, not an ESP32-S3 (9) or ESP32-P4 (18). Refusing to flash."; exit 1 ;;
esac

# esptool: one on PATH (e.g. brew install esptool, the latest) first, else PlatformIO's bundled copy
if command -v esptool >/dev/null; then ESP=(esptool)
elif command -v esptool.py >/dev/null; then ESP=(esptool.py)
elif PIOD="${PLATFORMIO_CORE_DIR:-$HOME/.platformio-brewdeck}"; [[ -f "$PIOD/packages/tool-esptoolpy/esptool.py" ]]; then
  ESP=("$PIOD/penv/bin/python" "$PIOD/packages/tool-esptoolpy/esptool.py")
else echo "esptool not found (brew install esptool, or pip install esptool)"; exit 1; fi
VER="$("${ESP[@]}" version 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)"
if [[ "${VER%%.*}" -ge 5 ]] 2>/dev/null; then W=write-flash; FM=--flash-mode; FF=--flash-freq; FS=--flash-size; SI=get-security-info
else W=write_flash; FM=--flash_mode; FF=--flash_freq; FS=--flash_size; SI=get_security_info; fi
echo "Using esptool $VER: ${ESP[*]}  (serial ports: $(ls /dev/cu.usb* /dev/cu.wch* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | tr '\n' ' '))"

# ${a[@]+"${a[@]}"}: macOS bash 3.2 with set -u calls an empty "${a[@]}" unbound
PORTARG=(); [[ -n "${PORT:-}" ]] && PORTARG=(--port "$PORT")

# Encrypted or plain write: asked from the chip itself, never guessed. A Gaggiuino Gen 3 screen has flash encryption
# on (development mode): it needs --encrypt (a plain write would not boot) and a full backup of its original flash,
# the only way back. A Sunton board bought on its own has it off: it needs a plain write (an encrypted one would not
# boot). The firmware never turns encryption on, so a plain board stays plain.
INFO="$("${ESP[@]}" ${PORTARG[@]+"${PORTARG[@]}"} $SI 2>&1)" || { echo "$INFO" | tail -5; echo "Could not read the chip's security info. Refusing to flash."; exit 1; }
# Without PORT, esptool finds a board by itself, and the write must go to the board just checked (with two boards
# plugged in, a second search could reach the other one: an encrypted screen given a plain write does not boot)
if [[ -z "${PORT:-}" ]]; then
  FOUND="$(echo "$INFO" | sed -n -e 's/^Connected to .* on \(.*\):$/\1/p' -e 's/^Serial port \(.*\)$/\1/p' | tail -1)"
  if [[ -z "$FOUND" ]]; then
    echo "Could not tell which serial port the chip answered on. Set PORT=... and try again. Refusing to flash."; exit 1
  fi
  PORTARG=(--port "$FOUND")
  echo "Port: $FOUND (found by esptool; set PORT to choose another)"
fi
CHIP="$(echo "$INFO" | sed -n 's/^Chip ID: *\([0-9]*\).*/\1/p' | head -1)"
if [[ "$CHIP" != "$IMG_CHIP" ]]; then
  echo "These images are for the $IMG_NAME (chip id $IMG_CHIP), the connected chip says chip id ${CHIP:-unknown}. Refusing to flash."; exit 1
fi
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
echo "$IMG_NAME, flash encryption: $ENC${CRYPT:+ (SPI_BOOT_CRYPT_CNT $CRYPT)} -> $MODE_ENC write"
HAVE_BACKUP=0; [[ -f "$BACKUP" && $(wc -c < "$BACKUP") -eq 16777216 ]] && HAVE_BACKUP=1
if [[ $HAVE_BACKUP == 0 ]]; then
  if [[ $MODE_ENC == encrypted ]]; then
    echo "Backup missing or not 16 MB: $BACKUP (set GAGGI_BACKUP). An encrypted screen is never flashed without it"
    echo "(see the README, \"Installing\", step 2). Refusing to flash."; exit 1
  elif [[ "${NO_BACKUP:-0}" != 1 ]]; then
    echo "No backup of this board's original flash: $BACKUP. Make one first (README, \"Installing\", step 2), or run"
    echo "with NO_BACKUP=1 to flash this plain board without it. Refusing to flash."; exit 1
  fi
  echo "No backup (NO_BACKUP=1, plain board): flashing anyway."
fi
ENCARG=(); [[ $MODE_ENC == encrypted ]] && ENCARG=(--encrypt)
CMD=("${ESP[@]}" ${PORTARG[@]+"${PORTARG[@]}"} --baud 460800 $W ${ENCARG[@]+"${ENCARG[@]}"} $FM dio $FF 80m $FS 16MB
     $BOOT_AT "$IMG/bootloader.bin" 0x8000 "$IMG/partitions.bin" 0xe000 "$IMG/boot_app0.bin" 0x10000 "$IMG/firmware.bin")
if [[ "${DRY_RUN:-0}" == 1 ]]; then echo "DRY_RUN=1, would run: ${CMD[*]}"; exit 0; fi
"${CMD[@]}"
echo "Flashed ($MODE_ENC write). The screen restarts by itself; if the picture looks wrong, unplug and replug USB."
