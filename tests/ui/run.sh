#!/usr/bin/env bash
# Shared UI scenarios (docs/ui-features.md, "How the checks work"): runs every scenario in tests/ui/scenarios against
# one UI in the emulator, without a window. A scenario is an emulator script (stdin commands, see sim/emu.cpp) whose
# checks print PASS / FAIL. Output (emulator log, the UI's commands) in tests/ui/out/<ui>/.
# Usage: tests/ui/run.sh [ui]    ui: a folder under firmware/src (default: ui, Brewdeck's own) or a path to a UI folder
#        SCENARIO=x1 tests/ui/run.sh    only the scenarios whose file name starts with x1
set -euo pipefail
UI="${1:-ui}"; UI="${UI%/}"
[[ "$UI" == */* ]] && UI="$(cd "$UI" && pwd)"   # a path: absolute, the build runs from elsewhere
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
BIN="$(UI="$UI" sim/build-emu.sh | sed -n 's|^built ||p')"
[[ -x "$BIN" ]] || { echo "emulator build failed"; exit 1; }
OUT="tests/ui/out/$(basename "$BIN")"; mkdir -p "$OUT"
TO=(); command -v timeout >/dev/null && TO=(timeout 300)
pass=0; fail=0
for s in tests/ui/scenarios/"${SCENARIO:-}"*.txt; do
  n="$(basename "$s" .txt)"
  set +e
  SDL_VIDEODRIVER=dummy EMU_CMDLOG="$OUT/$n.cmds" "${TO[@]}" "$BIN" < "$s" > "$OUT/$n.log" 2>&1
  rc=$?
  set -e
  checks="$(grep -cE '^(PASS|FAIL) ' "$OUT/$n.log" || true)"
  if [[ $rc -eq 0 && $checks -gt 0 ]]; then
    pass=$((pass + 1)); echo "ok    $n ($checks checks)"
  else
    fail=$((fail + 1)); echo "FAIL  $n (exit $rc, log $OUT/$n.log)"
    grep -E '^FAIL ' "$OUT/$n.log" | sed 's/^/      /' || true
  fi
done
echo "$(basename "$BIN"): $pass passed, $fail failed"
[[ $fail -eq 0 ]]
