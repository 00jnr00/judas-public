/* Theme broadcast: keeps framed tools (Tools tab, Learn visual tutor) in the same theme as the app.
   Protocol in docs/THEMES.md. A framed page gets ?judas_theme=<name>&judas_origin=<this origin> in its URL and,
   on every theme change and once after it loads, a postMessage {type:"judas-theme", name, dark, vars, fonts_css} sent to
   the frame's exact origin (never "*"). Self-contained; app.js and tools.js call window.JudasTheme. */
(function () {
  "use strict";
  var current = null, cache = {}, frames = [];

  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }

  function originOf(src) {
    try { var o = new URL(src, location.href).origin; return o && o !== "null" ? o : null; } catch (e) { return null; }
  }
  function themeData(name) {
    var key = name || "current";
    if (cache[key]) return cache[key];
    cache[key] = fetch("/api/skins/" + encodeURIComponent(key)).then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    });
    cache[key].catch(function () { delete cache[key]; });
    return cache[key];
  }
  function live() {
    frames = frames.filter(function (f) { return f.el.isConnected; });
    return frames;
  }
  function post(f) {
    if (!f.origin) return;
    themeData(current).then(function (t) {
      var w = f.el.contentWindow;
      if (!w) return;
      var vars = Object.assign({}, t.vars || {}), F = window.JudasFonts;
      if (F) Object.assign(vars, F.overrides());   // the user's font choice wins over the theme's fonts
      w.postMessage({ type: "judas-theme", name: t.name, dark: !!t.dark, vars: vars,
                      fonts_css: F ? F.cssUrl() : undefined }, f.origin);
    }, function () { /* theme unknown: nothing to send */ });
  }

  window.JudasTheme = {
    name: function () { return current; },
    /* called by app.js whenever the skin changes (and once at boot) */
    set: function (name) { current = name || null; live().forEach(post); },
    /* re-send to every framed tool (the font choice changed) */
    refresh: function () { live().forEach(post); },
    /* url with the theme params added, for a frame's src */
    decorate: function (url) {
      try {
        var u = new URL(url, location.href);
        u.searchParams.set("judas_theme", current || lsGet("judas.skin") || "current");
        u.searchParams.set("judas_origin", location.origin);
        return u.toString();
      } catch (e) { return url; }
    },
    /* start following the theme for an iframe; src must already be set (use decorate) */
    track: function (fr) {
      if (!fr) return;
      var src = fr.getAttribute("src") || "";
      var rec = frames.filter(function (f) { return f.el === fr; })[0];
      if (!rec) {
        rec = { el: fr, origin: null };
        frames.push(rec);
        fr.addEventListener("load", function () { rec.origin = originOf(fr.getAttribute("src") || ""); post(rec); });
      }
      rec.origin = originOf(src);  // the load event above posts the theme once the page is there
    }
  };
  current = lsGet("judas.skin");
})();
