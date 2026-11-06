/* Judas phone UI. A separate page from the desktop app: same JSON/SSE API (docs/API.md), no app.js.
   Routes (location.hash): "" or "#chat|cowork|code|learn|swarm|engineering" = home, "#s/<id>" = session, "#planner". */
(function () {
  "use strict";
  /* Tabs: Chat (with Code and Cowork as sub-tabs), Swarm, Learn, and Planner when the server has it on. Every list
     shows one mode only; there is no mixed view. */
  var SUBS = [["chat", "Chat"], ["code", "Code"], ["cowork", "Cowork"]];
  /* Cowork's own sub-tabs and, per sub-tab, the one-column panes (id, label); the first pane is the default */
  var CW_SUBS = [["cowork", "General"], ["engineering", "Engineering"], ["design", "Design"], ["research", "Research"], ["write", "Write"]];
  var CW_PANES = {
    engineering: [["chat", "Chat"], ["model", "Model"], ["calcs", "Calcs"]],
    design: [["canvas", "Canvas"], ["versions", "Versions"], ["chat", "Chat"]],
    research: [["report", "Report"], ["sources", "Sources"], ["chat", "Chat"]],
    write: [["doc", "Document"], ["notes", "Notes"], ["chat", "Chat"]]
  };
  function isCw(m) { return CW_SUBS.some(function (x) { return x[0] === m; }); }
  var MODE_NAMES = { chat: "Chat", code: "Code", cowork: "Cowork", swarm: "Swarm", learn: "Learn", engineering: "Engineering", design: "Design", research: "Research", write: "Write" };
  var HINTS = {
    chat: "Ask anything. Chat needs no project folder.",
    cowork: "Describe a bigger job. The lead plans it and may start workers.",
    code: "Pick a project or a folder, then say what to read or change. Every edit is snapshotted.",
    learn: "Say what you want to understand. Judas asks a few questions before teaching.",
    swarm: "Pick a project or working folder and describe the job. The lead builds teams and runs them.",
    engineering: "Pick a project or folder and describe the part with numbers. Judas calculates, models when CAD is available, and saves notes to the artifacts folder.",
    design: "Describe what to sketch. Results land on the canvas; every change adds a version.",
    research: "Ask a question. Sources are numbered as they are read and the report grows with them.",
    write: "Open a document, then write. Ask for a read; suggestions land in Notes and your text is never rewritten."
  };
  var PLACEHOLDER = { chat: "Message Judas", cowork: "Describe the job", code: "What should Judas change?", learn: "What do you want to understand?", swarm: "Describe the job for the lead", engineering: "Describe the part or problem", design: "Describe what to sketch", research: "What should it find out?", write: "Ask for a read or a check" };
  var NEEDS_CWD = { code: true, swarm: true, engineering: true, write: true }, WANTS_CWD = { cowork: true, design: true, research: true };
  /* modes that can attach to a project (Learn is a tutor, no project); Chat may, but never has to */
  var PROJECT_MODE = { chat: true, cowork: true, code: true, swarm: true, engineering: true, design: true, research: true, write: true };

  var $ = function (s) { return document.querySelector(s); };
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { if (v == null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) { /* ignore */ } }
  function reduceMotion() { return window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches; }

  var ICON = {
    menu: '<svg viewBox="0 0 24 24"><circle cx="5" cy="12" r="1.3"/><circle cx="12" cy="12" r="1.3"/><circle cx="19" cy="12" r="1.3"/></svg>',
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
    refresh: '<svg viewBox="0 0 24 24"><path d="M20 11a8 8 0 10-2.3 5.7M20 4v7h-7"/></svg>',
    cube: '<svg viewBox="0 0 24 24"><path d="M12 3l8 4.5v9L12 21l-8-4.5v-9zM12 12l8-4.5M12 12v9M12 12L4 7.5"/></svg>',
    burger: '<svg viewBox="0 0 24 24"><path d="M4 8h16M4 16h10"/></svg>',
    compose: '<svg viewBox="0 0 24 24"><path d="M12 5H6.5A2.5 2.5 0 004 7.5v10A2.5 2.5 0 006.5 20h10a2.5 2.5 0 002.5-2.5V12"/><path d="M17.4 3.6a2 2 0 012.9 2.9L12 14.8l-3.6.8.8-3.6z"/></svg>',
    plus: '<svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg>',
    t_chat: '<svg viewBox="0 0 24 24"><path d="M21 12a8 8 0 01-11.6 7.1L4 20.5l1.3-4.4A8 8 0 1121 12z"/></svg>',
    t_swarm: '<svg viewBox="0 0 24 24"><circle cx="12" cy="6" r="2.3"/><circle cx="6" cy="17" r="2.3"/><circle cx="18" cy="17" r="2.3"/><path d="M11 8.2L7 14.8M13 8.2l4 6.6M8.4 17h7.2"/></svg>',
    t_learn: '<svg viewBox="0 0 24 24"><path d="M2.5 9L12 4.5 21.5 9 12 13.5z"/><path d="M6 11.3V16c0 1.4 2.7 3 6 3s6-1.6 6-3v-4.7M21.5 9v5"/></svg>',
    t_tasks: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/></svg>',
    t_plan: '<svg viewBox="0 0 24 24"><rect x="4" y="5" width="16" height="15" rx="2.5"/><path d="M4 10h16M9 3v4M15 3v4"/></svg>'
  };
  function icon(name) { var s = el("span"); s.innerHTML = ICON[name]; return s.firstChild; }

  /* ---------- state ---------- */
  var S = {
    mode: "chat", view: "home", sid: null, sess: null, agents: [], team: null, more: false, firstId: null,
    running: false, health: "unknown", state: null, queue: [], atts: [], cwd: lsGet("judas.m.cwd") || "",
    project: lsGet("judas.m.project") || "", projects: [], dproj: "",
    approvals: null /* null = unsupported */, apprWanted: true, live: null, sessions: [], drawerN: 40,
    chatSub: "chat", planner: false, plannerKnown: false, lv: null
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
    if (!sheetOpen && !drawerOpen) { fn(); return; }
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
    var imgs = [];
    if (window.JudasRich) s = JudasRich.liftImages(s, function (h) { imgs.push(h); return "\u0003" + (imgs.length - 1) + "\u0003"; });
    s = s.replace(/\[([^\]\n]+)\]\((https?:\/\/[^\s)]+|mailto:[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>')
      .replace(/(^|[\s(])(https?:\/\/[^\s<)]+[^\s<).,;:!?])/g, '$1<a href="$2" target="_blank" rel="noopener noreferrer">$2</a>')
      .replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>")
      .replace(/(^|[^*\w])\*([^*\n]+)\*(?!\w)/g, "$1<em>$2</em>")
      .replace(/(^|[^_\w])_([^_\n]+)_(?!\w)/g, "$1<em>$2</em>");
    s = s.replace(/\u0003(\d+)\u0003/g, function (m, i) { return imgs[+i]; });
    return s.replace(/\u0001(\d+)\u0001/g, function (m, i) { return "<code>" + codes[+i] + "</code>"; });
  }
  function md(text) {
    var parts = String(text).split(/```/), out = "";
    for (var i = 0; i < parts.length; i++) {
      if (i % 2 === 1) {
        var lm = /^([^\n]*)\n/.exec(parts[i]);
        var special = lm && window.JudasRich ? JudasRich.fence(lm[1].trim(), parts[i].slice(lm[0].length)) : null;
        if (special) { out += special; continue; }
        var body = parts[i].replace(/^[^\n]*\n/, function (m) { return /^\s*[\w+#.-]*\s*\n$/.test(m) ? "" : m; });
        out += '<div class="code-wrap"><button type="button" class="code-copy">Copy</button><pre><code>' + esc(body.replace(/\n$/, "")) + "</code></pre></div>";
        continue;
      }
      var lines = parts[i].split("\n"), list = null, items = [], listHead = -1, headAt = -1, para = [], m;
      var fp = function () { if (para.length) { out += "<p>" + para.join("<br>") + "</p>"; para = []; headAt = -1; } };
      var fl = function () {
        if (!list) return;
        var allTasks = list === "ul" && items.length && items.every(function (it) { return it.task; });
        if (allTasks) {
          var done = items.filter(function (it) { return it.task.checked; }).length, bar = "";
          if (listHead >= 0) {   // a heading right above the list gets a done/total counter and a thin bar
            out = out.slice(0, listHead) + JudasRich.taskCount(done, items.length) + out.slice(listHead);
            bar = JudasRich.taskBar(done, items.length);
          }
          out += bar + '<ul class="rc-tasks">' + items.map(function (it) { return JudasRich.taskLi(it.html, it.task.checked, it.task.key); }).join("") + "</ul>";
        } else out += "<" + list + ">" + items.map(function (it) { return "<li>" + (it.orig || it.html) + "</li>"; }).join("") + "</" + list + ">";
        list = null; items = []; listHead = -1;
      };
      var addItem = function (kind, txt) {
        fp();
        if (list !== kind) { fl(); list = kind; listHead = headAt; headAt = -1; }
        var tk = kind === "ul" && window.JudasRich ? JudasRich.taskMatch(txt) : null;
        items.push({ html: inline(tk ? tk.text : txt), orig: inline(txt), task: tk });
      };
      for (var j = 0; j < lines.length; j++) {
        var ln = lines[j], tb = window.JudasRich ? JudasRich.tableAt(lines, j, inline) : null;
        if (tb) { fp(); fl(); headAt = -1; out += tb.html; j = tb.next - 1; }
        else if ((m = /^(#{1,4})\s+(.*)$/.exec(ln))) { fp(); fl(); out += "<h" + m[1].length + ">" + inline(m[2]); headAt = out.length; out += "</h" + m[1].length + ">"; }
        else if ((m = /^\s*[-*+]\s+(.*)$/.exec(ln))) addItem("ul", m[1]);
        else if ((m = /^\s*\d+[.)]\s+(.*)$/.exec(ln))) addItem("ol", m[1]);
        else if ((m = /^>\s?(.*)$/.exec(ln))) { fp(); fl(); headAt = -1; out += "<blockquote>" + inline(m[1]) + "</blockquote>"; }
        else if (/^\s*([-*_])\1{2,}\s*$/.test(ln)) { fp(); fl(); headAt = -1; out += "<hr>"; }
        else if (ln.trim() === "") { fp(); fl(); }
        else { fl(); headAt = -1; para.push(inline(ln)); }
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
    var d = el("div", "msg user"), rest = String(t), chips = [], pics = [];
    rest = rest.replace(ATT_RE, function (m, a) {
      var id = /id="([0-9a-fA-F]+)"/.exec(a), k = /kind="([^"]*)"/.exec(a);
      if (id && k && k[1] === "image") pics.push({ id: id[1], name: attName(a) }); else chips.push(attName(a));
      return "";
    }).replace(IMG_RE, function (m, id, n) { pics.push({ id: id, name: n || "image" }); return ""; });
    rest = rest.replace(/^\n+|\n+$/g, "");
    if (pics.length) {
      var g = el("div", "att-pics");
      pics.forEach(function (p) { g.appendChild(thumb("/api/attachments/" + encodeURIComponent(p.id), p.name)); });
      d.appendChild(g);
    }
    if (rest) d.appendChild(el("div", null, rest));
    chips.forEach(function (c) { d.appendChild(el("span", "att", c)); });
    return d;
  }
  function thumb(src, name) {
    var b = el("button", "rc-img att-pic"); b.type = "button"; b.setAttribute("data-full", src); b.title = name || "image";
    b.setAttribute("aria-label", "Open " + (name || "image"));
    var im = el("img"); im.alt = name || ""; im.loading = "lazy"; im.decoding = "async"; im.src = src; b.appendChild(im);
    return b;
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
    $("#dot").className = "dot " + dot;
    var t = h === "off" ? "offline" : !st ? "..." : !st.loaded ? "no model" : st.active > 0 ? "busy" : "idle";
    $("#menu-btn").setAttribute("aria-label", "Menu, server " + t);
  }
  function pollState() {
    return get("/state", { timeout: 6000 }).then(function (st) {
      var was = S.health === "off";
      setHealth("ok", st); netFails = 0;
      if (was || !$("#offline").hidden) { hideOffline(); route(true); checkPlanner(); }
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
  setInterval(function () { if (!document.hidden) pollState(); }, 5000);
  document.addEventListener("visibilitychange", function () { if (!document.hidden) pollState(); });
  window.addEventListener("online", function () { pollState(); });

  /* host bridge (Android wrapper): pull to refresh only at the top of the active scroller */
  function host() { return window.JudasHost || null; }
  function reportScroll() {
    var h = host(); if (!h || !h.setAtTop) return;
    var sc = S.view === "session" ? $("#log") : null;
    try { h.setAtTop(S.view === "home" || (!!sc && sc.scrollTop <= 0)); } catch (e) { /* ignore */ }
  }
  $("#log").addEventListener("scroll", reportScroll, { passive: true });

  /* ---------- routing ---------- */
  function parseHash() {
    var h = (location.hash || "").replace(/^#/, "");
    if (h === "planner") return { view: "planner" };
    if (h === "visual") return { view: "visual" };
    var m = /^s\/(.+)$/.exec(h);
    if (m) return { view: "session", sid: decodeURIComponent(m[1]) };
    if (MODE_NAMES[h]) return { view: "home", mode: h };
    return { view: "home" };
  }
  function go(hash) { if (location.hash === hash) route(true); else location.hash = hash; }
  function route(force) {
    var r = parseHash(), key = JSON.stringify(r);
    if (!force && key === lastRoute) return;
    lastRoute = key;
    if (r.view === "planner" && !S.planner && S.plannerKnown) { go("#chat"); return; }
    if (r.view === "session") { showView("session"); if (S.sid !== r.sid || !S.sess || force === true) openSession(r.sid); }
    else if (r.view === "planner") { showView("planner"); openPlanner(); }
    else if (r.view === "visual") { if (S.lvKnown && !(S.lv && S.lv.url)) { go("#learn"); return; } S.mode = "learn"; showView("visual"); openVisual(); }
    else { S.mode = r.mode || (S.view === "home" ? S.mode : "chat"); showView("home"); renderHome(); }
  }
  window.addEventListener("hashchange", function () { route(); });
  window.addEventListener("popstate", function () {
    if (artView) { closeArtifact(true); return; }
    if (sheetOpen) { hideSheet(); return; }
    if (drawerOpen) { hideDrawer(); return; }
    route();
  });
  function tabOf(mode) { return mode === "swarm" ? "swarm" : mode === "learn" ? "learn" : "chat"; }
  function showView(v) {
    S.view = v;
    $("#v-home").hidden = v !== "home"; $("#v-sess").hidden = v !== "session"; $("#v-plan").hidden = v !== "planner"; $("#v-vis").hidden = v !== "visual";
    if (v !== "session") { stopLive(); S.sid = null; S.sess = null; $("#approvals").hidden = true; $("#queue").hidden = true; $("#agents-chip").hidden = true; $("#jump").hidden = true; }
    if (v !== "visual") stopVisualPoll();
    renderBar(); renderTabs(); renderDock(); reportScroll();
    if (v !== "session") { S.lastCtx = 0; setCtxMeter(0); }
    if (v === "planner" || v === "visual") { try { host() && host().setAtTop && host().setAtTop(false); } catch (e) { /* ignore */ } }
  }

  /* ---------- top bar + tabs ---------- */
  function pills(items, cur, fn) {
    var box = el("div", "pills"); box.setAttribute("role", "tablist");
    items.forEach(function (it) {
      var b = el("button", null, it[1]); b.type = "button"; b.setAttribute("role", "tab");
      b.setAttribute("aria-selected", it[0] === cur ? "true" : "false");
      b.onclick = function () { fn(it[0]); };
      box.appendChild(b);
    });
    return box;
  }
  function renderBar() {
    var mid = $("#bar-mid"); mid.textContent = "";
    var v = S.view;
    if (v === "home" && tabOf(S.mode) === "chat") mid.appendChild(pills(SUBS, isCw(S.mode) ? "cowork" : S.mode, function (m) { go("#" + (m === "cowork" ? cwLast() : m)); }));
    else if ((v === "home" || v === "visual") && S.mode === "learn" && S.lv && S.lv.url)
      mid.appendChild(pills([["chat", "Tutor"], ["visual", S.lv.label || "Visual"]], v === "visual" ? "visual" : "chat", function (k) { go(k === "visual" ? "#visual" : "#learn"); }));
    else if (v === "session") {
      var t = el("button", "title", S.sess ? (S.sess.title || "New session") : (S.titleHint || "Loading...")); t.type = "button";
      t.id = "sess-title"; t.setAttribute("aria-label", "Conversation options");
      t.onclick = function () { if (S.sess) sessionMenu(); };
      mid.appendChild(t);
    } else mid.appendChild(el("h1", "title static", v === "planner" ? "Planner" : MODE_NAMES[S.mode] || "Judas"));
    $("#visual-chip").hidden = !(v === "session" && S.mode === "learn" && S.lv && S.lv.url);
    $("#drawer-btn").style.visibility = v === "planner" || v === "visual" ? "hidden" : "";
  }
  function cwLast() { var v = lsGet("judas.coworkSub"); return isCw(v) ? v : "cowork"; }

  /* ---------- Cowork sub-tabs: pill row, pane switch and the stage (web/cowork/cowork.js) ---------- */
  var CW = { sid: "", mode: "", pane: "" };
  function cwActive() { return isCw(S.mode) && (S.view === "home" || S.view === "session"); }
  function cwPane(p) { CW.pane = p; cwApply(); }
  function cwApply() {
    var bar = $("#cw-sub"), on = cwActive();
    bar.hidden = !on;
    var stage = $("#cw-stage");
    if (!on) { $("#v-cw").hidden = true; return; }
    var panes = CW_PANES[S.mode];
    if (S.mode !== "cowork") lsSet("judas.coworkSub", S.mode);
    else if (lsGet("judas.coworkSub") !== "cowork") lsSet("judas.coworkSub", "cowork");
    if (panes && !panes.some(function (x) { return x[0] === CW.pane; })) CW.pane = panes[0][0];
    bar.textContent = "";
    var row = pills(CW_SUBS, S.mode, function (m) { go("#" + m); }); row.classList.add("cw-subpills");
    bar.appendChild(row);
    if (panes) {
      var seg = pills(panes, CW.pane, cwPane); seg.classList.add("cw-seg");
      bar.appendChild(seg);
    }
    var showStage = !!panes && CW.pane !== "chat";
    $("#v-cw").hidden = !showStage;
    $("#v-home").hidden = showStage || S.view !== "home";
    $("#v-sess").hidden = showStage || S.view !== "session";
    $("#dock").hidden = showStage;
    stage.dataset.pane = CW.pane;
    if (window.JudasCowork) {
      var sid = S.view === "session" ? (S.sid || "") : "";
      JudasCowork.mode(S.mode);
      if (sid !== CW.sid || CW.mode !== S.mode) { CW.sid = sid; CW.mode = S.mode; if (JudasCowork.mode && panes) JudasCowork.session(sid); }
    }
  }
  function cwEnsureSession() {
    if (S.sid) return Promise.resolve(S.sid);
    var body = { mode: S.mode };
    if (S.project) body.project = S.project; else if (S.cwd) body.cwd = S.cwd;
    return post("/api/sessions", body).then(function (r) {
      var id = typeof r === "string" ? r : (r.session && r.session.id) || r.id;
      if (!id) throw new Error("server returned no session");
      go("#s/" + encodeURIComponent(id));
      return new Promise(function (ok, bad) {
        var n = 0, t = setInterval(function () { if (S.sid === id) { clearInterval(t); ok(id); } else if (++n > 40) { clearInterval(t); bad(new Error("could not open the new session")); } }, 100);
      });
    });
  }
  if (window.JudasCowork) JudasCowork.init({
    phone: true, stage: $("#cw-stage"), root: document.body,
    getJSON: function (u) { return get(u); }, md: function (t) { return md(t); }, input: $("#input"),
    sid: function () { return S.view === "session" ? (S.sid || "") : ""; },
    mode: function () { return S.mode; },
    cwd: function () { var pj = S.project ? projectById(S.project) : null; return (S.sess && S.sess.cwd) || (pj && pj.folder) || S.cwd || ""; },
    ensureSession: cwEnsureSession, pane: cwPane
  });
  function renderTabs() {
    cwApply();
    var nav = $("#tabbar"); nav.textContent = "";
    var cur = S.view === "planner" ? "planner" : tabOf(S.view === "session" && S.sess ? modeOf(S.sess) : S.mode);
    var items = [["chat", "Chat", "t_chat"], ["swarm", "Swarm", "t_swarm"], ["learn", "Learn", "t_learn"]];
    items.push(["tasks", "Tasks", "t_tasks"]);
    if (S.planner) items.push(["planner", "Planner", "t_plan"]);
    items.forEach(function (it) {
      var b = el("button"); b.type = "button"; b.appendChild(icon(it[2])); b.appendChild(el("span", null, it[1]));
      if (it[0] === cur) b.setAttribute("aria-current", "page");
      b.onclick = function () {
        if (it[0] === "tasks") { openTasks(); return; }  // tasks open as a sheet over whatever is on screen
        if (it[0] === cur && S.view === "home") return;
        go(it[0] === "planner" ? "#planner" : it[0] === "chat" ? "#" + (tabOf(S.mode) === "chat" ? S.mode : S.chatSub) : "#" + it[0]);
      };
      nav.appendChild(b);
    });
  }
  function checkPlanner() {
    return get("/api/mach12", { timeout: 6000 }).then(function (r) {
      var on = !!(r && r.available && r.enabled);
      S.plannerKnown = true;
      if (on !== S.planner) { S.planner = on; renderTabs(); if (!on && S.view === "planner") go("#chat"); }
    }, function (e) { if (!e.net) { S.plannerKnown = true; if (S.planner) { S.planner = false; renderTabs(); } } });
  }

  /* ---------- home (a new conversation) + drawer ---------- */
  function renderHome() {
    if (tabOf(S.mode) === "chat") S.chatSub = S.mode;
    $("#home-title").textContent = MODE_NAMES[S.mode];
    $("#home-hint").textContent = HINTS[S.mode];
    renderBar(); renderTabs(); renderDock();
    mountSuggestions();
    if (drawerOpen) loadSessions();
  }
  /* suggested prompts on the empty home view (web/suggestions.js) */
  var sgCtl = null;
  function mountSuggestions() {
    var host = $("#home-sg");
    if (!host || !window.Suggest) return;
    if (sgCtl && sgCtl.el.parentNode === host) { sgCtl.setMode(S.mode); }
    else { sgCtl = Suggest.mount(host, { mode: S.mode, input: $("#input"), count: 3 }); }
    host.hidden = S.mode === "swarm";
  }
  function modeOf(s) { return s.mode || "chat"; }
  var sessReq = 0;
  function loadSessions() {
    var req = ++sessReq, t0 = Date.now();
    return get("/api/sessions?limit=120").then(function (r) {
      if (req !== sessReq) return;
      var list = r.sessions || [];
      /* sessions the feed announced while this request was in flight may be missing from its snapshot */
      Object.keys(feedTouched).forEach(function (id) {
        var t = feedTouched[id];
        if (t.at >= t0 && !list.some(function (x) { return x.id === id; })) list.push(t.s);
      });
      S.sessions = list; drawDrawer();
    }, function (e) { if (e.net) return; toast("Could not load sessions: " + e.message, true); });
  }
  /* the global feed keeps the drawer current: sessions started elsewhere (scripts, the API, the desktop)
     appear without reopening it. Falls back to polling while the stream is down. */
  var feed = { es: null, poll: null, retry: null }, feedTouched = {};
  function feedSession(s) {
    if (!s || !s.id) return;
    feedTouched[s.id] = { at: Date.now(), s: s };
    var i = -1;
    S.sessions.forEach(function (x, n) { if (x.id === s.id) i = n; });
    if (i >= 0) S.sessions[i] = Object.assign({}, S.sessions[i], s); else S.sessions.push(s);
    S.sessions.sort(function (a, b) { return (b.updated_ms || 0) - (a.updated_ms || 0); });
    if (S.sess && S.sess.id === s.id && s.title) S.sess.title = s.title;
  }
  function connectFeed() {
    if (!window.EventSource) { startFeedPoll(); return; }
    var es = new EventSource("/api/events");
    feed.es = es;
    es.onopen = function () { stopFeedPoll(); loadSessions(); };
    es.onmessage = function (m) {
      var ev;
      try { ev = JSON.parse(m.data); } catch (e) { return; }
      if (ev.type === "session.created" || ev.type === "session.updated") {
        feedSession(ev.session); drawCards();
      } else if (ev.type === "session.deleted") {
        delete feedTouched[ev.id];
        S.sessions = S.sessions.filter(function (x) { return x.id !== ev.id; }); drawCards();
      } else if (ev.type === "turn.started" || ev.type === "turn.finished") {
        var started = ev.type === "turn.started", known = false;
        S.sessions.forEach(function (x) { if (x.id === ev.session) { x.running = started; known = true; } });
        if (known) drawCards(); else loadSessions();
      } else if (ev.type === "artifact.render") {
        if (window.JudasArtifactCapture) JudasArtifactCapture.onFeed(ev);
      } else if (ev.type === "gap") {
        loadSessions();
      }
    };
    es.onerror = function () {
      es.close(); feed.es = null; startFeedPoll();
      clearTimeout(feed.retry); feed.retry = setTimeout(connectFeed, 5000);
    };
  }
  function startFeedPoll() { if (!feed.poll) feed.poll = setInterval(function () { if (drawerOpen) loadSessions(); }, 5000); }
  function stopFeedPoll() { clearInterval(feed.poll); feed.poll = null; }
  var drawerOpen = false, drawerFocus = null;
  function openDrawer() {
    if (drawerOpen) return;
    drawerOpen = true; drawerFocus = document.activeElement;
    S.dq = ""; S.dhits = null; $("#d-q").value = ""; $("#d-clear").hidden = true;
    $("#drawer").hidden = false; $("#drawer-bg").hidden = false;
    history.pushState({ drawer: 1 }, "");
    drawDrawer(); loadSessions();
    S.drawerN = 40;
  }
  function hideDrawer() {
    drawerOpen = false; $("#drawer").hidden = true; $("#drawer-bg").hidden = true;
    if (drawerFocus && drawerFocus.focus) { try { drawerFocus.focus(); } catch (e) { /* ignore */ } }
  }
  function closeDrawer() { if (drawerOpen) history.back(); }
  function dayGroup(ms) {
    var d = new Date(ms), n = new Date(), t0 = new Date(n.getFullYear(), n.getMonth(), n.getDate()).getTime();
    if (ms >= t0) return "Today";
    if (ms >= t0 - 86400000) return "Yesterday";
    if (ms >= t0 - 7 * 86400000) return "Previous 7 days";
    return "Older";
  }
  function drawDrawer() {
    var nc = $("#new-chat"); nc.textContent = ""; nc.appendChild(icon("compose")); nc.appendChild(el("span", null, "New chat"));
    var box = $("#d-list"); box.textContent = "";
    var q = (S.dq || "").trim(), ql = q.toLowerCase();
    $("#d-head").textContent = q ? "Search results" : MODE_NAMES[S.mode] + " history";
    var dp = $("#d-proj");
    dp.hidden = !S.projects.length;
    if (dp.hidden) S.dproj = "";
    else {
      dp.textContent = "";
      var all = el("option", null, "All projects"); all.value = ""; dp.appendChild(all);
      S.projects.forEach(function (p) { var o = el("option", null, p.name); o.value = p.id; dp.appendChild(o); });
      if (S.dproj && !projectById(S.dproj)) S.dproj = "";
      dp.value = S.dproj;
    }
    var list = S.sessions.filter(function (s) {
      if (S.dproj && s.project_id !== S.dproj) return false;
      return q ? (s.title || "Untitled").toLowerCase().indexOf(ql) >= 0 : modeOf(s) === S.mode;
    });
    var shown = list.slice(0, S.drawerN || 40);
    $("#d-more").hidden = list.length <= shown.length;
    if (!list.length && !q) box.appendChild(el("p", "d-none", "Nothing here yet."));
    var lastG = "";
    if (q && list.length) box.appendChild(el("div", "d-group", "Titles"));
    shown.forEach(function (s) {
      var g = dayGroup(s.updated_ms || 0);
      if (!q && g !== lastG) { box.appendChild(el("div", "d-group", g)); lastG = g; }
      var row = el("div", "d-row" + (S.view === "session" && S.sid === s.id ? " on" : ""));
      var b = el("button", "d-open"); b.type = "button";
      b.appendChild(el("span", "t", s.title || "Untitled"));
      var pn = s.project_name || (s.project_id && projectById(s.project_id) ? projectById(s.project_id).name : "");
      if (pn) b.appendChild(el("span", "d-tag", pn));
      if (s.running) b.appendChild(el("span", "dot busy"));
      b.onclick = function () { afterClose(function () { go("#s/" + encodeURIComponent(s.id)); }); };
      var mo = el("button", "d-more-btn"); mo.type = "button"; mo.setAttribute("aria-label", "Options for " + (s.title || "session"));
      mo.appendChild(icon("menu")); mo.onclick = function () { sessionActions(s); };
      row.appendChild(b); row.appendChild(mo); box.appendChild(row);
    });
    if (q) drawHits(box, q, list);
  }
  /* message hits from the server search; sessions already listed by title are skipped */
  function snippetNode(sn) {
    var n = el("span", "sn"), re = /\[([^\]\n]*)\]/g, last = 0, m;
    while ((m = re.exec(sn))) {
      if (m.index > last) n.appendChild(document.createTextNode(sn.slice(last, m.index)));
      n.appendChild(el("mark", null, m[1])); last = re.lastIndex;
    }
    if (last < sn.length) n.appendChild(document.createTextNode(sn.slice(last)));
    return n;
  }
  function drawHits(box, q, titled) {
    var ids = {}; titled.forEach(function (s) { ids[s.id] = 1; });
    var hits = (S.dhits && S.dhits.q === q ? S.dhits.list : []).filter(function (h) { return !ids[h.session]; });
    if (S.dhits && S.dhits.q === q && S.dhits.pending && !hits.length) box.appendChild(el("p", "d-none", "Searching messages..."));
    if (hits.length) box.appendChild(el("div", "d-group", "In messages"));
    hits.forEach(function (h) {
      var row = el("div", "d-row d-hit" + (S.view === "session" && S.sid === h.session ? " on" : ""));
      var b = el("button", "d-open"); b.type = "button";
      b.appendChild(el("span", "t", h.title || "Untitled"));
      b.appendChild(snippetNode(h.snippet || ""));
      b.onclick = function () { afterClose(function () { S.hl = q; go("#s/" + encodeURIComponent(h.session)); highlightSoon(q); }); };
      row.appendChild(b); box.appendChild(row);
    });
    if (!hits.length && !titled.length && !(S.dhits && S.dhits.pending)) box.appendChild(el("p", "d-none", "No matches."));
  }
  /* after opening a hit, scroll to the first message containing the query (best effort, up to ~3 s while history loads) */
  function highlightSoon(q) {
    var tries = 0, ql = q.toLowerCase();
    var t = setInterval(function () {
      tries++;
      var msgs = document.querySelectorAll("#log .msg"), hit = null;
      for (var i = 0; i < msgs.length && !hit; i++) if ((msgs[i].textContent || "").toLowerCase().indexOf(ql) >= 0) hit = msgs[i];
      if (hit) { clearInterval(t); try { hit.scrollIntoView({ block: "center" }); } catch (e) { /* ignore */ } hit.classList.add("hl-hit"); setTimeout(function () { hit.classList.remove("hl-hit"); }, 2400); }
      else if (tries > 10) clearInterval(t);
    }, 300);
  }
  var dqTimer = null, dqReq = 0;
  function onSearchInput() {
    var q = $("#d-q").value; S.dq = q; $("#d-clear").hidden = !q;
    clearTimeout(dqTimer);
    var t = q.trim();
    if (t.length < 2) { S.dhits = null; drawDrawer(); return; }
    S.dhits = { q: t, list: [], pending: true }; drawDrawer();   // titles filter instantly
    dqTimer = setTimeout(function () {
      var req = ++dqReq;
      get("/api/sessions/search?limit=30&q=" + encodeURIComponent(t)).then(function (r) {
        if (req !== dqReq || (S.dq || "").trim() !== t) return;
        S.dhits = { q: t, list: (r && r.hits) || [] }; drawDrawer();
      }, function () {   // 404 on an older server or offline: the title filter still works
        if (req !== dqReq) return;
        S.dhits = { q: t, list: [] }; drawDrawer();
      });
    }, 250);
  }
  $("#d-q").addEventListener("input", onSearchInput);
  $("#d-proj").addEventListener("change", function () { S.dproj = this.value; drawDrawer(); });
  $("#d-clear").onclick = function () { $("#d-q").value = ""; onSearchInput(); $("#d-q").focus(); };
  function drawCards() { if (drawerOpen) drawDrawer(); }
  $("#drawer-btn").appendChild(icon("burger"));
  $("#drawer-btn").onclick = openDrawer;
  $("#drawer-bg").onclick = closeDrawer;
  $("#d-more").onclick = function () { S.drawerN = (S.drawerN || 40) + 40; drawDrawer(); };
  $("#new-chat").onclick = function () { afterClose(function () { S.sid = null; go("#" + S.mode); setTimeout(function () { $("#input").focus(); }, 60); }); };
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
        s.title = v; if (S.sess && S.sess.id === s.id) { S.sess.title = v; renderBar(); }
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

  /* ---------- location: "Share my location with Judas" (web/location.js) ---------- */
  function locationRow(box) {
    var L = window.JudasLocation;
    if (!L) return;
    box.appendChild(el("div", "s-label", "Location"));
    var row = el("div", "opts"), b = el("button", null, "Share my location with Judas"); b.type = "button";
    b.setAttribute("aria-pressed", String(L.enabled())); row.appendChild(b); box.appendChild(row);
    var st = el("p", "sub"); st.style.margin = "8px 0 0"; box.appendChild(st);
    function refresh() { L.status().then(function (s) { st.textContent = L.describe(s); st.style.color = ""; }); }
    b.onclick = function () {
      b.disabled = true;
      (L.enabled() ? L.disable() : L.enable()).then(function () { b.disabled = false; b.setAttribute("aria-pressed", String(L.enabled())); refresh(); },
        function (e) { b.disabled = false; b.setAttribute("aria-pressed", "false"); st.textContent = e.message; st.style.color = "var(--error)"; });
    };
    refresh();
  }

  /* ---------- menu ---------- */
  function openMenu() {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Judas"));
      var st = S.state;
      sh.appendChild(el("p", "sub", S.health === "off" ? "Server offline" : st ? (st.model || "no model") + (st.loaded ? "" : " (not loaded)") + (st.version ? " · v" + st.version : "") : "Checking..."));
      var box = el("div", "sbody"); sh.appendChild(box);
      sItem(box, "refresh", "Reload", function () { location.reload(); });
      sItem(box, "cal", "Automated tasks", function () { openTasks(); });
      sItem(box, "cube", "Artifacts", function () { openArtifacts(); });
      sItem(box, "desk", "Open desktop version", function () { location.href = "/"; });
      if (host() && host().openSettings) sItem(box, "gear", "App settings", function () { afterClose(function () { host().openSettings(); }); });
      locationRow(box);
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
      fontPicker(box);
    });
  }
  /* Same catalog and the same storage keys as Settings > Fonts on the desktop (web/fonts.js), so a choice follows the device. */
  function fontPicker(box) {
    var F = window.JudasFonts;
    if (!F || !F.ui) return;
    [["ui", "UI font", "judas.font.ui", F.ui], ["mono", "Mono font", "judas.font.mono", F.mono]].forEach(function (p) {
      box.appendChild(el("div", "s-label", p[1]));
      var sel = el("select", "fsel"); sel.setAttribute("aria-label", p[1]);
      var cur = lsGet(p[2]) || "";
      var opts = [["", "Theme default"], ["system", "System default"]].concat(p[3].map(function (f) { return [f.id, f.name]; }));
      if (cur.indexOf("custom:") === 0) opts.push([cur, cur.slice(7) + " (installed)"]);
      opts.forEach(function (o) {
        var op = el("option", null, o[1]); op.value = o[0]; if (o[0] === cur) op.selected = true;
        var st = F.stackFor ? F.stackFor(p[0], o[0]) : null; if (st) op.style.fontFamily = st;
        sel.appendChild(op);
      });
      sel.onchange = function () { lsSet(p[2], sel.value || null); F.apply(); };
      box.appendChild(sel);
    });
  }
  /* ---------- automated tasks (same /api/tasks as the desktop Tasks tab) ---------- */
  var UNITS = [["minutes", 60], ["hours", 3600], ["days", 86400]];
  function pad2(n) { return (n < 10 ? "0" : "") + n; }
  function dtLocal(ms) { var d = new Date(ms); return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate()) + "T" + pad2(d.getHours()) + ":" + pad2(d.getMinutes()); }
  function whenText(ms) {
    if (!ms) return "-";
    var d = new Date(ms), same = d.toDateString() === new Date().toDateString();
    var t = d.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    return same ? "today " + t : d.toLocaleDateString([], { weekday: "short", month: "short", day: "numeric" }) + " " + t;
  }
  function everyText(sec) {
    for (var i = UNITS.length - 1; i >= 0; i--) if (sec && sec % UNITS[i][1] === 0) { var n = sec / UNITS[i][1]; return "every " + (n === 1 ? UNITS[i][0].replace(/s$/, "") : n + " " + UNITS[i][0]); }
    return "every " + sec + " s";
  }
  function schedText(t) {
    if (t.kind === "once") return (t.enabled ? "Once, " : "Ran once, ") + whenText(t.run_at_ms);
    if (t.kind === "interval") return "Repeats " + everyText(t.interval_s) + (t.interval_s % 86400 === 0 && t.next_ms ? " at " + new Date(t.next_ms).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" }) : "") + (t.enabled && t.next_ms ? ", next " + whenText(t.next_ms) : t.enabled ? "" : " (paused)");
    return "Manual: runs when you tap Run now";
  }
  function openTasks() {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Automated tasks"));
      sh.appendChild(el("p", "sub", "Saved prompts Judas runs on a schedule while it is up. Runs show up as Cowork sessions."));
      var box = el("div", "sbody"); sh.appendChild(box);
      var nb = el("button", "primary t-new", "New task"); nb.type = "button"; nb.onclick = function () { taskForm({}); };
      box.appendChild(nb);
      var list = el("div", "t-list"); list.appendChild(el("p", "dim", "Loading...")); box.appendChild(list);
      get("/api/tasks").then(function (r) {
        list.textContent = "";
        var ts = ((r && r.tasks) || []).slice().sort(function (a, b) { return (b.created_ms || 0) - (a.created_ms || 0); });
        if (!ts.length) { list.appendChild(el("p", "dim", "No tasks yet.")); return; }
        ts.forEach(function (t) { list.appendChild(taskCard(t)); });
      }, function (e) { list.textContent = ""; list.appendChild(el("p", "dim", "Could not load tasks: " + e.message)); });
    });
  }
  /* ---------- artifacts: list sheet, full-screen viewer in a sandboxed iframe, cards under tool results ---------- */
  var artView = null;
  var ART_KIND = { html: "Web page", svg: "Diagram", markdown: "Document", model3d: "3D model", image: "Image" };
  function openArtifacts() {
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Artifacts"));
      sh.appendChild(el("p", "sub", "Pages, documents, diagrams and 3D views Judas made. Every change is kept as a version."));
      var box = el("div", "sbody"); sh.appendChild(box);
      box.appendChild(el("p", "dim", "Loading..."));
      get("/api/artifacts").then(function (r) {
        box.textContent = "";
        var list = (r && r.artifacts) || [];
        if (!list.length) { box.appendChild(el("p", "dim", "Nothing yet. Ask Judas to make a page, a diagram or a 3D view.")); return; }
        list.forEach(function (m) {
          sItem(box, "cube", m.title, function () { afterClose(function () { viewArtifact(m.id, m.latest); }); }, "", (ART_KIND[m.kind] || m.kind) + " v" + m.latest);
        });
        if (window.JudasArtifactCapture) JudasArtifactCapture.ensure(list);
      }, function (e) { box.textContent = ""; box.appendChild(el("p", "dim", "Could not load artifacts: " + e.message)); });
    });
  }
  function viewArtifact(id, ver) {
    get("/api/artifacts/" + encodeURIComponent(id)).then(function (m) {
      closeArtifact(false);
      var v = el("div", "art-full"); v.setAttribute("role", "dialog"); v.setAttribute("aria-label", m.title);
      var bar = el("div", "art-fbar");
      var back = el("button", "icon-btn", ""); back.type = "button"; back.setAttribute("aria-label", "Back"); back.textContent = "\u2190";
      back.onclick = function () { history.back(); };
      bar.appendChild(back);
      bar.appendChild(el("b", null, m.title));
      var sel = el("select"); sel.setAttribute("aria-label", "Version");
      m.versions.slice().reverse().forEach(function (x) { var o = el("option", null, "v" + x.n + (x.note ? " " + clip(x.note, 30) : "")); o.value = x.n; sel.appendChild(o); });
      sel.value = String(ver || m.latest);
      bar.appendChild(sel);
      var tab = el("a", null, "Tab"); tab.target = "_blank"; tab.rel = "noopener";
      bar.appendChild(tab);
      v.appendChild(bar);
      var fr = el("iframe"); fr.setAttribute("sandbox", "allow-scripts"); fr.setAttribute("title", m.title);
      v.appendChild(fr);
      function load() { var u = "/a/" + encodeURIComponent(m.id) + "/v" + sel.value + "/"; fr.src = u; tab.href = u; }
      sel.onchange = load;
      load();
      document.body.appendChild(v);
      artView = v;
      history.pushState({ art: 1 }, "");
    }, function (e) { toast("Could not open it: " + e.message, true); });
  }
  function closeArtifact(fromPop) {
    if (!artView) return;
    artView.remove(); artView = null;
    if (!fromPop && history.state && history.state.art) history.back();
  }
  function artCards(output) {
    var out = [], re = /\[\[artifact:([a-z0-9]+)\|([^|\]]*)\|([a-z0-9]+)\|(\d+)\]\]/g, m;
    var txt = String(output || "");
    while ((m = re.exec(txt))) {
      (function (id, title, kind, ver) {
        var c = el("button", "art-chat"); c.type = "button";
        var th = el("div", "art-thumb");
        var img = new Image(); img.alt = "";
        img.onload = function () { th.appendChild(img); };
        img.src = "/api/artifacts/" + encodeURIComponent(id) + "/thumb?v=" + ver;
        c.appendChild(th);
        var meta = el("div", "art-meta");
        meta.appendChild(el("b", null, title));
        meta.appendChild(el("span", null, (ART_KIND[kind] || kind) + " v" + ver));
        c.appendChild(meta);
        c.onclick = function () { viewArtifact(id, +ver); };
        out.push(c);
      })(m[1], m[2], m[3], m[4]);
    }
    return out;
  }
  function taskCard(t) {
    var c = el("div", "t-card" + (t.enabled ? "" : " off"));
    var hd = el("div", "t-hd");
    hd.appendChild(el("b", null, t.title || clip(t.prompt || t.id, 60)));
    var sw = el("button", "t-sw"); sw.type = "button"; sw.setAttribute("role", "switch");
    sw.setAttribute("aria-checked", t.enabled ? "true" : "false"); sw.setAttribute("aria-label", (t.enabled ? "Pause " : "Enable ") + (t.title || "task"));
    sw.onclick = function () { post("/api/tasks/" + encodeURIComponent(t.id) + "/toggle").then(openTasks, function (e) { toast("Could not change it: " + e.message, true); }); };
    hd.appendChild(sw); c.appendChild(hd);
    c.appendChild(el("div", "t-sched", schedText(t)));
    var pr = el("div", "t-pr", clip(t.prompt || "", 220)); pr.setAttribute("role", "button"); pr.tabIndex = 0;
    pr.onclick = function () { taskForm(t); };
    c.appendChild(pr);
    if (t.last_run_ms) c.appendChild(el("div", "t-last", "Last run " + whenText(t.last_run_ms)));
    var row = el("div", "t-acts");
    var run = el("button", "plain", "Run now"); run.type = "button";
    run.onclick = function () {
      run.disabled = true;
      post("/api/tasks/" + encodeURIComponent(t.id) + "/run").then(function (r) {
        var sid = typeof r.session === "string" ? r.session : (r.session && r.session.id) || "";
        afterClose(function () { if (sid) go("#s/" + encodeURIComponent(sid)); else toast("Started"); });
      }, function (e) { run.disabled = false; toast("Run failed: " + e.message, true); });
    };
    row.appendChild(run);
    var ed = el("button", "plain", "Edit"); ed.type = "button"; ed.onclick = function () { taskForm(t); };
    row.appendChild(ed);
    if (t.last_session) {
      var op = el("button", "plain", "Open last"); op.type = "button";
      op.onclick = function () { afterClose(function () { go("#s/" + encodeURIComponent(t.last_session)); }); };
      row.appendChild(op);
    }
    var del = el("button", "plain t-del", "Delete"); del.type = "button";
    del.onclick = function () {
      if (del.dataset.armed) api("DELETE", "/api/tasks/" + encodeURIComponent(t.id)).then(openTasks, function (e) { toast("Delete failed: " + e.message, true); });
      else { del.dataset.armed = "1"; del.textContent = "Tap again"; setTimeout(function () { delete del.dataset.armed; del.textContent = "Delete"; }, 3000); }
    };
    row.appendChild(del);
    c.appendChild(row);
    return c;
  }
  function taskForm(pre) {
    openSheet(function (sh) {
      var editing = !!pre.id;
      sh.appendChild(el("h2", null, editing ? "Edit task" : "New task"));
      var box = el("div", "sbody"); sh.appendChild(box);
      function field(label, node) { box.appendChild(el("div", "s-label", label)); box.appendChild(node); return node; }
      var title = field("Name (optional)", el("input", "s-field")); title.type = "text"; title.value = pre.title || ""; title.placeholder = "Morning briefing";
      var prompt = field("What should Judas do?", el("textarea", "s-field t-area")); prompt.rows = 5; prompt.value = pre.prompt || "";
      prompt.placeholder = "Look at today's mach12 plan and send me a short summary on Matrix.";
      var kind = editing ? (pre.kind || "manual") : "interval";
      box.appendChild(el("div", "s-label", "When"));
      var kinds = el("div", "opts"); box.appendChild(kinds);
      var onceBox = el("div", "t-sub"), intBox = el("div", "t-sub"); box.appendChild(onceBox); box.appendChild(intBox);
      var when = el("input", "s-field"); when.type = "datetime-local"; when.value = dtLocal(Date.now() + 3600 * 1000); onceBox.appendChild(when);
      var erow = el("div", "t-every"); intBox.appendChild(erow);
      erow.appendChild(el("span", null, "Every"));
      var every = el("input", "s-field"); every.type = "number"; every.min = "1"; every.inputMode = "numeric"; every.value = "1"; erow.appendChild(every);
      var unit = el("select", "fsel"); UNITS.forEach(function (u) { var o = el("option", null, u[0]); o.value = String(u[1]); if (u[1] === 86400) o.selected = true; unit.appendChild(o); }); erow.appendChild(unit);
      intBox.appendChild(el("div", "s-label", "Starting at"));
      var start = el("input", "s-field t-start"); start.type = "datetime-local"; intBox.appendChild(start);
      var d0 = new Date(); d0.setDate(d0.getDate() + 1); d0.setHours(8, 0, 0, 0); start.value = dtLocal(d0.getTime());
      if (editing && pre.kind === "once" && pre.run_at_ms) when.value = dtLocal(pre.run_at_ms);
      if (editing && pre.kind === "interval" && pre.interval_s) {
        var u = UNITS.slice().reverse().filter(function (x) { return pre.interval_s % x[1] === 0; })[0] || UNITS[0];
        unit.value = String(u[1]); every.value = String(Math.round(pre.interval_s / u[1]));
        if (pre.next_ms) start.value = dtLocal(pre.next_ms);
      }
      intBox.appendChild(el("p", "t-hint", "Daily tasks keep this time of day."));
      [["interval", "Repeat"], ["once", "Once"], ["manual", "Manual"]].forEach(function (k) {
        var b = el("button", null, k[1]); b.type = "button"; b.dataset.k = k[0];
        b.onclick = function () { kind = k[0]; sync(); };
        kinds.appendChild(b);
      });
      function sync() {
        Array.prototype.forEach.call(kinds.children, function (b) { b.setAttribute("aria-pressed", String(b.dataset.k === kind)); });
        onceBox.hidden = kind !== "once"; intBox.hidden = kind !== "interval";
      }
      sync();
      var cwd = null;
      if (pre.cwd || editing) { cwd = field("Folder (optional)", el("input", "s-field mono")); cwd.type = "text"; cwd.value = pre.cwd || ""; }
      var err = el("p", "t-err"); err.hidden = true; box.appendChild(err);
      var row = el("div", "s-btns"); box.appendChild(row);
      var back = el("button", "plain", "Back"); back.type = "button"; back.onclick = openTasks;
      var save = el("button", "primary", editing ? "Save changes" : "Save task"); save.type = "button";
      function fail(m) { err.textContent = m; err.hidden = false; }
      save.onclick = function () {
        err.hidden = true;
        var p = prompt.value.trim();
        if (!p) { fail("Write what Judas should do."); return; }
        var body = { prompt: p, kind: kind };
        if (!editing) body.enabled = true;
        if (title.value.trim() || editing) body.title = title.value.trim();
        if (cwd && (cwd.value.trim() || editing)) body.cwd = cwd.value.trim();
        if (kind === "once") {
          var t = new Date(when.value).getTime();
          if (!t || isNaN(t)) { fail("Pick a date and time."); return; }
          if (t < Date.now()) { fail("That time is in the past."); return; }
          body.run_at_ms = t;
        } else if (kind === "interval") {
          var n = parseInt(every.value, 10);
          if (!n || n < 1) { fail("Repeat every 1 or more."); return; }
          body.interval_s = n * parseInt(unit.value, 10);
          var st = new Date(start.value).getTime();
          if (!st || isNaN(st)) { fail("Pick when it should first run."); return; }
          body.run_at_ms = st;
        }
        save.disabled = true;
        var req = editing ? api("PATCH", "/api/tasks/" + encodeURIComponent(pre.id), body) : post("/api/tasks", body);
        req.then(function () { toast(editing ? "Task updated" : "Task saved"); openTasks(); }, function (e) { save.disabled = false; fail("Could not save: " + e.message); });
      };
      row.appendChild(back); row.appendChild(save);
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

  /* ---------- Learn: visual tutor ----------
     [learn] visual_url is the tutor's address on the server (usually http://localhost:8024), which a phone cannot
     reach. Judas forwards it on its own port (proxy_port in /api/learn/visual); off the server machine we frame
     that forwarder on the host this page was opened from. */
  var visBase = "", visTimer = null, visStarting = false, visStartErr = "";
  function visualUrl() {
    var lv = S.lv, h = location.hostname;
    if (!lv || !lv.url) return "";
    if (lv.proxy_port > 0 && h && !/^(localhost|127\.0\.0\.1|\[::1\])$/.test(h)) return "http://" + h + ":" + lv.proxy_port + "/";
    return lv.url;
  }
  function loadVisual() {
    return get("/api/learn/visual", { timeout: 6000 }).then(function (r) {
      var had = !!(S.lv && S.lv.url);
      S.lv = r && typeof r === "object" && !Array.isArray(r) && r.url ? r : null; S.lvKnown = true;
      if ((!!(S.lv && S.lv.url)) !== had) renderBar();
      if (S.view === "visual") { if (S.lv) renderVisual(); else go("#learn"); }
    }, function (e) { if (!e.net) { S.lv = null; S.lvKnown = true; renderBar(); if (S.view === "visual") go("#learn"); } });
  }
  function openVisual() {
    renderBar(); renderVisual();
    clearInterval(visTimer); visTimer = setInterval(loadVisual, 4000); loadVisual();
  }
  function stopVisualPoll() { clearInterval(visTimer); visTimer = null; }
  function visualMsg(title, text, code, btn) {
    var box = $("#vis-msg"); box.textContent = "";
    box.appendChild(el("span", "coin big", "Ag"));
    box.appendChild(el("h1", null, title));
    var p = el("p"); p.appendChild(document.createTextNode(text));
    if (code) { p.appendChild(document.createTextNode(" ")); p.appendChild(el("code", null, code)); }
    box.appendChild(p);
    if (btn) box.appendChild(btn);
    box.hidden = false;
  }
  function renderVisual() {
    var lv = S.lv, fr = $("#vis-frame"), box = $("#vis-msg");
    if (!lv || !lv.url) { fr.hidden = true; if (S.lvKnown) visualMsg("No visual tutor", "Set [learn] visual_url in the Judas config to add one."); else box.hidden = true; return; }
    var url = visualUrl(), mixed = location.protocol === "https:" && url.indexOf("http:") === 0;
    if (lv.up && !mixed) {
      box.hidden = true; fr.hidden = false;
      if (visBase !== url) {
        visBase = url;
        api("POST", "/api/learn/visual/theme", { name: lsGet("judas.skin") || "current" }).then(null, function () { /* default look */ }).then(function () {
          fr.src = window.JudasTheme ? JudasTheme.decorate(url) : url;
          if (window.JudasTheme) JudasTheme.track(fr);
        });
      }
      return;
    }
    fr.hidden = true; visBase = "";
    if (lv.up && mixed) { visualMsg("Open Judas over http", "This page is secure but the visual tutor is plain http, which the browser blocks inside a frame. Open the http address of this server instead, or open the tutor on its own:", url); return; }
    var unit = lv.unit || "learnharness";
    var b = null;
    if (lv.unit) {
      b = el("button", "primary", visStarting ? "Starting..." : "Start it"); b.type = "button"; b.disabled = visStarting;
      b.onclick = function () {
        visStarting = true; visStartErr = ""; renderVisual();
        post("/api/learn/visual", { on: true }).then(function () {
          var t0 = Date.now();
          (function check() {
            loadVisual().then(function () {
              if (S.lv && S.lv.up) { visStarting = false; renderVisual(); }
              else if (Date.now() - t0 > 45000) { visStarting = false; visStartErr = "It did not come up within 45 s. Check the " + unit + " unit."; renderVisual(); }
              else setTimeout(check, 1500);
            });
          })();
        }, function (e) { visStarting = false; visStartErr = "Could not start it: " + e.message; renderVisual(); });
      };
    }
    visualMsg(visStarting ? "Starting the visual tutor" : "The visual tutor is not running", visStartErr || "Start it on the desktop with", visStartErr ? "" : "systemctl --user start " + unit, b);
  }
  $("#visual-chip").onclick = function () { go("#visual"); };

  /* ---------- session ---------- */
  function sessionHeader() { renderBar(); }
  var cur = null; /* render context for the open session */
  function stick() { var l = $("#log"); return l.scrollHeight - l.scrollTop - l.clientHeight < 90; }
  function toBottom(force) { var l = $("#log"); if (force || cur && cur.stick) l.scrollTop = l.scrollHeight; reportScroll(); }
  $("#log").addEventListener("scroll", function () {
    var near = stick(); if (cur) cur.stick = near; $("#jump").hidden = near;
  }, { passive: true });
  $("#jump").appendChild(icon("down"));
  $("#jump").onclick = function () { cur.stick = true; toBottom(true); $("#jump").hidden = true; };
  function sessionMenu() {
    var s = S.sess;
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, s.title || "Session"));
      if (s.project_id) { var pj = projectById(s.project_id); sh.appendChild(el("p", "sub", "Project: " + (s.project_name || (pj && pj.name) || s.project_id))); }
      if (s.cwd) sh.appendChild(el("p", "sub mono", s.cwd));
      if (S.lastCtx && S.state && S.state.ctx_total) sh.appendChild(el("p", "sub", "Context " + kfmt(S.lastCtx) + " of " + kfmt(S.state.ctx_total) + " tokens (" + Math.round(100 * S.lastCtx / S.state.ctx_total) + "%)"));
      sItem(sh, "edit", "Rename", function () { afterClose(function () { renameSession(s); }); });
      sItem(sh, "cal", "Schedule as a task", function () {
        var first = (S.msgs || []).filter(function (m) { return m && m.role === "user"; })[0];
        taskForm({ title: s.title || "", prompt: first ? plainUser(contentText(first.content)) : "", cwd: s.cwd || "" });
      });
      if (S.running) sItem(sh, "stop", "Stop the turn", function () { afterClose(cancelTurn); });
      sItem(sh, "trash", "Delete", function () { afterClose(function () { deleteSession(s); }); }, "danger");
    });
  }

  function newCtx() { return { stick: true, bubble: null, bubbleText: "", think: null, tools: {}, typing: null, raf: 0 }; }
  function clearLog() { if (cur && cur.work) cur.work.hide(); $("#log").textContent = ""; cur = newCtx(); }
  function addNote(text, err) { var n = el("div", "msg note" + (err ? " err" : ""), text); $("#log").appendChild(n); return n; }
  function addUser(text) { if (window.JudasLearn) JudasLearn.noteUser($("#log"), text); var n = userNode(text); $("#log").appendChild(n); return n; }
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
    if (window.JudasLearn && /^learn_/.test(name || "")) {
      var lc = JudasLearn.toolCall(name, args, $("#log"));
      if (lc) return { row: lc, sum: el("span"), out: el("pre"), outLbl: el("div"), name: name, args: args, t0: Date.now() };
    }
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
    if (ok !== false && o.indexOf("[[artifact:") >= 0) {
      var after = t.row;
      artCards(o).forEach(function (c) { after.after(c); after = c; });
    }
    if (o) { t.out.textContent = o.length > 6000 ? o.slice(0, 6000) + "\n… (" + (o.length - 6000) + " more characters)" : o; t.out.hidden = false; t.outLbl.hidden = false; }
  }
  /* the working indicator: friendly rotating words, or the tool name while a tool runs */
  function showTyping(on, label) {
    if (!cur) return;
    if (!cur.work && window.JudasRich) cur.work = JudasRich.work($("#log"));
    if (!cur.work) return;
    if (on) { cur.typing = true; cur.tlabel = label || null; cur.work.show(cur.tlabel); }
    else if (cur.typing) { cur.typing = false; cur.work.hide(); }
  }
  function keepTypingLast() { if (cur && cur.typing) showTyping(true, cur.tlabel); }

  /* ---------- per-answer footer: Copy + a small technical summary; context meter in the top bar ---------- */
  function newTurnStats() { return { calls: 0, tools: 0, prompt: 0, cached: 0, out: 0, pre_s: 0, gen_s: 0, queue_s: 0, ctx: 0 }; }
  function addTurnStats(t, u, ntools) {
    if (!u) return;
    t.calls++; t.tools += ntools || 0;
    t.prompt += u.prompt || 0; t.cached += u.cached || 0; t.out += u.completion || 0;
    t.pre_s += u.prefill_s || 0; t.gen_s += u.generate_s || 0; t.queue_s += u.queue_s || 0;
    t.ctx = (u.prompt || 0) + (u.completion || 0);
  }
  function kfmt(n) { return n >= 10000 ? (n / 1000).toFixed(0) + "k" : n >= 1000 ? (n / 1000).toFixed(1) + "k" : String(n); }
  function sfmt(x) { return x >= 60 ? Math.floor(x / 60) + "m " + Math.round(x % 60) + "s" : x.toFixed(x < 10 ? 1 : 0) + "s"; }
  function statsBits(t) {
    var fresh = Math.max(0, t.prompt - t.cached), b = [];
    if (t.gen_s > 0.05 && t.out > 0) b.push((t.out / t.gen_s).toFixed(1) + " tok/s");
    if (t.pre_s > 0.05 && fresh > 0) b.push("prefill " + Math.round(fresh / t.pre_s) + " tok/s");
    b.push(kfmt(t.prompt) + " in" + (t.cached ? " (" + kfmt(t.cached) + " cached)" : "") + ", " + kfmt(t.out) + " out");
    b.push(sfmt(t.pre_s + t.gen_s + t.queue_s));
    if (t.tools) b.push(t.tools + (t.tools === 1 ? " tool" : " tools"));
    return b;
  }
  function addFoot(node, t) {
    var f = el("div", "msg-foot");
    var c = el("button", "act-copy", "Copy"); c.type = "button"; c.setAttribute("aria-label", "Copy this answer");
    f.appendChild(c);
    if (t && t.calls) {
      var st = el("span", "msg-stats", statsBits(t).join(" \u00b7 "));
      st.title = "Generation speed, prefill speed on uncached tokens, tokens in and out, model time, tool calls";
      f.appendChild(st);
      S.lastCtx = t.ctx;
    }
    node.appendChild(f);
  }
  function setCtxMeter(used) {
    var m = $("#ctx-meter"), total = (S.state && S.state.ctx_total) || 0;
    if (!m) return;
    var on = S.view === "session" && used > 0 && total > 0;
    m.hidden = !on;
    if (!on) return;
    var pct = Math.min(100, Math.round(100 * used / total));
    m.firstChild.style.width = Math.max(1, pct) + "%";
    m.className = "ctx-meter" + (pct >= 85 ? " hot" : pct >= 60 ? " warm" : "");
    m.title = "Context: " + kfmt(used) + " of " + kfmt(total) + " tokens (" + pct + "%)";
  }
  function copyText(text, btn) {
    function done(ok) { if (!btn) return; var l = btn.textContent; btn.textContent = ok ? "Copied" : "Copy failed"; setTimeout(function () { btn.textContent = l === "Copied" || l === "Copy failed" ? "Copy" : l; }, 1400); }
    function legacy() {
      var t = document.createElement("textarea");
      t.value = text; t.setAttribute("readonly", ""); t.style.position = "fixed"; t.style.top = "0"; t.style.opacity = "0";
      document.body.appendChild(t); t.select(); t.setSelectionRange(0, text.length);
      var ok = false; try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
      t.remove(); done(ok);
    }
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(function () { done(true); }, legacy);
    else legacy();
  }
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest(".code-copy, .act-copy");
    if (!b) return;
    e.preventDefault();
    if (b.classList.contains("code-copy")) { var c = b.parentNode.querySelector("code"); copyText(c ? c.textContent : "", b); return; }
    var msg = b.closest(".msg"); copyText(msg && msg._raw ? plainUser(msg._raw) : "", b);
  });

  function renderMessages(messages) {
    var log = $("#log"), childAgents = {}, tools = {}, ts = newTurnStats(), lastA = null;
    (S.agents || []).forEach(function (a) { if (a.parent) childAgents[a.id] = true; });
    clearLog();
    function flush() { if (lastA) addFoot(lastA, ts); ts = newTurnStats(); lastA = null; }
    messages.forEach(function (m) {
      var b = m.body || {};
      if (m.agent && childAgents[m.agent]) return;   // workers live in the agents sheet
      if (b.role === "user") { flush(); addUser(contentText(b.content)); return; }
      if (b.role === "assistant") {
        addTurnStats(ts, b.judas_stats, (b.tool_calls || []).length);
        if (b.reasoning_content) addThink(b.reasoning_content);
        if (b.content && contentText(b.content).trim()) { lastA = addAsst(contentText(b.content)); lastA._raw = contentText(b.content); }
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
    if (!S.running) flush();
    setCtxMeter(S.lastCtx);
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
    S.titleHint = "Loading..."; clearLog(); sessionHeader();
    $("#approvals").hidden = true; $("#agents-chip").hidden = true; $("#jump").hidden = true;
    renderDock();
    return refreshSession(true).then(function () {
      startLive();
      loadApprovalsNow(); loadTeam().then(renderAgentsChip);
    }, function (e) {
      if (e.net) return;
      S.titleHint = "Session"; renderBar();
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
      if (full) S.mode = modeOf(S.sess);   // before the bar and tabs are drawn, so a Learn session lights the Learn tab
      sessionHeader(); renderTabs(); renderAgentsChip(); renderMessages(S.msgs);
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
    if (window.JudasLearn) JudasLearn.onEvent(ev);
    var d = ev.data || {}, lead = !ev.parent;
    if (ev.type === "gap") return;
    if (window.JudasCowork) JudasCowork.event(ev);
    if (ev.type === "calcs" || ev.type === "sources" || ev.type === "suggestions" || ev.type === "cad" || ev.type === "artifact") return;
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
        if (!cur.think) { cur.think = addThink(""); cur.thinkText = ""; }
        cur.thinkText += d.text || ""; cur.think.txt.textContent = cur.thinkText; showTyping(true); keepTypingLast(); toBottom();
        break;
      }
      case "tool_call": {
        cur.bubble = null; cur.think = null;
        var t = addTool(d.name || "tool", d.args); cur.tools[d.id] = t;
        showTyping(true, window.JudasRich ? JudasRich.toolLabel(d.name) : null); toBottom(); break;
      }
      case "tool_result": {
        var tt = cur.tools[d.id];
        if (tt) { finishTool(tt, d.ok, d.output, (Date.now() - tt.t0) / 1000); }
        cur.bubble = null; cur.think = null; showTyping(true); keepTypingLast(); toBottom(); break;
      }
      case "message": cur.bubble = null; cur.think = null; break;
      case "status":
        if (d.error) { addNote("Error: " + d.error, true); showTyping(false); toBottom(); }
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
  $("#attach").appendChild(icon("plus"));
  function fit() { input.style.height = "auto"; input.style.height = Math.min(input.scrollHeight, Math.round(window.innerHeight * 0.3)) + "px"; }
  input.addEventListener("input", function () { fit(); renderSend(); });
  input.addEventListener("keydown", function (e) { if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) { e.preventDefault(); send(); } });
  function curMode() { return S.view === "session" && S.sess ? modeOf(S.sess) : S.mode; }
  function needsFolderNow() { return S.view === "home" && !!PROJECT_MODE[S.mode]; }
  function projectById(id) { for (var i = 0; i < S.projects.length; i++) if (S.projects[i].id === id) return S.projects[i]; return null; }
  function loadProjects() {
    return get("/api/projects", { timeout: 8000 }).then(function (r) {
      S.projects = (r && r.projects) || []; S.projectRoot = (r && r.root) || "";
      if (S.project && !projectById(S.project)) { S.project = ""; lsSet("judas.m.project", null); }
      renderDock(); if (drawerOpen) drawDrawer();
    }, function () { /* an older server has no projects: the folder sheet still works */ });
  }
  function setProject(id) { S.project = id || ""; lsSet("judas.m.project", id || null); renderDock(); }
  /* with the keyboard up the tab bar steps aside */
  /* tapping a composer button must not blur the field: the tab bar would come back and shift the button under the finger */
  ["#send", "#attach", "#folder"].forEach(function (q) { $(q).addEventListener("pointerdown", function (e) { if (document.activeElement === input) e.preventDefault(); }); });
  input.addEventListener("focus", function () { document.body.classList.add("typing"); });
  input.addEventListener("blur", function () { document.body.classList.remove("typing"); });
  /* keyboard detection for the empty home view: the visual viewport shrinks well below the largest height seen */
  (function () {
    var vv = window.visualViewport, maxH = Math.max(window.innerHeight, vv ? vv.height : 0);
    function chk() {
      var h = vv ? vv.height : window.innerHeight;
      if (h > maxH) maxH = h;
      document.body.classList.toggle("kbd", maxH - h > 140);
    }
    if (vv) vv.addEventListener("resize", chk);
    window.addEventListener("resize", chk);
    window.addEventListener("orientationchange", function () { setTimeout(function () { maxH = window.innerHeight; chk(); }, 300); });
  })();
  function renderDock() {
    $("#dock").hidden = S.view === "planner" || S.view === "visual" || (cwActive() && !!CW_PANES[S.mode] && CW.pane !== "chat");
    input.placeholder = PLACEHOLDER[curMode()] || "Message Judas";
    var f = $("#folder");
    if (needsFolderNow()) {
      f.hidden = false; f.textContent = ""; f.appendChild(icon("folder"));
      var pj = S.project ? projectById(S.project) : null;
      var lab = pj ? pj.name : S.cwd ? clip(S.cwd, 34) : (NEEDS_CWD[S.mode] ? "Choose project" : "Project (optional)");
      f.appendChild(el("span", null, lab)); f.classList.toggle("need", !pj && !S.cwd && !!NEEDS_CWD[S.mode]);
      f.setAttribute("aria-label", pj ? "Project: " + pj.name : "Project or working folder: " + (S.cwd || "not set"));
    } else f.hidden = true;
    renderSend();
  }
  var interruptNext = false;
  function renderSend() {
    var has = input.value.trim() || S.atts.some(function (a) { return a.state === "ok"; });
    var ib = $("#interrupt"); if (ib) ib.hidden = !(S.running && S.view === "session" && has);
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
  $("#interrupt").onclick = function () { interruptNext = true; send(); };
  function cancelTurn() {
    if (!S.sid) return;
    post("/api/sessions/" + encodeURIComponent(S.sid) + "/cancel").then(function () { toast("Stopping..."); }, function (e) { toast("Stop failed: " + e.message, true); });
  }
  /* the project sheet: pick a project, make one, or fall back to a plain folder */
  function tail(path) { var p = String(path || "").split("/").filter(Boolean); return p.length > 2 ? ".../" + p.slice(-2).join("/") : path; }
  $("#folder").onclick = function () {
    loadProjects();
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Project"));
      sh.appendChild(el("p", "sub", NEEDS_CWD[S.mode] ? "Required for " + S.mode + ": a project or a folder on the desktop." : "Optional. A project gives the chat a folder and standing instructions."));
      var box = el("div", "sbody"); sh.appendChild(box);
      var none = sItem(box, "folder", S.mode === "chat" ? "No project" : "Just a folder", function () {
        if (S.mode === "chat") afterClose(function () { setProject(""); S.cwd = ""; lsSet("judas.m.cwd", null); renderDock(); });
        else afterClose(folderSheet);
      }, "", S.mode === "chat" ? "" : (S.cwd ? clip(tail(S.cwd), 22) : ""));
      if (!S.project) none.setAttribute("aria-current", "true");
      S.projects.forEach(function (p) {
        var b = sItem(box, "folder", p.name, function () { afterClose(function () { setProject(p.id); }); }, "", clip(tail(p.folder), 22));
        if (p.id === S.project) b.setAttribute("aria-current", "true");
      });
      sItem(box, "plus", "New project", function () { afterClose(newProjectSheet); });
    });
  };
  function folderSheet() {
    var recents = []; try { recents = JSON.parse(lsGet("judas.m.cwds") || "[]"); } catch (e) { /* ignore */ }
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "Working folder"));
      sh.appendChild(el("p", "sub", NEEDS_CWD[S.mode] ? "Required for " + S.mode + ". An absolute path on the desktop." : "Optional. An absolute path on the desktop."));
      var inp = el("input", "s-field mono"); inp.type = "text"; inp.value = S.cwd; inp.placeholder = "/home/you/src/project";
      inp.autocapitalize = "off"; inp.autocomplete = "off"; inp.spellcheck = false; inp.setAttribute("aria-label", "Folder path");
      sh.appendChild(inp);
      var rec = el("div", "sbody");
      recents.slice(0, 6).forEach(function (r) { sItem(rec, "folder", r, function () { inp.value = r; }); });
      sh.appendChild(rec);
      var row = el("div", "s-btns");
      var clr = el("button", "plain", "Clear"); clr.type = "button"; clr.onclick = function () { afterClose(function () { S.cwd = ""; lsSet("judas.m.cwd", null); renderDock(); }); };
      var ok = el("button", "primary", "Use folder"); ok.type = "button";
      function use() { var v = inp.value.trim(); afterClose(function () { S.cwd = v; lsSet("judas.m.cwd", v || null); setProject(""); }); }
      ok.onclick = use; inp.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); use(); } });
      row.appendChild(clr); row.appendChild(ok); sh.appendChild(row);
    });
  }
  function newProjectSheet() {
    var recents = []; try { recents = JSON.parse(lsGet("judas.m.cwds") || "[]"); } catch (e) { /* ignore */ }
    openSheet(function (sh) {
      sh.appendChild(el("h2", null, "New project"));
      var name = el("input", "s-field"); name.type = "text"; name.placeholder = "Name"; name.maxLength = 120;
      name.autocomplete = "off"; name.setAttribute("aria-label", "Project name");
      sh.appendChild(name);
      var kinds = el("div", "opts"), mk = el("button", null, "Make a new folder"), ex = el("button", null, "Use an existing folder");
      mk.type = ex.type = "button"; kinds.appendChild(mk); kinds.appendChild(ex); sh.appendChild(kinds);
      var hint = el("p", "sub"), pathBox = el("div"), inp = el("input", "s-field mono");
      inp.type = "text"; inp.placeholder = "/home/you/src/project"; inp.autocapitalize = "off"; inp.autocomplete = "off"; inp.spellcheck = false;
      inp.setAttribute("aria-label", "Folder path"); pathBox.appendChild(inp);
      var rec = el("div", "sbody"); recents.slice(0, 4).forEach(function (r) { sItem(rec, "folder", r, function () { inp.value = r; }); }); pathBox.appendChild(rec);
      var make = true;
      function sync() {
        mk.setAttribute("aria-pressed", String(make)); ex.setAttribute("aria-pressed", String(!make));
        pathBox.hidden = make; hint.hidden = !make;
        hint.textContent = "Judas makes the folder under " + (S.projectRoot || "~/Judas/Projects") + " and never deletes it.";
      }
      mk.onclick = function () { make = true; sync(); }; ex.onclick = function () { make = false; sync(); };
      sh.appendChild(hint); sh.appendChild(pathBox); sync();
      var ins = el("textarea", "s-field"); ins.rows = 3; ins.placeholder = "Instructions for every chat in this project (optional)"; ins.maxLength = 8000;
      ins.style.paddingTop = "12px"; ins.style.resize = "vertical"; sh.appendChild(ins);
      var err = el("p", "sub"); err.style.color = "var(--error)"; err.hidden = true; sh.appendChild(err);
      var row = el("div", "s-btns");
      var c = el("button", "plain", "Cancel"); c.type = "button"; c.onclick = closeSheet;
      var ok = el("button", "primary", "Create"); ok.type = "button";
      ok.onclick = function () {
        var body = { name: name.value.trim(), mode: make ? "make" : "existing", instructions: ins.value };
        if (!body.name) { err.textContent = "Give the project a name."; err.hidden = false; return; }
        if (!make) { body.folder = inp.value.trim(); if (!body.folder) { err.textContent = "Enter the folder, an absolute path."; err.hidden = false; return; } }
        ok.disabled = true;
        post("/api/projects", body).then(function (p) {
          if (!make) pushRecentCwd(body.folder);
          return loadProjects().then(function () { afterClose(function () { setProject(p.id); }); });
        }, function (e) { ok.disabled = false; err.textContent = e.message; err.hidden = false; });
      };
      row.appendChild(c); row.appendChild(ok); sh.appendChild(row);
    });
  }
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
    var a = { name: f.name || "file", state: "up", id: null, kind: /^image\//.test(f.type || "") ? "image" : "" };
    if (a.kind === "image") { try { a.preview = URL.createObjectURL(f); } catch (e) { /* no blob URLs */ } }
    S.atts.push(a); renderAtts();
    fetch("/api/attachments?name=" + encodeURIComponent(a.name), { method: "POST", headers: { "Content-Type": f.type || "application/octet-stream" }, body: f })
      .then(function (r) { return r.json().then(function (j) { if (!r.ok) throw new Error((j && j.error && (j.error.message || j.error)) || "HTTP " + r.status); return j; }); })
      .then(function (j) { a.state = "ok"; a.id = j.id; a.kind = j.kind; renderAtts(); }, function (e) { a.state = "err"; a.err = e.message; renderAtts(); });
  }
  function renderAtts() {
    var box = $("#atts"); box.textContent = ""; box.hidden = !S.atts.length;
    S.atts.forEach(function (a) {
      var img = a.kind === "image" && (a.preview || (a.state === "ok" && a.id));
      var p = el("span", "pill" + (a.state === "err" ? " err" : "") + (img ? " has-pic" : ""));
      if (img) p.appendChild(thumb(a.preview || "/api/attachments/" + encodeURIComponent(a.id), a.name));
      p.appendChild(el("span", null, a.name + (a.state === "up" ? " (uploading)" : a.state === "err" ? " (failed)" : "")));
      var x = el("button", null, "×"); x.type = "button"; x.setAttribute("aria-label", "Remove " + a.name);
      x.onclick = function () { S.atts = S.atts.filter(function (z) { return z !== a; }); dropPreview(a); renderAtts(); };
      p.appendChild(x); box.appendChild(p);
    });
    renderSend();
  }

  function dropPreview(a) { if (a.preview) { try { URL.revokeObjectURL(a.preview); } catch (e) { /* ignore */ } a.preview = null; } }

  /* queue: server-side when the session JSON exposes `queued`, else held here until the turn ends */
  function serverQueue() { return S.sess && Array.isArray(S.sess.queued); }
  function qText(q) { return typeof q === "string" ? q : (q && (q.content || q.text || q.prompt)) || "message"; }
  function renderQueue() {
    var box = $("#queue"); box.textContent = "";
    var items = (serverQueue() ? S.sess.queued.map(function (q) { return { id: q && q.id, text: qText(q), server: true }; }) : []).concat(S.queue.filter(function (q) { return q.sid === S.sid; }));
    box.hidden = S.view !== "session" || !items.length;
    if (!items.length) return;
    box.appendChild(el("div", "q-cap", items.length === 1 ? "Queued: sends when this turn ends" : items.length + " queued: sent one by one when this turn ends"));
    items.forEach(function (q) {
      var row = el("div", "q-item");
      row.appendChild(el("span", "q-text", clip(q.text.replace(/\s+/g, " "), 120)));
      if (q.server && q.id) {
        var now = el("button", "q-now", "Send now"); now.type = "button";
        now.setAttribute("aria-label", "Stop the current turn and send this now");
        now.onclick = function () {
          now.disabled = true;
          post(queueURL(q.id) + "/send").then(function (r) { setServerQueue(r && r.queue); addUser(q.text); beginLocalTurn(); toast("Interrupted"); },
            function (e) { now.disabled = false; toast("Could not send it: " + e.message, true); loadQueue(); });
        };
        row.appendChild(now);
      }
      var x = el("button", "q-x", "×"); x.type = "button"; x.setAttribute("aria-label", "Remove queued message");
      x.onclick = function () {
        if (!q.server) { S.queue = S.queue.filter(function (z) { return z !== q; }); renderQueue(); return; }
        api("DELETE", queueURL(q.id)).then(function (r) { setServerQueue(r && r.queue); }, function (e) { toast("Could not remove it: " + e.message, true); loadQueue(); });
      };
      row.appendChild(x);
      box.appendChild(row);
    });
  }
  function queueURL(qid) { return "/api/sessions/" + encodeURIComponent(S.sid) + "/queue/" + encodeURIComponent(qid); }
  function setServerQueue(list) { if (S.sess && Array.isArray(list)) S.sess.queued = list; renderQueue(); }
  function loadQueue() { if (S.sid) get("/api/sessions/" + encodeURIComponent(S.sid) + "/queue").then(function (r) { setServerQueue(r && r.queue); }, function () { /* leave it */ }); }
  function flushQueue() {
    if (S.running || serverQueue()) return;
    var i = S.queue.findIndex(function (q) { return q.sid === S.sid; });
    if (i < 0) return;
    var q = S.queue.splice(i, 1)[0]; renderQueue();
    transmit(q.text, q.ids, q.names);
  }

  if (window.JudasLearn) JudasLearn.init({
    md: md,
    send: function (t) { var keep = input.value; input.value = t; send(); input.value = keep; fit(); }
  });
  if (window.JudasLearn) JudasLearn.mount({
    top: $("#learn-top"),
    home: $("#learn-home"),
    mode: function () { return S.view === "session" ? ((S.sess && S.sess.mode) || S.mode) : (S.view === "home" ? S.mode : ""); },
    sid: function () { return S.view === "session" ? S.sid : null; },
    practice: function (text) {
      // Start the session through the API, then open it: no waiting on the composer.
      post("/api/sessions/new/messages", { content: text, mode: "learn" }).then(function (r) {
        var sid = typeof r.session === "string" ? r.session : ((r.session || {}).id || "");
        if (!sid) throw new Error("the server returned no session");
        S.mode = "learn";
        go("#s/" + encodeURIComponent(sid));
      }, function (e) { toast("Could not start practice: " + e.message, true); });
    }
  });
  function send() {
    var text = input.value.trim(), ready = S.atts.filter(function (a) { return a.state === "ok"; });
    if (!text && !ready.length) return;
    if (S.atts.some(function (a) { return a.state === "up"; })) { toast("Wait for the upload to finish"); return; }
    if (S.atts.some(function (a) { return a.state === "err"; })) { toast("Remove the file that failed to send", true); return; }
    var ids = ready.map(function (a) { return a.id; }), names = ready.map(function (a) { return { name: a.name, id: a.id, kind: a.kind }; });
    if (S.view === "session" && S.running) {
      if (interruptNext) { interruptNext = false; input.value = ""; S.atts = []; renderAtts(); fit(); transmit(text, ids, names, false, true); renderSend(); return; }
      if (serverQueue()) { transmit(text, ids, names, true); }
      else { S.queue.push({ sid: S.sid, text: text || names.map(function (n) { return n.name; }).join(", "), ids: ids, names: names }); renderQueue(); }
      input.value = ""; S.atts = []; renderAtts(); fit(); renderSend(); return;
    }
    var mode = curMode();
    if (S.view === "home" && NEEDS_CWD[mode] && !S.cwd && !S.project) { toast("Choose a project or folder first", true); $("#folder").focus(); return; }
    input.value = ""; S.atts.forEach(dropPreview); S.atts = []; renderAtts(); fit();
    transmit(text, ids, names);
  }
  function transmit(text, ids, names, queued, interrupt) {
    var mode = curMode(), fresh = !S.sid;
    var body = { content: text, mode: mode };
    if (interrupt) body.interrupt = true;
    if (ids.length) body.attachments = ids;
    var proj = fresh && S.project && PROJECT_MODE[mode] ? S.project : "";
    if (proj) body.project = proj;   // the project's folder is the working directory
    else if (fresh && S.cwd && (NEEDS_CWD[mode] || WANTS_CWD[mode])) body.cwd = S.cwd;
    var shown = text || ""; names.forEach(function (n) {
      var nm = String(n.name || "file").replace(/["|\]\n<>]/g, "_");
      shown += (shown ? "\n" : "") + (n.kind === "image" && n.id ? "[[image:" + n.id + "|" + nm + "]]" : '<attachment name="' + nm + '" kind="file">\n\n</attachment>');
    });
    if (fresh) { showView("session"); clearLog(); S.running = true; S.titleHint = clip(text || (names[0] && names[0].name) || "New session", 40); renderBar(); S.msgs = []; }
    else if (!queued) { var stay = cur && cur.stick; }
    if (!queued) { addUser(shown); beginLocalTurn(); }   // a queued message shows in the queue card, not as sent
    var url = "/api/sessions/" + encodeURIComponent(S.sid || "new") + "/messages";
    return post(url, body).then(function (r) {
      var sid = typeof r.session === "string" ? r.session : ((r.session || {}).id || S.sid);
      if (!sid) throw new Error("the server returned no session");
      if (fresh) {
        S.sid = sid; S.sess = { id: sid, title: clip(text || (names[0] && names[0].name) || "New session", 60), mode: mode, cwd: S.cwd, project_id: proj };
        if (!proj && S.cwd && (NEEDS_CWD[mode] || WANTS_CWD[mode])) pushRecentCwd(S.cwd);
        lastRoute = JSON.stringify({ view: "session", sid: sid });
        location.hash = "#s/" + encodeURIComponent(sid);
        S.turn = { running: true, event_floor: 0 };
        sessionHeader(); startLive(); S.live.inTurn = true; S.live.turnAt = Date.now();
      } else if (S.live) { S.live.turnAt = Date.now(); if (!S.live.inTurn) { S.live.inTurn = true; } }
      if (r && r.queued) { setServerQueue(r.queue); toast("Queued: sends when this turn ends"); }
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
  initTheme();
  route(true);
  loadProjects();
  /* /m/?q=<text> opens a new Chat with the text in the composer, not sent */
  (function () {
    var q = null;
    try { q = new URLSearchParams(location.search).get("q"); } catch (e) { /* old browser */ }
    if (!q || !q.trim()) return;
    S.sid = null; S.mode = "chat"; lastRoute = "";
    history.replaceState(null, "", location.pathname + "#chat");
    route(true);
    input.value = q.slice(0, 4000); fit(); renderSend();
    setTimeout(function () { try { input.focus(); input.setSelectionRange(input.value.length, input.value.length); } catch (e) { /* ignore */ } }, 120);
  })();
  connectFeed();
  checkPlanner();
  loadVisual();
  renderDock();
  pollState().then(function () { if (S.health === "off") showOffline(); });
  // the page is the whole app: an in-page reload keeps the hash, so the current screen comes back
  window.addEventListener("pageshow", function (e) { if (e.persisted) pollState(); });
})();
