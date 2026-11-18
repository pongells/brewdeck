#!/usr/bin/env bash
# Host-side render test: builds LVGL + the real ui.cpp on the desktop and dumps screens to PNG
# (no hardware needed). Usage: ./build.sh && ./render out && python3 topng.py out
# Needs: gcc/g++, python3 + Pillow (pip install pillow), git (first run clones LVGL 9.6.0 into ./lvgl).
set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"; P="$HERE/../firmware"; OUT="$HERE/obj"; mkdir -p "$OUT" out
LV=v9.6.0                       # the LVGL release the UI is built with (firmware/platformio.ini has the same)
[[ -d lvgl ]] || git clone --depth 1 --branch $LV https://github.com/lvgl/lvgl.git lvgl
# objects of another LVGL version or another lv_conf.h are never reused (the object names do not change)
STAMP="$LV $(cksum < "$P/include/lv_conf.h" | cut -d' ' -f1)"
[[ "$(cat "$OUT/.lvgl" 2>/dev/null)" == "$STAMP" ]] || { rm -f "$OUT"/*.o; echo "$STAMP" > "$OUT/.lvgl"; }
FLAGS="-O1 -DSIM_DIR=\"$HERE\" -DLV_SIM_HOST -DLV_CONF_INCLUDE_SIMPLE -I$P/include -I$HERE/lvgl -I$P/src/ui -I$P/src/core -w"
# file passed as $1 (not -I{}): BSD xargs on macOS limits -I replacements to 255 bytes
export FLAGS HERE OUT
(cd lvgl && find src -name '*.c') | xargs -P 8 -n 1 sh -c 'o="$OUT/$(echo "$1" | tr / _).o"; [ -f "$o" ] || gcc $FLAGS -c "$HERE/lvgl/$1" -o "$o"' _
for f in "$P"/src/ui/fonts/*.c; do gcc $FLAGS -c "$f" -o "$OUT/font_$(basename "$f").o"; done
for f in "$P"/src/ui/ui.cpp "$P"/src/ui/screens/*.cpp; do g++ $FLAGS -std=c++14 -c "$f" -o "$OUT/ui_$(basename "$f" .cpp).o"; done
gcc $FLAGS -c "$P/src/core/lv_mem_psram.c" -o "$OUT/lv_mem_psram.o"   # LVGL's heap (malloc here)
for f in parse preview pb; do g++ $FLAGS -std=c++14 -c "$P/src/core/$f.cpp" -o "$OUT/$f.o"; done
g++ $FLAGS -std=c++14 -c "$HERE/harness.cpp" -o "$OUT/harness.o"
g++ $FLAGS -std=c++14 -c "$HERE/null_core.cpp" -o "$OUT/null_core.o"   # ui.cpp's commands go nowhere here
g++ "$OUT"/*.o -o "$HERE/render" -lm
echo built
