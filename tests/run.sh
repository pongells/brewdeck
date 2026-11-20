#!/usr/bin/env bash
# Host unit tests for the firmware's pure parts (no hardware, no Arduino): REST JSON readers, profile preview geometry,
# WebSocket protobuf decoder and command builders, stored shots + shot history index, core state and command rules.
# Fixtures are real machine replies in sim/data. Built with ASan + UBSan.
# Usage: tests/run.sh [-v]     (-v: preview test also prints ASCII plots). Needs a C++ compiler (c++ / clang++).
set -euo pipefail
cd "$(dirname "$0")/.."
CORE=firmware/src/core; D=sim/data
OUT="$(mktemp -d "${TMPDIR:-/tmp}/gaggi-tests.XXXXXX")"; trap 'rm -rf "$OUT"' EXIT   # own folder: runs can overlap
CXX="${CXX:-c++}"; FLAGS="-std=c++14 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$CORE"
V="${1:-}"
fail=0
run() {   # name, sources..., -- args...
  local name="$1"; shift; local src=(); while [[ "$1" != "--" ]]; do src+=("$1"); shift; done; shift
  echo "== $name"
  if ! "$CXX" $FLAGS "${src[@]}" -o "$OUT/$name"; then echo "== $name DID NOT COMPILE"; fail=1; return; fi
  if "$OUT/$name" "$@"; then :; else echo "== $name FAILED"; fail=1; fi
}
run parse_test   tests/parse_test.cpp   $CORE/parse.cpp                -- $D/profiles_all.json $D/profile_8.json $D/profile_1.json $D/status.json
run preview_test tests/preview_test.cpp $CORE/parse.cpp $CORE/preview.cpp -- $V $D/profile_8.json $D/profile_1.json $D/profile_4.json $D/profile_5.json
run pb_test      tests/pb_test.cpp      $CORE/pb.cpp                   -- $D/ws_idle.bin
run shot_test    tests/shot_test.cpp    $CORE/parse.cpp $CORE/pb.cpp     -- $D/shot_298.json $D/shot_297.json $D/ws_idle.bin $D/ws_shot.bin
run core_state_test tests/core_state_test.cpp $CORE/core_state.cpp $CORE/preview.cpp $CORE/pb.cpp $CORE/parse.cpp --
[[ $fail == 0 ]] && echo "ALL TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
