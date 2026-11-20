#!/usr/bin/env python3
"""Records the animations and stills of the user manual (docs/manual.md) from the emulator (sim/emu, the real UI code
against a fake machine that plays a recorded shot and a recorded steam session).

Usage: python3 tools/gen_manual.py [scene ...]      (default: all; build the emulator first: sim/build-emu.sh)
Output: docs/manual/<scene>.gif (animations) and docs/manual/<name>.png (stills). Needs Pillow.

Each scene is an emulator script (stdin commands, see sim/emu.cpp) plus a few shorthands:
  frame              grab the screen now (one GIF frame)
  frames N MS        N frames, MS apart
  still NAME         save a PNG still as docs/manual/NAME.png
  image NAME         save a PNG as docs/images/NAME.png (the README's screenshots, scene "readme")
Screen coordinates are the panel's (800 x 480). The emulator runs in real time; steam plays at 10x ("Steam, fast").
"""
import os, shutil, subprocess, sys, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'manual')
EMU = os.path.join(ROOT, 'sim', 'emu')
W, H = 640, 384                                    # GIF size (the panel is 800 x 480)

# scene: (script, ms per GIF frame, last frame held ms)
SCENES = {
    'start': ('''wait 1500
button Splash
frames 22 250
''', 250, 1500),
    'adjust': ('''wait 2500
frame
frame
tap 744 336
frames 3 300
tap 744 336
frames 8 300
tap 600 336
frames 4 300
tap 100 322
frames 8 300
''', 300, 1500),
    'profiles': ('''wait 2500
frame
frame
tap 650 52
frames 6 300
tap 590 135
frames 8 300
''', 300, 1500),
    'restore': ('''wait 2500
tap 320 336
wait 200
tap 320 336
wait 200
tap 320 336
frames 3 300
wait 4000
tap 650 52
frames 6 300
tap 732 152
frames 8 300
tap 467 306
frames 4 300
wait 2500
tap 750 50
frames 6 300
''', 300, 1500),
    'shot': ('''wait 2500
frame
key s
frames 42 1000
frames 6 1000
''', 220, 2000),
    'review': ('''wait 1500
button Shot > review
wait 10000
frame
frame
drag 700 230 120 230
frame
drag 120 230 300 230
frame
drag 300 230 460 230
frame
drag 460 230 620 230
frame
tap 598 52
wait 1500
frames 3 400
drag 620 230 250 230
frame
''', 700, 2000),
    'steam': ('''wait 1500
button Steam, fast
frames 48 500
''', 260, 2000),
    'flush': ('''wait 2500
frame
tap 725 430
frames 6 400
tap 725 430
frames 3 400
hold 725 430 1200
frames 14 400
''', 400, 1500),
    'notices': ('''wait 2500
frame
button Pressure release
frames 16 300
button Notification
wait 300
button Notification
wait 300
button Notification
frames 8 400
frames 4 1500
tap 561 50
frames 5 400
''', 300, 2500),
    'standby': ('''wait 1500
button Display
wait 500
tap 715 296
wait 500
tap 750 50
wait 800
key i
frames 30 600
''', 300, 1000),
    'offline': ('''wait 2500
frame
key o
frames 8 800
button +20 s
frames 4 800
key o
frames 4 800
''', 600, 1500),
    'stills': ('''wait 2500
still ready
button Menu
wait 800
still menu
button Display
wait 800
still display
button About
wait 800
still about
button Shot history
wait 2500
still history
button Wi-Fi list
wait 1500
still wifi
button Ready
wait 800
''', 0, 0),
    'readme': ('''wait 3000
image ready
tap 650 52
wait 1200
image profiles
tap 750 50
wait 800
button Splash
wait 600
image splash
wait 5000
key s
wait 27000
image brewing
wait 18000
drag 700 230 480 230
wait 800
image review
tap 718 52
wait 1500
button Steam, fast
wait 15000
image steam
''', 0, 0),
}
IMAGES = os.path.join(ROOT, 'docs', 'images')


def raw_to_image(path):
    return Image.frombuffer('RGB', (800, 480), open(path, 'rb').read(), 'raw', 'BGR;16', 0, 1)


def run(name):
    tmp = tempfile.mkdtemp(prefix='manual-')
    try:
        scene(name, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def scene(name, tmp):
    script, ms, hold = SCENES[name]
    lines, frames, stills, n = [], [], [], 0
    for ln in script.strip().split('\n'):
        p = ln.split()
        if p[0] == 'frame' or p[0] == 'frames':
            count, gap = (1, 0) if p[0] == 'frame' else (int(p[1]), int(p[2]))
            for _ in range(count):
                f = os.path.join(tmp, '%04d.raw' % n); n += 1
                lines.append('save ' + f); frames.append(f)
                if gap: lines.append('wait %d' % gap)
        elif p[0] in ('still', 'image'):
            f = os.path.join(tmp, '%s_%s.raw' % (p[0], p[1]))
            lines.append('save ' + f); stills.append((p[1], f, IMAGES if p[0] == 'image' else OUT))
        else:
            lines.append(ln)
    lines += ['wait 500', 'quit']
    r = subprocess.run([EMU], input='\n'.join(lines) + '\n', text=True, capture_output=True, timeout=600)
    missing = [f for f in frames + [f for _, f, _ in stills] if not os.path.exists(f)]
    if r.returncode != 0 or missing:   # a crashed emulator must not leave a shorter GIF over the good one
        sys.exit('%s: the emulator exited with %d and %d of %d frames are missing; nothing written\n%s'
                 % (name, r.returncode, len(missing), len(frames) + len(stills), r.stderr[-2000:]))
    os.makedirs(OUT, exist_ok=True)
    for nm, f, d in stills:
        raw_to_image(f).save(os.path.join(d, nm + '.png'), optimize=True)
        print('still', nm)
    imgs = [raw_to_image(f).resize((W, H), Image.LANCZOS) for f in frames]
    if imgs:
        # one palette for all frames (no flicker), built from samples across the whole scene so that colours that
        # appear only in some frames (the gold Done button, a banner) survive
        step = max(1, len(imgs) // 10)
        sample = imgs[::step][:12]
        sheet = Image.new('RGB', (W, H * len(sample)))
        for i, im in enumerate(sample): sheet.paste(im, (0, i * H))
        pal = sheet.quantize(colors=200, method=Image.Quantize.MEDIANCUT)
        # the UI's key colours always get their own entry (thin lines like the gold flow curve lose them otherwise)
        key = ['F5C04A', '7CC4FF', 'FF6B6B', 'E5484D', '4ADE80', 'FFA94D', 'F2F2F3', '8E8E96', '5E5E66', '1A1A1D', '0A0A0B',
               '000000', '7EE2A8', '10291A', 'FFC089', '33200F', '9FD0FF', '10233A', '26262A', '3A3A40', '4A4A52']
        colors = pal.getpalette()[:200 * 3]
        for h in key: colors += [int(h[i:i + 2], 16) for i in (0, 2, 4)]
        pal = Image.new('P', (1, 1)); pal.putpalette(colors + [0] * (768 - len(colors)))
        q = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in imgs]
        dur = [ms] * len(q); dur[-1] = hold
        out = os.path.join(OUT, name + '.gif')
        q[0].save(out, save_all=True, append_images=q[1:], duration=dur, loop=0, optimize=True)
        print('gif', name, len(q), 'frames', os.path.getsize(out) // 1024, 'KB')


if __name__ == '__main__':
    if not os.path.exists(EMU): sys.exit('build the emulator first: sim/build-emu.sh')
    for s in (sys.argv[1:] or list(SCENES)):
        run(s)
