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

## Theme protocol for framed pages

The app themes framed pages live (details in `docs/THEMES.md`).

- Each iframe `src` gets `judas_theme=<theme name>` and `judas_origin=<encodeURIComponent(app origin)>` query params.
- On every theme change, and once when the frame has loaded, the app posts
  `{type: "judas-theme", name, dark, vars, fonts_css}` to the frame with `targetOrigin` set to the frame URL's exact origin.
  `vars` maps CSS variable names (`--bg`, `--text`, `--accent`, `--radius`, `--font-ui`, ...) to values; `dark` is a boolean.
- The framed page must only accept the message when `event.origin === judas_origin` (the value it was given) and
  `event.source === window.parent`, then apply the vars:

```js
var origin = new URLSearchParams(location.search).get("judas_origin");
addEventListener("message", function (e) {
  if (e.origin !== origin || e.source !== window.parent) return;
  var m = e.data;
  if (!m || m.type !== "judas-theme" || !m.vars) return;
  for (var k in m.vars) document.documentElement.style.setProperty(k, m.vars[k]);
  document.documentElement.style.colorScheme = m.dark ? "dark" : "light";
});
```

- `vars` includes the user's font choice (Settings > Fonts): `--font-ui` and `--font-mono` carry the chosen stacks, and
  `--font-scale` / `--line-h` appear when text size or line height were changed. `fonts_css` is the absolute URL of the
  bundled `/fonts.css` (Latin OFL fonts served by Judas). A same-origin page (such as `/mach12/`) loads it with a
  `<link rel="stylesheet">` so those family names resolve; other origins cannot load the font files and fall back to
  the stack's system fonts.

```js
if (m.fonts_css && !document.getElementById("judas-fonts")) {
  var l = document.createElement("link"); l.id = "judas-fonts"; l.rel = "stylesheet"; l.href = m.fonts_css;
  document.head.appendChild(l);
}
```

Pages that ignore the message keep their own look.

## Built-in modules

A private build can compile in extra modules (CMake options) that serve their own page under the Judas server;
a Tools sub-tab can frame one with a same-origin relative URL such as `url = "/planner/"`.
