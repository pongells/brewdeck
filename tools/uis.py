#!/usr/bin/env python3
"""The list of UIs in their own repositories (uis.json), for .github/workflows/uis.yml and by hand.

  tools/uis.py validate              check uis.json (fields, an exact commit, unique names)
  tools/uis.py matrix                the names as a JSON list (the workflow's matrix)
  tools/uis.py fetch <name> <dir>    that UI's repository at its pinned commit into <dir>; prints the UI folder's path
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
FIELDS = ("name", "title", "repo", "commit", "folder", "license", "description")


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
        if u.get("name") in names:
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
    elif cmd == "check" and len(sys.argv) == 4:
        return check(find(uis, sys.argv[2]), sys.argv[3])
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
