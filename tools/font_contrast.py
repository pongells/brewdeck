#!/usr/bin/env python3
"""Steeper antialiased edges for the UI's 4-bit fonts (made with lv_font_conv, options in each file's header): every
pixel's coverage v (0..15) goes through an S-curve around the middle, so half-covered edge pixels move towards empty
or full. Same letter shapes and spacing; on the 4.3" 800x480 panel the text looks sharper (light / regular Inter
Tight looked soft with plain coverage). Stronger hinting (lv_font_conv --autohint-strong) was tried: it broke kerning.

Usage: python3 tools/font_contrast.py firmware/src/ui/fonts/it_400_16.c [...]   (run once on a freshly generated file)
"""
import re, sys

K = 1.5                                           # slope at the middle; 1 = unchanged
LUT = [max(0, min(15, round((v - 7.5) * K + 7.5))) for v in range(16)]
MARK = ' * Edge contrast: tools/font_contrast.py (x%g)\n' % K

for path in sys.argv[1:]:
    src = open(path).read()
    if 'Edge contrast:' in src:
        print(path, 'already done'); continue
    if '--bpp 4' not in src:
        print(path, 'skipped (not 4 bpp)'); continue
    a = src.index('glyph_bitmap[] = {')
    b = src.index('};', a)
    body = re.sub(r'0x([0-9a-fA-F]{2})',
                  lambda m: '0x%02x' % (LUT[int(m.group(1), 16) >> 4] << 4 | LUT[int(m.group(1), 16) & 15]), src[a:b])
    head = src[:a]
    k = head.index(' * Opts:')
    k = head.index('\n', k) + 1
    open(path, 'w').write(head[:k] + MARK + head[k:] + body + src[b:])
    print(path, 'done')
