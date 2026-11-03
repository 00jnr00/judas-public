/* Judas phone UI. A separate page from the desktop app: same JSON/SSE API (docs/API.md), no app.js.
   Routes (location.hash): "" or "#chat|cowork|code|learn|swarm" = home, "#s/<id>" = session, "#planner". */
(function () {
  "use strict";
  var MODES = [["chat", "Chat"], ["cowork", "Cowork"], ["code", "Code"], ["learn", "Learn"], ["swarm", "Swarm"]];
  var HINTS = {
    chat: "Ask anything. Chat needs no project folder.",
    cowork: "Describe a bigger job. The lead plans it and may start workers.",
    code: "Pick a project folder, then say what to read or change. Every edit is snapshotted.",
    learn: "Say what you want to understand. Judas asks a few questions before teaching.",
    swarm: "Pick a working folder and describe the job. The lead builds teams and runs them."
  };
  var PLACEHOLDER = { chat: "Message Judas", cowork: "Describe the job", code: "What should Judas change?", learn: "What do you want to understand?", swarm: "Describe the job for the lead" };
  var NEEDS_CWD = { code: true, swarm: true }, WANTS_CWD = { cowork: true };

  var $ = function (s) { return document.querySelector(s); };
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { if (v == null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) { /* ignore */ } }
  function reduceMotion() { return window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches; }

  var ICON = {
    menu: '<svg viewBox="0 0 24 24"><circle cx="12" cy="5" r="1.2"/><circle cx="12" cy="12" r="1.2"/><circle cx="12" cy="19" r="1.2"/></svg>',
    back: '<svg viewBox="0 0 24 24"><path d="M15 5l-7 7 7 7"/></svg>',
    clip: '<svg viewBox="0 0 24 24"><path d="M21 11l-8.5 8.5a5 5 0 01-7-7L14 4a3.3 3.3 0 014.7 4.7l-8.4 8.4a1.7 1.7 0 01-2.4-2.4L15 7"/></svg>',
    folder: '<svg viewBox="0 0 24 24"><path d="M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z"/></svg>',
    up: '<svg viewBox="0 0 24 24"><path d="M12 19V5M5 12l7-7 7 7"/></svg>',
    stop: '<svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2" fill="currentColor"/></svg>',
    down: '<svg viewBox="0 0 24 24"><path d="M12 5v14M5 12l7 7 7-7"/></svg>',
    edit: '<svg viewBox="0 0 24 24"><path d="M4 20h4L19 9l-4-4L4 16z"/></svg>',
    trash: '<svg viewBox="0 0 24 24"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg>',
    cal: '<svg viewBox="0 0 24 24"><rect x="4" y="5" width="16" height="15" rx="2"/><path d="M4 10h16M9 3v4M15 3v4"/></svg>',
    desk: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/></svg>',
    gear: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 3v3M12 18v3M3 12h3M18 12h3M5.6 5.6l2.1 2.1M16.3 16.3l2.1 2.1M18.4 5.6l-2.1 2.1M7.7 16.3l-2.1 2.1"/></svg>',
    refresh: '<svg viewBox="0 0 24 24"><path d="M20 11a8 8 0 10-2.3 5.7M20 4v7h-7"/></svg>'
  };
  function icon(name) { var s = el("span"); s.innerHTML = ICON[name]; return s.firstChild; }

  /* ---------- state ---------- */
  var S = {
    mode: "chat", view: "home", sid: null, sess: null, agents: [], team: null, more: false, firstId: null,
    running: false, health: "unknown", state: null, queue: [], atts: [], cwd: lsGet("judas.m.cwd") || "",
    approvals: null /* null = unsupported */, apprWanted: true, live: null, sessions: [], showN: 12, preview: {}
  };
  var sheetOpen = false, netFails = 0, lastRoute = "";

  /* ---------- api ---------- */
  function NetErr(msg) { var e = new Error(msg || "network error"); e.net = true; return e; }
  function api(method, url, body, opts) {
    var init = { method: method, headers: {} };
    if (body !== undefined && !(body instanceof Blob)) { init.headers["Content-Type"] = "application/json"; init.body = JSON.stringify(body); }
    else if (body instanceof Blob) { init.body = body; init.headers["Content-Type"] = body.type || "application/octet-stream"; }
    var ctl = null, to = null;
    if (window.AbortController) { ctl = new AbortController(); init.signal = ctl.signal; to = setTimeout(function () { ctl.abort(); }, (opts && opts.timeout) || 20000); }
    return fetch(url, init).then(function (r) {
      clearTimeout(to);
      return r.text().then(function (t) {
        var j = null; try { j = t ? JSON.parse(t) : null; } catch (e) { /* not json */ }
        if (!r.ok) {
          var er = new Error((j && ((j.error && j.error.message) || j.error)) || ("HTTP " + r.status));
          er.status = r.status; throw er;
        }
        netFails = 0;
        return j;
      });
    }, function () { clearTimeout(to); throw NetErr(); });
  }
  var get = function (u, o) { return api("GET", u, undefined, o); };
  var post = function (u, b) { return api("POST", u, b === undefined ? {} : b); };

  /* ---------- toast + sheet ---------- */
  var toastT = null;
  function toast(msg, err) {
    var t = $("#toast"); t.textContent = msg; t.className = "toast" + (err ? " err" : ""); t.hidden = false;
    clearTimeout(toastT); toastT = setTimeout(function () { t.hidden = true; }, 3500);
  }
  var sheetFocus = null;
  function openSheet(build) {
    var sh = $("#sheet"); sh.textContent = "";
    sh.appendChild(el("div", "grab"));
    build(sh);
    $("#sheet-bg").hidden = false; sh.hidden = false;
    if (!sheetOpen) { sheetOpen = true; sheetFocus = document.activeElement; history.pushState({ sheet: 1 }, ""); }
    var f = sh.querySelector("input,textarea,.s-item,button"); if (f && f.tagName !== "BUTTON") { try { f.focus(); } catch (e) { /* ignore */ } }
  }
  function hideSheet() {
    $("#sheet").hidden = true; $("#sheet-bg").hidden = true; sheetOpen = false;
    if (sheetFocus && sheetFocus.focus) { try { sheetFocus.focus(); } catch (e) { /* ignore */ } }
  }
  function closeSheet() { if (sheetOpen) history.back(); }
  $("#sheet-bg").addEventListener("click", closeSheet);
  document.addEventListener("keydown", function (e) { if (e.key === "Escape" && sheetOpen) closeSheet(); });
  function sItem(sh, ic, label, fn, cls, small) {
    var b = el("button", "s-item" + (cls ? " " + cls : "")); b.type = "button";
    if (ic) b.appendChild(icon(ic));
    b.appendChild(el("span", null, label));
    if (small) b.appendChild(el("small", null, small));
    b.onclick = function () { fn(); };
    sh.appendChild(b); return b;
  }
  /* run fn after the sheet is closed (history.back is async) */
  function afterClose(fn) {
    if (!sheetOpen) { fn(); return; }
    var h = function () { window.removeEventListener("popstate", h); setTimeout(fn, 0); };
    window.addEventListener("popstate", h); history.back();
  }
  function confirmSheet(title, text, label, fn) {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, title));
      sh.appendChild(el("p", "sub", text));
      var row = el("div", "s-btns");
      var c = el("button", "plain", "Cancel"); c.type = "button"; c.onclick = closeSheet;
      var d = el("button", "danger", label); d.type = "button"; d.onclick = function () { afterClose(fn); };
      row.appendChild(c); row.appendChild(d); sh.appendChild(row);
    });
  }
  function promptSheet(title, value, label, fn, placeholder) {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, title));
      var inp = el("input", "s-field"); inp.type = "text"; inp.value = value || ""; inp.setAttribute("aria-label", title);
      inp.autocomplete = "off"; inp.autocapitalize = "off"; inp.spellcheck = false; if (placeholder) inp.placeholder = placeholder;
      sh.appendChild(inp);
      var row = el("div", "s-btns");
      var c = el("button", "plain", "Cancel"); c.type = "button"; c.onclick = closeSheet;
      var ok = el("button", "primary", label); ok.type = "button";
      function go() { var v = inp.value.trim(); afterClose(function () { fn(v); }); }
      ok.onclick = go; inp.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); go(); } });
      row.appendChild(c); row.appendChild(ok); sh.appendChild(row);
      if (placeholder === undefined) setTimeout(function () { inp.select(); }, 30);
    });
  }

  /* ---------- markdown (escape first, then a small safe subset) ---------- */
  function inline(s) {
    var codes = [];
    s = esc(s).replace(/`([^`\n]+)`/g, function (m, c) { codes.push(c); return "\u0001" + (codes.length - 1) + "\u0001"; });
    s = s.replace(/\[([^\]\n]+)\]\((https?:\/\/[^\s)]+|mailto:[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>')
      .replace(/(^|[\s(])(https?:\/\/[^\s<)]+[^\s<).,;:!?])/g, '$1<a href="$2" target="_blank" rel="noopener noreferrer">$2</a>')
      .replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>")
      .replace(/(^|[^*\w])\*([^*\n]+)\*(?!\w)/g, "$1<em>$2</em>")
      .replace(/(^|[^_\w])_([^_\n]+)_(?!\w)/g, "$1<em>$2</em>");
    return s.replace(/\u0001(\d+)\u0001/g, function (m, i) { return "<code>" + codes[+i] + "</code>"; });
  }
  function md(text) {
    var parts = String(text).split(/```/), out = "";
    for (var i = 0; i < parts.length; i++) {
      if (i % 2 === 1) {
        var body = parts[i].replace(/^[^\n]*\n/, function (m) { return /^\s*[\w+#.-]*\s*\n$/.test(m) ? "" : m; });
        out += "<pre><code>" + esc(body.replace(/\n$/, "")) + "</code></pre>";
        continue;
      }
      var lines = parts[i].split("\n"), list = null, para = [], m;
      var fp = function () { if (para.length) { out += "<p>" + para.join("<br>") + "</p>"; para = []; } };
      var fl = function () { if (list) { out += "</" + list + ">"; list = null; } };
      for (var j = 0; j < lines.length; j++) {
        var ln = lines[j];
        if ((m = /^(#{1,4})\s+(.*)$/.exec(ln))) { fp(); fl(); out += "<h" + m[1].length + ">" + inline(m[2]) + "</h" + m[1].length + ">"; }
        else if ((m = /^\s*[-*+]\s+(.*)$/.exec(ln))) { fp(); if (list !== "ul") { fl(); out += "<ul>"; list = "ul"; } out += "<li>" + inline(m[1]) + "</li>"; }
        else if ((m = /^\s*\d+[.)]\s+(.*)$/.exec(ln))) { fp(); if (list !== "ol") { fl(); out += "<ol>"; list = "ol"; } out += "<li>" + inline(m[1]) + "</li>"; }
        else if ((m = /^>\s?(.*)$/.exec(ln))) { fp(); fl(); out += "<blockquote>" + inline(m[1]) + "</blockquote>"; }
        else if (/^\s*([-*_])\1{2,}\s*$/.test(ln)) { fp(); fl(); out += "<hr>"; }
        else if (ln.trim() === "") { fp(); fl(); }
        else { fl(); para.push(inline(ln)); }
      }
      fp(); fl();
    }
    return out;
  }

  /* ---------- helpers ---------- */
  function ago(ms) {
    if (!ms) return "";
    var s = Math.max(0, (Date.now() - ms) / 1000);
    if (s < 60) return "now";
    if (s < 3600) return Math.floor(s / 60) + " min";
    if (s < 86400) return Math.floor(s / 3600) + " h";
    if (s < 7 * 86400) return Math.floor(s / 86400) + " d";
    return new Date(ms).toLocaleDateString(undefined, { month: "short", day: "numeric" });
  }
  function contentText(c) {
    if (typeof c === "string") return c;
    if (Array.isArray(c)) return c.map(function (p) { return p && typeof p.text === "string" ? p.text : ""; }).filter(Boolean).join("\n");
    return c == null ? "" : JSON.stringify(c);
  }
  var ATT_RE = /<attachment\s+([^>]*)>\n?([\s\S]*?)\n?<\/attachment>/g, IMG_RE = /\[\[image:([0-9a-fA-F]+)\|([^\]\n]*)\]\]/g;
  function attName(a) { var m = /name="([^"]*)"/.exec(a); return m ? m[1] : "file"; }
  function plainUser(t) {
    return String(t).replace(ATT_RE, function (m, a) { return "\n[" + attName(a) + "]\n"; }).replace(IMG_RE, function (m, id, n) { return "\n[" + n + "]\n"; }).replace(/\n{2,}/g, "\n").trim();
  }
  function userNode(t) {
    var d = el("div", "msg user"), rest = String(t), chips = [];
    rest = rest.replace(ATT_RE, function (m, a) { chips.push(attName(a)); return ""; }).replace(IMG_RE, function (m, id, n) { chips.push(n || "image"); return ""; });
    rest = rest.replace(/^\n+|\n+$/g, "");
    if (rest) d.appendChild(el("div", null, rest));
    chips.forEach(function (c) { d.appendChild(el("span", "att", c)); });
    return d;
  }
  function parseArgs(a) { if (a && typeof a === "object") return a; try { return JSON.parse(a); } catch (e) { return null; } }
  function firstLine(s) { return String(s == null ? "" : s).split("\n")[0].trim(); }
  function clip(s, n) { s = String(s); return s.length > n ? s.slice(0, n - 1) + "…" : s; }
  function base(p) { p = String(p || ""); var i = p.lastIndexOf("/"); return i >= 0 && i < p.length - 1 ? p.slice(i + 1) : p; }
  function toolSummary(name, args, st, secs) {
    var o = parseArgs(args) || {}, n = String(name || "tool"), l = n.toLowerCase(), run = st === "run";
    var path = o.path || o.file || o.file_path || o.filename || "";
    var t = secs != null && secs >= 0.1 ? " (" + (secs < 10 ? secs.toFixed(1) : Math.round(secs)) + " s)" : "";
    var s;
    if (/^(shell|bash|run_shell|exec|run_command|sh)$/.test(l) || (o.command && !path)) s = (run ? "running shell: " : "ran shell: ") + clip(firstLine(o.command || o.cmd || ""), 70);
    else if (/read|view|cat/.test(l) && path) s = (run ? "reading " : "read ") + base(path);
    else if (/write|create|append/.test(l) && path) s = (run ? "writing " : "wrote ") + base(path);
    else if (/edit|patch|replace/.test(l) && path) s = (run ? "editing " : "edited ") + base(path);
    else if (/grep|search|find|glob/.test(l)) s = (run ? "searching " : "searched ") + clip(firstLine(o.pattern || o.query || o.q || o.glob || path), 60);
    else if (/list|ls|tree/.test(l)) s = (run ? "listing " : "listed ") + (base(path) || ".");
    else if (/web|fetch|browse|url/.test(l)) s = (run ? "fetching " : "fetched ") + clip(firstLine(o.url || o.query || ""), 60);
    else { var k = Object.keys(o)[0]; s = (run ? "running " : "ran ") + n + (k ? ": " + clip(firstLine(typeof o[k] === "string" ? o[k] : JSON.stringify(o[k])), 60) : ""); }
    return s + (run ? "" : t);
  }
  function prettyArgs(a) {
    var o = parseArgs(a);
    if (!o || typeof o !== "object") return String(a == null ? "" : a);
    return Object.keys(o).map(function (k) {
      var v = typeof o[k] === "string" ? o[k] : JSON.stringify(o[k], null, 1);
      return k + ": " + v;
    }).join("\n");
  }

  /* ---------- theme + fonts ---------- */
  function themeColor() {
    var c = getComputedStyle(document.documentElement).getPropertyValue("--bg").trim();
    if (c) { var m = $("meta[name=theme-color]"); if (m) m.setAttribute("content", c); }
    if (window.JudasTheme && S.view === "planner") followTheme();
  }
  function setSkin(name, persist) {
    var link = $("#skin-link");
    link.addEventListener("load", themeColor, { once: true });
    link.setAttribute("href", name ? "/skin.css?name=" + encodeURIComponent(name) : "/skin.css");
    if (persist) lsSet("judas.skin", name);
    if (window.JudasTheme) JudasTheme.set(name);
  }
  function initTheme() {
    var saved = lsGet("judas.skin");
    if (saved) setSkin(saved, false);
    else {
      if (window.JudasTheme) JudasTheme.set(null);
      if (window.matchMedia && matchMedia("(prefers-color-scheme: light)").matches) {
        get("/api/skins").then(function (r) {
          var l = (r.skins || []).filter(function (s) { return s.dark === false; })[0];
          if (l && !lsGet("judas.skin")) setSkin(l.name, false);
        }, function () { /* keep the default skin */ });
      }
    }
    themeColor();
    // fonts.js may not exist in this build: probe before loading (an unknown path answers with HTML)
    fetch("/fonts.js", { method: "HEAD" }).then(function (r) {
      if (!r.ok || !/javascript/.test(r.headers.get("content-type") || "")) throw 0;
      var css = el("link"); css.rel = "stylesheet"; css.href = "/fonts.css"; document.head.appendChild(css);
      var sc = el("script"); sc.src = "/fonts.js"; document.head.appendChild(sc);
    }).catch(function () { applyOwnFontSize(); });
  }
  function applyOwnFontSize() {
    var n = parseInt(lsGet("judas.font.size"), 10);
    document.documentElement.style.fontSize = n >= 90 && n <= 125 ? n + "%" : "";
  }

  /* ---------- status / offline ---------- */
  function setHealth(h, st) {
    S.health = h; if (st) S.state = st;
    var dot = h === "off" ? "off" : (st && st.loaded && !(st.active > 0)) ? "ok" : (st && st.active > 0) ? "busy" : "";
    ["#dot", "#sess-dot"].forEach(function (id) { $(id).className = "dot " + dot; });
    var t = h === "off" ? "offline" : !st ? "..." : !st.loaded ? "no model" : st.active > 0 ? "busy" : "idle";
    var m = st && st.model ? String(st.model).replace(/\.gguf$/i, "") : "";
    $("#status-txt").textContent = (h !== "off" && st && st.loaded && m ? clip(m, 18) + " · " : "") + t;
    $("#sess-dot").title = t;
  }
  function pollState() {
    return get("/state", { timeout: 6000 }).then(function (st) {
      var was = S.health === "off";
      setHealth("ok", st); netFails = 0;
      if (was || !$("#offline").hidden) { hideOffline(); route(true); }
    }, function (e) {
      if (!e.net) return;
      netFails++;
      setHealth("off", null);
      if (netFails >= 2) showOffline();
    });
  }
  var offTimer = null, offCount = 0;
  function showOffline() {
    if ($("#offline").hidden) { $("#offline").hidden = false; $("#retry").focus(); }
    clearInterval(offTimer); offCount = 10;
    offTimer = setInterval(function () {
      offCount--; $("#off-auto").textContent = "Trying again in " + offCount + " s";
      if (offCount <= 0) { offCount = 10; retry(true); }
    }, 1000);
  }
  function hideOffline() { $("#offline").hidden = true; clearInterval(offTimer); $("#off-auto").textContent = ""; }
  function retry(silent) {
    var b = $("#retry"); if (!silent) { b.disabled = true; b.textContent = "Trying..."; }
    return pollState().then(function () {
      b.disabled = false; b.textContent = "Retry";
      if (!silent && !$("#offline").hidden) { $("#off-msg").textContent = "Still no answer. Check that Tailscale is connected and Judas is running."; }
    });
  }
  $("#retry").onclick = function () { retry(false); };
  $("#status").onclick = function () { pollState().then(function () { toast(S.health === "off" ? "Server offline" : "Server " + (S.state && S.state.loaded ? "ready" : "up, no model loaded")); }); };
  setInterval(function () { if (!document.hidden) pollState(); }, 5000);
  document.addEventListener("visibilitychange", function () { if (!document.hidden) pollState(); });
  window.addEventListener("online", function () { pollState(); });

  /* host bridge (Android wrapper): pull to refresh only at the top of the active scroller */
  function host() { return window.JudasHost || null; }
  function reportScroll() {
    var h = host(); if (!h || !h.setAtTop) return;
    var sc = S.view === "home" ? $("#home-scroll") : S.view === "session" ? $("#log") : null;
    try { h.setAtTop(!!sc && sc.scrollTop <= 0); } catch (e) { /* ignore */ }
  }
  ["#home-scroll", "#log"].forEach(function (s) { $(s).addEventListener("scroll", reportScroll, { passive: true }); });

  /* ---------- routing ---------- */
  function parseHash() {
    var h = (location.hash || "").replace(/^#/, "");
    if (h === "planner") return { view: "planner" };
    var m = /^s\/(.+)$/.exec(h);
    if (m) return { view: "session", sid: decodeURIComponent(m[1]) };
    for (var i = 0; i < MODES.length; i++) if (MODES[i][0] === h) return { view: "home", mode: h };
    return { view: "home" };
  }
  function go(hash) { if (location.hash === hash) route(true); else location.hash = hash; }
  function route(force) {
    var r = parseHash(), key = JSON.stringify(r);
    if (!force && key === lastRoute) return;
    lastRoute = key;
    if (r.view === "session") { showView("session"); if (S.sid !== r.sid || !S.sess || force === true) openSession(r.sid); }
    else if (r.view === "planner") { showView("planner"); openPlanner(); }
    else { if (r.mode) { S.mode = r.mode; lsSet("judas.m.mode", r.mode); } showView("home"); renderHome(); }
  }
  window.addEventListener("hashchange", function () { route(); });
  window.addEventListener("popstate", function (e) {
    if (sheetOpen) { hideSheet(); return; }
    route();
  });
  function showView(v) {
    S.view = v;
    $("#v-home").hidden = v !== "home"; $("#v-sess").hidden = v !== "session"; $("#v-plan").hidden = v !== "planner";
    $("#dock").hidden = v === "planner";
    $("#seg").hidden = v !== "home";
    if (v !== "session") { stopLive(); S.sid = null; S.sess = null; $("#approvals").hidden = true; }
    renderDock(); reportScroll();
    if (v === "planner") { try { host() && host().setAtTop && host().setAtTop(false); } catch (e) { /* ignore */ } }
  }

  /* ---------- home ---------- */
  function renderSeg() {
    var seg = $("#seg"); seg.textContent = "";
    MODES.forEach(function (m) {
      var b = el("button", null, m[1]); b.type = "button"; b.setAttribute("role", "tab");
      b.setAttribute("aria-selected", m[0] === S.mode ? "true" : "false");
      b.onclick = function () { S.mode = m[0]; lsSet("judas.m.mode", m[0]); S.showN = 12; renderSeg(); renderHome(); renderDock(); };
      seg.appendChild(b);
    });
  }
  function modeOf(s) { return s.mode || "chat"; }
  function renderHome() {
    $("#home-hint").textContent = HINTS[S.mode];
    loadSessions();
  }
  var sessReq = 0;
  function loadSessions() {
    var req = ++sessReq;
    return get("/api/sessions?limit=80").then(function (r) {
      if (req !== sessReq) return;
      S.sessions = r.sessions || []; drawCards();
    }, function (e) { if (e.net) return; toast("Could not load sessions: " + e.message, true); });
  }
  function drawCards() {
    var ul = $("#cards"); ul.textContent = "";
    var list = S.sessions.filter(function (s) { return modeOf(s) === S.mode; });
    var shown = list.slice(0, S.showN);
    $("#more-btn").hidden = list.length <= S.showN;
    if (!list.length) { $("#home-hint").textContent = HINTS[S.mode] + " Nothing here yet."; }
    else $("#home-hint").textContent = "Recent " + MODES.filter(function (m) { return m[0] === S.mode; })[0][1].toLowerCase() + " sessions";
    shown.forEach(function (s, i) {
      var li = el("li", "card");
      var b = el("button", "card-main"); b.type = "button";
      var top = el("span", "c-top");
      var ttl = el("span", "c-title"); if (s.running) ttl.appendChild(el("span", "c-run")); ttl.appendChild(document.createTextNode(s.title || "Untitled"));
      top.appendChild(ttl); top.appendChild(el("span", "c-time", ago(s.updated_ms)));
      b.appendChild(top);
      var last = el("span", "c-last", S.preview[s.id + ":" + s.updated_ms] || "");
      last.dataset.sid = s.id; if (!last.textContent) last.hidden = true;
      b.appendChild(last);
      if (NEEDS_CWD[S.mode] || WANTS_CWD[S.mode]) { if (s.cwd) b.appendChild(el("span", "c-meta mono", s.cwd)); }
      b.onclick = function () { go("#s/" + encodeURIComponent(s.id)); };
      b.setAttribute("aria-label", (s.title || "Untitled") + ", " + ago(s.updated_ms));
      var mo = el("button", "card-more"); mo.type = "button"; mo.setAttribute("aria-label", "Options for " + (s.title || "session"));
      mo.appendChild(icon("menu")); mo.onclick = function () { sessionActions(s); };
      li.appendChild(b); li.appendChild(mo); ul.appendChild(li);
      if (i < 10 && !S.preview[s.id + ":" + s.updated_ms]) fetchPreview(s, last);
    });
  }
  function fetchPreview(s, node) {
    get("/api/sessions/" + encodeURIComponent(s.id) + "?limit=4").then(function (r) {
      var ms = r.messages || [], t = "";
      for (var i = ms.length - 1; i >= 0 && !t; i--) {
        var b = ms[i].body || {};
        if (b.role === "assistant" || b.role === "user") t = contentText(b.content);
      }
      t = plainUser(t).replace(/\s+/g, " ").trim();
      if (!t) return;
      t = t.replace(/```/g, " ").replace(/[*_`#>]+/g, "").replace(/\s+/g, " ").trim();
      S.preview[s.id + ":" + s.updated_ms] = clip(t, 160);
      node.textContent = clip(t, 160); node.hidden = false;
    }, function () { /* no preview */ });
  }
  $("#more-btn").onclick = function () { S.showN += 20; drawCards(); };
  function sessionActions(s) {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, s.title || "Session"));
      sItem(sh, "edit", "Rename", function () { afterClose(function () { renameSession(s); }); });
      sItem(sh, "trash", "Delete", function () { afterClose(function () { deleteSession(s); }); }, "danger");
    });
  }
  function renameSession(s) {
    promptSheet("Rename session", s.title || "", "Save", function (v) {
      if (!v) return;
      api("PATCH", "/api/sessions/" + encodeURIComponent(s.id), { title: v }).then(function (r) {
        s.title = v; if (S.sess && S.sess.id === s.id) { S.sess.title = v; $("#sess-title").textContent = v; }
        drawCards(); toast("Renamed");
      }, function (e) { toast("Rename failed: " + e.message, true); });
    });
  }
  function deleteSession(s) {
    confirmSheet("Delete this session?", "“" + clip(s.title || "Untitled", 60) + "” and its history will be removed.", "Delete", function () {
      api("DELETE", "/api/sessions/" + encodeURIComponent(s.id), undefined, { timeout: 30000 }).then(function () {
        S.sessions = S.sessions.filter(function (x) { return x.id !== s.id; });
        if (S.view === "session" && S.sid === s.id) history.back(); else drawCards();
        toast("Deleted");
      }, function (e) { toast(e.status === 409 ? "Still stopping, try again in a moment" : "Delete failed: " + e.message, true); });
    });
  }

  /* ---------- menu ---------- */
  function openMenu() {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Judas"));
      var st = S.state;
      sh.appendChild(el("p", "sub", S.health === "off" ? "Server offline" : st ? (st.model || "no model") + (st.loaded ? "" : " (not loaded)") + (st.version ? " · v" + st.version : "") : "Checking..."));
      var box = el("div", "sbody"); sh.appendChild(box);
      sItem(box, "cal", "Planner", function () { afterClose(function () { go("#planner"); }); });
      sItem(box, "refresh", "Reload", function () { location.reload(); });
      sItem(box, "desk", "Open desktop version", function () { location.href = "/"; });
      if (host() && host().openSettings) sItem(box, "gear", "App settings", function () { afterClose(function () { host().openSettings(); }); });
      box.appendChild(el("div", "s-label", "Theme"));
      var th = el("div"); box.appendChild(th);
      get("/api/skins").then(function (r) {
        var cur = lsGet("judas.skin") || "";
        [["Dark", true], ["Light", false]].forEach(function (g) {
          var skins = (r.skins || []).filter(function (x) { return !!x.dark === g[1]; });
          if (!skins.length) return;
          th.appendChild(el("div", "s-label", g[0]));
          var row = el("div", "opts"); th.appendChild(row);
          skins.forEach(function (x) {
            var lab = String(x.label || x.name).replace(/\s*\(.*\)\s*$/, "").replace(/\s+(Dark|Light)$/i, "") || x.name;
            var b = el("button", null, lab); b.type = "button"; b.setAttribute("aria-pressed", x.name === cur ? "true" : "false");
            b.onclick = function () {
              setSkin(x.name, true);
              Array.prototype.forEach.call(th.querySelectorAll("button"), function (y) { y.setAttribute("aria-pressed", "false"); });
              b.setAttribute("aria-pressed", "true");
            };
            row.appendChild(b);
          });
        });
      }, function () { th.appendChild(el("span", "dim", "Themes unavailable")); });
      box.appendChild(el("div", "s-label", "Text size"));
      var sz = el("div", "opts"); box.appendChild(sz);
      var curSz = parseInt(lsGet("judas.font.size"), 10); if (!(curSz >= 90 && curSz <= 125)) curSz = 100;
      [["Small", 92], ["Normal", 100], ["Large", 112], ["Larger", 124]].forEach(function (o) {
        var b = el("button", null, o[0]); b.type = "button"; b.setAttribute("aria-pressed", String(o[1] === curSz || (curSz > 100 && o[1] === 112 && curSz < 118)));
        b.onclick = function () {
          lsSet("judas.font.size", o[1] === 100 ? null : String(o[1]));
          if (window.JudasFonts) JudasFonts.apply(); else applyOwnFontSize();
          Array.prototype.forEach.call(sz.children, function (x) { x.setAttribute("aria-pressed", "false"); }); b.setAttribute("aria-pressed", "true");
        };
        sz.appendChild(b);
      });
    });
  }
  $("#menu-btn").onclick = openMenu;
  $("#menu-btn").appendChild(icon("menu"));

  /* ---------- planner ---------- */
  var planLoaded = false;
  function openPlanner() {
    var fr = $("#plan-frame");
    if (!planLoaded) {
      planLoaded = true;
      fr.src = window.JudasTheme ? JudasTheme.decorate("/mach12/") : "/mach12/";
      if (window.JudasTheme) JudasTheme.track(fr);
    }
  }
  function followTheme() { if (window.JudasTheme) JudasTheme.set(lsGet("judas.skin")); }
  $("#plan-back").appendChild(icon("back"));
  $("#plan-back").onclick = function () { if (history.length > 1) history.back(); else go("#"); };

  /* ---------- session ---------- */
  function sessionHeader() {
    var s = S.sess || {};
    $("#sess-title").textContent = s.title || "New session";
  }
  var cur = null; /* render context for the open session */
  function stick() { var l = $("#log"); return l.scrollHeight - l.scrollTop - l.clientHeight < 90; }
  function toBottom(force) { var l = $("#log"); if (force || cur && cur.stick) l.scrollTop = l.scrollHeight; reportScroll(); }
  $("#log").addEventListener("scroll", function () {
    var near = stick(); if (cur) cur.stick = near; $("#jump").hidden = near;
  }, { passive: true });
  $("#jump").appendChild(icon("down"));
  $("#jump").onclick = function () { cur.stick = true; toBottom(true); $("#jump").hidden = true; };
  $("#back-btn").appendChild(icon("back"));
  $("#back-btn").onclick = function () { if (history.length > 1) history.back(); else go("#" + S.mode); };
  $("#sess-menu").appendChild(icon("menu"));
  $("#sess-menu").onclick = function () { if (S.sess) sessionMenu(); };
  $("#sess-title").onclick = function () { if (S.sess) sessionMenu(); };
  function sessionMenu() {
    var s = S.sess;
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, s.title || "Session"));
      if (s.cwd) sh.appendChild(el("p", "sub mono", s.cwd));
      sItem(sh, "edit", "Rename", function () { afterClose(function () { renameSession(s); }); });
      if (S.running) sItem(sh, "stop", "Stop the turn", function () { afterClose(cancelTurn); });
      sItem(sh, "trash", "Delete", function () { afterClose(function () { deleteSession(s); }); }, "danger");
    });
  }

  function newCtx() { return { stick: true, bubble: null, bubbleText: "", think: null, tools: {}, typing: null, raf: 0 }; }
  function clearLog() { $("#log").textContent = ""; cur = newCtx(); }
  function addNote(text, err) { var n = el("div", "msg note" + (err ? " err" : ""), text); $("#log").appendChild(n); return n; }
  function addUser(text) { var n = userNode(text); $("#log").appendChild(n); return n; }
  function addAsst(text) { var n = el("div", "msg asst"); n.innerHTML = md(text); $("#log").appendChild(n); return n; }
  function addThink(text) {
    var r = el("div", "row think"); r.dataset.open = "false";
    var b = el("button"); b.type = "button"; b.setAttribute("aria-expanded", "false");
    b.appendChild(el("span", "car", "▸")); b.appendChild(el("span", "sum", "Thinking"));
    var body = el("div", "body"); var tx = el("div", "think-txt", text || ""); body.appendChild(tx);
    b.onclick = function () { var o = r.dataset.open !== "true"; r.dataset.open = String(o); b.setAttribute("aria-expanded", String(o)); };
    r.appendChild(b); r.appendChild(body); $("#log").appendChild(r);
    return { row: r, txt: tx };
  }
  function addTool(name, args) {
    var r = el("div", "row tcall run"); r.dataset.open = "false";
    var b = el("button"); b.type = "button"; b.setAttribute("aria-expanded", "false");
    b.appendChild(el("span", "car", "▸")); var sum = el("span", "sum", toolSummary(name, args, "run")); b.appendChild(sum);
    var body = el("div", "body");
    var a = prettyArgs(args);
    if (a) { body.appendChild(el("div", "lbl", "Input")); body.appendChild(el("pre", null, clip(a, 4000))); }
    var outLbl = el("div", "lbl", "Output"); outLbl.hidden = true; var out = el("pre"); out.hidden = true;
    body.appendChild(outLbl); body.appendChild(out);
    b.onclick = function () { var o = r.dataset.open !== "true"; r.dataset.open = String(o); b.setAttribute("aria-expanded", String(o)); };
    r.appendChild(b); r.appendChild(body); $("#log").appendChild(r);
    return { row: r, sum: sum, out: out, outLbl: outLbl, name: name, args: args, t0: Date.now() };
  }
  function finishTool(t, ok, output, secs) {
    t.row.classList.remove("run"); if (ok === false) t.row.classList.add("fail");
    var s = toolSummary(t.name, t.args, "done", secs);
    t.sum.textContent = ok === false ? "failed " + s.replace(/^(ran|read|wrote|edited|searched|listed|fetched) /, "") : s;
    var o = contentText(output);
    if (o) { t.out.textContent = o.length > 6000 ? o.slice(0, 6000) + "\n… (" + (o.length - 6000) + " more characters)" : o; t.out.hidden = false; t.outLbl.hidden = false; }
  }
  function showTyping(on) {
    if (!cur) return;
    if (on && !cur.typing) { var t = el("div", "typing"); t.setAttribute("aria-label", "Working"); t.innerHTML = "<i></i><i></i><i></i>"; $("#log").appendChild(t); cur.typing = t; }
    else if (!on && cur.typing) { cur.typing.remove(); cur.typing = null; }
  }
  function keepTypingLast() { if (cur && cur.typing) $("#log").appendChild(cur.typing); }

  function renderMessages(messages) {
    var log = $("#log"), childAgents = {}, tools = {};
    (S.agents || []).forEach(function (a) { if (a.parent) childAgents[a.id] = true; });
    clearLog();
    messages.forEach(function (m) {
      var b = m.body || {};
      if (m.agent && childAgents[m.agent]) return;   // workers live in the agents sheet
      if (b.role === "user") { addUser(contentText(b.content)); return; }
      if (b.role === "assistant") {
        if (b.reasoning_content) addThink(b.reasoning_content);
        if (b.content && contentText(b.content).trim()) addAsst(contentText(b.content));
        (b.tool_calls || []).forEach(function (tc) {
          var f = tc.function || {}, t = addTool(f.name || "tool", f.arguments);
          tools[tc.id] = t; t.at = m.created_ms;
        });
        return;
      }
      if (b.role === "tool") {
        var t = tools[b.tool_call_id], c = contentText(b.content);
        if (t) {
          var secs = m.created_ms && t.at ? (m.created_ms - t.at) / 1000 : null;
          if (secs != null && (secs < 0 || secs > 900)) secs = null;
          finishTool(t, !/^error/i.test(c), c, secs);
        }
      }
    });
    // a call with no result yet is "running" only while a turn is live
    Object.keys(tools).forEach(function (k) { if (!S.running) tools[k].row.classList.remove("run"); });
    if (S.more) {
      var ob = el("button", "older", "Load earlier messages"); ob.type = "button";
      ob.onclick = function () { loadOlder(ob); };
      log.insertBefore(ob, log.firstChild);
    }
    if (!log.childNodes.length) log.appendChild(el("div", "msg note", "Nothing here yet. Say something below."));
    toBottom(true);
  }
  function loadOlder(btn) {
    if (!S.firstId) return;
    btn.disabled = true; btn.textContent = "Loading...";
    get("/api/sessions/" + encodeURIComponent(S.sid) + "?limit=40&before=" + encodeURIComponent(S.firstId)).then(function (r) {
      var ms = r.messages || []; if (ms.length) S.firstId = ms[0].id;
      S.more = !!r.more_before;
      S.msgs = ms.concat(S.msgs || []);
      var pos = $("#log").scrollTop, h = $("#log").scrollHeight;
      renderMessages(S.msgs); var l = $("#log"); l.scrollTop = pos + (l.scrollHeight - h);
    }, function (e) { btn.disabled = false; btn.textContent = "Load earlier messages"; toast("Could not load: " + e.message, true); });
  }

  function openSession(id) {
    stopLive();
    S.sid = id; S.sess = null; S.agents = []; S.team = null; S.msgs = []; S.queue = S.queue.filter(function (q) { return q.sid === id; });
    S.running = false; S.approvals = null; S.apprWanted = true;
    clearLog(); sessionHeader(); $("#sess-title").textContent = "Loading...";
    $("#approvals").hidden = true; $("#agents-chip").hidden = true; $("#jump").hidden = true;
    renderDock();
    return refreshSession(true).then(function () {
      startLive();
      loadApprovalsNow(); loadTeam().then(renderAgentsChip);
    }, function (e) {
      if (e.net) return;
      $("#sess-title").textContent = "Session";
      clearLog(); addNote(e.status === 404 ? "This session no longer exists." : "Could not open it: " + e.message, true);
    });
  }
  function refreshSession(full) {
    var id = S.sid;
    return get("/api/sessions/" + encodeURIComponent(id) + "?limit=40").then(function (r) {
      if (S.sid !== id) return;
      S.sess = r.session || S.sess || {}; S.agents = r.agents || []; S.more = !!r.more_before;
      S.msgs = r.messages || []; S.firstId = S.msgs.length ? S.msgs[0].id : null;
      S.turn = r.turn || {};
      S.running = !!(S.turn.running) || (S.live && S.live.inTurn);
      sessionHeader(); renderAgentsChip(); renderMessages(S.msgs);
      if (S.running) { showTyping(true); }
      renderDock(); renderQueue();
      if (full) { S.mode = modeOf(S.sess); }
      return r;
    });
  }

  /* ---- live stream ---- */
  function stopLive() {
    if (S.live) { S.live.stopped = true; if (S.live.es) S.live.es.close(); clearTimeout(S.live.retry); clearInterval(S.live.poll); }
    S.live = null; S.running = false;
  }
  function startLive() {
    var t = S.turn || {};
    var since = t.running ? (t.event_floor || 0) : (t.events_head || 0);
    var L = S.live = { sid: S.sid, es: null, lastId: since, stopped: false, tries: 0, inTurn: !!t.running, retry: null, poll: null, calls: {} };
    if (t.running) { S.running = true; replayBubbleReset(); }
    connect(L);
    L.poll = setInterval(function () { if (!document.hidden && S.live === L) pollSession(L); }, 4000);
    pollSession(L);
  }
  function replayBubbleReset() { if (cur) { cur.bubble = null; cur.bubbleText = ""; cur.think = null; } }
  function connect(L) {
    if (L.stopped) return;
    var es = new EventSource("/api/sessions/" + encodeURIComponent(L.sid) + "/events?follow=1&since=" + L.lastId);
    L.es = es;
    es.onmessage = function (m) {
      if (S.live !== L) return;
      L.tries = 0;
      if (m.lastEventId) L.lastId = Math.max(L.lastId, Number(m.lastEventId) || 0);
      var ev; try { ev = JSON.parse(m.data); } catch (e) { return; }
      handleEvent(L, ev);
    };
    es.onerror = function () {
      if (S.live !== L || L.stopped) return;
      es.close();
      get("/api/sessions/" + encodeURIComponent(L.sid) + "?limit=1").then(function () {
        if (S.live !== L) return;
        L.tries++; L.retry = setTimeout(function () { connect(L); }, Math.min(8000, 800 * L.tries));
      }, function (e) { if (S.live !== L) return; if (e.net) { L.tries++; L.retry = setTimeout(function () { connect(L); }, Math.min(8000, 800 * L.tries)); } });
    };
  }
  function pollSession(L) {
    get("/api/sessions/" + encodeURIComponent(L.sid) + "?limit=1").then(function (r) {
      if (S.live !== L) return;
      S.agents = r.agents || S.agents; S.sess = r.session || S.sess;
      renderAgentsChip(); renderQueue();
      var running = !!(r.turn && r.turn.running);
      if (running && !S.running) { L.inTurn = true; setRunning(true); showTyping(true); }
      else if (!running && S.running && Date.now() - (L.turnAt || 0) > 3000) endTurn();
    }, function () { /* offline handled by the state poll */ });
    if (S.approvals !== null || S.apprWanted) pollApprovals();
    if (sheetOpen && L.agentSheet) refreshAgentsSheet();
  }
  function beginTurn() {
    var L = S.live; if (!L || L.inTurn) return;
    L.inTurn = true; L.turnAt = Date.now(); setRunning(true);
    replayBubbleReset(); showTyping(true);
  }
  function endTurn() {
    var L = S.live; if (L) { L.inTurn = false; }
    setRunning(false); showTyping(false);
    var id = S.sid;
    refreshSession(false).then(function () { if (S.sid === id) { flushQueue(); loadApprovalsNow(); } }, function () { /* ignore */ });
  }
  function setRunning(on) { S.running = on; renderDock(); }
  function handleEvent(L, ev) {
    var d = ev.data || {}, lead = !ev.parent;
    if (ev.type === "gap") return;
    if (ev.type === "approval_request") { S.approvals = S.approvals || []; loadApprovalsNow(); return; }
    if (ev.type === "approval_resolved") { loadApprovalsNow(); return; }
    if (ev.type === "spawned" || ev.type === "status" || ev.type === "activity" || ev.type === "team" || (ev.type === "done" && !lead)) { if (!lead || ev.type === "spawned") bumpAgents(); }
    if (!lead) return;
    if (!L.inTurn && ev.type !== "done" && ev.type !== "status") beginTurn();
    switch (ev.type) {
      case "delta": {
        showTyping(false);
        if (!cur.bubble) { cur.bubble = el("div", "msg asst"); cur.bubbleText = ""; $("#log").appendChild(cur.bubble); }
        cur.bubbleText += d.text || "";
        if (!cur.raf) cur.raf = requestAnimationFrame(function () { cur.raf = 0; if (cur.bubble) { cur.bubble.innerHTML = md(cur.bubbleText); toBottom(); } });
        break;
      }
      case "reasoning": {
        showTyping(false);
        if (!cur.think) { cur.think = addThink(""); cur.thinkText = ""; }
        cur.thinkText += d.text || ""; cur.think.txt.textContent = cur.thinkText; toBottom();
        break;
      }
      case "tool_call": {
        showTyping(false); cur.bubble = null; cur.think = null;
        var t = addTool(d.name || "tool", d.args); cur.tools[d.id] = t; toBottom(); break;
      }
      case "tool_result": {
        var tt = cur.tools[d.id];
        if (tt) { finishTool(tt, d.ok, d.output, (Date.now() - tt.t0) / 1000); }
        cur.bubble = null; cur.think = null; showTyping(true); keepTypingLast(); toBottom(); break;
      }
      case "message": cur.bubble = null; cur.think = null; break;
      case "status":
        if (d.error) { addNote("Error: " + d.error, true); toBottom(); }
        break;
      case "done":
        if (cur.raf) { cancelAnimationFrame(cur.raf); cur.raf = 0; if (cur.bubble) cur.bubble.innerHTML = md(cur.bubbleText); }
        endTurn(); break;
      case "session_deleted": toast("Session deleted"); go("#" + S.mode); break;
    }
  }

  /* ---- agents ---- */
  var agentsT = 0;
  function bumpAgents() {
    clearTimeout(agentsT);
    agentsT = setTimeout(function () { loadTeam().then(function () { renderAgentsChip(); if (sheetOpen && S.live && S.live.agentSheet) refreshAgentsSheet(); }); }, 400);
  }
  function loadTeam() {
    if (!S.sid) return Promise.resolve();
    var id = S.sid;
    return get("/api/sessions/" + encodeURIComponent(id) + "/team").then(function (r) { if (S.sid === id && r && Array.isArray(r.agents)) S.team = r; }, function () { /* team endpoint optional */ });
  }
  function agentList() {
    var base = (S.team && S.team.agents && S.team.agents.length) ? S.team.agents : S.agents || [];
    return base;
  }
  function workers() { return agentList().filter(function (a) { return a.parent; }); }
  function renderAgentsChip() {
    var w = workers(), c = $("#agents-chip");
    if (!w.length) { c.hidden = true; return; }
    var act = w.filter(function (a) { return /running|waiting|queued/.test(a.status); }).length;
    c.textContent = w.length + (w.length === 1 ? " agent" : " agents");
    if (act) { var d = el("span", "dot busy"); d.style.marginLeft = "8px"; d.style.display = "inline-block"; c.appendChild(d); }
    c.setAttribute("aria-label", w.length + " agents, " + act + " active");
    c.hidden = false;
  }
  $("#agents-chip").onclick = function () { loadTeam().then(openAgentsSheet); };
  function glyph(a) {
    var s = a.status;
    if (s === "done") return ["✓", "done"];
    if (s === "failed" || s === "killed") return ["✕", "bad"];
    if (s === "paused") return ["❚❚", ""];
    if (/running|waiting|queued/.test(s)) return ["●", "run"];
    return ["○", ""];
  }
  function agentState(a) {
    var s = a.activity && a.activity !== "idle" ? a.activity : a.status;
    if (a.tool) s += " · " + a.tool;
    return s;
  }
  function openAgentsSheet() {
    if (!S.live) return;
    S.live.agentSheet = true;
    openSheet(function (sh) { sh.appendChild(el("h2", null, "Agents")); var b = el("div", "sbody"); b.id = "ag-body"; sh.appendChild(b); fillAgents(); });
    var L = S.live; var chk = setInterval(function () { if (!sheetOpen || S.live !== L) { clearInterval(chk); if (L) L.agentSheet = false; } }, 700);
  }
  function refreshAgentsSheet() { loadTeam().then(fillAgents); }
  function fillAgents() {
    var box = $("#ag-body"); if (!box) return;
    box.textContent = "";
    var list = agentList();
    if (!list.length) box.appendChild(el("p", "sub", "No agents in this session."));
    list.forEach(function (a) {
      var r = el("div", "ag"); var g = glyph(a);
      r.appendChild(el("span", "glyph " + g[1], g[0]));
      var info = el("div", "info");
      info.appendChild(el("span", "nm", a.parent ? (a.member || a.role || "agent") : "Lead"));
      info.appendChild(el("span", "st", agentState(a) + (a.task ? " · " + clip(a.task, 60) : "")));
      r.appendChild(info);
      var act = /running|waiting|queued/.test(a.status);
      if (act) {
        var p = el("button", null, "Pause"); p.type = "button"; p.onclick = function () { agentCmd(a, "pause"); };
        var x = el("button", "x", "Cancel"); x.type = "button"; x.onclick = function () { agentCmd(a, "cancel"); };
        r.appendChild(p); r.appendChild(x);
      } else if (a.status === "paused" || a.status === "killed") {
        var rs = el("button", null, "Resume"); rs.type = "button"; rs.onclick = function () { agentCmd(a, "resume"); };
        r.appendChild(rs);
      }
      box.appendChild(r);
    });
  }
  function agentCmd(a, cmd) {
    post("/api/sessions/" + encodeURIComponent(S.sid) + "/agents/" + encodeURIComponent(a.id) + "/" + cmd).then(function () {
      a.status = cmd === "pause" ? "paused" : cmd === "cancel" ? "killed" : "running"; fillAgents(); bumpAgents();
    }, function (e) { toast(cmd + " failed: " + e.message, true); });
  }

  /* ---- approvals (feature-detected) ---- */
  var apprBusy = false;
  function pollApprovals() { if (S.running || S.approvals) loadApprovalsNow(); }
  function loadApprovalsNow() {
    if (!S.sid || apprBusy || S.apprWanted === false && S.approvals === null) return;
    apprBusy = true; var id = S.sid;
    get("/api/sessions/" + encodeURIComponent(id) + "/approvals").then(function (r) {
      apprBusy = false; if (S.sid !== id) return;
      // an older server answers unknown /api paths with the app shell (200, not JSON): treat that as unsupported
      if (!r || !Array.isArray(r.approvals)) { S.approvals = null; S.apprWanted = false; renderApprovals(); return; }
      S.approvals = r.approvals; renderApprovals();
    }, function (e) {
      apprBusy = false;
      if (e.status === 404 || e.status === 405 || e.status === 501) { S.approvals = null; S.apprWanted = false; renderApprovals(); }
    });
  }
  function renderApprovals() {
    var box = $("#approvals"), list = S.approvals || [];
    if (!list.length) { box.hidden = true; box.textContent = ""; return; }
    var a = list[0], card = el("div", "appr");
    var h = el("h3"); h.appendChild(document.createTextNode("Approve " + (a.tool || "action") + "? "));
    h.appendChild(el("small", null, (a.role || "agent") + (list.length > 1 ? " · +" + (list.length - 1) + " more" : "")));
    card.appendChild(h);
    var pre = el("pre");
    String(a.what || "").split("\n").slice(0, 80).forEach(function (ln, i, arr) {
      var s = el("span", /^\+(?!\+\+)/.test(ln) ? "a" : /^-(?!--)/.test(ln) ? "r" : null, ln);
      pre.appendChild(s); if (i < arr.length - 1) pre.appendChild(document.createTextNode("\n"));
    });
    if (a.what) card.appendChild(pre);
    var row = el("div", "btns");
    var d = el("button", "plain", "Deny"); d.type = "button";
    var ok = el("button", "primary", "Approve"); ok.type = "button";
    function answer(v) {
      d.disabled = ok.disabled = true;
      post("/api/sessions/" + encodeURIComponent(S.sid) + "/approvals/" + encodeURIComponent(a.id), { approve: v }).then(function () {
        S.approvals = (S.approvals || []).filter(function (x) { return x.id !== a.id; }); renderApprovals();
      }, function (e) {
        if (e.status === 404 || e.status === 409) { S.approvals = (S.approvals || []).filter(function (x) { return x.id !== a.id; }); renderApprovals(); }
        else { d.disabled = ok.disabled = false; toast("Could not answer: " + e.message, true); }
      });
    }
    d.onclick = function () { answer(false); }; ok.onclick = function () { answer(true); };
    row.appendChild(d); row.appendChild(ok); card.appendChild(row);
    box.textContent = ""; box.appendChild(card); box.hidden = false;
  }

  /* ---------- dock: composer ---------- */
  var input = $("#input"), sendBtn = $("#send");
  $("#attach").appendChild(icon("clip"));
  function fit() { input.style.height = "auto"; input.style.height = Math.min(input.scrollHeight, Math.round(window.innerHeight * 0.3)) + "px"; }
  input.addEventListener("input", function () { fit(); renderSend(); });
  input.addEventListener("keydown", function (e) { if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) { e.preventDefault(); send(); } });
  function curMode() { return S.view === "session" && S.sess ? modeOf(S.sess) : S.mode; }
  function needsFolderNow() { return S.view === "home" && (NEEDS_CWD[S.mode] || WANTS_CWD[S.mode]); }
  function renderDock() {
    $("#dock").hidden = S.view === "planner";
    input.placeholder = PLACEHOLDER[curMode()] || "Message Judas";
    var f = $("#folder");
    if (needsFolderNow()) {
      f.hidden = false; f.textContent = ""; f.appendChild(icon("folder"));
      var sp = el("span", null, S.cwd ? clip(S.cwd, 34) : (NEEDS_CWD[S.mode] ? "Choose folder" : "Folder (optional)"));
      f.appendChild(sp); f.classList.toggle("need", !S.cwd && !!NEEDS_CWD[S.mode]);
      f.setAttribute("aria-label", "Working folder: " + (S.cwd || "not set"));
    } else f.hidden = true;
    renderSeg(); $("#seg").hidden = S.view !== "home";
    renderSend();
  }
  function renderSend() {
    var has = input.value.trim() || S.atts.some(function (a) { return a.state === "ok"; });
    sendBtn.className = "send"; sendBtn.textContent = "";
    if (S.running && S.view === "session") {
      if (has) { sendBtn.classList.add("queue"); sendBtn.appendChild(icon("up")); sendBtn.setAttribute("aria-label", "Queue message"); sendBtn.disabled = false; }
      else { sendBtn.classList.add("stop"); sendBtn.appendChild(icon("stop")); sendBtn.setAttribute("aria-label", "Stop"); sendBtn.disabled = false; }
    } else {
      sendBtn.appendChild(icon("up")); sendBtn.setAttribute("aria-label", "Send"); sendBtn.disabled = !has;
    }
  }
  sendBtn.onclick = function () {
    if (S.running && S.view === "session" && !input.value.trim() && !S.atts.length) cancelTurn(); else send();
  };
  function cancelTurn() {
    if (!S.sid) return;
    post("/api/sessions/" + encodeURIComponent(S.sid) + "/cancel").then(function () { toast("Stopping..."); }, function (e) { toast("Stop failed: " + e.message, true); });
  }
  $("#folder").onclick = function () {
    var recents = []; try { recents = JSON.parse(lsGet("judas.m.cwds") || "[]"); } catch (e) { /* ignore */ }
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Working folder"));
      sh.appendChild(el("p", "sub", NEEDS_CWD[S.mode] ? "Required for " + S.mode + ". An absolute path on the desktop." : "Optional. An absolute path on the desktop."));
      var inp = el("input", "s-field mono"); inp.type = "text"; inp.value = S.cwd; inp.placeholder = "/home/junior/src/project";
      inp.autocapitalize = "off"; inp.autocomplete = "off"; inp.spellcheck = false; inp.setAttribute("aria-label", "Folder path");
      sh.appendChild(inp);
      var rec = el("div", "sbody");
      recents.slice(0, 6).forEach(function (r) { sItem(rec, "folder", r, function () { inp.value = r; }); });
      sh.appendChild(rec);
      var row = el("div", "s-btns");
      var clr = el("button", "plain", "Clear"); clr.type = "button"; clr.onclick = function () { afterClose(function () { S.cwd = ""; lsSet("judas.m.cwd", null); renderDock(); }); };
      var ok = el("button", "primary", "Use folder"); ok.type = "button";
      function use() { var v = inp.value.trim(); afterClose(function () { S.cwd = v; lsSet("judas.m.cwd", v || null); renderDock(); }); }
      ok.onclick = use; inp.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); use(); } });
      row.appendChild(clr); row.appendChild(ok); sh.appendChild(row);
    });
  };
  function pushRecentCwd(c) {
    var r = []; try { r = JSON.parse(lsGet("judas.m.cwds") || "[]"); } catch (e) { /* ignore */ }
    r = [c].concat(r.filter(function (x) { return x !== c; })).slice(0, 8); lsSet("judas.m.cwds", JSON.stringify(r));
  }

  /* attachments */
  $("#attach").onclick = function () { $("#file").click(); };
  $("#file").addEventListener("change", function () {
    var files = Array.prototype.slice.call(this.files || []); this.value = "";
    files.forEach(uploadFile);
  });
  function uploadFile(f) {
    var a = { name: f.name || "file", state: "up", id: null }; S.atts.push(a); renderAtts();
    fetch("/api/attachments?name=" + encodeURIComponent(a.name), { method: "POST", headers: { "Content-Type": f.type || "application/octet-stream" }, body: f })
      .then(function (r) { return r.json().then(function (j) { if (!r.ok) throw new Error((j && j.error && (j.error.message || j.error)) || "HTTP " + r.status); return j; }); })
      .then(function (j) { a.state = "ok"; a.id = j.id; a.kind = j.kind; renderAtts(); }, function (e) { a.state = "err"; a.err = e.message; renderAtts(); });
  }
  function renderAtts() {
    var box = $("#atts"); box.textContent = ""; box.hidden = !S.atts.length;
    S.atts.forEach(function (a) {
      var p = el("span", "pill" + (a.state === "err" ? " err" : ""));
      p.appendChild(el("span", null, a.name + (a.state === "up" ? " (uploading)" : a.state === "err" ? " (failed)" : "")));
      var x = el("button", null, "×"); x.type = "button"; x.setAttribute("aria-label", "Remove " + a.name);
      x.onclick = function () { S.atts = S.atts.filter(function (z) { return z !== a; }); renderAtts(); };
      p.appendChild(x); box.appendChild(p);
    });
    renderSend();
  }

  /* queue: server-side when the session JSON exposes `queued`, else held here until the turn ends */
  function serverQueue() { return S.sess && Array.isArray(S.sess.queued); }
  function qText(q) { return typeof q === "string" ? q : (q && (q.content || q.text || q.prompt)) || "message"; }
  function renderQueue() {
    var box = $("#queue"); box.textContent = "";
    var items = (serverQueue() ? S.sess.queued.map(function (q) { return { text: qText(q), server: true }; }) : []).concat(S.queue.filter(function (q) { return q.sid === S.sid; }));
    box.hidden = S.view !== "session" || !items.length;
    items.forEach(function (q) {
      var p = el("span", "pill dimmed"); p.appendChild(el("span", null, clip(q.text.replace(/\s+/g, " "), 60)));
      if (!q.server) {
        var x = el("button", null, "×"); x.type = "button"; x.setAttribute("aria-label", "Remove queued message");
        x.onclick = function () { S.queue = S.queue.filter(function (z) { return z !== q; }); renderQueue(); };
        p.appendChild(x);
      } else p.appendChild(el("span", "dim", " ")); 
      box.appendChild(p);
    });
  }
  function flushQueue() {
    if (S.running || serverQueue()) return;
    var i = S.queue.findIndex(function (q) { return q.sid === S.sid; });
    if (i < 0) return;
    var q = S.queue.splice(i, 1)[0]; renderQueue();
    transmit(q.text, q.ids, q.names);
  }

  function send() {
    var text = input.value.trim(), ready = S.atts.filter(function (a) { return a.state === "ok"; });
    if (!text && !ready.length) return;
    if (S.atts.some(function (a) { return a.state === "up"; })) { toast("Wait for the upload to finish"); return; }
    if (S.atts.some(function (a) { return a.state === "err"; })) { toast("Remove the file that failed to send", true); return; }
    var ids = ready.map(function (a) { return a.id; }), names = ready.map(function (a) { return a.name; });
    if (S.view === "session" && S.running) {
      if (serverQueue()) { transmit(text, ids, names, true); }
      else { S.queue.push({ sid: S.sid, text: text || names.join(", "), ids: ids, names: names }); renderQueue(); }
      input.value = ""; S.atts = []; renderAtts(); fit(); renderSend(); return;
    }
    var mode = curMode();
    if (S.view === "home" && NEEDS_CWD[mode] && !S.cwd) { toast("Choose a working folder first", true); $("#folder").focus(); return; }
    input.value = ""; S.atts = []; renderAtts(); fit();
    transmit(text, ids, names);
  }
  function transmit(text, ids, names, queued) {
    var mode = curMode(), fresh = !S.sid;
    var body = { content: text, mode: mode };
    if (ids.length) body.attachments = ids;
    if (fresh && S.cwd && (NEEDS_CWD[mode] || WANTS_CWD[mode])) body.cwd = S.cwd;
    var shown = text || ""; names.forEach(function (n) { shown += (shown ? "\n" : "") + '<attachment name="' + n + '" kind="file">\n\n</attachment>'; });
    if (fresh) { showView("session"); clearLog(); S.running = true; $("#sess-title").textContent = clip(text || names[0] || "New session", 40); S.msgs = []; }
    else if (!queued) { var stay = cur && cur.stick; }
    if (!queued) { addUser(shown); beginLocalTurn(); } else { addUser(shown); }
    var url = "/api/sessions/" + encodeURIComponent(S.sid || "new") + "/messages";
    return post(url, body).then(function (r) {
      var sid = typeof r.session === "string" ? r.session : ((r.session || {}).id || S.sid);
      if (!sid) throw new Error("the server returned no session");
      if (fresh) {
        S.sid = sid; S.sess = { id: sid, title: clip(text || names[0] || "New session", 60), mode: mode, cwd: S.cwd };
        if (S.cwd && (NEEDS_CWD[mode] || WANTS_CWD[mode])) pushRecentCwd(S.cwd);
        lastRoute = JSON.stringify({ view: "session", sid: sid });
        location.hash = "#s/" + encodeURIComponent(sid);
        S.turn = { running: true, event_floor: 0 };
        sessionHeader(); startLive(); S.live.inTurn = true; S.live.turnAt = Date.now();
      } else if (S.live) { S.live.turnAt = Date.now(); if (!S.live.inTurn) { S.live.inTurn = true; } }
      setRunning(true);
    }, function (e) {
      if (S.live) S.live.inTurn = false;
      showTyping(false); setRunning(false);
      addNote("Send failed: " + e.message, true);
      if (!input.value) { input.value = text; fit(); }
      if (fresh) { S.sid = null; showView("home"); lastRoute = ""; history.replaceState(null, "", location.pathname + "#" + S.mode); route(true); input.value = text; fit(); renderSend(); }
      renderSend();
    });
  }
  function beginLocalTurn() { if (!cur) cur = newCtx(); cur.stick = true; cur.bubble = null; cur.think = null; setRunning(true); showTyping(true); toBottom(true); if (S.live) { S.live.inTurn = true; S.live.turnAt = Date.now(); } }

  /* ---------- boot ---------- */
  $("#menu-btn").setAttribute("aria-haspopup", "dialog");
  var savedMode = lsGet("judas.m.mode"); if (savedMode && MODES.some(function (m) { return m[0] === savedMode; })) S.mode = savedMode;
  initTheme();
  renderSeg();
  route(true);
  renderDock();
  pollState().then(function () { if (S.health === "off") showOffline(); });
  // the page is the whole app: an in-page reload keeps the hash, so the current screen comes back
  window.addEventListener("pageshow", function (e) { if (e.persisted) pollState(); });
})();
