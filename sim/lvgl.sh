#!/usr/bin/env bash
# Compiles LVGL for the host builds (render harness, emulator, web demo) into <objdir>/lvgl. LVGL's version is read
# from firmware/platformio.ini; the objects are rebuilt when it, lv_conf.h, the compiler or the flags change.
# Usage: sim/lvgl.sh <objdir> <cc> <flags...>
set -euo pipefail
SIM="$(cd "$(dirname "$0")" && pwd)"; P="$SIM/../firmware"
OUT="$1/lvgl"; CC="$2"; shift 2; FLAGS="$*"
LV="v$(sed -n 's|.*lvgl/lvgl@\([0-9.]*\).*|\1|p' "$P/platformio.ini" | head -1)"
if [[ "$(git -C "$SIM/lvgl" describe --tags 2>/dev/null)" != "$LV" ]]; then
  rm -rf "$SIM/lvgl"
  git clone --quiet --depth 1 --branch "$LV" https://github.com/lvgl/lvgl.git "$SIM/lvgl"
fi
mkdir -p "$OUT"
STAMP="$LV $(cksum < "$P/include/lv_conf.h" | cut -d' ' -f1) $CC $FLAGS"
[[ "$(cat "$OUT/.stamp" 2>/dev/null)" == "$STAMP" ]] || { rm -f "$OUT"/*.o; echo "$STAMP" > "$OUT/.stamp"; }
export CC FLAGS SIM OUT
# the file goes in as $1, not -I{}: BSD xargs on macOS limits -I replacements to 255 bytes
(cd "$SIM/lvgl" && find src -name '*.c') |
  xargs -P 8 -n 1 sh -c 'o="$OUT/$(echo "$1" | tr / _).o"; [ -f "$o" ] || $CC $FLAGS -c "$SIM/lvgl/$1" -o "$o"' _
