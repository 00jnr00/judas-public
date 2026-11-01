/* Tools tab: sub-tabs from [[tools.tabs]] (GET /api/tools/tabs). Each shows its page in an iframe and, when it names a
   skill, a "Plan with Judas" side chat (a normal Judas session with that skill loaded). Also adds the built-in
   planner switch to Settings. Self-contained; app.js only lists the "tools" screen and calls JudasTools.open(). */
(function () {
  "use strict";
  var tabs = [], planner = { available: false, enabled: false }, cur = null, panels = {};

  function $(s) { return document.querySelector(s); }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function jget(p) { return fetch(p).then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); }); }
  function jpost(p, b) {
    return fetch(p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(b || {}) })
      .then(function (r) { return r.json().then(function (j) { if (!r.ok) throw new Error((j.error && j.error.message) || "HTTP " + r.status); return j; }); });
  }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } }

  function load() {
    return jget("/api/tools/tabs").then(function (r) {
      tabs = r.tabs || [];
      planner = r.mach12 || planner;
      var btn = $('#rail button[data-screen="tools"]');
      if (btn) btn.hidden = !tabs.length;
      return r;
    }, function () { tabs = []; });
  }

  /* ---------- the Tools screen ---------- */
  function open() {
    load().then(function () {
      var bar = $("#tools-bar"), body = $("#tools-body");
      bar.textContent = "";
      if (!tabs.length) { body.textContent = ""; body.appendChild(el("p", "dim", "No tools are configured. Add a [[tools.tabs]] entry to config.toml.")); return; }
      tabs.forEach(function (t) {
        var b = el("button", "tools-tab", t.label || t.name);
        b.type = "button"; b.dataset.name = t.name;
        b.onclick = function () { select(t.name); };
        bar.appendChild(b);
      });
      var want = lsGet("judas.tools.tab");
      select(tabs.some(function (t) { return t.name === want; }) ? want : tabs[0].name);
    });
  }

  function select(name) {
    var t = tabs.filter(function (x) { return x.name === name; })[0];
    if (!t) return;
    cur = t.name; lsSet("judas.tools.tab", name);
    Array.prototype.forEach.call(document.querySelectorAll("#tools-bar .tools-tab"), function (b) { b.classList.toggle("active", b.dataset.name === name); });
    var body = $("#tools-body");
    body.textContent = "";
    var wrap = el("div", "tools-wrap" + (t.skill ? " with-panel" : ""));
    var main = el("div", "tools-frame");
    if (t.url) {
      var fr = el("iframe");
      fr.src = t.url; fr.title = t.label || t.name;
      fr.setAttribute("loading", "lazy");
      main.appendChild(fr);
      var open = el("a", "tools-open small", "Open in a new tab");
      open.href = t.url; open.target = "_blank"; open.rel = "noopener";
      main.appendChild(open);
    } else {
      main.appendChild(el("p", "dim", "This tool has no page. Use the panel to work with it."));
    }
    wrap.appendChild(main);
    if (t.skill) wrap.appendChild(panel(t));
    body.appendChild(wrap);
  }

  /* ---------- "Plan with Judas": a compact chat that loads the tab's skill ---------- */
  function panel(t) {
    var st = panels[t.name] || (panels[t.name] = { sid: null, timer: null, until: 0 });
    var box = el("aside", "tools-panel");
    var head = el("div", "tp-head");
    head.appendChild(el("b", null, "Plan with Judas"));
    var mode = el("select"); mode.setAttribute("aria-label", "Mode");
    ["chat", "cowork"].forEach(function (m) { var o = el("option", null, m); o.value = m; mode.appendChild(o); });
    head.appendChild(mode);
    var fresh = el("button", "btn small ghost", "New"); fresh.type = "button";
    head.appendChild(fresh);
    box.appendChild(head);
    var log = el("div", "tp-log"); log.setAttribute("aria-live", "polite");
    box.appendChild(log);
    var form = el("form", "tp-form");
    var ta = el("textarea"); ta.rows = 2; ta.placeholder = t.prompt_hint || "Ask Judas"; ta.setAttribute("aria-label", "Message");
    var send = el("button", "btn primary", "Send"); send.type = "submit";
    form.appendChild(ta); form.appendChild(send);
    box.appendChild(form);
    var note = el("div", "tp-note small dim", "Uses the \"" + t.skill + "\" skill. Nothing is changed until you say yes.");
    box.appendChild(note);

    function say(cls, text) { var d = el("div", "tp-msg " + cls); d.textContent = text; log.appendChild(d); log.scrollTop = log.scrollHeight; return d; }
    function render(j) {
      log.textContent = "";
      (j.messages || []).forEach(function (m) {
        var b = m.body || {};
        if (m.agent && (j.agents || []).some(function (a) { return a.id === m.agent && a.parent; })) return;
        var txt = typeof b.content === "string" ? b.content : (Array.isArray(b.content) ? b.content.map(function (p) { return p && p.text || ""; }).join("\n") : "");
        if (b.role === "user") say("user", txt.replace(/^\[skill:[^\]]*\][^\n]*\n\n/, ""));
        else if (b.role === "assistant") {
          if (txt) say("assistant", txt);
          (b.tool_calls || []).forEach(function (tc) { say("tool", "tool: " + ((tc.function || {}).name || "call")); });
        }
      });
      if (j.turn && j.turn.running) say("tool", "working...");
      else if (j.turn && j.turn.status === "error") say("err", j.turn.reason || "The turn failed.");
    }
    function poll() {
      clearTimeout(st.timer);
      if (!st.sid) return;
      jget("/api/sessions/" + encodeURIComponent(st.sid) + "?limit=200").then(function (j) {
        render(j);
        send.disabled = false;
        if ((j.turn && j.turn.running) || Date.now() < st.until) st.timer = setTimeout(poll, 1000);
      }, function () { st.timer = setTimeout(poll, 3000); });
    }
    fresh.onclick = function () { clearTimeout(st.timer); st.sid = null; log.textContent = ""; };
    form.onsubmit = function (e) {
      e.preventDefault();
      var text = ta.value.trim();
      if (!text) return;
      ta.value = ""; send.disabled = true;
      var content = st.sid ? text : "[skill:" + t.skill + "] Load the \"" + t.skill + "\" skill with load_skill and follow it for this request.\n\n" + text;
      say("user", text);
      jpost("/api/sessions/" + (st.sid || "new") + "/messages", { content: content, mode: mode.value }).then(function (r) {
        st.sid = r.session || st.sid;
        st.until = Date.now() + 4000;   /* the turn may not be marked running yet */
        poll();
      }, function (err) { send.disabled = false; say("err", String(err.message || err)); });
    };
    ta.addEventListener("keydown", function (e) { if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); form.requestSubmit(); } });
    if (st.sid) poll();
    return box;
  }

  /* ---------- Settings: built-in planner switch ---------- */
  function settingsBlock() {
    jget("/api/mach12").then(function (r) {
      var host = $("#screen-settings .page");
      if (!host) return;
      var old = $("#mach12-settings");
      if (old) old.remove();
      if (!r.available) return;
      var box = el("div", null); box.id = "mach12-settings";
      box.appendChild(el("h3", null, "Built-in planner (mach12)"));
      box.appendChild(el("p", "dim", "Runs the mach12 day planner inside Judas at /mach12/, reading " + r.data_dir + ". The Tools > mach12 tab shows it. The switch takes effect at once (no restart) and is saved to [mach12].enabled in config.toml."));
      var lab = el("label", "auto-toggle");
      var cb = el("input"); cb.type = "checkbox"; cb.checked = !!r.enabled; cb.style.width = "auto";
      lab.appendChild(cb); lab.appendChild(document.createTextNode(" Serve mach12 from Judas"));
      var msg = el("span", "small dim", r.enabled ? "  on" : "  off");
      lab.appendChild(msg);
      cb.onchange = function () {
        cb.disabled = true;
        jpost("/api/mach12", { enabled: cb.checked }).then(function () { msg.textContent = cb.checked ? "  on" : "  off"; cb.disabled = false; load(); },
          function (err) { msg.textContent = "  " + err.message; cb.checked = !cb.checked; cb.disabled = false; });
      };
      box.appendChild(lab);
      var dk = $("#daemon-kv");
      if (dk && dk.previousElementSibling && dk.previousElementSibling.previousElementSibling) host.insertBefore(box, dk.previousElementSibling.previousElementSibling);
      else host.appendChild(box);
    }, function () {});
  }
  var scr = document.getElementById("screen-settings");
  if (scr && window.MutationObserver)
    new MutationObserver(function () { if (scr.classList.contains("active")) settingsBlock(); }).observe(scr, { attributes: true, attributeFilter: ["class"] });

  window.JudasTools = { open: open, reload: load };
  load();
  /* app.js runs before this file: if the page was opened on #tools, fill the screen now */
  if (location.hash === "#tools") open();
})();
