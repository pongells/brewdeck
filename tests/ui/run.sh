#!/usr/bin/env bash
# Shared UI scenarios (docs/ui-features.md, "How the checks work"): builds the emulator for one UI, runs every
# scenario in tests/ui/scenarios against it without a window (tests/ui/report.py) and prints the UI's status table:
# done / partial / FAILED / missing / omitted / not checked yet / by hand per feature, from the results and the UI's
# ui-features.txt. Output (status.md, emulator logs, the UI's commands) in tests/ui/out/<emulator>/.
# Usage: tests/ui/run.sh [ui]    ui: a folder under firmware/src (default: ui, Brewdeck's own) or a path to a UI folder
#        SCENARIO=x1 tests/ui/run.sh    only the scenarios whose file name starts with x1
#        UI_OUT=<folder>                where the results go (a UI's own repo: its checks/ folder)
set -euo pipefail
UI="${1:-ui}"; UI="${UI%/}"
[[ "$UI" == */* ]] && UI="$(cd "$UI" && pwd)"   # a path: absolute, the build runs from elsewhere
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
UD="$UI"; [[ "$UI" == */* ]] || UD="$ROOT/firmware/src/$UI"
BIN="$(UI="$UI" sim/build-emu.sh | sed -n 's|^built ||p')"
[[ -x "$BIN" ]] || { echo "emulator build failed"; exit 1; }
exec python3 tests/ui/report.py --emu "$BIN" --ui "$UD" --out "${UI_OUT:-tests/ui/out/$(basename "$BIN")}" --only "${SCENARIO:-}"
