#!/usr/bin/env python3
"""Converts the RGB565 frames the render harness and the emulator write (*.raw, 800x480, little-endian) to PNG files
next to them. Usage: python3 sim/topng.py <dir>   Needs Pillow (pip install pillow)."""
import glob, struct, sys
from PIL import Image

W, H = 800, 480


def to_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) != W * H * 2:
        sys.exit('%s: %d bytes, expected %d (800x480 RGB565)' % (path, len(data), W * H * 2))
    im = Image.new('RGB', (W, H))
    px = im.load()
    for i, c in enumerate(struct.unpack('<%dH' % (W * H), data)):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        px[i % W, i // W] = ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)
    out = path[:-4] + '.png'
    im.save(out)
    print(out)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for p in sorted(glob.glob(sys.argv[1] + '/*.raw')):
        to_png(p)
