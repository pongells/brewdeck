#!/usr/bin/env python3
"""Builds a fake machine for the emulator from Gaggiuino's community profile library (the folder the machine's web UI
imports from: github.com/zer0-bit/gaggiuino, branch "community", profiles/), to test the screens with many, long
profile names. Takes the longest names (the screen keeps up to 24 profiles), named as the web UI imports them
("<name> (community)"), the longest one active; the shots and recordings come from sim/data.

Usage: python3 -I tools/community_profiles.py <out_dir> [count]
       EMU_DATA=<out_dir> sim/emu
Only reads public files from GitHub (unauthenticated API: 60 requests per hour, this uses one plus raw downloads).
"""
import glob, json, os, shutil, sys, urllib.request

INDEX = 'https://api.github.com/repos/zer0-bit/gaggiuino/contents/profiles?ref=community'


def get(url):
    req = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'brewdeck-tools'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out, count = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 24
    os.makedirs(out, exist_ok=True)
    profiles = []
    for e in json.loads(get(INDEX)):
        if not e['name'].endswith('.json'):
            continue
        try:
            p = json.loads(get(e['download_url']))
        except (ValueError, OSError) as err:
            print('skipped', e['name'], err)
            continue
        p['name'] = p['name'] + ' (community)'
        profiles.append(p)
    profiles.sort(key=lambda p: (-len(p['name']), p['name']))   # longest first, then by name (stable)
    profiles = profiles[:count]
    listing = []
    for i, p in enumerate(profiles, 1):
        with open(os.path.join(out, 'profile_%d.json' % i), 'w') as f:
            json.dump(p, f)
        listing.append({'id': i, 'name': p['name'], 'selected': 'true' if i == 1 else 'false'})
    with open(os.path.join(out, 'profiles_all.json'), 'w') as f:
        json.dump(listing, f)
    sim_data = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sim', 'data')
    for f in glob.glob(os.path.join(sim_data, '*.bin')) + glob.glob(os.path.join(sim_data, 'shot_*.json')):
        shutil.copy(f, out)
    print('%d profiles in %s (active: %s); run: EMU_DATA=%s sim/emu' % (len(listing), out, listing[0]['name'], out))


if __name__ == '__main__':
    main()
