#!/usr/bin/env bash
# Signs a firmware image for updates over Wi-Fi: writes update-<chip>.txt next to the image copy
# brewdeck-<version>-<chip>.bin in <out dir>. The screen downloads the .txt, checks its signature with the public key
# built into the firmware (core/update_key.h), then the image's size and SHA-256 against it.
#   tools/sign-update.sh <version> <chip> <firmware.bin> <private key .pem> <out dir>
# update-<chip>.txt, two lines:
#   {"version":"v0.3.0","chip":"esp32p4","size":1503232,"sha256":"<64 hex>"}
#   <base64 of the DER ECDSA P-256 / SHA-256 signature of line 1, without its newline>
# Make a key pair (once; keep the private key out of every repository):
#   openssl ecparam -name prime256v1 -genkey -noout -out update-key.pem
#   openssl ec -in update-key.pem -pubout -out update-key.pub.pem
set -euo pipefail
VER="${1:?usage: tools/sign-update.sh <version> <chip> <firmware.bin> <key.pem> <out dir>}"
CHIP="${2:?chip, e.g. esp32p4}"
IMG="${3:?firmware image}"
KEY="${4:?private key (.pem)}"
OUT="${5:?output folder}"
[[ "$VER" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must look like v1.2.3, not $VER"; exit 1; }
[[ "$CHIP" =~ ^esp32(s3|p4)$ ]] || { echo "chip must be esp32s3 or esp32p4, not $CHIP"; exit 1; }
mkdir -p "$OUT"
BIN="$OUT/brewdeck-$VER-$CHIP.bin"
cp "$IMG" "$BIN"
SIZE=$(wc -c < "$BIN" | tr -d ' ')
SUM=$( (command -v sha256sum >/dev/null && sha256sum "$BIN" || shasum -a 256 "$BIN") | cut -d' ' -f1)
LINE=$(printf '{"version":"%s","chip":"%s","size":%s,"sha256":"%s"}' "$VER" "$CHIP" "$SIZE" "$SUM")
SIG=$(printf '%s' "$LINE" | openssl dgst -sha256 -sign "$KEY" | base64 | tr -d '\n')
printf '%s\n%s\n' "$LINE" "$SIG" > "$OUT/update-$CHIP.txt"
# check it with the public half before anything is published
PUB=$(mktemp)
SIGF=$(mktemp)
trap 'rm -f "$PUB" "$SIGF"' EXIT
openssl ec -in "$KEY" -pubout -out "$PUB" 2>/dev/null
printf '%s' "$SIG" | base64 -d > "$SIGF" 2>/dev/null || printf '%s' "$SIG" | base64 -D > "$SIGF"
printf '%s' "$LINE" | openssl dgst -sha256 -verify "$PUB" -signature "$SIGF" >/dev/null
echo "Signed: $OUT/update-$CHIP.txt ($BIN, $SIZE bytes)"
