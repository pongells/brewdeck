#!/usr/bin/env bash
# Installs the ESP-IDF release firmware/build.sh builds with (its IDF_VERSION) into ~/esp/esp-idf-<version>, or
# IDF_PATH, with the tools for the ESP32-S3, ESP32-P4 and ESP32-C6 (in ~/.espressif, or IDF_TOOLS_PATH). Run it once,
# and again when IDF_VERSION changes; an existing install is only checked. CI runs it too (.github/actions/esp-idf).
# Needs git, Python 3, CMake and Ninja (macOS: brew install cmake ninja; Debian / Ubuntu: apt install cmake
# ninja-build ccache libusb-1.0-0).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="$(sed -n 's/^IDF_VERSION=//p' "$ROOT/firmware/build.sh")"
[[ -n "$VER" ]] || { echo "IDF_VERSION not found in firmware/build.sh" >&2; exit 1; }
DIR="${IDF_PATH:-$HOME/esp/esp-idf-$VER}"
if [[ ! -f "$DIR/export.sh" ]]; then
  echo "Getting ESP-IDF $VER into $DIR ..."
  mkdir -p "$(dirname "$DIR")"
  git clone -q -b "$VER" --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$DIR"
fi
HAVE="$(git -C "$DIR" describe --tags --exact-match 2>/dev/null || true)"
[[ "$HAVE" == "$VER" ]] || { echo "$DIR holds ESP-IDF '${HAVE:-unknown}', not $VER: remove it or set IDF_PATH" >&2; exit 1; }
"$DIR/install.sh" esp32s3,esp32p4,esp32c6
echo "ESP-IDF $VER ready in $DIR (firmware/build.sh loads it by itself)."
