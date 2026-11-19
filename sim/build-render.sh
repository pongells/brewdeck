#!/usr/bin/env bash
# Render harness: Brewdeck's screens (ui.cpp + screens/) on the desktop, drawn to files (no hardware needed).
# Usage: sim/build-render.sh && sim/render sim/out && python3 sim/topng.py sim/out
# Needs: gcc/g++, python3 + Pillow (pip install pillow), git (the first run clones LVGL into sim/lvgl).
set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"; P="$HERE/../firmware"; OUT="$HERE/obj"; mkdir -p "$OUT" out
FLAGS="-O1 -DSIM_DIR=\"$HERE\" -DLV_SIM_HOST -DLV_CONF_INCLUDE_SIMPLE -I$P/include -I$HERE/lvgl -I$P/src/ui -I$P/src/core -w"
./lvgl.sh "$OUT" gcc $FLAGS
rm -f "$OUT"/*.o
for f in "$P"/src/ui/fonts/*.c; do gcc $FLAGS -c "$f" -o "$OUT/font_$(basename "$f").o"; done
for f in "$P"/src/ui/ui.cpp "$P"/src/ui/screens/*.cpp; do g++ $FLAGS -std=c++14 -c "$f" -o "$OUT/ui_$(basename "$f" .cpp).o"; done
gcc $FLAGS -c "$P/src/core/lv_mem_psram.c" -o "$OUT/lv_mem_psram.o"   # LVGL's heap (malloc here)
for f in parse preview pb; do g++ $FLAGS -std=c++14 -c "$P/src/core/$f.cpp" -o "$OUT/$f.o"; done
g++ $FLAGS -std=c++14 -c "$HERE/harness.cpp" -o "$OUT/harness.o"
g++ $FLAGS -std=c++14 -c "$HERE/null_core.cpp" -o "$OUT/null_core.o"   # ui.cpp's commands go nowhere here
g++ "$OUT"/*.o "$OUT"/lvgl/*.o -o "$HERE/render" -lm
echo built
