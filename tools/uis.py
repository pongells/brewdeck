#!/usr/bin/env python3
"""The list of UIs in their own repositories (uis.json), for .github/workflows/uis.yml and by hand.

  tools/uis.py validate              check uis.json (fields, an exact commit, unique names)
  tools/uis.py matrix                the names as a JSON list (the workflow's matrix)
  tools/uis.py fetch <name> <dir>    that UI's repository at its pinned commit into <dir>; prints the UI folder's path
  tools/uis.py catalog <version> <chip> <images dir> <key.pem> <out dir>
                                     the release's UI catalogue: <images dir>/screen.bin (Brewdeck) and <name>.bin per
                                     listed UI (missing ones are left out) -> <out dir>/brewdeck-<version>[-<name>]-
                                     <chip>.bin and catalog-<chip>.txt, signed like update-<chip>.txt (sign-update.sh)
  tools/uis.py preview <out> <screen.raw | .png> [<screen> <screen>]
                                     800 x 480 screenshots (the emulator's "save", RGB565, or PNGs: Ready, brewing,
                                     steam) as the UI's preview: each shrunk to 320 x 192 RGB565, one after another;
                                     catalog picks up <images dir>/<name>.preview
  tools/uis.py check <name> <status.md>
                                     the listing bar on a status table from tests/ui/run.sh: S1 (says it is
                                     unofficial) and X1 (no command without a user action) must be "done"; the whole
                                     table goes to the run summary ($GITHUB_STEP_SUMMARY) when set

Only UIs that pass are listed; everything else in the table is reported, not required (docs/custom-ui.md).
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REQUIRED = ("S1", "X1")
PREVIEW_W, PREVIEW_H = 320, 192   # firmware/src/core/update_parse.h REL_PREVIEW_W / _H
PREVIEW_FRAMES = 3                # REL_PREVIEW_FRAMES: Ready, brewing, steam
FRAME = PREVIEW_W * PREVIEW_H * 2
FIELDS = ("name", "title", "repo", "commit", "folder", "license", "description")
# Brewdeck's own UI, always first in a catalogue (its BD_UI name is "screen")
BREWDECK = {"name": "screen", "title": "Brewdeck", "description": "This screen's own interface: every screen, shot review, history and settings."}
# what the screen's catalogue parser takes (firmware/src/core/update_parse.h): no { } [ ] " \ in text, limited lengths
TEXT_OK = re.compile(r'[^{}\[\]"\\]*')


def load():
    with open(os.path.join(ROOT, "uis.json")) as f:
        return json.load(f)["uis"]


def validate(uis):
    errors, names = [], set()
    for i, u in enumerate(uis):
        where = u.get("name") or "entry %d" % i
        for k in FIELDS:
            if not isinstance(u.get(k), str) or not u[k].strip():
                errors.append("%s: missing %s" % (where, k))
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,14}", u.get("name", "")):
            errors.append("%s: name must be lower case letters, digits and '-', at most 15 (the BD_UI name)" % where)
        if not re.fullmatch(r"[0-9a-f]{40}", u.get("commit", "")):
            errors.append("%s: commit must be a full 40-character SHA (no branch or tag)" % where)
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", u.get("repo", "")):
            errors.append("%s: repo must be owner/name on GitHub" % where)
        f = u.get("folder", "")
        if f.startswith("/") or ".." in f.split("/"):
            errors.append("%s: folder must be a path inside the repository" % where)
        for k, cap in (("title", 23), ("description", 95)):
            v = u.get(k, "")
            if len(v) > cap or not TEXT_OK.fullmatch(v) or not v.isprintable():
                errors.append("%s: %s must be at most %d printable characters without { } [ ] \" \\" % (where, k, cap))
        if u.get("name") in names or u.get("name") == BREWDECK["name"]:
            errors.append("%s: name listed twice" % where)
        names.add(u.get("name"))
    return errors


def find(uis, name):
    for u in uis:
        if u["name"] == name:
            return u
    sys.exit("no UI named %s in uis.json" % name)


def fetch(u, dest):
    # only the pinned commit, never a branch: what was reviewed is what is built
    os.makedirs(dest, exist_ok=True)
    git = lambda *a: subprocess.run(["git", "-C", dest] + list(a), check=True, stdout=subprocess.DEVNULL)
    git("init", "-q")
    git("fetch", "-q", "--depth", "1", "https://github.com/%s.git" % u["repo"], u["commit"])
    git("checkout", "-q", "--detach", "FETCH_HEAD")
    head = subprocess.run(["git", "-C", dest, "rev-parse", "HEAD"], check=True, capture_output=True, text=True)
    if head.stdout.strip() != u["commit"]:
        sys.exit("%s: fetched %s, expected %s" % (u["name"], head.stdout.strip(), u["commit"]))
    path = os.path.join(dest, u["folder"])
    if not os.path.isdir(path):
        sys.exit("%s: no folder %s at that commit" % (u["name"], u["folder"]))
    print(path)


def catalog(uis, version, chip, images, key, out):
    import hashlib
    import base64
    import shutil
    import tempfile
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", version) or not re.fullmatch(r"esp32(s3|p4)", chip):
        sys.exit("catalog: version vX.Y.Z and chip esp32s3 / esp32p4 expected")
    os.makedirs(out, exist_ok=True)
    entries = []
    for u in [BREWDECK] + uis:
        src = os.path.join(images, u["name"] + ".bin")
        if not os.path.isfile(src):
            print("catalog: %s not built, left out" % u["name"])
            continue
        fn = "brewdeck-%s-%s.bin" % (version, chip) if u is BREWDECK else "brewdeck-%s-%s-%s.bin" % (version, u["name"], chip)
        shutil.copyfile(src, os.path.join(out, fn))
        data = open(src, "rb").read()
        e = {"name": u["name"], "title": u["title"], "file": fn, "size": len(data),
             "sha256": hashlib.sha256(data).hexdigest(), "about": u["description"]}
        pv = os.path.join(images, u["name"] + ".preview")
        if os.path.isfile(pv):
            pdata = open(pv, "rb").read()
            if not pdata or len(pdata) % FRAME or len(pdata) // FRAME > PREVIEW_FRAMES:
                sys.exit("catalog: %s is not 1 to %d %d x %d RGB565 pictures" % (pv, PREVIEW_FRAMES, PREVIEW_W, PREVIEW_H))
            pfn = "preview-%s-%s.rgb565" % (version, u["name"])
            shutil.copyfile(pv, os.path.join(out, pfn))
            e.update({"preview": pfn, "psize": len(pdata), "psha256": hashlib.sha256(pdata).hexdigest()})
        else:
            print("catalog: no preview for %s" % u["name"])
        entries.append(e)
    if not entries or entries[0]["name"] != BREWDECK["name"]:
        sys.exit("catalog: Brewdeck's own image (screen.bin) is missing")
    line = json.dumps({"version": version, "chip": chip, "uis": entries}, separators=(",", ":"), ensure_ascii=True)
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key], input=line.encode(), check=True,
                         capture_output=True).stdout
    with tempfile.TemporaryDirectory() as t:   # check it with the public half before anything is published
        pub, sf = os.path.join(t, "pub.pem"), os.path.join(t, "sig")
        subprocess.run(["openssl", "ec", "-in", key, "-pubout", "-out", pub], check=True, capture_output=True)
        open(sf, "wb").write(sig)
        subprocess.run(["openssl", "dgst", "-sha256", "-verify", pub, "-signature", sf], input=line.encode(),
                       check=True, capture_output=True)
    path = os.path.join(out, "catalog-%s.txt" % chip)
    with open(path, "w") as f:
        f.write(line + "\n" + base64.b64encode(sig).decode() + "\n")
    print("Signed: %s (%s)" % (path, ", ".join(e["name"] for e in entries)))


def preview(out, srcs):
    from PIL import Image
    if not 1 <= len(srcs) <= PREVIEW_FRAMES:
        sys.exit("preview: 1 to %d screens" % PREVIEW_FRAMES)
    o = bytearray()
    for src in srcs:
        if src.endswith(".png"):
            im = Image.open(src).convert("RGB")
        else:   # the emulator's save: 800 x 480 RGB565 little-endian
            raw = open(src, "rb").read()
            if len(raw) != 800 * 480 * 2:
                sys.exit("preview: %s is not an 800 x 480 RGB565 screen" % src)
            px = bytearray(800 * 480 * 3)
            for i in range(800 * 480):
                v = raw[2 * i] | raw[2 * i + 1] << 8
                r, g, b = v >> 11, (v >> 5) & 63, v & 31
                px[3 * i:3 * i + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            im = Image.frombytes("RGB", (800, 480), bytes(px))
        rgb = im.resize((PREVIEW_W, PREVIEW_H), Image.LANCZOS).tobytes()
        for i in range(0, len(rgb), 3):
            v = (rgb[i] >> 3) << 11 | (rgb[i + 1] >> 2) << 5 | rgb[i + 2] >> 3
            o += bytes((v & 255, v >> 8))
    open(out, "wb").write(o)
    print("preview: %s (%d x %d x %d RGB565)" % (out, len(srcs), PREVIEW_W, PREVIEW_H))


def check(u, status_md):
    text = open(status_md).read()
    status = {}
    for line in text.splitlines():
        m = re.match(r"\|\s*([A-Z][0-9]+)\s*\|[^|]*\|[^|]*\|\s*([^|]*?)\s*\|", line)
        if m:
            status[m.group(1)] = m.group(2)
    bad = ["%s %s" % (k, status.get(k, "not in the table")) for k in REQUIRED if status.get(k) != "done"]
    head = "## %s (%s @ %s)\n\n" % (u["title"], u["repo"], u["commit"][:7])
    verdict = ("**Listed: no** (needs %s done; %s)\n\n" % (", ".join(REQUIRED), "; ".join(bad)) if bad
               else "**Listed: yes** (%s done; the rest is reported, not required)\n\n" % ", ".join(REQUIRED))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(head + verdict + text + "\n")
    print(verdict.strip())
    return 1 if bad else 0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    uis = load()
    errors = validate(uis)
    if errors:
        print("uis.json:\n  " + "\n  ".join(errors))
        return 1
    cmd = sys.argv[1]
    if cmd == "validate":
        print("uis.json: %d UI(s), ok" % len(uis))
    elif cmd == "matrix":
        print(json.dumps([u["name"] for u in uis]))
    elif cmd == "fetch" and len(sys.argv) == 4:
        fetch(find(uis, sys.argv[2]), sys.argv[3])
    elif cmd == "catalog" and len(sys.argv) == 7:
        catalog(uis, *sys.argv[2:7])
    elif cmd == "preview" and 4 <= len(sys.argv) <= 3 + PREVIEW_FRAMES:
        preview(sys.argv[2], sys.argv[3:])
    elif cmd == "check" and len(sys.argv) == 4:
        return check(find(uis, sys.argv[2]), sys.argv[3])
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
