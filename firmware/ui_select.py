# PlatformIO pre-script: which UI folder under src/ is built with the core. Brewdeck's own is "ui"; another UI lives
# in its own folder (e.g. src/ui_minimal, the example) and implements the bd_ui_* functions of
# src/core/brewdeck_core.h. Pick it with the environment variable BREWDECK_UI (build-pio.sh: UI=<folder>).
import os
Import("env")

ui = os.environ.get("BREWDECK_UI", "").strip().strip("/") or "ui"
src = env.subst("$PROJECT_SRC_DIR")
if ui in ("core", "board", "shared", "touchtest") or not os.path.isdir(os.path.join(src, ui)):
    raise SystemExit("BREWDECK_UI=%s: no such UI folder in %s" % (ui, src))
env.Replace(SRC_FILTER=["-<*>", "+<shared/>", "+<board/>", "+<core/>", "+<%s/>" % ui])
print("Brewdeck UI: src/%s" % ui)
