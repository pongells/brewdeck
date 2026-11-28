#!/usr/bin/env bash
# A UI's pictures for the release's interface catalogue (tools/uis.py catalog): builds the emulator for the UI and,
# with the fake machine online, saves three screens: Ready, brewing (the recorded shot, past half way) and steam (the
# recorded session, while the wand is open). Each is shrunk to 320 x 192 RGB565; the out file holds them one after
# another (Ready first). A screen the UI does not reach in time is left out, with the ones after it.
#   tools/release-preview.sh <ui folder name under firmware/src, or a path> <out file>
# Needs SDL2 (no window: SDL_VIDEODRIVER=dummy) and Pillow.
set -euo pipefail
UI="${1:?usage: tools/release-preview.sh <ui> <out>}"
OUT="${2:?output file}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "$UI" == */* ]] && UI="$(cd "$UI" && pwd)"
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
cd "$ROOT"
BIN="$(UI="$UI" sim/build-emu.sh | sed -n 's|^built ||p')"
[[ -x "$BIN" ]] || { echo "emulator build failed"; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# every UI tags these screens (docs/ui-features.md); the recordings play at 10x: the shot (~33 s) is past half way
# 2 s after the brewing screen opens, the steam session (3:20) has the wand open ~15 s after the steam screen opens
grab() {   # <name> <commands after Ready>
  printf 'wait 1500\nwait_for screen.ready 20000\n%s\nsave %s\nquit\n' "$2" "$TMP/$1.raw" |
    SDL_VIDEODRIVER=dummy "$BIN" > "$TMP/$1.log" 2>&1 || true
  [[ -s "$TMP/$1.raw" ]] && ! grep -q "^FAIL" "$TMP/$1.log"
}
FRAMES=()
if grab ready 'wait 1500'; then
  FRAMES+=("$TMP/ready.raw")
  if grab brewing $'machine shot 10\nexpect_visible screen.brewing 3000\nwait 2000'; then
    FRAMES+=("$TMP/brewing.raw")
    grab steam $'machine steam 10\nexpect_visible screen.steam 3000\nwait 15000' && FRAMES+=("$TMP/steam.raw")
  fi
fi
(( ${#FRAMES[@]} )) || { echo "no Ready screen: no preview"; exit 1; }
python3 tools/uis.py preview "$OUT" "${FRAMES[@]}"
