#!/usr/bin/env python3
"""Make a clean copy of the project for publishing, and refuse to call it clean unless the privacy scan passes.

    python3 tools/export_public.py [target-folder]      # default: ../ESP32-ADSB-Radar-public (replaced if it exists)

What is left out: everything .gitignore names (secrets, the local PlatformIO config, flash dumps, build output, generated ESP-IDF files),
tools/pii_denylist.txt, vendor files whose licence is unknown (reference/viewe-board), macOS/editor litter.
Afterwards tools/privacy_scan.py runs on the copy. No git repository is created: do `git init` in the copy yourself (so that the author name and
e-mail of the commits are the ones you choose).
"""
import fnmatch, os, shutil, subprocess, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
EXTRA_EXCLUDE = ["reference/viewe-board", "tools/pii_denylist.txt", "docs/screenshots/_raw", "**/.DS_Store", "**/__pycache__"]


def patterns():
    out = []
    for l in open(os.path.join(ROOT, ".gitignore")):
        l = l.strip()
        if l and not l.startswith("#"):
            out.append(l.rstrip("/"))
    return out + EXTRA_EXCLUDE


def excluded(rel, pats):
    parts = rel.split("/")
    for p in pats:
        if "/" in p.strip("/"):   # anchored path pattern
            q = p.lstrip("/")
            if fnmatch.fnmatch(rel, q) or rel == q or rel.startswith(q + "/"):
                return True
        else:                     # a name that may appear at any depth
            if any(fnmatch.fnmatch(part, p) for part in parts):
                return True
    return False


def main():
    target = os.path.normpath(sys.argv[1]) if len(sys.argv) > 1 else os.path.normpath(os.path.join(ROOT, "..", "ESP32-ADSB-Radar-public"))
    if os.path.commonpath([target, ROOT]) == ROOT:
        sys.exit("the target must be outside the project folder")
    if os.path.exists(target):
        shutil.rmtree(target)
    pats = patterns()
    n = 0
    for d, dirs, files in os.walk(ROOT):
        rel_d = os.path.relpath(d, ROOT).replace(os.sep, "/")
        dirs[:] = [x for x in dirs if not excluded(("" if rel_d == "." else rel_d + "/") + x, pats)]
        for f in files:
            rel = ("" if rel_d == "." else rel_d + "/") + f
            if excluded(rel, pats):
                continue
            dst = os.path.join(target, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(d, f), dst)
            n += 1
    print(f"{n} files copied to {target}")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "privacy_scan.py"), target])
    if r.returncode:
        print("\nNOT CLEAN: fix the findings above (in the project, then export again).")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
