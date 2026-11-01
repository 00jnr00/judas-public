# Tools tab

The web app has a **Tools** tab, hidden until at least one sub-tab is configured. Each sub-tab shows a page
in an iframe and, optionally, a **Plan with Judas** side panel: a compact chat that starts a normal Judas
session (chat or cowork mode) with a skill loaded.

## Config

```toml
[[tools.tabs]]
name = "planner"                 # id, [a-z0-9-]
label = "Planner"                # sub-tab text (defaults to name)
url = "http://box:8012"          # page to embed (optional)
skill = "planner"                # Judas skill the side chat loads (optional)
prompt_hint = "Plan my day"      # placeholder of the side chat (optional)
```

- `GET /api/tools/tabs` returns `{tabs: [{name, label, url, skill, prompt_hint, builtin}], mach12: {available, enabled}}`.
- The first message of a panel session asks the model to `load_skill` the named skill; later messages go to
  the same session. **New** starts another one. The panel polls `GET /api/sessions/<id>`.
- Embedding: the app's `index.html` is served with `Content-Security-Policy: frame-src 'self' <origins>`, where
  the origins are those of the configured tab URLs. Nothing else may be framed by the app. The embedded page
  must itself allow framing (no `X-Frame-Options: DENY`, no `frame-ancestors` that excludes the Judas origin).
  There is also an "Open in a new tab" link.
- Judas has no auth, so only embed pages you trust and reach over a trusted network.

## Built-in mach12 planner (optional, compile-time)

Built with `-DJUDAS_MACH12=ON` (CMake option; it defaults ON only when `src/mach12/` is present, and that module is not
included in this source tree, so public builds have it OFF and every reference compiled out).
It serves the mach12 day planner page and JSON API from inside the Judas server:

```toml
[mach12]
enabled = false                  # default off
data_dir = "~/.local/share/mach12"
```

- Page at `/mach12/`, API at `/mach12/api/...`; same files and JSON shapes as the standalone `mach12-serve`
  (notes revision check with 409, day notes, note files, vision board, block notes, energy score, archive,
  status ticking, Syncthing conflict-copy merge). It writes only `status.json` (progress), notes, day notes,
  note files, vision, block notes and the energy score, like `mach12-serve`; it never writes `events.json`.
- Statistics, accomplishment scoring (human half), sketches and pages, and handwriting transcription are ported too. Transcription runs `python3 mach12_ocr.py` code (from `$MACH12_HOME` or `~/src/mach12`) in a subprocess with a 900 s timeout, because the vision-model client lives there; override the command with `MACH12_OCR_CMD` (tests).
- **Settings > Built-in planner** switches it on or off live (no restart) and rewrites `[mach12].enabled`.
  While on, a tools tab named `mach12` embeds `/mach12/` (same origin); while off it falls back to its `url`.
- `GET/POST /api/mach12` report and set the switch (`{"enabled": true}`).
- Tests use a copy of `tests/fixtures/mach12`, never a real data directory.
