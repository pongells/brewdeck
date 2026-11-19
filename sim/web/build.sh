#!/usr/bin/env bash
# Web demo: Brewdeck's UI + the emulator's fake machine compiled to WebAssembly (see web.cpp).
# Usage: sim/web/build.sh            -> sim/web/dist/ (index.html, brewdeck.js, brewdeck.wasm, brewdeck.data)
#        python3 -m http.server -d sim/web/dist 8000   then open http://localhost:8000
# Needs: Emscripten (brew install emscripten), git (LVGL is cloned into sim/lvgl).
set -euo pipefail
cd "$(dirname "$0")"
WEB="$PWD"; SIM="$WEB/.."; P="$SIM/../firmware"; OBJ="$WEB/obj"; DIST="$WEB/dist"
mkdir -p "$OBJ" "$DIST"
FLAGS="-O2 -DSIM_DIR=\"\" -DLV_SIM_HOST -DLV_CONF_INCLUDE_SIMPLE -I$P/include -I$SIM/lvgl -I$P/src/ui -I$P/src/core -w"
"$SIM/lvgl.sh" "$OBJ" emcc $FLAGS
rm -f "$OBJ"/*.o
for f in "$P"/src/ui/fonts/*.c; do emcc $FLAGS -c "$f" -o "$OBJ/font_$(basename "$f").o"; done
for f in "$P"/src/ui/ui.cpp "$P"/src/ui/screens/*.cpp; do em++ $FLAGS -std=c++14 -c "$f" -o "$OBJ/ui_$(basename "$f" .cpp).o"; done
emcc $FLAGS -c "$P/src/core/lv_mem_psram.c" -o "$OBJ/lv_mem_psram.o"
for f in parse preview pb; do em++ $FLAGS -std=c++14 -c "$P/src/core/$f.cpp" -o "$OBJ/$f.o"; done
em++ $FLAGS -std=c++14 -c "$P/src/ui/ui_bridge.cpp" -o "$OBJ/ui_bridge.o"
em++ $FLAGS -std=c++14 -c "$P/src/core/core_state.cpp" -o "$OBJ/core_state.o"
em++ $FLAGS -std=c++14 -c "$WEB/web.cpp" -o "$OBJ/web.o"
# the recordings the fake machine plays, packed as /data (read_file opens SIM_DIR "/data/<name>")
DATA=()
for f in profiles_all.json ws_demo_shot.bin ws_steam.bin "$SIM"/data/profile_*.json "$SIM"/data/shot_*.json; do
  f="$(basename "$f")"; DATA+=(--preload-file "$SIM/data/$f@/data/$f")
done
em++ "$OBJ"/*.o "$OBJ"/lvgl/*.o -o "$DIST/brewdeck.js" -O2 "${DATA[@]}" \
  -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0 -sMODULARIZE=1 -sEXPORT_NAME=Brewdeck \
  -sEXPORTED_FUNCTIONS=_main,_web_frame,_web_fb,_web_backlight,_web_touch,_web_action,_web_status,_web_shot_playing,_web_steam_on \
  -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU16
cp "$WEB/index.html" "$DIST/index.html"
cp "$SIM/../docs/images/logo.svg" "$DIST/logo.svg"
ls -la "$DIST" | awk 'NR>1 {print $5, $9}'
echo "built sim/web/dist"
