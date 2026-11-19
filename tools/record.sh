#!/usr/bin/env bash
# Records what the screen shows, without loading the Wi-Fi meanwhile: the screen keeps the frames in its own memory
# (PSRAM) and they are downloaded in one go at the end.
#   ./tools/record.sh start [every_ms] [max_frames]   start recording (default every 2000 ms, until memory is full)
#   ./tools/record.sh status                          frames so far
#   ./tools/record.sh stop                            stop recording (the frames stay on the screen)
#   ./tools/record.sh fetch [dir]                     download and write dir/frame-NNNN-<seconds>.png (stops recording)
#   ./tools/record.sh free                            give the memory back to the screen
# Env HOST (default brewdeck.local; the ESP32-P4 board: HOST=brewdeck-p4.local). Needs a debug build
# (./build.sh ui-debug --flash). PNG via sips (built into macOS); elsewhere the .bmp files are kept.
set -euo pipefail
HOST="${HOST:-brewdeck.local}"
get() { curl -4 -fsS -m "${2:-10}" "http://$HOST$1"; }   # -4: macOS otherwise waits ~5 s for IPv6 over mDNS
case "${1:-}" in
  start)  q="every=${2:-2000}"; [ -n "${3:-}" ] && q="$q&max=$3"; get "/rec?$q" ;;
  status) get /rec ;;
  stop)   get /rec/stop ;;
  free)   get /rec/free ;;
  fetch)
    dir="${2:-rec-$(date +%Y%m%d-%H%M%S)}"
    mkdir -p "$dir"
    get /rec.bin 600 > "$dir/rec.bin"
    python3 -I - "$dir/rec.bin" "$dir" <<'EOF'
import struct, sys
data, out = open(sys.argv[1], 'rb').read(), sys.argv[2]
pos = n = 0
while pos + 16 <= len(data):
    magic, ms, ln, w, h = struct.unpack_from('<4sIIHH', data, pos)
    if magic != b'BDRF':
        sys.exit('bad frame at byte %d' % pos)
    runs = struct.unpack_from('<%dH' % (ln // 2), data, pos + 16)
    px = bytearray()
    for i in range(0, len(runs), 2):
        px += struct.pack('<H', runs[i + 1]) * runs[i]
    if len(px) != w * h * 2:
        sys.exit('frame %d has %d pixels, expected %d' % (n, len(px) // 2, w * h))
    hdr = bytearray(66)   # BMP, RGB565 bit fields, top-down rows
    struct.pack_into('<2sIII', hdr, 0, b'BM', 66 + len(px), 0, 66)
    struct.pack_into('<IiiHHIIiiIIIII', hdr, 14, 40, w, -h, 1, 16, 3, len(px), 0, 0, 0, 0, 0xF800, 0x07E0, 0x001F)
    open('%s/frame-%04d-%07.1fs.bmp' % (out, n, ms / 1000), 'wb').write(hdr + px)
    pos += 16 + ln
    n += 1
print('%d frames' % n)
EOF
    rm "$dir/rec.bin"
    if command -v sips >/dev/null; then
      for f in "$dir"/*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"; done
    fi
    echo "$dir" ;;
  *) sed -n '2,10p' "$0"; exit 1 ;;
esac
