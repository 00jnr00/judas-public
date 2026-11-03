/* Judas web fonts: choice of UI font, mono font, text size and line height, per browser (localStorage).
   Fonts are bundled woff2 files served by Judas itself (web/fonts.css, web/fonts/), never from a CDN, so phones and
   iPads over the tailnet get them too. The choice overrides the theme's --font-ui / --font-mono (inline on <html>),
   and is added to the vars the theme broadcast sends to framed tools (theme.js, docs/THEMES.md).
   Self-contained: renders its own Settings > Fonts section into #font-settings; app.js needs no changes. */
(function () {
  "use strict";
  var SANS = ',system-ui,-apple-system,"Segoe UI",sans-serif', SERIF = ',Georgia,"Times New Roman",serif',
      MONO = ',ui-monospace,"SFMono-Regular",Menlo,Consolas,monospace';
  var SYS_UI = 'system-ui,-apple-system,"Segoe UI",Roboto,sans-serif';
  var SYS_MONO = 'ui-monospace,"SFMono-Regular",Menlo,Consolas,monospace';
  function f(id, name, tail) { return { id: id, name: name, stack: '"' + name + '"' + tail }; }
  var UI_FONTS = [
    f("inter", "Inter", SANS), f("ibm-plex-sans", "IBM Plex Sans", SANS),
    f("atkinson", "Atkinson Hyperlegible", SANS), f("lexend", "Lexend", SANS),
    f("source-sans-3", "Source Sans 3", SANS), f("nunito-sans", "Nunito Sans", SANS),
    f("space-grotesk", "Space Grotesk", SANS), f("literata", "Literata", SERIF),
    f("source-serif-4", "Source Serif 4", SERIF), f("lora", "Lora", SERIF)
  ];
  var MONO_FONTS = [
    f("jetbrains-mono", "JetBrains Mono", MONO), f("ibm-plex-mono", "IBM Plex Mono", MONO),
    f("fira-code", "Fira Code", MONO), f("source-code-pro", "Source Code Pro", MONO),
    f("commit-mono", "Commit Mono", MONO), f("space-mono", "Space Mono", MONO)
  ];
  var LH = [["compact", "Compact", 1.4], ["normal", "Normal", 1.6], ["relaxed", "Relaxed", 1.85]];
  var K = { ui: "judas.font.ui", mono: "judas.font.mono", size: "judas.font.size", lh: "judas.font.lh" };

  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { if (v == null || v === "") localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) { /* private mode */ } }
  function cleanFamily(s) { return String(s || "").replace(/[^A-Za-z0-9 ._-]/g, "").replace(/\s+/g, " ").trim().slice(0, 64); }

  /* choice values: "" theme default, "system", a catalog id, or "custom:Family Name" */
  function stackFor(kind, v) {
    if (!v) return null;
    if (v === "system") return kind === "ui" ? SYS_UI : SYS_MONO;
    if (v.indexOf("custom:") === 0) {
      var fam = cleanFamily(v.slice(7));
      return fam ? '"' + fam + '"' + (kind === "ui" ? SANS : MONO) : null;
    }
    var list = kind === "ui" ? UI_FONTS : MONO_FONTS;
    for (var i = 0; i < list.length; i++) if (list[i].id === v) return list[i].stack;
    return null;
  }
  function state() {
    var size = parseInt(lsGet(K.size), 10);
    if (!(size >= 90 && size <= 125)) size = 100;
    var lh = lsGet(K.lh), lhv = 1.6;
    LH.forEach(function (x) { if (x[0] === lh) lhv = x[2]; });
    return { ui: lsGet(K.ui) || "", mono: lsGet(K.mono) || "", size: size, lh: LH.some(function (x) { return x[0] === lh; }) ? lh : "normal", lhv: lhv };
  }
  /* the vars the choice overrides, for the theme broadcast (empty when everything is theme default) */
  function overrides() {
    var s = state(), v = {};
    var u = stackFor("ui", s.ui), m = stackFor("mono", s.mono);
    if (u) v["--font-ui"] = u;
    if (m) v["--font-mono"] = m;
    if (s.size !== 100) v["--font-scale"] = String(s.size / 100);
    if (s.lh !== "normal") v["--line-h"] = String(s.lhv);
    return v;
  }

  var styleEl = null;
  function ensureStyle() {
    if (styleEl) return;
    styleEl = document.createElement("style");
    /* html font-size scales every rem; body gets the line height. Defaults equal the existing look. */
    styleEl.textContent = "html{font-size:calc(100% * var(--font-scale,1))}body{font-size:1rem;line-height:var(--line-h,1.6)}";
    document.head.appendChild(styleEl);
  }
  function apply() {
    ensureStyle();
    var root = document.documentElement.style, o = overrides();
    ["--font-ui", "--font-mono", "--font-scale", "--line-h"].forEach(function (k) {
      if (o[k]) root.setProperty(k, o[k]); else root.removeProperty(k);
    });
  }
  function changed() {
    apply();
    renderPreview();
    if (window.JudasTheme && window.JudasTheme.refresh) window.JudasTheme.refresh();
  }

  window.JudasFonts = {
    ui: UI_FONTS, mono: MONO_FONTS, overrides: overrides, apply: apply,
    cssUrl: function () { return location.origin + "/fonts.css"; },
    state: state,
    stackFor: stackFor
  };

  /* ---------- Settings > Fonts ---------- */
  var box = null, prevBox = null;
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }

  function fontList(kind) {
    var cur = lsGet(K[kind]) || "";
    var wrap = el("div", "fnt-list");
    wrap.setAttribute("role", "radiogroup");
    wrap.setAttribute("aria-label", kind === "ui" ? "UI font" : "Mono font");
    function opt(value, label, style, note) {
      var b = el("button", "fnt-opt" + (value === cur ? " active" : ""));
      b.type = "button";
      b.setAttribute("role", "radio");
      b.setAttribute("aria-checked", value === cur ? "true" : "false");
      b.dataset.value = value;
      var n = el("span", "fnt-name", label);
      if (style) n.style.fontFamily = style;
      b.appendChild(n);
      if (note) b.appendChild(el("span", "fnt-note", note));
      b.addEventListener("click", function () { lsSet(K[kind], value); changed(); renderList(kind); });
      wrap.appendChild(b);
    }
    opt("", "Theme default", null, "what the theme sets");
    opt("system", "System default", kind === "ui" ? SYS_UI : SYS_MONO, "this device");
    (kind === "ui" ? UI_FONTS : MONO_FONTS).forEach(function (x) { opt(x.id, x.name, x.stack); });
    var isCustom = cur.indexOf("custom:") === 0;
    if (isCustom) opt(cur, cleanFamily(cur.slice(7)), stackFor(kind, cur), "installed on this device");
    var row = el("div", "fnt-custom");
    var inp = el("input"); inp.type = "text"; inp.placeholder = "Use a font installed on this device (family name)";
    inp.setAttribute("aria-label", "Installed font family"); inp.maxLength = 64; inp.autocomplete = "off"; inp.spellcheck = false;
    if (isCustom) inp.value = cleanFamily(cur.slice(7));
    var go = el("button", "btn small", "Use"); go.type = "button";
    function useIt() {
      var fam = cleanFamily(inp.value);
      lsSet(K[kind], fam ? "custom:" + fam : ""); changed(); renderList(kind);
    }
    go.addEventListener("click", useIt);
    inp.addEventListener("keydown", function (e) { if (e.key === "Enter") useIt(); });
    row.appendChild(inp); row.appendChild(go);
    var holder = el("div");
    holder.appendChild(wrap); holder.appendChild(row);
    return holder;
  }
  var lists = {};
  function renderList(kind) {
    var old = lists[kind];
    var scroll = old ? old.querySelector(".fnt-list").scrollTop : 0;
    var neu = fontList(kind);
    if (old && old.parentNode) old.parentNode.replaceChild(neu, old);
    lists[kind] = neu;
    var l = neu.querySelector(".fnt-list"); if (l) l.scrollTop = scroll;
    return neu;
  }
  function renderPreview() {
    if (!prevBox) return;
    prevBox.textContent = "";
    prevBox.appendChild(el("p", "fnt-prev-ui", "Judas runs one model and many agents. The quick brown fox jumps over the lazy dog, 0123456789."));
    prevBox.appendChild(el("pre", "fnt-prev-mono", "const answer = items.filter(x => x.ok)\n  .map(x => x.id); // 0O 1lI {} => !=="));
  }
  function build() {
    box = document.getElementById("font-settings");
    if (!box) return;
    box.textContent = "";
    var s = state();
    var cols = el("div", "fnt-cols");
    [["ui", "UI font"], ["mono", "Mono font"]].forEach(function (p) {
      var c = el("div", "fnt-col");
      c.appendChild(el("b", "fnt-h", p[1]));
      var l = fontList(p[0]); lists[p[0]] = l; c.appendChild(l);
      cols.appendChild(c);
    });
    box.appendChild(cols);

    var sz = el("div", "fnt-row");
    sz.appendChild(el("b", "fnt-h", "Text size"));
    var rng = el("input"); rng.type = "range"; rng.min = "90"; rng.max = "125"; rng.step = "5"; rng.value = String(s.size);
    rng.setAttribute("aria-label", "Text size percent");
    var val = el("span", "fnt-val", s.size + "%");
    rng.addEventListener("input", function () { val.textContent = rng.value + "%"; lsSet(K.size, rng.value === "100" ? "" : rng.value); changed(); });
    sz.appendChild(rng); sz.appendChild(val);
    box.appendChild(sz);

    var lr = el("div", "fnt-row");
    lr.appendChild(el("b", "fnt-h", "Line height"));
    var seg = el("div", "fnt-seg");
    LH.forEach(function (x) {
      var b = el("button", "btn small" + (x[0] === s.lh ? " active" : ""), x[1]); b.type = "button";
      b.setAttribute("aria-pressed", x[0] === s.lh ? "true" : "false");
      b.addEventListener("click", function () {
        lsSet(K.lh, x[0] === "normal" ? "" : x[0]); changed();
        Array.prototype.forEach.call(seg.children, function (c) { c.classList.remove("active"); c.setAttribute("aria-pressed", "false"); });
        b.classList.add("active"); b.setAttribute("aria-pressed", "true");
      });
      seg.appendChild(b);
    });
    lr.appendChild(seg);
    box.appendChild(lr);

    prevBox = el("div", "fnt-prev");
    prevBox.setAttribute("aria-live", "polite");
    box.appendChild(prevBox);
    renderPreview();
    var rs = el("button", "btn small", "Reset fonts"); rs.type = "button";
    rs.addEventListener("click", function () { Object.keys(K).forEach(function (k) { lsSet(K[k], ""); }); changed(); build(); });
    box.appendChild(rs);
  }

  var css = ".fnt-cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,260px),1fr));gap:var(--s4,16px);max-width:var(--col,720px)}" +
    ".fnt-h{display:block;margin:0 0 .4em;color:var(--dim)}" +
    ".fnt-list{max-height:320px;overflow:auto;border:1px solid var(--border);border-radius:var(--radius,8px);background:var(--surface)}" +
    ".fnt-opt{display:flex;width:100%;justify-content:space-between;align-items:baseline;gap:.8em;background:none;border:0;border-left:4px solid transparent;padding:.5em .8em;text-align:left;font-size:1.1rem}" +
    ".fnt-opt:hover{background:var(--surface2)}.fnt-opt.active{border-left-color:var(--accent);background:var(--accent-soft,var(--surface2))}" +
    ".fnt-note{color:var(--dim);font:.75rem var(--font-ui,system-ui)}" +
    ".fnt-custom{display:flex;gap:var(--s2,8px);margin-top:var(--s2,8px)}.fnt-custom input{flex:1;min-width:0}" +
    ".fnt-row{display:flex;align-items:center;gap:var(--s3,12px);margin:var(--s3,12px) 0;flex-wrap:wrap}.fnt-row .fnt-h{margin:0;min-width:7em}" +
    ".fnt-row input[type=range]{width:min(100%,260px);padding:0}.fnt-seg{display:flex;gap:var(--s2,8px)}" +
    ".fnt-seg .btn.active{background:var(--accent);border-color:var(--accent);color:var(--bg)}" +
    ".fnt-prev{margin:var(--s3,12px) 0;padding:var(--s3,12px);border:1px dashed var(--border);border-radius:var(--radius,8px);max-width:var(--col,720px)}" +
    ".fnt-prev p{margin:0 0 .6em}.fnt-prev pre{margin:0;white-space:pre-wrap}";

  function boot() {
    var st = document.createElement("style"); st.textContent = css; document.head.appendChild(st);
    apply();
    build();
  }
  apply();   // as early as possible: no flash of the theme font on reload
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
})();
