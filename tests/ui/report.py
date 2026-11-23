#!/usr/bin/env python3
"""Runs the shared UI scenarios against one UI and prints its status table (docs/ui-features.md).

Called by tests/ui/run.sh after it built the emulator for the UI. Reads:
  - docs/ui-features.md: the features (ID, level, check) from its tables;
  - <ui folder>/ui-features.txt: the UI's exceptions, one per line: "<ID> omitted|partial|missing <note>";
  - tests/ui/scenarios/*.txt: emulator scripts; each names what it checks in a "# checks: X1, O5" line.
A scenario is skipped when every feature it checks is omitted or missing. Writes <out>/status.md and the logs.
Exit 1 when a scenario failed or the exceptions file breaks the rules.
"""
import argparse, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATUSES = ('omitted', 'partial', 'missing')


def read_features(path):
    feats, section = [], ''
    for line in open(path, encoding='utf-8'):
        if line.startswith('## '):
            section = line[3:].strip()
            if section.startswith('How the checks work'):
                break
            continue
        m = re.match(r'^\| ([A-Z]\d+) \| (.*?) \| (Required|Expected|Optional) \| (.*?) \|', line)
        if m:
            rest = line[m.end():]
            feats.append(dict(id=m[1], name=m[2].strip(), level=m[3], check=m[4].strip(), section=section,
                              checked=rest.rsplit('|', 1)[0] if '|' in rest else rest))
    return feats


def read_exceptions(path, known, levels):
    exc, errors = {}, []
    if not os.path.exists(path):
        return exc, errors
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 2)
        fid, st, note = parts[0], parts[1] if len(parts) > 1 else '', parts[2] if len(parts) > 2 else ''
        where = '%s:%d' % (os.path.basename(path), n)
        if fid not in known:
            errors.append('%s: no feature %s in docs/ui-features.md' % (where, fid))
        elif st not in STATUSES:
            errors.append('%s: %s: status "%s" (omitted, partial or missing)' % (where, fid, st))
        elif st == 'omitted' and levels[fid] == 'Required':
            errors.append('%s: %s is Required and cannot be omitted (mark it missing, or change its level in the list)'
                          % (where, fid))
        elif fid in exc:
            errors.append('%s: %s listed twice' % (where, fid))
        else:
            exc[fid] = (st, note)
    return exc, errors


def read_scenarios(folder, only):
    out, errors = [], []
    for f in sorted(os.listdir(folder)):
        if not f.endswith('.txt') or not f.startswith(only):
            continue
        ids = []
        for line in open(os.path.join(folder, f), encoding='utf-8'):
            m = re.match(r'^#\s*checks:\s*(.+)$', line.strip())
            if m:
                ids = [i.strip() for i in m[1].split(',') if i.strip()]
                break
        if not ids:
            errors.append('%s: no "# checks: <IDs>" line' % f)
        out.append((f[:-4], os.path.join(folder, f), ids))
    return out, errors


