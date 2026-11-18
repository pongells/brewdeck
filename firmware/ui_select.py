# PlatformIO pre-script: which UI folder under src/ is built with the core. Brewdeck's own is "ui"; another UI lives
# in its own folder (e.g. src/ui_minimal, the example) and implements the bd_ui_* functions of
# src/core/brewdeck_core.h. Pick it with the environment variable BREWDECK_UI (build-pio.sh: UI=<folder>).
# The board layer comes from the env's custom_board (platformio.ini): src/board/common (shared by both boards) plus
# src/board/s3 (the Sunton ESP32-S3) or src/board/p4 (the Guition ESP32-P4). The env's -I flags pick the same board's
# board.h.
import os
Import("env")

ui = os.environ.get("BREWDECK_UI", "").strip().strip("/") or "ui"
src = env.subst("$PROJECT_SRC_DIR")
if ui in ("core", "board", "touchtest") or not os.path.isdir(os.path.join(src, ui)):
    raise SystemExit("BREWDECK_UI=%s: no such UI folder in %s" % (ui, src))
board = env.GetProjectOption("custom_board", "s3")
if board not in ("s3", "p4"):
    raise SystemExit("custom_board=%s: expected s3 or p4" % board)
env.Replace(SRC_FILTER=["-<*>", "+<board/common/>", "+<board/%s/>" % board, "+<core/>", "+<%s/>" % ui])
print("Brewdeck UI: src/%s, board %s" % (ui, board))
