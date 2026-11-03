# Themes

One theme system serves the web app, framed tools and the TUI. A theme is a small TOML file: colors plus a few
design tokens. The web app receives them as CSS variables.

## Where themes live

- Built in (shipped in the binary): the `judas` default (defined in `src/core/skin.h`) and every `skins/*.toml` of the
  repo. They are embedded at build time by `cmake/embed_skins.cmake`, so they exist without any config dir.
- User themes: `~/.config/judas/skins/*.toml`. A user file with the same `name` replaces the built-in one.
- Built-in families, each with a dark and a light variant: `ember-plum`, `graphite`, `glacier`, `phosphor`, `cinder`,
  `tide` (for example `glacier-dark`, `glacier-light`). Older themes: `iscariot`, `work`, `parchment`.

## File format

```toml
name = "glacier-dark"        # id, defaults to the file name
label = "Glacier Dark"       # shown in Settings, defaults to name
dark = true                  # false sets color-scheme: light

[tokens]                     # optional; any subset
radius = '10px'
radius_sm = '5px'

[colors]                     # any subset; missing keys come from the default judas palette
bg = "#070B12"
```

Use single quotes for token values that contain double quotes (font stacks).

## Tokens

| Key | CSS variable | Default |
| --- | --- | --- |
| `radius` | `--radius` | `8px` |
| `radius_sm` | `--radius-sm` | `4px` |
| `font_ui` | `--font-ui` | `system-ui,-apple-system,"Segoe UI",sans-serif` |
| `font_mono` | `--font-mono` | `ui-monospace,"SFMono-Regular",Menlo,Consolas,monospace` |
| `space` | `--space` | `8px` (base unit; `--s1`..`--s6` are 0.5x to 4x of it) |

Colors, each emitted as `--<key>`: `bg surface surface2 border title accent text dim ok warn error thinking added removed`.
`--accent-soft` (accent at 16% alpha) is derived.

## Endpoints

- `GET /skin.css?name=<name>`: `:root{...}` with every variable above. No name means the server's active skin.
- `GET /api/skins`: every theme as `{name,label,dark,colors,tokens}`.
- `GET /api/skins/<name>`: `{name,label,dark,vars:{"--bg":"#..",...}}`; `<name>` may be `current` (the server's active skin).
- `POST /api/learn/visual/theme {name}`: maps a theme onto the Learn visual tutor (see below).

## Adding a theme

1. Create `skins/<name>.toml` (names are short material, color or mood words; nothing personal).
2. Ship a dark and a light variant when it is a family.
3. Run `python3 tools/skin-contrast.py` and fix any failing pair. `ctest` runs it as `skin_contrast`.
4. Rebuild; the file is embedded automatically (CMake globs `skins/`).
5. Preview: `python3 tools/swarm-ui-shot.py --repo-skins --skin <name> --tabs map --sizes 1440x1000`.

## Contrast tool

`tools/skin-contrast.py [--table] [dir ...]` (default `skins/`) loads the built-in default and every theme (missing
keys fall back to the default) and checks WCAG 2 ratios:

- `text`, `title`, `dim`, `accent` against `bg`, `surface`, `surface2`: at least 4.5
- `ok`, `warn`, `error` against `bg`, `surface`: at least 3.0

Exit status 1 on any failure. Note: a user file in `~/.config/judas/skins` overrides a built-in of the same name and is
not checked.

## Framed tools follow the theme

Details for tool authors are in `docs/TOOLS-TABS.md`. Summary: the web app adds
`?judas_theme=<name>&judas_origin=<encoded app origin>` to each framed URL (Tools tabs and the Learn tutor) and posts

```js
{ type: "judas-theme", name: "glacier-dark", dark: true, vars: { "--bg": "#070B12", "--radius": "10px", ... } }
```

to the frame's `contentWindow` on every theme change and once after the frame loads. `targetOrigin` is the frame URL's
exact origin, never `*`. The receiver must check `event.origin === judas_origin` and `event.source === window.parent`
before applying `vars` to `document.documentElement.style`.

## Learn visual tutor

The tutor (LearnHarness, run unmodified) has no postMessage hook, but it has an appearance setting
(`PUT /api/settings/appearance`, replace-whole, with `mode` dark or light and `background` as a hex). When Learn opens
its tutor frame, and on every theme change, Judas reads the tutor's current appearance, sets only `mode` (from `dark`)
and `background` (from `bg`), writes it back and reloads the frame. Its other appearance fields are kept. If the tutor
is unreachable the mapping is skipped silently.
