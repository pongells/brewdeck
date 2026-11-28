#!/usr/bin/env bash
# A UI's Ready screen as its preview in the release's interface catalogue (tools/uis.py catalog): builds the emulator
# for the UI, waits for its Ready screen with the fake machine online, saves it and shrinks it to 320 x 192 RGB565.
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
RAW="$(mktemp)"
trap 'rm -f "$RAW"' EXIT
# every UI tags its Ready screen screen.ready (docs/ui-features.md); the live values settle within a second
printf 'wait 1500\nwait_for screen.ready 20000\nwait 1500\nsave %s\nquit\n' "$RAW" | SDL_VIDEODRIVER=dummy "$BIN" > /dev/null
python3 tools/uis.py preview "$RAW" "$OUT"
