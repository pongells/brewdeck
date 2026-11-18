#!/usr/bin/env bash
# Interactive emulator: a UI on the core API in an SDL2 window with a fake machine from sim/data (see emu.cpp).
# Usage: sim/build-emu.sh && sim/emu        Needs: brew install sdl2 (SDL2 API; Homebrew ships sdl2-compat on SDL3).
#        UI=ui_minimal sim/build-emu.sh && sim/emu-ui_minimal    another UI folder under firmware/src (default: ui)
# Reuses the LVGL objects that sim/build.sh compiled (runs it first if they are missing).
set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"; P="$HERE/../firmware"; OUT="$HERE/obj"
UI="${UI:-ui}"; UI="${UI%/}"; UD="$P/src/$UI"
[[ -d "$UD" ]] || { echo "no UI folder $UD"; exit 1; }
[[ -d obj && -f obj/ui_ui.o ]] || ./build.sh
SDL_CFLAGS="$(sdl2-config --cflags)"; SDL_LIBS="$(sdl2-config --libs)"
FLAGS="-O1 -DSIM_DIR=\"$HERE\" -DLV_SIM_HOST -DLV_CONF_INCLUDE_SIMPLE -I$P/include -I$HERE/lvgl -I$UD -I$P/src/core -w"
EO="$HERE/obj-emu"; UO="$EO/ui-$UI"; mkdir -p "$EO" "$UO"
rm -f "$EO"/ui_*.o "$EO"/font_*.o "$UO"/*.o      # UI objects live in obj-emu/ui-<folder> (older builds kept them here)
# every C / C++ file of the UI folder (Brewdeck's: ui.cpp, screens/, ui_bridge.cpp, fonts/)
(cd "$UD" && find . -name '*.cpp' -o -name '*.c') | while read -r f; do
  o="$UO/$(echo "${f#./}" | tr / _).o"
  case "$f" in *.c) gcc $FLAGS -c "$UD/$f" -o "$o";; *) g++ $FLAGS -std=c++14 -c "$UD/$f" -o "$o";; esac
done
for f in parse preview pb; do g++ $FLAGS -std=c++14 -c "$P/src/core/$f.cpp" -o "$EO/$f.o"; done
g++ $FLAGS -std=c++14 -c "$P/src/core/core_state.cpp" -o "$EO/core_state.o"   # the fake core's state (shared)
g++ $FLAGS -I$P/src/ui $SDL_CFLAGS -std=c++14 -c "$HERE/emu.cpp" -o "$EO/emu.o"   # ui.h: Brewdeck's test hooks
rm -f "$EO"/panel_*.o
if [[ "$UI" != ui ]]; then   # the emulator's control panel uses two of Brewdeck's fonts (its UI brings them otherwise)
  for f in it_400_13 it_600_16; do gcc $FLAGS -c "$P/src/ui/fonts/$f.c" -o "$EO/panel_$f.o"; done
fi
BIN="$HERE/emu"; [[ "$UI" == ui ]] || BIN="$HERE/emu-$UI"
g++ $(ls "$OUT"/*.o | grep -v -e '/ui_[a-z]*\.o$' -e '/harness.o$' -e '/null_core.o$' -e '/parse.o$' -e '/preview.o$' -e '/pb.o$' -e '/font_') "$EO"/*.o "$UO"/*.o \
    -o "$BIN" $SDL_LIBS -lm
echo "built sim/$(basename "$BIN")"
