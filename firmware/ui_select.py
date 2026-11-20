# PlatformIO pre-script: which UI folder under src/ is built with the core. Brewdeck's own is "ui"; another UI lives
# in its own folder (e.g. src/ui_minimal, the example) and implements the bd_ui_* functions of
# src/core/brewdeck_core.h. Pick it with the environment variable BREWDECK_UI (build.sh: UI=<folder>).
# The board layer comes from the env's custom_board (platformio.ini): src/board/common (shared by both boards) plus
# src/board/s3 (the Sunton ESP32-S3) or src/board/p4 (the Guition ESP32-P4). The env's -I flags pick the same board's
# board.h.
import os
Import("env")

ui = os.environ.get("BREWDECK_UI", "").strip().strip("/") or "ui"
src = env.subst("$PROJECT_SRC_DIR")
NOT_UIS = ("core", "board", "touchtest", "c6update")   # the other folders under src/
if "/" in ui or ui.startswith(".") or ui in NOT_UIS or not os.path.isdir(os.path.join(src, ui)):
    raise SystemExit("BREWDECK_UI=%s: no such UI folder in %s" % (ui, src))
board = env.GetProjectOption("custom_board", "s3")
if board not in ("s3", "p4"):
    raise SystemExit("custom_board=%s: expected s3 or p4" % board)
env.Replace(SRC_FILTER=["-<*>", "+<board/common/>", "+<board/%s/>" % board, "+<core/>", "+<%s/>" % ui])
print("Brewdeck UI: src/%s, board %s" % (ui, board))

# The firmware's version: the git tag ("v0.2.0" on a release), "v0.2.0-3-gabc1234" between releases (3 commits after
# it), "-dirty" with uncommitted changes, a bare commit hash without tags (shallow CI checkouts). Written to a header
# in the build folder (core.cpp includes it if present), rewritten only when it changes so only that file recompiles.
import subprocess
try:
    ver = subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"],
                                  cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL).decode().strip()
except Exception:
    ver = ""
gen = os.path.join(env.subst("$BUILD_DIR"), "gen")
os.makedirs(gen, exist_ok=True)
hdr = os.path.join(gen, "brewdeck_version.h")
text = '#pragma once\n#define BREWDECK_VERSION "%s"\n' % ver.replace('"', "")
if not os.path.exists(hdr) or open(hdr).read() != text:
    open(hdr, "w").write(text)
env.Append(CPPPATH=[gen])
print("Brewdeck version: %s" % (ver or "unknown"))
