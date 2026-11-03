#!/usr/bin/env python3
"""Write every built-in theme as one JSON file for framed tools and other apps that share the Judas themes.

Shape: {"themes": [{"name", "label", "dark", "vars": {"--bg": "#..", ..., "--radius": "8px", ...}}]}, the same
vars GET /api/skins/<name> returns. Defaults come from src/core/skin.h, themes from skins/*.toml.

    python3 tools/export-themes.py OUT.json
"""
import json
import os
import re
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def defaults():
    h = open(os.path.join(ROOT, "src", "core", "skin.h")).read()
    pairs = dict(re.findall(r'\{"([a-z_0-9]+)",\s*"((?:[^"\\]|\\.)*)"\}', h))
    pairs = {k: v.replace('\\"', '"') for k, v in pairs.items()}
    tok_keys = ("radius", "radius_sm", "font_ui", "font_mono", "space")
    colors = {k: v for k, v in pairs.items() if v.startswith("#")}
    tokens = {k: pairs[k] for k in tok_keys if k in pairs}
    return colors, tokens


def to_vars(colors, tokens):
    v = {"--" + k: c for k, c in colors.items()}
    v.update({"--" + k.replace("_", "-"): t for k, t in tokens.items()})
    acc = colors.get("accent", "")
    if re.fullmatch(r"#[0-9a-fA-F]{6}", acc):
        v["--accent-soft"] = "rgba(%d,%d,%d,0.16)" % tuple(int(acc[i:i + 2], 16) for i in (1, 3, 5))
    return v


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    colors, tokens = defaults()
    themes = [{"name": "judas", "label": "Judas", "dark": True, "vars": to_vars(colors, tokens)}]
    skins_dir = os.path.join(ROOT, "skins")
    for f in sorted(os.listdir(skins_dir)):
        if not f.endswith(".toml"):
            continue
        t = tomllib.load(open(os.path.join(skins_dir, f), "rb"))
        name = t.get("name", f[:-5])
        themes.append({"name": name, "label": t.get("label", name), "dark": bool(t.get("dark", True)),
                       "vars": to_vars({**colors, **t.get("colors", {})}, {**tokens, **t.get("tokens", {})})})
    with open(sys.argv[1], "w") as out:
        json.dump({"themes": themes}, out, indent=1)
        out.write("\n")
    print("%d themes -> %s" % (len(themes), sys.argv[1]))


if __name__ == "__main__":
    main()
