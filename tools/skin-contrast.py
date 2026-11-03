#!/usr/bin/env python3
"""WCAG contrast check for every Judas theme.

Loads the built-in default palette (parsed from src/core/skin.h) and every skins/*.toml (keys a file leaves out
fall back to the default, exactly as the C++ loader does), then checks:
  text, title, dim, accent  vs  bg, surface, surface2   >= 4.5
  ok, warn, error           vs  bg, surface             >= 3.0

Usage: skin-contrast.py [--table] [skins-dir ...]     exit 1 if anything fails.
"""
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEXT_KEYS = ["text", "title", "dim", "accent"]
TEXT_BG = ["bg", "surface", "surface2"]
SIGNAL_KEYS = ["ok", "warn", "error"]
SIGNAL_BG = ["bg", "surface"]
TEXT_MIN = 4.5
SIGNAL_MIN = 3.0


def lum(hexcol):
    h = hexcol.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    c = [v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4 for v in c]
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def ratio(a, b):
    la, lb = lum(a), lum(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


def default_colors():
    src = (ROOT / "src/core/skin.h").read_text()
    block = src[src.index("colors = {"):src.index("};", src.index("colors = {"))]
    return dict(re.findall(r'\{"(\w+)",\s*"(#[0-9a-fA-F]{6})"\}', block))


def load_all(dirs):
    base = default_colors()
    skins = [("judas (built-in)", True, dict(base))]
    for d in dirs:
        for f in sorted(Path(d).glob("*.toml")):
            t = tomllib.loads(f.read_text())
            colors = dict(base)
            colors.update(t.get("colors", {}))
            skins.append((t.get("name", f.stem), t.get("dark", True), colors))
    return skins


def check(name, colors):
    rows = []
    for keys, bgs, lo in ((TEXT_KEYS, TEXT_BG, TEXT_MIN), (SIGNAL_KEYS, SIGNAL_BG, SIGNAL_MIN)):
        for k in keys:
            for b in bgs:
                r = ratio(colors[k], colors[b])
                rows.append((k, b, r, lo, r + 1e-9 >= lo))
    return rows


def main(argv):
    table = "--table" in argv
    dirs = [a for a in argv if not a.startswith("--")] or [ROOT / "skins"]
    bad = 0
    skins = load_all(dirs)
    for name, dark, colors in skins:
        rows = check(name, colors)
        fails = [r for r in rows if not r[4]]
        worst = min(rows, key=lambda r: r[2] - r[3])
        status = "ok" if not fails else "FAIL"
        print(f"{status:4} {name:24} {'dark' if dark else 'light':5} tightest {worst[0]}/{worst[1]} {worst[2]:.2f} (min {worst[3]})")
        for k, b, r, lo, ok in fails:
            print(f"       {k} on {b}: {r:.2f} < {lo}")
        bad += len(fails)
        if table:
            for k, b, r, lo, ok in rows:
                print(f"       {k:6} on {b:8} {r:5.2f}")
    print(f"{len(skins)} themes checked, {bad} failing pairs")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
