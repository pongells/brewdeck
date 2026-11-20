#!/usr/bin/env bash
# Grabs what the screen shows right now: ./tools/screenshot.sh [out.png]   (env HOST, default brewdeck.local;
#   the ESP32-P4 board: HOST=brewdeck-p4.local)
# Needs the ui-debug firmware (./build.sh ui-debug --flash; release builds have no web server) with Wi-Fi configured. PNG via sips (built into macOS); elsewhere the .bmp is kept.
set -euo pipefail
HOST="${HOST:-brewdeck.local}"
OUT="${1:-screen-$(date +%Y%m%d-%H%M%S).png}"
# -4: macOS otherwise waits ~5 s for an IPv6 answer from mDNS first
if [[ "$OUT" == *.bmp ]]; then curl -4 -fsS -m 60 -o "$OUT" "http://$HOST/screen.bmp"; echo "$OUT"; exit 0; fi
BMP="${OUT%.*}.bmp"
curl -4 -fsS -m 60 -o "$BMP" "http://$HOST/screen.bmp"
if command -v sips >/dev/null; then sips -s format png "$BMP" --out "$OUT" >/dev/null && rm "$BMP" && echo "$OUT"
else echo "$BMP"; fi
