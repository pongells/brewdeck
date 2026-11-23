#!/usr/bin/env bash
# After a shot or a steam session on the panel: were the frames fast enough? (docs/ui-features.md B6 and X5)
#   ./tools/frames.sh          the newest session        ./tools/frames.sh all    every session kept (up to 4)
#   env HOST (default brewdeck.local; the ESP32-P4 board: HOST=brewdeck-p4.local)
# Reads GET /frames from the screen (debug builds only: ./build.sh ui-debug --flash); asks nothing of the machine.
# A session passes when, after its first 1.5 s (the screen opening, a full redraw, only reported), no frame took
# 150 ms or more and at most 2 % took 40 ms or more (Brewdeck: most frames under 40 ms, full redraws ~50-150 ms).
# Exit 1 if one failed, 2 if the screen has no session yet or did not answer.
set -euo pipefail
HOST="${HOST:-brewdeck.local}"
REP="$(curl -4 -fsS -m 10 "http://$HOST/frames")" || { echo "no answer from $HOST/frames (a debug build?)"; exit 2; }
grep -q '^session ' <<<"$REP" || { echo "no shot or steam session since the screen started"; exit 2; }
[[ "${1:-}" == all ]] || REP="$(grep -m1 '^session ' <<<"$REP")"
awk '/^session / {
  delete v; for (i = 2; i <= NF; i++) { k = $i; sub(/=.*/, "", k); x = $i; sub(/^[^=]*=/, "", x); v[k] = x }
  pct = (v["frames"] > 0 ? int(100 * v["over40"] / v["frames"]) : 0)
  verdict = "ok"; if (v["over150"] > 0 || 50 * v["over40"] > v["frames"]) { verdict = "SLOW"; fail = 1 }
  printf "%-5s %-5s %5.1f s, %5d frames, avg %2d ms, longest %3d ms at %.1f s; >=40 ms: %d (%d %%), >=80: %d, >=150: %d; opening %d ms%s\n",
    verdict, v["kind"], v["dur_s"], v["frames"], v["avg_ms"], v["max_ms"], v["max_at_s"], v["over40"], pct, v["over80"],
    v["over150"], v["open_max_ms"], (v["open"] == "yes" ? ", still running" : "")
  printf "      slowest (ms@s): %s\n", (v["slowest"] == "" ? "none" : v["slowest"])
} END { exit fail }' <<<"$REP"
