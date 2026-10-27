"use strict";
/* Judas web app: vanilla JS, no build step, no network beyond the local daemon. */
(function () {
  var $ = function (s, r) { return (r || document).querySelector(s); };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } }

  function api(path, opts) {
    return fetch(path, opts).then(function (r) {
      setBanner(false);
      return r.text().then(function (t) {
        if (!r.ok) {
          var msg = t;
          try {
            var je = JSON.parse(t).error;
            msg = je && je.message ? je.message : (typeof je === "string" ? je : t);
          } catch (e) { /* plain text */ }
          throw new Error(msg || ("HTTP " + r.status));
        }
        return t;
      });
    }, function (err) { setBanner(true); throw err; });
  }
  function patchJSON(path, body) {
    return api(path, { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) })
      .then(function (t) { return t ? JSON.parse(t) : {}; });
  }
  function getJSON(path) { return api(path).then(function (t) { return JSON.parse(t); }); }
  function postJSON(path, body) {
    return api(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) })
      .then(function (t) { return t ? JSON.parse(t) : {}; });
  }
  function setBanner(down) { $("#banner").hidden = !down; }

  function fmtTime(ms) {
    if (!ms) return "";
    var d = new Date(ms), now = new Date();
    return d.toDateString() === now.toDateString() ? d.toLocaleTimeString() : d.toLocaleString();
  }
  function fmtNum(n) { return n == null ? "-" : Number(n).toLocaleString(); }

  /* ---------- markdown-ish renderer ---------- */
  function inline(s) {
    s = esc(s);
    s = s.replace(/`([^`\n]+)`/g, "<code>$1</code>");
    s = s.replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>");
    return s;
  }
  function md(text) {
    var parts = String(text).split(/```/);
    var out = "";
    for (var i = 0; i < parts.length; i++) {
      if (i % 2 === 1) {
        var body = parts[i].replace(/^[^\n]*\n/, function (m) { return /^\s*[\w+-]*\s*\n$/.test(m) ? "" : m; });
        out += "<pre><code>" + esc(body.replace(/\n$/, "")) + "</code></pre>";
        continue;
      }
      var lines = parts[i].split("\n"), list = null, para = [];
      var flushPara = function () { if (para.length) { out += "<p>" + para.join("<br>") + "</p>"; para = []; } };
      var flushList = function () { if (list) { out += "</" + list + ">"; list = null; } };
      for (var j = 0; j < lines.length; j++) {
        var ln = lines[j], m;
        if ((m = /^\s*[-*]\s+(.*)$/.exec(ln))) {
          flushPara(); if (list !== "ul") { flushList(); out += "<ul>"; list = "ul"; }
          out += "<li>" + inline(m[1]) + "</li>";
        } else if ((m = /^\s*\d+[.)]\s+(.*)$/.exec(ln))) {
          flushPara(); if (list !== "ol") { flushList(); out += "<ol>"; list = "ol"; }
          out += "<li>" + inline(m[1]) + "</li>";
        } else if (ln.trim() === "") {
          flushPara(); flushList();
        } else {
          flushList(); para.push(inline(ln));
        }
      }
      flushPara(); flushList();
    }
    return out;
  }

  /* ---------- tab info: one source for the screen blurbs and the Guide ---------- */
  var INFO = {
    chat: { label: "Chat", mode: true,
      desc: "A quick conversation with the model. Ask a question, get an answer.",
      when: "Use it for quick questions, drafting, explaining, or thinking out loud. No project folder is needed, and helper agents rarely get involved." },
    cowork: { label: "Cowork", mode: true,
      desc: "Hand the model a larger job. It plans, splits the work across helper agents, and reports back. Watch them on the Agents board.",
      when: "Use it for multi-step or parallel work, like auditing a codebase or researching several options at once. The agent board sits beside the conversation, and Schedule this turns a prompt into a saved task." },
    code: { label: "Code", mode: true,
      desc: "Point the model at a project folder and let it read and edit files. Every change is saved as a snapshot you can undo.",
      when: "Use it when the agent should work in your repo. A project folder is required (recent folders are remembered). The Changes panel lists the files touched and links each one to its snapshot." },
    agents: { label: "Agents",
      desc: "Judas runs one model shared by many agents at once. A lead agent can spawn worker agents that each take on part of a job and run at the same time on that one model. This board shows every agent in the current session: the lead on top, its workers nested under it, each with its status and token count. It is how you see parallel work happening.",
      when: "Open it while a job is running to see who is doing what.",
      more: [
        "The lead is the agent you talk to. When a job is big, it can spawn workers and hand each one a piece. They all run at once on the same model, so nothing extra is loaded.",
        "Status words: queued means waiting for a free slot on the model, running means generating now, waiting means paused for a worker or a tool, done means finished, failed means it hit an error, killed means it was cancelled.",
        "Tokens: in is the text the agent read, out is the text it wrote. Bigger numbers mean more work.",
        "If you only ever chat, you will see one card, the lead. That is normal."] },
    tasks: { label: "Tasks",
      desc: "Saved prompts that run on demand, once at a set time, or on a repeating schedule. Scheduled runs appear as new sessions automatically.",
      when: "Use it for things you repeat, like a nightly check, or for a job you want to start later." },
    memory: { label: "Memory",
      desc: "What Judas remembers: the shared memory file every assistant reads, the part of it that goes into every prompt, the facts Judas wrote itself, and the skills it has learned.",
      when: "Open it to check what Judas knows about you, spot a wrong fact it saved, and remove it. Only facts Judas wrote (tagged [judas]) can be removed here; edit the rest in the file itself.",
      more: [
        "In every prompt: these sections are pasted into each conversation so the model always has them. The rest it reaches with its memory_read and memory_search tools.",
        "Written by Judas: facts it saved with memory_append, often from the background review after a long turn. Remove anything wrong or stale.",
        "Skills: Markdown instructions that load when a trigger word appears or the model asks for one. Agent skills were written by Judas; pinned skills are protected from its edits; archived ones were retired by the curator."] },
    snapshots: { label: "Snapshots",
      desc: "Every file the model changes is recorded here before it changes, so you can see the diff and undo any step, even whole runs.",
      when: "Open it when you want to review or undo what the model did. Revert this change undoes one step. Revert to here rolls back everything after it." },
    resources: { label: "Resources",
      desc: "Live view of the one model: which model is loaded, GPU memory used, how the shared context is filling, and each agent slot. The thirty coins show how much of the context is spent.",
      when: "Open it if things feel slow or you want to know how full the model's memory is." },
    heartbeat: { label: "Heartbeat",
      desc: "A read-only health check of your whole stack. Collectors gather the facts, the model summarizes them and flags anything wrong, and alerts go to your channel.",
      when: "Open it to see the latest report, run a check right now, read the raw facts each collector found, and send a test message to confirm your alert channel works. The report refreshes on its own while the screen is open." },
    stats: { label: "Statistics",
      desc: "How much you have used Judas: totals, a daily usage grid, and which models you run.",
      when: "Just for curiosity and bookkeeping." },
    settings: { label: "Settings",
      desc: "Pick a theme and see the API version. Themes restyle both the web app and the terminal UI.",
      when: "Change the look here. The choice is saved in this browser only, for now." },
    guide: { label: "Guide",
      desc: "Plain-language help for every tab. Start with the three modes, then the tools." }
  };
  var MODES = ["chat", "cowork", "code"];
  var SCREENS = ["chat", "cowork", "code", "agents", "tasks", "memory", "snapshots", "resources", "heartbeat", "stats", "settings", "guide"];
  var SCREEN_IDS = ["work", "agents", "tasks", "memory", "snapshots", "resources", "heartbeat", "stats", "settings", "guide"];
  var curScreen = "chat", curMode = "chat", sideHidden = false;

  function pageHead(name) {
    var h = el("header", "page-head");
    h.appendChild(el("h2", null, INFO[name].label));
    h.appendChild(el("p", "desc", INFO[name].desc));
    return h;
  }

  /* ---------- navigation ---------- */
  function markNav(name) {
    var id = MODES.indexOf(name) >= 0 ? "work" : name;
    SCREEN_IDS.forEach(function (s) { $("#screen-" + s).classList.toggle("active", s === id); });
    var inTools = false;
    Array.prototype.forEach.call(document.querySelectorAll("#rail button[data-screen]"), function (b) {
      var on = b.dataset.screen === name;
      b.classList.toggle("active", on);
      if (on && MODES.indexOf(name) < 0) inTools = true;
    });
    $("#rail-more").classList.toggle("active", inTools);
    $("#rail").classList.remove("more-open");
    $("#rail-more").setAttribute("aria-expanded", "false");
    if (location.hash !== "#" + name) history.replaceState(null, "", "#" + name);
  }
  function syncNav() {
    if (MODES.indexOf(curScreen) >= 0) { curScreen = curMode; markNav(curScreen); }
  }
  function show(name, opts) {
    if (SCREENS.indexOf(name) < 0) name = "chat";
    curScreen = name;
    if (MODES.indexOf(name) >= 0) enterMode(name, opts && opts.keep);
    markNav(name);
    if (name === "snapshots") loadSnapshots();
    if (name === "agents" || name === "cowork") renderBoard();
    if (name === "code") renderChanges();
    if (name === "tasks") { loadTasks(); applyTaskPrefill(); }
    if (name === "guide") renderGuide();
    if (name === "settings") renderSettings();
    if (name === "resources") renderResources();
    if (name === "heartbeat") openHeartbeat();
    if (name === "stats") loadStats();
    if (name === "memory") loadMemory();
  }
  $("#rail").addEventListener("click", function (e) {
    var b = e.target.closest("button[data-screen]");
    if (b) show(b.dataset.screen);
  });
  $("#rail-more").onclick = function () {
    var r = $("#rail"), open = !r.classList.contains("more-open");
    r.classList.toggle("more-open", open);
    $("#rail-more").setAttribute("aria-expanded", open ? "true" : "false");
  };
  document.addEventListener("click", function (e) {
    var g = e.target.closest("[data-screen-go]");
    if (g) { show(g.getAttribute("data-screen-go")); return; }
    if (!e.target.closest("#rail")) $("#rail").classList.remove("more-open");
  });

  /* ---------- state polling (header + resources) ---------- */
  var lastState = null;
  function pollState() {
    getJSON("/state").then(function (s) {
      lastState = s;
      var model = s.loaded ? (s.model || "model") : "no model loaded";
      $("#hdr-model").textContent = model + (s.hybrid ? " (hybrid)" : "");
      $("#hdr-model").title = s.model_path || "";
      var used = s.vram_used_mib || 0, tot = s.vram_total_mib || 0;
      $("#hdr-vram-txt").textContent = "VRAM " + (used / 1024).toFixed(1) + "/" + (tot / 1024).toFixed(1) + " GiB";
      $("#hdr-vram-bar").style.width = (tot ? Math.min(100, used / tot * 100) : 0) + "%";
      var ka = s.ctx_active || 0, kt = s.ctx_total || 0, kq = s.queued || 0;
      $("#hdr-kv-txt").textContent = "KV " + fmtNum(ka) + "/" + fmtNum(kt) + (kq ? "  " + kq + " queued" : "") + (s.preemptions ? "  " + s.preemptions + " pre" : "");
      $("#hdr-kv-bar").style.width = (kt ? Math.min(100, ka / kt * 100) : 0) + "%";
      if ($("#screen-resources").classList.contains("active")) renderResources();
      if ($("#screen-settings").classList.contains("active")) renderDaemon();
    }, function () { lastState = null; });
  }
  setInterval(pollState, 2000);

  /* ---------- sessions and modes ---------- */
  var SEALED_KEY = "judas.sealed";
  function sealedSet() { try { return JSON.parse(lsGet(SEALED_KEY) || "[]") || []; } catch (e) { return []; } }
  function isSealed(id) { return !!id && sealedSet().indexOf(id) >= 0; }
  function markSealed(id) { var a = sealedSet(); if (a.indexOf(id) < 0) a.push(id); lsSet(SEALED_KEY, JSON.stringify(a)); }
  function humanBytes(n) {
    n = Number(n) || 0;
    if (n < 1024) return n + " B";
    var u = ["KB", "MB", "GB", "TB"], i = -1;
    do { n /= 1024; i++; } while (n >= 1024 && i < u.length - 1);
    return (n >= 100 ? Math.round(n) : Math.round(n * 10) / 10) + " " + u[i];
  }
  var actionBusy = false;
  function syncSessionActions() {
    var has = !!cur.id, sealed = isSealed(cur.id);
    $("#sess-actions").hidden = !has;
    $("#sealed-tag").hidden = !sealed;
    $("#recover").hidden = sealed;
    $("#seal").disabled = sealed || actionBusy;
    $("#recover").disabled = actionBusy;
    renderAutoToggle();
    renderTurnBanner();
  }
  /* A turn that stopped short: paused (a cap was hit with work left, resumable) or failed (engine error). */
  var turnInfo = { status: "idle", reason: "", open_items: 0 };
  var autoDefault = false; // agents.auto_resume from the daemon
  function setTurnInfo(t) {
    turnInfo = { status: (t && t.status) || "idle", reason: (t && t.reason) || "", open_items: (t && t.open_items) || 0 };
    if (t && typeof t.auto_resume_default === "boolean") autoDefault = t.auto_resume_default;
    renderTurnBanner();
  }
  function renderTurnBanner() {
    var b = $("#turn-banner"), st = turnInfo.status, shown = !!cur.id && (st === "paused" || st === "error" || st === "failed");
    b.hidden = !shown;
    if (!shown) return;
    var txt;
    if (st === "paused") {
      txt = "paused: " + (turnInfo.reason || "a limit was reached");
      if (turnInfo.open_items) txt += " (" + turnInfo.open_items + " plan item" + (turnInfo.open_items === 1 ? "" : "s") + " still open)";
    } else {
      txt = "failed: " + (turnInfo.reason || "the model run hit an error");
    }
    b.className = "turn-banner" + (st === "paused" ? "" : " failed");
    $("#turn-banner-text").textContent = txt;
    $("#turn-resume").hidden = st !== "paused";
    $("#turn-resume").disabled = false;
  }
  function renderAutoToggle() {
    var box = $("#auto-resume"), v = cur.session && cur.session.auto_resume;
    box.checked = typeof v === "boolean" ? v : autoDefault;
  }
  $("#auto-resume").addEventListener("change", function () {
    if (!cur.id) return;
    var on = this.checked, sid = cur.id;
    patchJSON("/api/sessions/" + encodeURIComponent(sid), { auto_resume: on }).then(function (r) {
      if (cur.id === sid) { cur.session = r; renderAutoToggle(); }
    }, function (e) { addNote("Could not change auto-resume: " + e.message, true); renderAutoToggle(); });
  });
  $("#turn-resume").addEventListener("click", function () {
    if (!cur.id) return;
    var btn = this, sid = cur.id;
    btn.disabled = true;
    // the banner follows the turn's own events (spawned hides it, a new pause shows it again)
    postJSON("/api/sessions/" + encodeURIComponent(sid) + "/resume").then(function () {
      /* nothing to do: the stream's events carry the turn */
    }, function (e) { btn.disabled = false; addNote("Resume failed: " + e.message, true); });
  });
  var sessions = [];
  var sessionsMore = false; // the server has older sessions than the ones loaded
  var running = {};         // session id -> true while a turn is running (from the global event feed)
  var cur = mkCur(null);
  var live = null; // the open session's event stream and live turn state
  var sessFilter = "all";
  var lastUserText = "";

  function mkCur(id) { return { id: id, session: null, agents: [], fileops: [], snaps: [], cps: {}, more: false, oldest: 0 }; }

  /* the mode of each session lives on the server (any device sees the same one) */
  function sessionById(id) {
    for (var i = 0; i < sessions.length; i++) if (sessions[i].id === id) return sessions[i];
    return cur.session && cur.session.id === id ? cur.session : null;
  }
  function knownMode(m) { return MODES.indexOf(m) >= 0; }
  function modeOf(id) { var s = sessionById(id); return s && knownMode(s.mode) ? s.mode : "chat"; }
  function setModeOf(id, m) {
    var s = sessionById(id);
    if (s) s.mode = m;
    if (cur.session && cur.session.id === id) cur.session.mode = m;
    return api("/api/sessions/" + encodeURIComponent(id), {
      method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ mode: m })
    }).catch(function () { /* banner already set by api */ });
  }
  /* sessions used to remember their mode in this browser: hand those to the server once */
  function migrateModes() {
    if (lsGet("judas.modesMigrated")) return;
    var old;
    try { old = JSON.parse(lsGet("judas.modes") || "{}") || {}; } catch (e) { old = {}; }
    sessions.forEach(function (s) {
      if (s.mode === "chat" && (old[s.id] === "cowork" || old[s.id] === "code")) setModeOf(s.id, old[s.id]);
    });
    lsSet("judas.modesMigrated", "1");
  }

  /* recent project folders */
  function recents() { try { var r = JSON.parse(lsGet("judas.recentDirs") || "[]"); return Array.isArray(r) ? r : []; } catch (e) { return []; } }
  function pushRecent(d) {
    var r = recents().filter(function (x) { return x !== d; });
    r.unshift(d);
    lsSet("judas.recentDirs", JSON.stringify(r.slice(0, 8)));
    renderRecent();
  }
  function renderRecent() {
    var dl = $("#cwd-recent"), chips = $("#cwd-chips"), r = recents();
    dl.textContent = ""; chips.textContent = "";
    r.forEach(function (d) { var o = el("option"); o.value = d; dl.appendChild(o); });
    r.slice(0, 5).forEach(function (d) {
      var b = el("button", "chip", d); b.type = "button"; b.title = d;
      b.onclick = function () { $("#cwd").value = d; $("#cwd-err").hidden = true; };
      chips.appendChild(b);
    });
  }

  var MODE_UI = {
    chat: { title: "Chat", send: "Send", ph: "Ask anything. Enter sends, Shift+Enter for a new line.", cwd: "" },
    cowork: { title: "Cowork", send: "Delegate", ph: "Describe the larger job. Enter sends, Shift+Enter for a new line.", cwd: "Working folder (optional)" },
    code: { title: "Code", send: "Send", ph: "Tell Judas what to read or change in this project. Enter sends, Shift+Enter for a new line.", cwd: "Project folder (required)" }
  };
  var EMPTY_HINT = {
    chat: "Ask anything below. Chat needs no project folder.",
    cowork: "Describe the job below. The lead agent plans it and may spawn workers; watch them on the agent board.",
    code: "Enter a project folder above, then tell Judas what to read or change. Every edit becomes a snapshot."
  };
  function showEmpty() { logEl.appendChild(el("div", "empty", EMPTY_HINT[curMode])); }
  function clearEmpty() { Array.prototype.forEach.call(logEl.querySelectorAll(".empty"), function (n) { n.remove(); }); }

  function applyMode(m) {
    curMode = m;
    var ui = MODE_UI[m], w = $("#screen-work");
    w.setAttribute("data-mode", m);
    w.classList.toggle("side-hidden", sideHidden);
    $("#work-title").textContent = ui.title;
    $("#work-desc").textContent = INFO[m].desc;
    $("#mode-tag").textContent = "Mode: " + ui.title;
    $("#send").textContent = ui.send;
    $("#input").placeholder = ui.ph;
    $("#cwd-label").textContent = ui.cwd;
    $("#cwd-err").hidden = true;
    $("#sched").hidden = m !== "cowork";
    var pt = $("#panel-toggle");
    pt.hidden = m === "chat";
    pt.textContent = (sideHidden ? "Show " : "Hide ") + (m === "code" ? "changes" : "agents");
    renderRecent(); renderSessions(); renderChanges(); renderBoard();
  }

  /* picking a mode from the rail moves to that mode's latest session, or a fresh one */
  function enterMode(m, keep) {
    if (!keep && cur.id && modeOf(cur.id) !== m) {
      var pick = sessions.filter(function (s) { return modeOf(s.id) === m; })[0];
      if (pick) openSession(pick.id); else newSession(m);
      return;
    }
    applyMode(m);
  }

  function sortSessions() { sessions.sort(function (a, b) { return (b.updated_ms || 0) - (a.updated_ms || 0); }); }
  function loadSessions() {
    return getJSON("/api/sessions").then(function (r) {
      var first = r.sessions || [], seen = {};
      first.forEach(function (x) { seen[x.id] = true; });
      var floor = first.length && r.more ? first[first.length - 1].updated_ms : 0;
      // keep older pages the user already pulled in, drop rows the server no longer has in this window
      var older = floor ? sessions.filter(function (x) { return !seen[x.id] && (x.updated_ms || 0) < floor; }) : [];
      first.forEach(function (x) { if (x.running) running[x.id] = true; else delete running[x.id]; });
      if (typeof r.auto_resume_default === "boolean") autoDefault = r.auto_resume_default;
      sessions = first.concat(older);
      sessionsMore = !!r.more || (older.length > 0 && sessionsMore);
      sortSessions();
      renderSessions();
      migrateModes();
    }, function () { /* banner already set */ });
  }
  function loadOlderSessions() {
    var last = sessions[sessions.length - 1];
    if (!last) return;
    getJSON("/api/sessions?before=" + encodeURIComponent(last.updated_ms || 0)).then(function (r) {
      var have = {};
      sessions.forEach(function (x) { have[x.id] = true; });
      (r.sessions || []).forEach(function (x) { if (!have[x.id]) sessions.push(x); });
      sessionsMore = !!r.more;
      sortSessions();
      renderSessions();
    }, function () { /* banner already set */ });
  }
  /* the global event feed keeps the list live; it falls back to polling if the stream cannot be held */
  var feed = { es: null, poll: null, retry: null };
  function upsertSession(s) {
    if (!s || !s.id) return;
    var i = -1;
    sessions.forEach(function (x, n) { if (x.id === s.id) i = n; });
    if (i >= 0) sessions[i] = s; else sessions.push(s);
    if (cur.session && cur.session.id === s.id) { cur.session = s; $("#chat-title").textContent = s.title || s.id; }
    sortSessions();
  }
  function connectFeed() {
    if (!window.EventSource) { startFeedPoll(); return; }
    var es = new EventSource("/api/events");
    feed.es = es;
    es.onopen = function () {
      stopFeedPoll();
      loadSessions(); // anything that happened while the stream was down
    };
    es.onmessage = function (m) {
      var ev;
      try { ev = JSON.parse(m.data); } catch (e) { return; }
      if (ev.type === "session.created" || ev.type === "session.updated") {
        upsertSession(ev.session);
        renderSessions();
      } else if (ev.type === "session.deleted") {
        sessions = sessions.filter(function (x) { return x.id !== ev.id; });
        delete running[ev.id];
        renderSessions();
        if (cur.id === ev.id) { newSession(curMode); }
      } else if (ev.type === "turn.started") {
        running[ev.session] = true; renderSessions();
      } else if (ev.type === "turn.finished") {
        delete running[ev.session]; renderSessions();
      } else if (ev.type === "gap") {
        loadSessions();
      }
    };
    es.onerror = function () {
      es.close();
      feed.es = null;
      startFeedPoll();
      clearTimeout(feed.retry);
      feed.retry = setTimeout(connectFeed, 5000);
    };
  }
  function startFeedPoll() { if (!feed.poll) feed.poll = setInterval(loadSessions, 5000); }
  function stopFeedPoll() { clearInterval(feed.poll); feed.poll = null; }
  function renderSessions() {
    var box = $("#sess-list");
    box.textContent = "";
    var list = sessions.filter(function (s) {
      return sessFilter === "all" || s.id === cur.id || !knownMode(s.mode) || modeOf(s.id) === curMode;
    });
    if (!list.length) {
      var p = el("p", "dim small", sessFilter === "all" ? "No sessions yet." : "No " + MODE_UI[curMode].title + " sessions yet. Start one from the message box.");
      p.style.padding = ".8em"; box.appendChild(p);
    }
    list.forEach(function (s) {
      var row = el("div", "sess-row");
      var b = el("button", "sess-item" + (s.id === cur.id ? " active" : ""));
      b.type = "button";
      var tt = el("span", "t", s.title || s.id);
      if (running[s.id]) { var dot = el("span", "run-dot", "● "); dot.title = "a turn is running"; tt.insertBefore(dot, tt.firstChild); }
      b.appendChild(tt);
      var sub = el("span", "s");
      var m = modeOf(s.id);
      sub.appendChild(el("span", "tag mt m-" + m, MODE_UI[m].title));
      if (s.turn_status === "paused") sub.appendChild(el("span", "tag paused", "paused"));
      else if (s.turn_status === "error") sub.appendChild(el("span", "tag failed", "failed"));
      sub.appendChild(document.createTextNode(fmtTime(s.updated_ms)));
      b.appendChild(sub);
      b.onclick = function () { openSession(s.id); syncNav(); $("#sess-pane").classList.remove("open"); };
      var del = el("button", "sess-del", "×");
      del.type = "button";
      del.title = "Delete this chat";
      del.onclick = function (e) { e.stopPropagation(); deleteSession(s); };
      row.appendChild(b);
      row.appendChild(del);
      box.appendChild(row);
    });
    if (sessionsMore) {
      var more = el("button", "sess-more", "Show older chats");
      more.type = "button";
      more.onclick = loadOlderSessions;
      box.appendChild(more);
    }
    Array.prototype.forEach.call(document.querySelectorAll("#sess-filter button"), function (b) {
      b.classList.toggle("active", b.dataset.f === sessFilter);
      if (b.dataset.f === "mode") b.textContent = "Only " + MODE_UI[curMode].title;
    });
  }
  $("#sess-filter").addEventListener("click", function (e) {
    var b = e.target.closest("button[data-f]");
    if (b) { sessFilter = b.dataset.f; renderSessions(); }
  });
  $("#sess-toggle").onclick = function () { $("#sess-pane").classList.toggle("open"); };
  $("#panel-toggle").onclick = function () {
    sideHidden = !sideHidden;
    $("#screen-work").classList.toggle("side-hidden", sideHidden);
    $("#panel-toggle").textContent = (sideHidden ? "Show " : "Hide ") + (curMode === "code" ? "changes" : "agents");
  };
  function deleteSession(s) {
    if (!confirm("Delete “" + (s.title || s.id) + "” for good? This removes the chat and its undo history. Files it changed on disk are left as they are; use Recover first if you want those undone."))
      return;
    api("/api/sessions/" + encodeURIComponent(s.id), { method: "DELETE" }).then(function () {
      var wasCur = cur.id === s.id;
      sessions = sessions.filter(function (x) { return x.id !== s.id; });
      renderSessions();
      if (wasCur) newSession(curMode);
    }, function (err) { window.alert("Could not delete the chat: " + err.message); });
  }
  function newSession(m) {
    stopLive();
    cur = mkCur(null);
    logEl.textContent = "";
    applyMode(m);
    showEmpty();
    $("#chat-title").textContent = "New session";
    syncSessionActions();
    $("#cwd").value = m === "code" ? (recents()[0] || "") : "";
    renderSessions(); renderBoard(); renderChanges();
  }
  $("#new-session").onclick = function () { newSession(curMode); $("#sess-pane").classList.remove("open"); };

  function openSession(id) {
    stopLive();
    cur = mkCur(id);
    applyMode(modeOf(id));
    logEl.textContent = "";
    syncSessionActions();
    return loadSession(id);
  }
  function openInMode(sid, m) {
    setModeOf(sid, m).then(loadSessions).then(function () { show(m, { keep: true }); openSession(sid); syncNav(); });
  }
  var PAGE = 200; // messages per page of history
  function sessionURL(id, before) {
    return "/api/sessions/" + encodeURIComponent(id) + "?limit=" + PAGE + (before ? "&before=" + before : "");
  }
  function firstId(msgs) { return msgs.length ? msgs[0].id : 0; }
  function loadSession(id) {
    return getJSON(sessionURL(id)).then(function (r) {
      if (cur.id !== id) return;
      cur.session = r.session; cur.agents = r.agents || [];
      $("#chat-title").textContent = (r.session && r.session.title) || id;
      $("#cwd").value = (r.session && r.session.cwd) || "";
      var t = r.turn || {}, msgs = r.messages || [];
      setTurnInfo(t);
      // a turn in progress is rebuilt from its replayed events, so history stops where the turn began
      if (t.running) msgs = msgs.filter(function (m) { return m.id <= t.msg_floor; });
      cur.more = !!r.more_before; cur.oldest = firstId(r.messages || []);
      renderHistory(msgs);
      renderBoard();
      loadSessionSnaps();
      attachLive(id, t.running ? t.event_floor : t.events_head);
    }, function (e) { addNote("Could not load session: " + e.message, true); });
  }
  function loadOlder(btn) {
    var id = cur.id;
    if (!id || !cur.oldest) return;
    btn.disabled = true;
    getJSON(sessionURL(id, cur.oldest)).then(function (r) {
      if (cur.id !== id) return;
      var msgs = r.messages || [];
      cur.more = !!r.more_before;
      if (msgs.length) cur.oldest = msgs[0].id;
      renderHistory(msgs, true);
    }, function (e) { btn.disabled = false; addNote("Could not load older messages: " + e.message, true); });
  }
  function loadSessionSnaps() {
    var id = cur.id;
    if (!id) return;
    getJSON("/api/snapshots?session=" + encodeURIComponent(id) + "&limit=50").then(function (r) {
      if (cur.id !== id) return;
      cur.snaps = (r.snapshots || []).slice().sort(function (a, b) { return (b.created_ms || 0) - (a.created_ms || 0); });
      renderChanges();
    }, function () { /* banner */ });
  }

  /* ---------- chat rendering ---------- */
  var logEl = $("#log");
  function nearBottom() { return logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 80; }
  function scrollDown(force) { if (force || nearBottom()) logEl.scrollTop = logEl.scrollHeight; }
  function addNote(text, bad) { var d = el("div", "msg assistant" + (bad ? " err" : ""), text); logEl.appendChild(d); scrollDown(true); return d; }
  function addUser(text) { lastUserText = text; logEl.appendChild(el("div", "msg user", text)).style.whiteSpace = "pre-wrap"; scrollDown(true); }
  function addAssistant(text) { var d = el("div", "msg assistant"); d.innerHTML = md(text); logEl.appendChild(d); return d; }
  function addThink(text, open) {
    var d = el("details", "think");
    if (open) d.open = true;
    d.appendChild(el("summary", null, "thinking"));
    d.appendChild(el("div", "body", text));
    logEl.appendChild(d);
    return d;
  }
  function shortArgs(args) {
    var o = args;
    if (typeof args === "string") { try { o = JSON.parse(args); } catch (e) { return args.length > 90 ? args.slice(0, 90) + "..." : args; } }
    if (o && typeof o === "object") {
      var s = Object.keys(o).map(function (k) {
        var v = o[k]; if (typeof v !== "string") v = JSON.stringify(v);
        v = v.replace(/\s+/g, " ");
        return k + "=" + (v.length > 50 ? v.slice(0, 50) + "..." : v);
      }).join(" ");
      return s.length > 140 ? s.slice(0, 140) + "..." : s;
    }
    return String(o);
  }
  function addToolCall(name, args) {
    var d = el("details", "tool");
    var sm = el("summary");
    sm.appendChild(el("span", "st", "▸"));
    sm.appendChild(el("span", "nm", name));
    sm.appendChild(el("span", "ar", " " + shortArgs(args)));
    d.appendChild(sm);
    var full = typeof args === "string" ? args : JSON.stringify(args, null, 2);
    d.appendChild(el("pre", null, full));
    logEl.appendChild(d);
    return d;
  }
  function addToolResult(name, ok, output, checkpoint) {
    var d = el("details", "tool" + (ok === false ? " bad" : ""));
    var sm = el("summary");
    sm.appendChild(el("span", "st", ok === false ? "✗" : "✓"));
    sm.appendChild(el("span", "nm", name || "result"));
    var out = String(output == null ? "" : output);
    sm.appendChild(el("span", "ar", " " + out.replace(/\s+/g, " ").slice(0, 100)));
    if (checkpoint) {
      var a = el("a", null, "view snapshot " + String(checkpoint).slice(0, 8));
      a.onclick = function (ev) { ev.preventDefault(); ev.stopPropagation(); wantSnap = String(checkpoint); show("snapshots"); };
      sm.appendChild(a);
    }
    d.appendChild(sm);
    d.appendChild(el("pre", null, out.length > 6000 ? out.slice(0, 6000) + "\n[truncated]" : out));
    logEl.appendChild(d);
    return d;
  }

  /* ---------- file activity (Code mode side panel) ---------- */
  function isFileTool(name) { return /read|write|edit|patch|create|delete|remove|rename|move|append|replace|mkdir|file/i.test(name || ""); }
  function opPath(args) {
    var o = args;
    if (typeof args === "string") { try { o = JSON.parse(args); } catch (e) { return ""; } }
    if (!o || typeof o !== "object") return "";
    var k = ["path", "file_path", "file", "filename", "target", "dest"];
    for (var i = 0; i < k.length; i++) if (typeof o[k[i]] === "string") return o[k[i]];
    for (var key in o) if (typeof o[key] === "string" && o[key].length < 200) return o[key];
    return "";
  }
  function noteOp(name, args, ok, cp, worker) {
    if (!isFileTool(name)) return;
    cur.fileops.push({ name: name, path: opPath(args), ok: ok !== false, cp: cp || null, worker: !!worker });
  }
  function openSnap(id) { wantSnap = String(id); show("snapshots"); }
  function renderChanges() {
    var box = $("#changes");
    if (!box) return;
    box.textContent = "";
    var ops = cur.fileops || [], snaps = cur.snaps || [];
    if (!ops.length && !snaps.length) {
      box.appendChild(el("p", "dim", "Nothing yet. When the model reads or edits files, they are listed here."));
      return;
    }
    if (ops.length) {
      box.appendChild(el("div", "side-sec", "File activity"));
      ops.slice().reverse().slice(0, 60).forEach(function (o) {
        var r = el("div", "fop" + (o.ok ? "" : " bad"));
        r.appendChild(el("span", "st", o.ok ? "✓" : "✗"));
        r.appendChild(el("span", "nm", o.name + (o.worker ? " (helper)" : "")));
        if (o.cp) {
          var a = el("a", null, "view snapshot");
          a.onclick = function () { openSnap(o.cp); };
          r.appendChild(a);
        }
        r.appendChild(el("span", "pth mono", o.path || "(no path)"));
        box.appendChild(r);
      });
    }
    if (snaps.length) {
      box.appendChild(el("div", "side-sec", "Undo points (" + snaps.length + ")"));
      snaps.forEach(function (s) {
        var b = el("button", "snaprow");
        b.type = "button";
        b.appendChild(el("span", null, s.label || s.id));
        b.appendChild(el("span", "s", fmtTime(s.created_ms) + "  " + (s.files || []).length + " files" + (s.reverted ? "  reverted" : "")));
        b.onclick = function () { openSnap(s.id); };
        box.appendChild(b);
      });
    }
  }

  function olderBar() {
    var d = el("div", "older");
    var b = el("button", "chip", "Load older messages");
    b.type = "button";
    b.onclick = function () { loadOlder(b); };
    d.appendChild(b);
    return d;
  }
  // prepend=true renders an older page above what is shown (keeping the scroll position)
  function renderHistory(messages, prepend) {
    var keep = null, h0 = logEl.scrollHeight;
    if (prepend) {
      keep = document.createDocumentFragment();
      Array.prototype.forEach.call(logEl.querySelectorAll(".older"), function (n) { n.remove(); });
      while (logEl.firstChild) keep.appendChild(logEl.firstChild);
    } else {
      logEl.textContent = "";
      cur.fileops = [];
    }
    var childAgents = {};
    cur.agents.forEach(function (a) { if (a.parent) childAgents[a.id] = true; });
    var toolNames = {}, toolArgs = {};
    messages.forEach(function (m) {
      var child = !!(m.agent && childAgents[m.agent]); // workers live on the board, not in the conversation
      var b = m.body || {};
      if (b.role === "user") { if (!child) addUser(typeof b.content === "string" ? b.content : JSON.stringify(b.content)); return; }
      if (b.role === "assistant") {
        if (!child) {
          if (b.reasoning_content) addThink(b.reasoning_content, false);
          if (b.content) addAssistant(typeof b.content === "string" ? b.content : JSON.stringify(b.content));
        }
        (b.tool_calls || []).forEach(function (tc) {
          var f = tc.function || {};
          toolNames[tc.id] = f.name; toolArgs[tc.id] = f.arguments;
          if (!child) addToolCall(f.name || "tool", f.arguments);
        });
        return;
      }
      if (b.role === "tool") {
        var c = typeof b.content === "string" ? b.content : JSON.stringify(b.content);
        var nm = toolNames[b.tool_call_id], ok = !/^error/i.test(c), cp = cur.cps[b.tool_call_id] || null;
        noteOp(nm, toolArgs[b.tool_call_id], ok, cp, child);
        if (!child) addToolResult(nm, ok, c, cp);
      }
    });
    if (keep) logEl.appendChild(keep);
    if (!logEl.childNodes.length) showEmpty();
    if (cur.more) logEl.insertBefore(olderBar(), logEl.firstChild);
    renderChanges();
    if (prepend) logEl.scrollTop += logEl.scrollHeight - h0; else scrollDown(true);
  }

  /* ---------- live turn ---------- */
  var wantSnap = null;

  function stopLive() {
    if (live) { live.stopped = true; if (live.es) live.es.close(); clearTimeout(live.retry); clearInterval(live.poll); }
    live = null; setRunning(false);
  }
  function setRunning(on) {
    $("#turn-pill").hidden = !on;
    $("#cancel").hidden = !on;
    $("#send").disabled = on;
  }
  function isRunning() { return !$("#cancel").hidden; }

  function newAgentRec(id, parent, role, task) {
    return { id: id, parent: parent || "", role: role || "agent", task: task || "", status: "running", pt: 0, ct: 0, last: "", buf: "" };
  }
  function getAgent(id) {
    if (!live.agents[id]) live.agents[id] = newAgentRec(id, "", "agent", "");
    return live.agents[id];
  }
  function syncAgents(rows) {
    (rows || []).forEach(function (a) {
      var rec = live.agents[a.id] || (live.agents[a.id] = newAgentRec(a.id, a.parent, a.role, a.task));
      rec.status = a.status; rec.parent = a.parent || ""; rec.task = a.task || rec.task; rec.role = a.role || rec.role;
      rec.pt = Math.max(rec.pt, a.prompt_tokens || 0); rec.ct = Math.max(rec.ct, a.completion_tokens || 0);
    });
  }
  function resetTurnState() {
    live.bubble = null; live.bubbleText = ""; live.think = null; live.thinkText = ""; live.calls = {}; live.callArgs = {};
  }

  /* One event stream stays open for as long as a session is open, across turns: a turn started from this tab,
     another tab, curl or the scheduler all arrive on it. `since` is the event id to resume after; the server
     replays the turn in progress from its first event, so a refreshed page rebuilds the partial message. */
  function attachLive(sid, since) {
    stopLive();
    live = { sid: sid, es: null, agents: {}, bubble: null, bubbleText: "", think: null, thinkText: "", calls: {}, callArgs: {},
             retry: null, poll: null, stopped: false, tries: 0, lastId: since || 0, inTurn: false, echoed: false,
             turnAt: 0, attachedAt: Date.now() };
    syncAgents(cur.agents);
    // backstop: refresh the agent rows now and then, and notice a turn the stream did not tell us about
    live.poll = setInterval(function () {
      var mine = live;
      if (!mine) return;
      getJSON("/api/sessions/" + encodeURIComponent(sid) + "?limit=1").then(function (r) {
        if (live !== mine) return;
        syncAgents(r.agents);
        setTurnInfo(r.turn);   // catches a pause or failure the stream did not tell us about
        var lead = (r.agents || []).filter(function (a) { return !a.parent; });
        var busy = lead.some(function (a) { return a.status === "running" || a.status === "queued" || a.status === "waiting"; });
        var now = Date.now();
        if (!busy && mine.inTurn && now - mine.turnAt > 3000) endTurn();
        else if (busy && !mine.inTurn && now - mine.attachedAt > 4000) { mine.inTurn = true; mine.turnAt = now; setRunning(true); }
        renderBoard();
      }, function () { /* ignore */ });
    }, 4000);
    connect();
    renderBoard();
  }

  function connect() {
    if (!live || live.stopped) return;
    var mine = live;
    var es = new EventSource("/api/sessions/" + encodeURIComponent(live.sid) + "/events?follow=1&since=" + live.lastId);
    live.es = es;
    es.onmessage = function (m) {
      if (live !== mine) return;
      mine.tries = 0;
      if (m.lastEventId) mine.lastId = Math.max(mine.lastId, Number(m.lastEventId) || 0);
      var ev;
      try { ev = JSON.parse(m.data); } catch (e) { return; }
      handleEvent(ev);
    };
    es.onerror = function () {
      if (live !== mine || mine.stopped) return;
      es.close();
      // dropped (network, daemon restart): resume after the last id we saw, unless the session is gone
      getJSON("/api/sessions/" + encodeURIComponent(mine.sid) + "?limit=1").then(function () {
        if (live === mine) retry();
      }, function (e) {
        if (live !== mine) return;
        if (/no such session/i.test(e.message)) return;
        retry();
      });
    };
  }
  function retry() {
    if (!live || live.stopped) return;
    live.tries++;
    var wait = Math.min(8000, 800 * live.tries);
    live.retry = setTimeout(connect, wait);
  }

  function ensureBubble() {
    if (!live.bubble) {
      live.bubble = el("div", "msg assistant");
      live.bubbleText = "";
      logEl.appendChild(live.bubble);
    }
  }
  function beginTurn(task) {
    live.inTurn = true; live.turnAt = Date.now();
    setRunning(true);
    clearEmpty();
    if (!live.echoed && task) addUser(task);
    live.echoed = false;
    resetTurnState();
  }

  function handleEvent(ev) {
    if (!live) return;
    var d = ev.data || {}, lead = !ev.parent, a;
    if (ev.type === "gap") return; // the ring dropped older events; the history reload on turn end repairs the log
    if (ev.type === "review") { // hidden background review: one muted line, no board entry, no turn state
      if (d.summary) { var rn = addNote("review: " + d.summary, false); rn.classList.add("review-line"); }
      return;
    }
    if (lead && !live.inTurn && ev.type !== "done" && ev.type !== "status") beginTurn(ev.type === "spawned" ? d.task : "");
    switch (ev.type) {
      case "spawned":
        if (lead) setTurnInfo({ status: "running" });
        a = live.agents[ev.agent] || (live.agents[ev.agent] = newAgentRec(ev.agent, ev.parent, d.role || ev.role, d.task));
        a.parent = ev.parent || ""; a.role = d.role || ev.role || a.role; a.task = d.task || a.task; a.status = "running";
        break;
      case "status":
        a = getAgent(ev.agent);
        if (d.error) { a.status = "failed"; a.last = "error: " + d.error; if (lead) addNote("Error: " + d.error, true); }
        else if (lead && d.turn_status === "paused") a.status = "paused";
        else if (d.status) a.status = d.status;
        break;
      case "delta":
        a = getAgent(ev.agent); a.parent = ev.parent || a.parent; a.status = "running";
        a.buf = (a.buf + (d.text || "")).slice(-600);
        var ll = a.buf.split("\n").filter(function (x) { return x.trim(); }).pop();
        if (ll) a.last = ll.trim().slice(0, 200);
        if (lead) {
          var stick = nearBottom();
          ensureBubble();
          live.bubbleText += d.text || "";
          live.bubble.innerHTML = md(live.bubbleText);
          if (stick) scrollDown(true);
        }
        break;
      case "reasoning":
        a = getAgent(ev.agent); a.status = "running";
        if (lead) {
          if (!live.think) { live.think = addThink("", false); live.thinkText = ""; live.thinkBody = live.think.lastChild; }
          live.thinkText += d.text || "";
          live.thinkBody.textContent = live.thinkText;
          live.thinkBody.scrollTop = live.thinkBody.scrollHeight;
          scrollDown();
        }
        break;
      case "tool_call":
        a = getAgent(ev.agent); a.last = "tool: " + (d.name || "") + " " + shortArgs(d.args);
        live.calls[d.id] = d.name; live.callArgs[d.id] = d.args;
        if (lead) {
          live.bubble = null; live.think = null;
          addToolCall(d.name || "tool", d.args); scrollDown();
        }
        break;
      case "tool_result":
        a = getAgent(ev.agent);
        if (d.ok === false) a.last = "failed: " + (d.name || "tool");
        if (d.checkpoint) cur.cps[d.id] = d.checkpoint;
        noteOp(d.name || live.calls[d.id], live.callArgs[d.id], d.ok, d.checkpoint, !lead);
        if (isFileTool(d.name || live.calls[d.id])) renderChanges();
        if (d.checkpoint) loadSessionSnaps();
        if (lead) {
          live.bubble = null; live.think = null;
          addToolResult(d.name || live.calls[d.id], d.ok, d.output, d.checkpoint); scrollDown();
        }
        break;
      case "message":
        a = getAgent(ev.agent);
        if (d.usage) { a.pt += d.usage.prompt || 0; a.ct += d.usage.completion || 0; }
        if (lead) { live.bubble = null; live.think = null; }
        break;
      case "done":
        a = getAgent(ev.agent);
        a.status = d.status || "done"; if (d.result && !lead) a.last = String(d.result).replace(/\s+/g, " ").slice(0, 200);
        if (lead) {
          setTurnInfo({ status: d.status === "failed" ? "error" : (d.status || "done"), reason: d.reason || "", open_items: turnInfo.open_items });
          renderBoard(); endTurn(); return;
        }
        break;
      case "session_deleted":
        if (cur.id === live.sid) newSession(curMode);
        return;
    }
    renderBoard();
  }

  // The turn finished: re-read the stored history (the stream stays attached for the next turn).
  function endTurn() {
    if (!live) return;
    var mine = live, sid = live.sid;
    mine.inTurn = false;
    mine.echoed = false;
    setRunning(false);
    resetTurnState();
    loadSessions();
    getJSON(sessionURL(sid)).then(function (r) {
      if (cur.id !== sid || live !== mine) return;
      cur.agents = r.agents || [];
      cur.session = r.session || cur.session;
      setTurnInfo(r.turn);
      cur.more = !!r.more_before; cur.oldest = firstId(r.messages || []);
      syncAgents(cur.agents);
      if (mine.inTurn) return; // a new turn began while we were reading: leave the live render alone
      renderHistory(r.messages || []);
      renderBoard();
      loadSessionSnaps();
    }, function () { /* leave the live render as is */ });
  }

  /* ---------- sending ---------- */
  var input = $("#input");
  input.addEventListener("keydown", function (e) {
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); $("#composer").requestSubmit(); }
  });
  $("#cwd").addEventListener("input", function () { $("#cwd-err").hidden = true; });
  $("#composer").addEventListener("submit", function (e) {
    e.preventDefault();
    var text = input.value.trim();
    if (!text || isRunning()) return;
    var cwd = $("#cwd").value.trim(), mode = curMode;
    if (mode === "code" && !cwd) { $("#cwd-err").hidden = false; $("#cwd").focus(); return; }
    var body, url;
    if (/^\/learn(?:\s|$)/.test(text)) {
      // The server rewrites /learn to the shared prompt, so the TUI and the web chat send identical text.
      body = { request: text.slice(6).trim() };
      if (cur.id) body.session = cur.id;
      if (mode !== "chat" && cwd) body.cwd = cwd;
      url = "/api/learn";
    } else {
      body = { content: text };
      if (mode !== "chat" && cwd) body.cwd = cwd; // Chat sends no folder; the daemon uses its default
      var id = cur.id || "new";
      body.mode = mode; // the server owns the mode; a new session is created with it
      url = "/api/sessions/" + encodeURIComponent(id) + "/messages";
    }
    clearEmpty();
    addUser(text);
    if (live) live.echoed = true; // the turn's spawned event must not draw this message a second time
    input.value = "";
    setRunning(true);
    postJSON(url, body).then(function (r) {
      var sid = typeof r.session === "string" ? r.session : ((r.session || {}).id || cur.id); // POST replies with the bare id
      if (!sid) throw new Error("server returned no session");
      var fresh = !cur.id;
      cur.id = sid;
      cur.session = { id: sid, title: text.slice(0, 60), cwd: cwd, mode: mode };
      syncSessionActions();
      if (mode !== "chat" && cwd) pushRecent(cwd);
      $("#chat-title").textContent = cur.session.title;
      loadSessions();
      if (fresh || !live || live.sid !== sid) {
        cur.agents = [];
        attachLive(sid, 0); // a new session's ring holds only this turn: replay it from the start
        live.echoed = true;
        setRunning(true);
      }
    }).catch(function (err) {
      if (live) live.echoed = false;
      setRunning(false);
      addNote("Send failed: " + err.message, true);
      input.value = text;
    });
  });
  $("#cancel").onclick = function () {
    if (!cur.id) return;
    postJSON("/api/sessions/" + encodeURIComponent(cur.id) + "/cancel").catch(function (e) { addNote("Cancel failed: " + e.message, true); });
  };
  /* session actions: Recover (undo everything) and Seal (drop undo history) */
  $("#recover").onclick = function () {
    var id = cur.id;
    if (!id || actionBusy) return;
    if (!window.confirm("Undo every change this chat made? Files it wrote will be reverted to how they were before.")) return;
    actionBusy = true; syncSessionActions();
    postJSON("/api/sessions/" + encodeURIComponent(id) + "/recover").then(function (r) {
      actionBusy = false; syncSessionActions();
      if (cur.id !== id) return;
      if (r.error) { addNote("Recover problem: " + r.error, true); }
      var parts = [
        "Restored: " + (r.restored || []).length,
        "Removed: " + (r.removed || []).length
      ];
      var conf = (r.conflicts || []).length, irr = (r.irreversible || []).length;
      if (conf) parts.push("Conflicts (left as they are): " + conf);
      if (irr) parts.push("Could not be undone: " + irr);
      var msg = "Recover finished. " + parts.join(", ") + ".";
      var det = (r.conflicts || []).concat(r.irreversible || []).map(function (x) { return typeof x === "string" ? x : (x.path || x.cmd || JSON.stringify(x)); });
      if (det.length) msg += "\n" + det.slice(0, 10).join("\n") + (det.length > 10 ? "\n..." : "");
      addNote(msg, !!(conf || irr || r.error));
      loadSessionSnaps();
    }).catch(function (e) {
      actionBusy = false; syncSessionActions();
      addNote("Recover failed: " + e.message, true);
    });
  };
  $("#seal").onclick = function () {
    var id = cur.id;
    if (!id || actionBusy || isSealed(id)) return;
    if (!window.confirm("Seal this chat? The result stays, but you will no longer be able to undo its individual steps.")) return;
    actionBusy = true; syncSessionActions();
    postJSON("/api/sessions/" + encodeURIComponent(id) + "/seal").then(function (r) {
      actionBusy = false;
      markSealed(id);
      syncSessionActions();
      if (cur.id !== id) return;
      addNote("Sealed, freed " + humanBytes(r.freed_bytes));
      loadSessionSnaps();
    }).catch(function (e) {
      actionBusy = false; syncSessionActions();
      addNote("Seal failed: " + e.message, true);
    });
  };
  /* Cowork: turn the current prompt into a saved task */
  $("#sched").onclick = function () {
    prefillTask = { prompt: input.value.trim() || lastUserText, cwd: $("#cwd").value.trim() };
    show("tasks");
  };


  /* ---------- memory (shared memory file, prompt block, Judas-written facts, skills) ---------- */
  var mem = { m: null, sk: null, err: "", q: "", busy: "" };
  function loadMemory() {
    renderMemory();
    return Promise.all([
      getJSON("/api/memory").then(function (r) { mem.m = r; mem.err = ""; }, function (e) { mem.err = e.message || "failed"; }),
      getJSON("/api/skills").then(function (r) { mem.sk = r; }, function () { mem.sk = null; })
    ]).then(renderMemory);
  }
  function memHas(text, q) { return !q || String(text).toLowerCase().indexOf(q) >= 0; }
  function renderMemory() {
    var box = $("#mem");
    if (!box) return;
    var hadFocus = document.activeElement && document.activeElement.id === "mem-q";
    box.textContent = "";
    var head = el("div", "stats-head");
    head.appendChild(pageHead("memory"));
    var rf = el("button", "btn small", "Refresh"); rf.type = "button"; rf.onclick = loadMemory;
    head.appendChild(rf);
    box.appendChild(head);
    var m = mem.m;
    if (mem.err) { box.appendChild(el("div", "field-err", "Could not load memory: " + mem.err)); return; }
    if (!m) { box.appendChild(el("p", "dim", "Loading...")); return; }
    if (!m.exists) {
      box.appendChild(el("p", "dim", "No memory file at " + m.path + ". Set [memory] path in config.toml."));
    } else {
      var tiles = el("div", "stats card");
      var kb = statTile((m.bytes / 1024).toFixed(1) + " KB", "File size (budget ~50 KB)");
      if (m.bytes > 50 * 1024) kb.classList.add("warn");
      tiles.appendChild(kb);
      tiles.appendChild(statTile(fmtNum((m.sections || []).length), "Sections"));
      tiles.appendChild(statTile(fmtNum(m.bullets), "Facts"));
      tiles.appendChild(statTile(fmtNum((m.judas_lines || []).length), "Written by Judas"));
      var pt = statTile(fmtNum(m.prompt_tokens) + " / " + fmtNum(m.max_memory_tokens), "Prompt tokens");
      if (m.truncated) pt.classList.add("warn");
      tiles.appendChild(pt);
      box.appendChild(tiles);
      var src = el("p", "dim small", m.path + (m.mtime_ms ? "  ·  changed " + fmtRel(m.mtime_ms) : ""));
      box.appendChild(src);

      var qi = el("input", "mem-q"); qi.id = "mem-q"; qi.type = "search";
      qi.placeholder = "Search memory and skills"; qi.value = mem.q; qi.setAttribute("aria-label", "Search memory");
      qi.oninput = function () { mem.q = qi.value; renderMemory(); };
      box.appendChild(qi);
      var q = mem.q.trim().toLowerCase();

      /* in the prompt */
      box.appendChild(el("h3", null, "In every prompt"));
      var pc = el("div", "card");
      var chips = el("div", "chips");
      (m.inject_sections || []).forEach(function (n) { chips.appendChild(el("span", "tag m-cowork", n)); });
      if (!(m.inject_sections || []).length) pc.appendChild(el("p", "dim", "No sections are injected ([memory] inject_sections is empty)."));
      pc.appendChild(chips);
      if (m.truncated) pc.appendChild(el("p", "field-err", "The block is cut off at prompt.max_memory_tokens; the model only sees the start of it."));
      var pd = el("details", "hb-fact");
      var ps = el("summary"); ps.appendChild(el("b", "fn", "Exact text the model gets")); pd.appendChild(ps);
      pd.appendChild(el("pre", "fb", m.prompt_block || "(empty)"));
      pc.appendChild(pd);
      box.appendChild(pc);

      /* written by judas */
      var mine = (m.judas_lines || []).filter(function (x) { return memHas(x.line + " " + x.section, q); });
      box.appendChild(el("h3", null, "Written by Judas"));
      var jc = el("div", "card");
      if (!(m.judas_lines || []).length) jc.appendChild(el("p", "dim", "Judas has not saved any facts yet."));
      else if (!mine.length) jc.appendChild(el("p", "dim", "No Judas facts match."));
      mine.slice().reverse().forEach(function (x) {
        var row = el("div", "mem-row");
        row.appendChild(el("span", "tag", x.section));
        row.appendChild(el("span", "mem-line", x.line.replace(/^- /, "")));
        var rm = el("button", "btn small danger", mem.busy === x.line ? "Removing..." : "Remove");
        rm.type = "button"; rm.disabled = !!mem.busy;
        rm.onclick = function () {
          if (!confirm("Remove this fact from the shared memory file?\n\n" + x.line)) return;
          mem.busy = x.line; renderMemory();
          postJSON("/api/memory/forget", { line: x.line }).then(function () { mem.busy = ""; return loadMemory(); },
            function (e) { mem.busy = ""; alert("Not removed: " + e.message); loadMemory(); });
        };
        row.appendChild(rm);
        jc.appendChild(row);
      });
      box.appendChild(jc);

      /* all sections */
      box.appendChild(el("h3", null, "All sections"));
      var shown = 0;
      (m.sections || []).forEach(function (sec) {
        if (!memHas(sec.text, q)) return;
        shown++;
        var d = el("details", "hb-fact");
        if (q) d.open = true;
        var sm = el("summary");
        sm.appendChild(el("b", "fn", sec.name));
        var meta = sec.bullets + " facts" + (sec.judas ? ", " + sec.judas + " by Judas" : "") + (sec.injected ? "  ·  in every prompt" : "");
        sm.appendChild(el("span", "fl", meta));
        d.appendChild(sm);
        var body = el("div", "fb mem-md");
        var txt = sec.text;
        if (q) txt = txt.split("\n").filter(function (l) { return /^#/.test(l) || l.toLowerCase().indexOf(q) >= 0; }).join("\n");
        body.innerHTML = md(txt);
        d.appendChild(body);
        box.appendChild(d);
      });
      if (!shown) box.appendChild(el("p", "dim", q ? "No sections match." : "The file has no ## sections."));
    }

    /* skills */
    var sk = mem.sk, q2 = mem.q.trim().toLowerCase();
    box.appendChild(el("h3", null, "Skills"));
    if (!sk) { box.appendChild(el("p", "dim", "Could not read skills.")); }
    else {
      var list = (sk.skills || []).filter(function (s) { return memHas(s.name + " " + s.description + " " + (s.triggers || []).join(" ") + " " + s.body, q2); });
      box.appendChild(el("p", "dim small", sk.dir + "  ·  autoload " + sk.autoload + (sk.enabled ? "" : "  ·  disabled")));
      if (!(sk.skills || []).length) box.appendChild(el("p", "dim", "No skills yet. Judas writes them with skill_manage, or run /learn in a chat."));
      list.forEach(function (s) {
        var d = el("details", "hb-fact");
        var sm = el("summary");
        sm.appendChild(el("b", "fn", s.name));
        var tags = el("span", "chips");
        tags.appendChild(el("span", "tag" + (s.source === "agent" ? " m-cowork" : ""), s.source.indexOf("external:") === 0 ? "external" : s.source));
        if (s.pinned) tags.appendChild(el("span", "tag sealed", "pinned"));
        if (s.usage && s.usage.state === "stale") tags.appendChild(el("span", "tag paused", "stale"));
        tags.appendChild(el("span", "tag", "v" + s.version));
        sm.appendChild(tags);
        sm.appendChild(el("span", "fl", s.description));
        d.appendChild(sm);
        var u = s.usage;
        var info = (u ? u.loads + " loads, " + u.autoloads + " auto, " + u.patches + " edits" + (u.last_used_ms ? ", last used " + fmtRel(u.last_used_ms) : ", never used") : "no usage recorded");
        if ((s.triggers || []).length) info += "  ·  triggers: " + s.triggers.join(", ");
        d.appendChild(el("p", "dim small mem-skinfo", info));
        var body = el("div", "fb mem-md"); body.innerHTML = md(s.body || ""); d.appendChild(body);
        box.appendChild(d);
      });
      if ((sk.skills || []).length && !list.length) box.appendChild(el("p", "dim", "No skills match."));
      if ((sk.archived || []).length) box.appendChild(el("p", "dim small", "Archived by the curator: " + sk.archived.join(", ")));
      if ((sk.quarantined || []).length) box.appendChild(el("p", "field-err", "Quarantined by the skills guard: " + sk.quarantined.join(", ")));
    }
    if (hadFocus) { var f = $("#mem-q"); if (f) { f.focus(); f.setSelectionRange(f.value.length, f.value.length); } }
  }

  /* ---------- agents board (Agents tab and the Cowork side panel) ---------- */
  function boardAgents() {
    if (live) return Object.keys(live.agents).map(function (k) { return live.agents[k]; });
    return cur.agents.map(function (a) {
      var r = newAgentRec(a.id, a.parent, a.role, a.task);
      r.status = a.status; r.pt = a.prompt_tokens || 0; r.ct = a.completion_tokens || 0;
      r.last = a.result ? String(a.result).replace(/\s+/g, " ").slice(0, 200) : "";
      return r;
    });
  }
  function agentCard(a, kids, isLead) {
    var c = el("div", "agent " + a.status + (isLead ? " lead" : ""));
    var hd = el("div", "hd");
    hd.appendChild(el("span", "role", isLead ? "lead: " + a.role : a.role));
    hd.appendChild(el("span", "badge " + a.status, a.status));
    hd.appendChild(el("span", "id mono", String(a.id).slice(0, 10)));
    c.appendChild(hd);
    if (a.task) c.appendChild(el("div", "task", a.task.length > 240 ? a.task.slice(0, 240) + "..." : a.task));
    c.appendChild(el("div", "tk mono", "in " + fmtNum(a.pt) + "  out " + fmtNum(a.ct) + " tokens"));
    c.appendChild(el("div", "last", a.last || ""));
    if (kids && kids.length) {
      var k = el("div", "kids");
      kids.forEach(function (x) { k.appendChild(agentCard(x, null, false)); });
      c.appendChild(k);
    }
    return c;
  }
  function renderBoard() {
    var targets = [];
    if (curScreen === "agents") targets.push($("#board"));
    if (curScreen === "cowork") targets.push($("#side-board"));
    if (!targets.length) return;
    var list = boardAgents();
    var workers = list.filter(function (a) { return a.parent; }).length;
    var sub = cur.id
      ? ((cur.session && cur.session.title) || cur.id) + (live && live.inTurn ? ": turn running, " : ": ") + list.length + " agent" + (list.length === 1 ? "" : "s") + (workers ? " (" + workers + " worker" + (workers === 1 ? "" : "s") + ")" : "")
      : "No session selected.";
    $("#agents-sub").textContent = sub;
    $("#side-sub").textContent = cur.id ? sub : "No agents yet. Send a job and the lead agent appears here.";
    var ids = {};
    list.forEach(function (a) { ids[a.id] = true; });
    targets.forEach(function (board) {
      board.textContent = "";
      if (!list.length) {
        board.appendChild(el("p", "dim", "No agents yet. Send a message in Chat, Cowork or Code. The lead agent appears here, and its workers appear under it when it spawns them."));
        return;
      }
      list.filter(function (a) { return !a.parent || !ids[a.parent]; }).forEach(function (r) {
        var kids = list.filter(function (a) { return a.parent === r.id; });
        board.appendChild(agentCard(r, kids, !r.parent));
      });
    });
  }

  /* ---------- tasks ---------- */
  var tasks = [], runMsgs = {}, prefillTask = null;
  function fmtInterval(s) {
    s = Number(s) || 0;
    if (s && s % 3600 === 0) return "every " + (s / 3600) + (s === 3600 ? " hour" : " hours");
    if (s && s % 60 === 0) return "every " + (s / 60) + (s === 60 ? " minute" : " minutes");
    return "every " + s + " seconds";
  }
  function localDtValue(ms) {
    var d = new Date(ms), p = function (n) { return (n < 10 ? "0" : "") + n; };
    return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + "T" + p(d.getHours()) + ":" + p(d.getMinutes());
  }
  function syncKind() {
    var k = $("#t-kind").value;
    document.querySelector(".kind-once").hidden = k !== "once";
    document.querySelector(".kind-interval").hidden = k !== "interval";
  }
  $("#t-kind").onchange = syncKind;
  $("#t-when").value = localDtValue(Date.now() + 3600 * 1000);
  function applyTaskPrefill() {
    if (!prefillTask) return;
    $("#t-prompt").value = prefillTask.prompt || "";
    $("#t-cwd").value = prefillTask.cwd || "";
    $("#task-new").open = true;
    prefillTask = null;
    $("#t-prompt").focus();
  }
  function taskErr(msg) { var e = $("#t-err"); e.textContent = msg || ""; e.hidden = !msg; }
  $("#task-form").addEventListener("submit", function (e) {
    e.preventDefault();
    taskErr("");
    var prompt = $("#t-prompt").value.trim(), kind = $("#t-kind").value;
    if (!prompt) { taskErr("Write a prompt: what should the model do?"); return; }
    var body = { prompt: prompt, kind: kind, enabled: true };
    var title = $("#t-title").value.trim(), cwd = $("#t-cwd").value.trim();
    if (title) body.title = title;
    if (cwd) body.cwd = cwd;
    if (kind === "once") {
      var t = new Date($("#t-when").value).getTime();
      if (!t || isNaN(t)) { taskErr("Pick the date and time to run."); return; }
      if (t < Date.now()) { taskErr("That time is in the past. Pick a later time."); return; }
      body.run_at_ms = t;
    } else if (kind === "interval") {
      var n = parseInt($("#t-every").value, 10);
      if (!n || n < 1) { taskErr("Enter how often to repeat, 1 or more."); return; }
      body.interval_s = n * parseInt($("#t-unit").value, 10);
    }
    postJSON("/api/tasks", body).then(function () {
      $("#task-form").reset(); syncKind();
      $("#t-when").value = localDtValue(Date.now() + 3600 * 1000);
      $("#task-new").open = false;
      loadTasks();
    }, function (err) { taskErr("Could not create the task: " + err.message); });
  });
  function loadTasks() {
    return getJSON("/api/tasks").then(function (r) {
      tasks = r.tasks || [];
      renderTasks(); // the server files task runs under Cowork
    }, function (e) {
      var box = $("#task-list"); box.textContent = "";
      box.appendChild(el("p", "dim", "Could not load tasks: " + e.message));
    });
  }
  function fmtRel(ms) {
    if (!ms) return "-";
    var d = ms - Date.now(), s = Math.round(Math.abs(d) / 1000), t;
    if (s < 45) t = s + " sec";
    else if (s < 3600) t = Math.round(s / 60) + " min";
    else if (s < 86400) t = Math.round(s / 3600) + " h";
    else t = Math.round(s / 86400) + " days";
    return d >= 0 ? "in " + t : t + " ago";
  }
  function metaRow(dl, k, v) { dl.appendChild(el("dt", null, k)); var dd = el("dd"); if (v && v.nodeType) dd.appendChild(v); else dd.textContent = v; dl.appendChild(dd); return dd; }
  function renderTasks() {
    var box = $("#task-list");
    box.textContent = "";
    if (!tasks.length) {
      box.appendChild(el("p", "dim", "No tasks yet. Use New task above, or press Schedule this in Cowork to save a prompt."));
      return;
    }
    tasks.slice().sort(function (a, b) { return (b.created_ms || 0) - (a.created_ms || 0); }).forEach(function (t) {
      var c = el("div", "task" + (t.enabled ? "" : " off"));
      var th = el("div", "th");
      th.appendChild(el("b", null, t.title || (t.prompt || "").slice(0, 60) || t.id));
      th.appendChild(el("span", "tag", t.kind));
      th.appendChild(el("span", "badge " + (t.enabled ? "ok" : ""), t.enabled ? "enabled" : "paused"));
      c.appendChild(th);
      c.appendChild(el("div", "pr", t.prompt || ""));
      var dl = el("dl", "meta");
      var sched = t.kind === "once" ? "once at " + fmtTime(t.run_at_ms) : t.kind === "interval" ? fmtInterval(t.interval_s) : "manual: runs only when you press Run now";
      metaRow(dl, "Schedule", sched);
      var next;
      if (t.kind === "manual") next = "-";
      else if (!t.enabled) next = t.kind === "once" && t.last_run_ms ? "already ran" : "paused";
      else if (t.next_ms) next = fmtRel(t.next_ms) + "  (" + fmtTime(t.next_ms) + ")";
      else next = "-";
      metaRow(dl, "Next run", next);
      var lastDd = metaRow(dl, "Last run", t.last_run_ms ? fmtRel(t.last_run_ms) + "  (" + fmtTime(t.last_run_ms) + ")" : "never");
      if (t.last_session) {
        var a = el("a", null, "  open in Cowork");
        a.href = "#cowork"; a.style.color = "var(--accent)";
        a.onclick = function (ev) { ev.preventDefault(); openInMode(t.last_session, "cowork"); };
        lastDd.appendChild(a);
      }
      if (t.cwd) metaRow(dl, "Folder", t.cwd).className = "mono";
      c.appendChild(dl);

      var acts = el("div", "acts");
      var sw = el("button", "switch"); sw.type = "button"; sw.setAttribute("role", "switch");
      sw.setAttribute("aria-checked", t.enabled ? "true" : "false");
      sw.appendChild(el("span", "trk")); sw.appendChild(el("span", null, "Enabled"));
      sw.onclick = function () { postJSON("/api/tasks/" + encodeURIComponent(t.id) + "/toggle").then(loadTasks, function (e) { window.alert("Toggle failed: " + e.message); }); };
      acts.appendChild(sw);
      acts.appendChild(el("span", "sp"));
      var run = el("button", "btn primary", "Run now"); run.type = "button";
      run.onclick = function () {
        run.disabled = true;
        postJSON("/api/tasks/" + encodeURIComponent(t.id) + "/run").then(function (r) {
          var s = r.session; runMsgs[t.id] = typeof s === "string" ? s : (s && s.id) || "";
          loadSessions(); loadTasks();
        }, function (e) { run.disabled = false; window.alert("Run failed: " + e.message); });
      };
      acts.appendChild(run);
      var del = el("button", "btn danger", "Delete"); del.type = "button";
      del.onclick = function () {
        if (!window.confirm("Delete the task \"" + (t.title || t.id) + "\"? Sessions it already created are kept.")) return;
        api("/api/tasks/" + encodeURIComponent(t.id), { method: "DELETE" }).then(loadTasks, function (e) { window.alert("Delete failed: " + e.message); });
      };
      acts.appendChild(del);
      c.appendChild(acts);

      if (runMsgs[t.id]) {
        var sid = runMsgs[t.id];
        var rm = el("div", "runmsg");
        rm.appendChild(el("div", null, "Started as a new session. Watch it on the Cowork agent board."));
        var ra = el("div", "acts");
        var oc = el("button", "btn primary small", "Open in Cowork"); oc.type = "button";
        oc.onclick = function () { delete runMsgs[t.id]; openInMode(sid, "cowork"); };
        var dm = el("button", "btn small", "Dismiss"); dm.type = "button";
        dm.onclick = function () { delete runMsgs[t.id]; renderTasks(); };
        if (sid) ra.appendChild(oc);
        ra.appendChild(dm);
        rm.appendChild(ra); c.appendChild(rm);
      }
      box.appendChild(c);
    });
  }
  setInterval(function () { if (curScreen === "tasks") loadTasks(); }, 15000);

  /* ---------- guide ---------- */
  function guideCard(name, feature) {
    var inf = INFO[name], c = el("div", "gcard" + (feature ? " feature" : ""));
    var h = el("h3"); h.appendChild(el("span", null, inf.label));
    var go = el("button", "btn small", "Open " + inf.label); go.type = "button"; go.setAttribute("data-screen-go", name);
    h.appendChild(go); c.appendChild(h);
    c.appendChild(el("p", null, inf.desc));
    if (inf.when) c.appendChild(el("p", "when", inf.when));
    if (inf.more) { var ul = el("ul"); inf.more.forEach(function (m) { ul.appendChild(el("li", null, m)); }); c.appendChild(ul); }
    return c;
  }
  function renderGuide() {
    var box = $("#guide");
    box.textContent = "";
    box.appendChild(pageHead("guide"));
    var intro = el("div", "note");
    intro.appendChild(el("p", null, "Judas is a local AI agent harness. One model runs on this machine and every agent shares it. The three modes are three ways to work with it. The tools are for watching, scheduling and undoing what it does."));
    intro.appendChild(el("p", null, "Not sure where to start? Use Chat for a question, Code to work on a project, and Cowork to hand off a bigger job."));
    intro.lastChild.style.marginBottom = "0";
    box.appendChild(intro);
    box.appendChild(el("h3", null, "The three modes"));
    var g1 = el("div", "ggrid");
    MODES.forEach(function (m) { g1.appendChild(guideCard(m)); });
    box.appendChild(g1);
    box.appendChild(el("h3", null, "Tools"));
    box.appendChild(guideCard("agents", true));
    var g2 = el("div", "ggrid");
    ["tasks", "memory", "snapshots", "resources", "heartbeat", "stats", "settings"].forEach(function (n) { g2.appendChild(guideCard(n)); });
    box.appendChild(g2);
  }

  /* ---------- snapshots ---------- */
  var snaps = [], snapSel = null;
  function loadSnapshots() {
    var q = "/api/snapshots?limit=200";
    return getJSON(q).then(function (r) {
      snaps = (r.snapshots || []).slice().sort(function (a, b) { return (b.created_ms || 0) - (a.created_ms || 0); });
      renderSnapList();
      if (wantSnap) { var w = wantSnap; wantSnap = null; selectSnap(w); }
    }, function () { /* banner */ });
  }
  $("#snap-refresh").onclick = loadSnapshots;
  function renderSnapList() {
    var box = $("#snap-list");
    box.textContent = "";
    if (!snaps.length) { var p = el("p", "dim", "No snapshots yet."); p.style.padding = ".8em"; box.appendChild(p); }
    snaps.forEach(function (s) {
      var b = el("button", "sess-item snap-item" + (String(s.id) === snapSel ? " active" : ""));
      b.appendChild(el("span", "t", s.label || s.id));
      var row = el("span", "row s");
      row.appendChild(document.createTextNode(fmtTime(s.created_ms) + " "));
      row.appendChild(el("span", "tag", (s.files || []).length + " files"));
      if ((s.irreversible || []).length) row.appendChild(el("span", "tag irr", "IRREVERSIBLE"));
      if (s.reverted) row.appendChild(el("span", "tag rev", "reverted"));
      b.appendChild(row);
      b.onclick = function () { selectSnap(s.id); };
      box.appendChild(b);
    });
  }
  function selectSnap(id) {
    snapSel = String(id);
    renderSnapList();
    var s = snaps.filter(function (x) { return String(x.id) === snapSel; })[0];
    var box = $("#snap-detail");
    box.textContent = "";
    if (!s) { box.appendChild(el("p", "dim", "Snapshot " + id + " is not in the list.")); return; }
    box.appendChild(el("h2", null, s.label || s.id));
    var meta = el("div", "files");
    meta.textContent = fmtTime(s.created_ms) + "  session " + (s.session || "-") + "  agent " + (s.agent || "-") + "  " + (s.committed ? "committed" : "uncommitted");
    box.appendChild(meta);
    if ((s.irreversible || []).length) {
      var ir = el("div", "result bad");
      ir.appendChild(el("b", null, "Irreversible side effects (a revert cannot undo these):"));
      var ul = el("ul");
      s.irreversible.forEach(function (x) { ul.appendChild(el("li", null, typeof x === "string" ? x : JSON.stringify(x))); });
      ir.appendChild(ul); box.appendChild(ir);
    }
    if ((s.files || []).length) {
      var fl = el("div", "files mono");
      fl.textContent = s.files.map(function (f) { return f.path; }).join("\n");
      fl.style.whiteSpace = "pre-wrap";
      box.appendChild(fl);
    }
    var act = el("div", "actions");
    var rv = el("button", "btn danger", s.reverted ? "Revert again" : "Revert this change");
    var rt = el("button", "btn danger", "Revert to here");
    rv.onclick = function () { doRevert(s, false, resultBox); };
    rt.onclick = function () { doRevert(s, true, resultBox); };
    act.appendChild(rv); act.appendChild(rt);
    box.appendChild(act);
    var resultBox = el("div");
    box.appendChild(resultBox);
    var diffBox = el("div", "diff mono");
    diffBox.appendChild(el("div", "meta", "loading diff..."));
    box.appendChild(diffBox);
    api("/api/snapshots/" + encodeURIComponent(s.id) + "/diff").then(function (t) {
      if (snapSel !== String(s.id)) return;
      diffBox.textContent = "";
      if (!t.trim()) { diffBox.appendChild(el("div", "meta", "(empty diff)")); return; }
      t.split("\n").forEach(function (ln) {
        var c = "";
        if (/^(\+\+\+|---|diff |index )/.test(ln)) c = "meta";
        else if (ln[0] === "+") c = "add";
        else if (ln[0] === "-") c = "del";
        else if (ln.slice(0, 2) === "@@") c = "hunk";
        diffBox.appendChild(el("div", c, ln));
      });
    }, function (e) { diffBox.textContent = "Could not load diff: " + e.message; });
  }
  function doRevert(s, to, out) {
    var msg = to
      ? "Revert everything back to the state at \"" + (s.label || s.id) + "\"? This undoes this and every later change."
      : "Revert \"" + (s.label || s.id) + "\"?";
    if ((s.irreversible || []).length) msg += "\n\nThis snapshot has irreversible side effects; a revert cannot undo them.";
    if (!window.confirm(msg)) return;
    out.textContent = "reverting...";
    postJSON("/api/snapshots/" + encodeURIComponent(s.id) + "/revert", to ? { to: true } : {}).then(function (r) {
      out.textContent = "";
      var box = el("div", "result" + (r.ok === false || (r.conflicts || []).length || r.error ? " bad" : ""));
      box.appendChild(el("b", null, r.ok === false ? "Revert failed" : "Revert finished"));
      if (r.error) box.appendChild(el("div", null, r.error));
      [["restored", r.restored], ["removed", r.removed], ["conflicts", r.conflicts], ["irreversible (not undone)", r.irreversible]].forEach(function (p) {
        if (p[1] && p[1].length) {
          box.appendChild(el("div", null, p[0] + " (" + p[1].length + ")"));
          var ul = el("ul");
          p[1].forEach(function (x) { ul.appendChild(el("li", "mono", typeof x === "string" ? x : JSON.stringify(x))); });
          box.appendChild(ul);
        }
      });
      out.appendChild(box);
      loadSnapshots();
    }, function (e) {
      out.textContent = "";
      out.appendChild(el("div", "result bad", "Revert failed: " + e.message));
    });
  }

  /* ---------- resources ---------- */
  function bar(frac, hot) {
    var b = el("div", "bar" + (hot ? " hot" : ""));
    var f = el("span"); f.style.width = Math.max(0, Math.min(100, frac * 100)) + "%";
    b.appendChild(f); return b;
  }
  function card(label, right) {
    var c = el("div", "card"), l = el("div", "lbl");
    l.appendChild(el("span", null, label)); l.appendChild(el("span", "mono", right || ""));
    c.appendChild(l); return c;
  }
  function renderResources() {
    var box = $("#res"), s = lastState;
    box.textContent = "";
    box.appendChild(pageHead("resources"));
    if (!s) { box.appendChild(el("p", "dim", "No state from the daemon.")); return; }
    var sub = el("p", "dim", (s.loaded ? s.model : "no model loaded") + (s.hybrid ? "  hybrid" : "") + (s.model_path ? "  " + s.model_path : ""));
    sub.style.overflowWrap = "anywhere"; box.appendChild(sub);

    var vu = s.vram_used_mib || 0, vt = s.vram_total_mib || 0;
    var cv = card("VRAM", (vu / 1024).toFixed(1) + " / " + (vt / 1024).toFixed(1) + " GiB");
    cv.appendChild(bar(vt ? vu / vt : 0, vt && vu / vt > 0.92)); box.appendChild(cv);

    var cu = s.ctx_used || 0, ct = s.ctx_total || 0, frac = ct ? cu / ct : 0;
    var cc = card("Context", fmtNum(cu) + " / " + fmtNum(ct) + " tokens");
    cc.appendChild(bar(frac, frac > 0.85));
    var coins = el("div", "coins");
    var spent = Math.round(frac * 30);
    for (var i = 0; i < 30; i++) coins.appendChild(el("i", i < spent ? "spent" : ""));
    cc.appendChild(coins);
    cc.appendChild(el("div", "dim", "thirty pieces: " + (30 - spent) + " left")).style.fontSize = ".75rem";
    box.appendChild(cc);

    var ka = s.ctx_active || 0, kf = s.ctx_free || 0, kq = (s.queue || []);
    var ck = card("KV cache", fmtNum(ka) + " / " + fmtNum(ct) + " tokens held by running requests");
    ck.appendChild(bar(ct ? ka / ct : 0, ct && ka / ct > 0.85));
    ck.appendChild(el("div", "dim kv-meter", fmtNum(kf) + " free after reserving " + fmtNum(s.kv_reserve || 0) + " for replies; " +
      kq.length + " queued; " + fmtNum(s.preemptions || 0) + " preemption" + (s.preemptions === 1 ? "" : "s")));
    kq.forEach(function (q) {
      ck.appendChild(el("div", "dim kv-meter", (q.tag || "request") + ": " + fmtNum(q.prompt_tokens) + " tokens, waited " + (q.waited_s || 0).toFixed(0) + " s" + (q.preempted ? ", preempted " + q.preempted + "x" : "")));
    });
    box.appendChild(ck);

    var cs = card("Agent slots", (s.active || 0) + " active, " + (s.queued || 0) + " queued, max " + (s.max_agents || 0));
    var grid = el("div", "slots");
    var seqs = s.seqs || [], n = Math.max(s.max_agents || 0, seqs.length);
    for (var k = 0; k < n; k++) {
      var q = seqs[k], ph = q ? (q.phase || "idle") : "idle";
      var sl = el("div", "slot " + ph);
      sl.appendChild(el("div", "ph", ph));
      sl.appendChild(el("div", "mono", q ? fmtNum(q.tokens) + " tok" : "empty"));
      sl.appendChild(el("div", "dim", q ? (q.tag || "seq " + q.id) : "slot " + (k + 1)));
      grid.appendChild(sl);
    }
    cs.appendChild(grid); box.appendChild(cs);

    var st = el("div", "card"), g = el("div", "stats");
    [["gen_tok_s", (s.gen_tok_s || 0).toFixed(1), "tokens/s (all agents)"],
     ["req", fmtNum(s.requests), "requests"],
     ["pt", fmtNum(s.total_prompt_tokens), "prompt tokens"],
     ["ct", fmtNum(s.total_completion_tokens), "completion tokens"]].forEach(function (x) {
      var d = el("div", "stat"); d.appendChild(el("b", null, x[1])); d.appendChild(el("span", null, x[2])); g.appendChild(d);
    });
    st.appendChild(g); box.appendChild(st);
  }

  /* ---------- statistics ---------- */
  var statsData = null;
  function ymd(d) {
    var m = d.getMonth() + 1, day = d.getDate();
    return d.getFullYear() + "-" + (m < 10 ? "0" : "") + m + "-" + (day < 10 ? "0" : "") + day;
  }
  function loadStats() {
    return getJSON("/api/stats").then(function (r) {
      statsData = r; renderStats();
    }, function (e) {
      statsData = null;
      var box = $("#stats"); box.textContent = "";
      box.appendChild(pageHead("stats"));
      box.appendChild(el("p", "dim", "Could not load statistics: " + e.message));
    });
  }
  function statTile(value, label) {
    var d = el("div", "stat"); d.appendChild(el("b", null, value)); d.appendChild(el("span", null, label)); return d;
  }
  function renderStats() {
    var box = $("#stats"), s = statsData;
    box.textContent = "";
    var head = el("div", "stats-head");
    head.appendChild(pageHead("stats"));
    var rf = el("button", "btn small", "Refresh"); rf.onclick = loadStats;
    head.appendChild(rf);
    box.appendChild(head);
    if (!s) return;
    var daily = s.daily || [], models = s.models || [];
    var byDate = {}, total = 0;
    daily.forEach(function (x) { if (x && x.date) { byDate[x.date] = (byDate[x.date] || 0) + (x.count || 0); } });
    var now = new Date(), y = now.getFullYear(), mo = now.getMonth(), dd = now.getDate();
    function sumDays(n) { // today and the n-1 days before it
      var t = 0;
      for (var i = 0; i < n; i++) t += byDate[ymd(new Date(y, mo, dd - i))] || 0;
      return t;
    }
    if (!(s.turns || 0) && !daily.length) {
      box.appendChild(el("p", "dim", "No usage yet. Send a message in Chat and it will show up here."));
    }
    var tiles = el("div", "stats card");
    tiles.appendChild(statTile(fmtNum(s.turns || 0), "Uses all time"));
    tiles.appendChild(statTile(fmtNum(sumDays(30)), "Last 30 days"));
    tiles.appendChild(statTile(fmtNum(sumDays(7)), "Last 7 days"));
    tiles.appendChild(statTile(fmtNum(s.sessions || 0), "Sessions"));
    var pt = s.prompt_tokens || 0, ct = s.completion_tokens || 0;
    var tk = statTile(fmtNum(pt + ct), "Tokens total");
    tk.title = fmtNum(pt) + " prompt + " + fmtNum(ct) + " completion";
    tiles.appendChild(tk);
    box.appendChild(tiles);

    /* models used */
    box.appendChild(el("h3", null, "Models used"));
    var mc = el("div", "card");
    if (!models.length) {
      mc.appendChild(el("p", "dim", s.model ? "Current model: " + s.model : "No models recorded yet."));
    } else {
      var ms = models.slice().sort(function (a, b) { return (b.sessions || 0) - (a.sessions || 0); });
      var mx = Math.max(1, ms[0].sessions || 0);
      ms.forEach(function (m) {
        var row = el("div", "mrow");
        var l = el("div", "lbl");
        l.appendChild(el("span", "mname", m.model || "unknown"));
        l.appendChild(el("span", "mono", fmtNum(m.sessions || 0) + " session" + (m.sessions === 1 ? "" : "s")));
        row.appendChild(l);
        row.appendChild(bar((m.sessions || 0) / mx, false));
        mc.appendChild(row);
      });
    }
    box.appendChild(mc);

    /* contribution grid: 53 week columns x 7 weekday rows, Sunday first, newest at right */
    box.appendChild(el("h3", null, "Daily usage, past 12 months"));
    var gc = el("div", "card");
    var wrap = el("div", "heat-scroll");
    var inner = el("div", "heat");
    var start = new Date(y, mo, dd - now.getDay() - 52 * 7);
    var weeks = 53, max = 0, i, w, r;
    for (i = 0; i < daily.length; i++) if ((daily[i].count || 0) > max) max = daily[i].count;
    var months = el("div", "heat-months");
    months.style.gridTemplateColumns = "repeat(" + weeks + ", var(--cell))";
    var grid = el("div", "heat-grid");
    grid.style.gridTemplateColumns = "repeat(" + weeks + ", var(--cell))";
    var MN = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    var lastMonth = -1, lastLabelCol = -10;
    for (w = 0; w < weeks; w++) {
      var first = new Date(start.getFullYear(), start.getMonth(), start.getDate() + w * 7);
      if (first.getMonth() !== lastMonth && w - lastLabelCol >= 3) {
        var ml = el("span", null, MN[first.getMonth()]);
        ml.style.gridColumn = String(w + 1);
        months.appendChild(ml);
        lastLabelCol = w;
      }
      lastMonth = first.getMonth();
    }
    var today = ymd(now);
    for (w = 0; w < weeks; w++) {
      for (r = 0; r < 7; r++) {
        var d = new Date(start.getFullYear(), start.getMonth(), start.getDate() + w * 7 + r);
        var key = ymd(d);
        var cell = el("i", "cell");
        cell.style.gridColumn = String(w + 1);
        cell.style.gridRow = String(r + 1);
        if (key > today) { cell.className = "cell future"; grid.appendChild(cell); continue; }
        var n = byDate[key] || 0, lv = 0;
        if (n > 0) { var q = n / max; lv = q <= 0.25 ? 1 : q <= 0.5 ? 2 : q <= 0.75 ? 3 : 4; }
        cell.className = "cell l" + lv;
        cell.title = n + (n === 1 ? " use on " : " uses on ") + key;
        grid.appendChild(cell);
      }
    }
    inner.appendChild(months); inner.appendChild(grid);
    wrap.appendChild(inner); gc.appendChild(wrap);
    var lg = el("div", "heat-legend dim");
    lg.appendChild(el("span", null, "less"));
    for (i = 0; i < 5; i++) lg.appendChild(el("i", "cell l" + i));
    lg.appendChild(el("span", null, "more"));
    gc.appendChild(lg);
    box.appendChild(gc);
    wrap.scrollLeft = wrap.scrollWidth; // newest week in view
  }

  /* ---------- heartbeat ---------- */
  var hb = { rep: null, chans: null, running: false, open: {}, test: null, testing: false, err: "" };
  var HB_SEV = { ok: "ok", notice: "notice", warn: "warn", critical: "critical" };
  function openHeartbeat() { renderHeartbeat(); loadHeartbeat(); loadChannels(); }
  function loadHeartbeat() {
    return getJSON("/api/heartbeat").then(function (r) { hb.rep = r; hb.err = ""; renderHeartbeat(); },
      function (e) { hb.err = e.message || "failed"; renderHeartbeat(); });
  }
  function loadChannels() {
    return getJSON("/api/channels").then(function (r) { hb.chans = r; renderHeartbeat(); },
      function () { hb.chans = null; renderHeartbeat(); });
  }
  function hbAbs(ms) { return new Date(ms).toLocaleString(); }
  function renderHeartbeat() {
    var box = $("#hb");
    if (!box) return;
    // keep the open/closed state of the facts rows across redraws
    Array.prototype.forEach.call(box.querySelectorAll("details.hb-fact"), function (d) { hb.open[d.getAttribute("data-k")] = d.open; });
    box.textContent = "";
    box.appendChild(pageHead("heartbeat"));
    var r = hb.rep;

    var card = el("div", "card hb-report");
    var top = el("div", "hb-top");
    var ran = r && r.ran_ms && r.summary;
    if (ran) {
      var sev = HB_SEV[r.severity] || "notice";
      top.appendChild(el("span", "sev " + sev, sev));
      var when = el("span", "hb-when", "Last ran " + fmtRel(r.ran_ms));
      when.title = hbAbs(r.ran_ms);
      top.appendChild(when);
    } else {
      top.appendChild(el("b", null, r ? "No heartbeat has run yet" : (hb.err ? "Heartbeat unavailable" : "Loading...")));
    }
    top.appendChild(el("span", "sp"));
    if (r) {
      top.appendChild(el("span", "badge " + (r.enabled ? "ok" : ""), r.enabled ? "enabled" : "disabled"));
      if (r.interval_s) top.appendChild(el("span", "dim small", fmtInterval(r.interval_s)));
    }
    var run = el("button", "btn primary", hb.running ? "Running..." : "Run now");
    run.type = "button"; run.disabled = hb.running;
    if (hb.running) run.prepend(el("span", "spin"));
    run.onclick = function () {
      hb.running = true; hb.err = ""; renderHeartbeat();
      postJSON("/api/heartbeat/run").then(function (rr) { hb.rep = rr; }, function (e) { hb.err = "Run failed: " + e.message; })
        .then(function () { hb.running = false; renderHeartbeat(); });
    };
    top.appendChild(run);
    card.appendChild(top);
    if (ran) card.appendChild(el("div", "hb-sum", r.summary));
    else if (r) card.appendChild(el("p", "dim", "Press Run now to gather the facts and get a first report. It takes a few seconds while the model summarizes."));
    if (hb.running) card.appendChild(el("p", "dim small", "Collecting facts and summarizing, this can take a few seconds."));
    if (hb.err) card.appendChild(el("div", "field-err", hb.err));
    box.appendChild(card);

    box.appendChild(el("h3", null, "Facts"));
    var keys = r && r.facts ? Object.keys(r.facts).sort() : [];
    if (!keys.length) box.appendChild(el("p", "dim", "No facts collected yet."));
    keys.forEach(function (k) {
      var f = r.facts[k] || {};
      var d = el("details", "hb-fact" + (f.error ? " bad" : ""));
      d.setAttribute("data-k", k);
      d.open = !!hb.open[k];
      var sm = el("summary");
      sm.appendChild(el("b", "fn", k));
      var first = f.error ? String(f.error) : String(f.text || "").split("\n")[0];
      sm.appendChild(el("span", "fl", first));
      d.appendChild(sm);
      var body = el("pre", "fb", f.error ? String(f.error) : String(f.text || ""));
      d.appendChild(body);
      if (f.error && f.text) d.appendChild(el("pre", "fb", String(f.text)));
      box.appendChild(d);
    });

    box.appendChild(el("h3", null, "Alert channels"));
    var cc = el("div", "card");
    var c = hb.chans, names = c && c.channels ? c.channels : [];
    if (!c) cc.appendChild(el("p", "dim", "Could not read the channel list."));
    else if (!names.length) cc.appendChild(el("p", "dim", "No channels are configured. Add one in config.toml to receive alerts."));
    else {
      var chips = el("div", "chips");
      names.forEach(function (n) { chips.appendChild(el("span", "tag" + (n === c["default"] ? " m-cowork" : ""), n + (n === c["default"] ? " (default)" : ""))); });
      cc.appendChild(chips);
      var row = el("div", "hb-test");
      var sel = null;
      if (names.length > 1) {
        sel = el("select"); sel.setAttribute("aria-label", "Channel to test");
        names.forEach(function (n) { var o = el("option", null, n); o.value = n; if (n === c["default"]) o.selected = true; sel.appendChild(o); });
        row.appendChild(sel);
      }
      var tb = el("button", "btn", hb.testing ? "Sending..." : "Send test");
      tb.type = "button"; tb.disabled = hb.testing;
      tb.onclick = function () {
        hb.testing = true; hb.test = null; renderHeartbeat();
        postJSON("/api/channels/test", sel ? { channel: sel.value } : {}).then(function (t) { hb.test = t; },
          function (e) { hb.test = { ok: false, error: e.message }; })
          .then(function () { hb.testing = false; renderHeartbeat(); });
      };
      row.appendChild(tb);
      if (hb.test) row.appendChild(el("span", "hb-res " + (hb.test.ok ? "ok" : "bad"), hb.test.ok ? "Sent. Check your channel." : "Failed: " + (hb.test.error || "unknown error")));
      cc.appendChild(row);
    }
    box.appendChild(cc);
  }
  setInterval(function () { if (curScreen === "heartbeat" && !hb.running) loadHeartbeat(); }, 20000);

  /* ---------- settings ---------- */
  function renderDaemon() {
    var kv = $("#daemon-kv"); kv.textContent = "";
    [["API base", location.origin], ["Version", lastState ? lastState.version : "unreachable"],
     ["Model", lastState ? lastState.model : "-"], ["Theme link", $("#skin-link").getAttribute("href")]].forEach(function (p) {
      kv.appendChild(el("dt", null, p[0])); kv.appendChild(el("dd", "mono", String(p[1] == null ? "-" : p[1])));
    });
  }
  function setSkin(name) {
    $("#skin-link").setAttribute("href", name ? "/skin.css?name=" + encodeURIComponent(name) : "/skin.css");
    if (name) lsSet("judas.skin", name);
    renderDaemon();
  }
  function renderModels() {
    var box = $("#model-switch"); if (!box) return;
    getJSON("/api/models").then(function (r) {
      box.textContent = "";
      var models = r.models || [];
      if (!models.length) { box.appendChild(el("p", "dim", "No models configured.")); return; }
      models.forEach(function (m) {
        var active = m.active;
        var b = el("button", "model-item" + (active ? " active" : ""));
        b.type = "button";
        var top = el("div", "sk-top");
        top.appendChild(el("b", null, m.name));
        top.appendChild(el("span", active ? "badge ok" : "tag", active ? "loaded" : "switch"));
        b.appendChild(top);
        b.appendChild(el("div", "dim small mono", m.path || ""));
        var sub = el("div", "dim small");
        sub.textContent = (m.gpu_layers > 0 ? "GPU" : "CPU") + " · ctx " + fmtNum(m.ctx_total);
        b.appendChild(sub);
        if (!active) b.onclick = function () { switchModel(m.name); };
        box.appendChild(b);
      });
    }, function () { box.textContent = ""; box.appendChild(el("p", "dim", "Could not load models.")); });
  }
  function switchModel(name) {
    if (!confirm("Switch to \u201c" + name + "\u201d? This restarts Judas and interrupts any running chats."))
      return;
    var box = $("#model-switch");
    function say(cls, text) { box.textContent = ""; box.appendChild(el("p", cls, text)); }
    say("dim", "Switching to " + name + "...");
    postJSON("/api/model", { name: name }).then(function (r) {
      if (!r.restarting) { renderModels(); return; }
      var t0 = Date.now();
      say("dim", "Loading " + name + "; Judas is restarting (about 10 to 30 s).");
      function check() {
        getJSON("/api/models").then(function (m) {
          if (m.loaded === name) { renderModels(); renderDaemon(); } else later();
        }, later);
      }
      function later() {
        if (Date.now() - t0 > 180000) { say("err", "Judas did not come back with " + name + " within 3 minutes. Check: journalctl --user -u judas.service"); return; }
        setTimeout(check, 2000);
      }
      setTimeout(check, 2000);
    }, function (e) { say("err", "Switch failed: " + ((e && e.message) || e || "no answer from Judas")); });
  }
  function renderSettings() {
    renderDaemon();
    renderModels();
    getJSON("/api/skins").then(function (r) {
      var box = $("#skins"); box.textContent = "";
      var active = lsGet("judas.skin");
      (r.skins || []).forEach(function (s) {
        var c = s.colors || {};
        var b = el("button", "skin" + (s.name === active ? " active" : ""));
        b.type = "button";
        var top = el("div", "sk-top");
        top.appendChild(el("b", null, s.label || s.name));
        top.appendChild(el("span", s.name === active ? "badge ok" : "tag", s.name === active ? "active" : (s.dark ? "dark" : "light")));
        b.appendChild(top);
        // a small live sample painted with the theme's own colors (data from /api/skins)
        var pv = el("div", "sk-prev");
        if (c.bg) pv.style.background = c.bg;
        if (c.border) pv.style.borderColor = c.border;
        var l1 = el("div", "l1", "Judas"); if (c.title) l1.style.color = c.title;
        var l2 = el("div", "l2", "The answer reads like this."); if (c.text) l2.style.color = c.text;
        var l3 = el("div", "l3"); if (c.accent) l3.style.background = c.accent;
        pv.appendChild(l1); pv.appendChild(l2); pv.appendChild(l3);
        b.appendChild(pv);
        var sw = el("div", "sw");
        ["bg", "surface2", "border", "title", "accent", "error", "ok"].forEach(function (k) {
          var i = el("i"); if (c[k]) i.style.background = c[k];
          sw.appendChild(i);
        });
        b.appendChild(sw);
        b.onclick = function () { setSkin(s.name); renderSettings(); };
        box.appendChild(b);
      });
    }, function () { $("#skins").textContent = "Could not load themes."; });
  }

  /* ---------- boot ---------- */
  Array.prototype.forEach.call(document.querySelectorAll("[data-desc]"), function (n) { n.textContent = INFO[n.getAttribute("data-desc")].desc; });
  sideHidden = !!(window.matchMedia && window.matchMedia("(max-width:760px)").matches);
  (function () { var s = lsGet("judas.skin"); if (s) setSkin(s); })();
  pollState();
  var startName = (location.hash || "#chat").slice(1);
  show(startName);
  if (!cur.id && !logEl.childNodes.length) showEmpty();
  connectFeed();
  loadSessions().then(function () {
    if (cur.id || !sessions.length) return;
    var pick = MODES.indexOf(startName) >= 0
      ? sessions.filter(function (s) { return modeOf(s.id) === curMode; })[0]
      : sessions[0];
    if (pick) { openSession(pick.id); syncNav(); }
  });
  window.addEventListener("hashchange", function () { show((location.hash || "#chat").slice(1)); });
})();