def run_scenario(emu, path, out, name):
    log, cmds = os.path.join(out, name + '.log'), os.path.join(out, name + '.cmds')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', EMU_CMDLOG=cmds)
    try:
        with open(path, 'rb') as fin, open(log, 'wb') as fout:
            rc = subprocess.run([emu], stdin=fin, stdout=fout, stderr=subprocess.STDOUT, env=env, timeout=300).returncode
    except subprocess.TimeoutExpired:
        rc = 'timeout'
    lines = open(log, encoding='utf-8', errors='replace').read().splitlines()
    passed = [l[5:] for l in lines if l.startswith('PASS ')]
    failed = [l[5:] for l in lines if l.startswith('FAIL ')]
    missing = [l[8:] for l in lines if l.startswith('MISSING ')]
    tags = {l[4:] for l in lines if l.startswith('TAG ')}
    ok = rc == 0 and passed and not failed
    # a tag the UI does not have ends the scenario: the feature is missing, not failed (emulator exit code 3)
    absent = rc == 3 and not failed and bool(missing)
    return dict(ok=bool(ok), absent=absent, rc=rc, passed=passed, failed=failed, missing=missing, tags=tags, log=log)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emu', required=True)
    ap.add_argument('--ui', required=True, help='the UI folder (holds ui-features.txt)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--only', default='', help='only the scenarios whose file name starts with this')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    feats = read_features(os.path.join(ROOT, 'docs', 'ui-features.md'))
    levels = {f['id']: f['level'] for f in feats}
    exc, errors = read_exceptions(os.path.join(a.ui, 'ui-features.txt'), levels.keys(), levels)
    scenarios, serr = read_scenarios(os.path.join(ROOT, 'tests', 'ui', 'scenarios'), a.only)
    errors += serr
    for name, _, ids in scenarios:
        errors += ['%s: checks %s, not in docs/ui-features.md' % (name, i) for i in ids if i not in levels]

    # run what applies
    results = {}                      # feature id -> list of (scenario, ok, missing tags)
    seen = set()                      # every tag seen in any scenario
    ran = False
    any_fail = False
    for name, path, ids in scenarios:
        if ids and all(exc.get(i, ('',))[0] in ('omitted', 'missing') for i in ids):
            print('skip  %s (%s left out)' % (name, ', '.join(ids)))
            continue
        r = run_scenario(a.emu, path, a.out, name)
        ran = True
        seen |= r['tags']
        any_fail |= not r['ok'] and not r['absent']
        if r['ok']:
            print('ok    %s (%d checks)' % (name, len(r['passed'])))
        elif r['absent']:
            print('miss  %s (no tag %s)' % (name, ', '.join(r['missing'])))
        else:
            print('FAIL  %s (exit %s, log %s)' % (name, r['rc'], os.path.relpath(r['log'], ROOT)))
            for f in r['failed']:
                print('      ' + f)
        for i in ids:
            results.setdefault(i, []).append((name, r['ok'] or r['absent'], r['missing'] if r['absent'] else []))

    # one status per feature
    rows, warnings = [], []
    for f in feats:
        st, note = exc.get(f['id'], ('', ''))
        res = results.get(f['id'])
        ftags = [t for t in re.findall(r'`([a-z_]+\.[a-z_.<>]+)`', f['checked'])] if 'Tag' in f['check'] else []
        absent = sorted({t for _, _, m in (res or []) for t in m})
        if st in ('omitted', 'missing'):
            status = st
            if any(t in seen for t in ftags):
                warnings.append('%s is marked %s, but its tag %s is there' % (f['id'], st, ' / '.join(ftags)))
        elif res and not all(ok for _, ok, _ in res):
            status = 'FAILED'
            note = (note + '; ' if note else '') + 'failed: ' + ', '.join(n for n, ok, _ in res if not ok)
        elif absent:
            status = 'partial' if st == 'partial' else 'missing'
            note = (note + '; ' if note else '') + 'no tag ' + ', '.join(absent)
        elif res:
            status = 'partial' if st == 'partial' else 'done'
        elif ftags and ran:             # a Tag check: the feature is there when its tag is (one of them, if several)
            if any(t in seen for t in ftags):
                status = 'partial' if st == 'partial' else 'done'
            else:                       # not seen: maybe built only when opened; a scenario that looks for it decides
                status = 'partial' if st == 'partial' else 'not checked yet'
                note = (note + '; ' if note else '') + 'tag %s not seen' % ' / '.join(ftags)
        elif st == 'partial':
            status = 'partial'
        elif re.search(r'Test|Tag', f['check']):
            status = 'not checked yet'
        else:
            status = 'by hand'          # Look, Panel, None: a person looks, or it is measured on the panel
        if status == 'done' and 'Look' in f['check']:
            note = (note + '; ' if note else '') + 'its look: by hand'
        rows.append(dict(f, status=status, note=note))

    ui_name = os.path.basename(os.path.normpath(a.ui))
    if ui_name == 'ui' and os.path.basename(os.path.dirname(os.path.normpath(a.ui))) != 'src':
        ui_name = os.path.basename(os.path.dirname(os.path.normpath(a.ui)))   # <repo>/ui: name it after the repo
    urgent = [r for r in rows if r['status'] == 'FAILED' or (r['level'] == 'Required' and r['status'] == 'missing')]
    counts = {}
    for r in rows:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    order = ('done', 'partial', 'FAILED', 'missing', 'omitted', 'not checked yet', 'by hand')
    summary = ', '.join('%d %s' % (counts[s], s) for s in order if s in counts)

    # terminal
    print('\nStatus of %s (%d features): %s' % (ui_name, len(rows), summary))
    if urgent:
        print('\nFirst:')
        for r in urgent:
            print('  %-4s %-8s %-15s %s%s' % (r['id'], r['level'], r['status'], r['name'],
                                           ('  (' + r['note'] + ')') if r['note'] else ''))
    section = None
    for r in rows:
        if r['section'] != section:
            section = r['section']
            print('\n' + section)
        print('  %-4s %-8s %-16s %s%s' % (r['id'], r['level'], r['status'], r['name'][:60],
                                         ('  (' + r['note'] + ')') if r['note'] else ''))

    # markdown, e.g. to replace a hand-kept FEATURES.md
    md = ['# %s: UI features' % ui_name, '',
          'From `tests/ui/run.sh` against [Brewdeck\'s feature list](https://github.com/pongells/brewdeck/blob/main/docs/ui-features.md). %s.' % summary.capitalize(), '']
    section = None
    for r in rows:
        if r['section'] != section:
            section = r['section']
            md += ([''] if md[-1] else []) + ['## ' + section, '', '| ID | Feature | Level | Status | Note |', '|---|---|---|---|---|']
        md.append('| %s | %s | %s | %s | %s |' % (r['id'], r['name'], r['level'], r['status'], r['note']))
    open(os.path.join(a.out, 'status.md'), 'w', encoding='utf-8').write('\n'.join(md) + '\n')
    print('\n%s written' % os.path.relpath(os.path.join(a.out, 'status.md'), ROOT))

    if warnings:
        print('\nWarnings:')
        for w in warnings:
            print('  ' + w)
    if errors:
        print('\nErrors:')
        for e in errors:
            print('  ' + e)
    return 1 if any_fail or errors else 0


if __name__ == '__main__':
    sys.exit(main())
