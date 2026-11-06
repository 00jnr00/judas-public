"use strict";
/* Judas web app: the "Plan and agents" side pane for Chat, Cowork, Code and Learn.
   Self-contained; app.js calls the five hooks below (init, reset, open, event, mode).
   Data: the lead's update_plan checklist (live from tool_call events, rebuilt from stored messages on open) and
   GET /api/sessions/<id>/team (AgentLive rows), kept fresh by the session stream and a 2 s poll while a turn runs.
   Agent controls use POST /api/sessions/<id>/agents/<agent>/pause|resume|cancel. Styles: agent-pane.css. */
(function () {
  var ctx = null;                    // {getJSON, postJSON, openSwarm} from app.js
  var PANE_MODES = { chat: 1, cowork: 1, code: 1, learn: 1, engineering: 1 };
  var ACTIVE = { queued: 1, prefilling: 1, generating: 1, tool: 1, waiting: 1, blocked: 1 };
  var WORKING = { generating: 1, prefilling: 1, tool: 1 };
  var WORD = { generating: "working", prefilling: "working", tool: "working", waiting: "waiting", blocked: "blocked", queued: "queued", idle: "idle", done: "done", failed: "failed", paused: "paused", cancelled: "stopped" };
  var S = { msgs: null, planLive: false, sid: "", mode: "", plan: "", agents: {}, teams: [], lead: "", fetchT: 0, pollT: 0, busyUntil: 0, sel: "", userOpen: null };
  var root = null, chip = null, body = null, pop = null, mq = null;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } }
  function phone() { return !!(mq && mq.matches); }

  /* ----- plan ----- */
  // "- [ ] item", "- [x] item", "1. [ ] item", "- item"; a [~] or [>] mark means in progress
  function parsePlan(text) {
    var out = [];
    String(text || "").split("\n").forEach(function (raw) {
      var line = raw.trim();
      if (!line) return;
      var m = /^(?:[-*+]|\d+[.)])?\s*\[( |x|X|~|>|-)\]\s*(.*)$/.exec(line);
      if (m) { out.push({ state: m[1] === " " ? "open" : (m[1] === "~" || m[1] === ">") ? "doing" : m[1] === "-" ? "skip" : "done", text: m[2] }); return; }
      var b = /^(?:[-*+]|\d+[.)])\s+(.*)$/.exec(line);
      out.push({ state: "note", text: b ? b[1] : line });
    });
    return out;
  }
  function setPlan(text) {
    if (typeof text !== "string" || text === S.plan) return;
    S.plan = text;
    render();
  }
  function planFromArgs(args) {
    var a = args;
    if (typeof a === "string") { try { a = JSON.parse(a); } catch (e) { return null; } }
    return a && typeof a.plan === "string" ? a.plan : null;
  }
  function scanHistory(msgs) {
    var found = null;
    (msgs || []).forEach(function (m) {
      var b = m.body || {};
      if (b.role !== "assistant") return;
      if (m.agent && S.lead && m.agent !== S.lead) return;
      (b.tool_calls || []).forEach(function (tc) {
        var f = tc.function || {};
        if (f.name === "update_plan") { var p = planFromArgs(f.arguments); if (p != null) found = p; }
      });
    });
    return found;
  }

  /* ----- agents ----- */
  function agent(id) {
    if (!S.agents[id]) S.agents[id] = { id: id, parent: "", role: "", task: "", status: "running", activity: "idle", tool: "", team: "", member: "", copy: "", model: "gpu", model_name: "", ctx_pct: null, depth: 0, last_output: "", result: "" };
    return S.agents[id];
  }
  function act(a) {
    if (a.status === "paused") return "paused";
    if (a.status === "killed") return "cancelled";
    return a.activity || "idle";
  }
  function glyph(s) {
    if (WORKING[s] || s === "done") return "●";
    if (s === "waiting" || s === "blocked" || s === "paused") return "◐";
    if (s === "failed") return "✕";
    return "○";
  }
  function tone(s) {
    if (WORKING[s] || s === "done") return "ok";
    if (s === "waiting" || s === "blocked" || s === "paused") return "warn";
    if (s === "failed") return "bad";
    return "idle";
  }
  function shortModel(a) {
    var n = String(a.model_name || "").split("/").pop().replace(/\.gguf$/i, "");
    if (!n) n = a.model === "cpu" ? "cpu" : "gpu";
    return n.length > 16 ? n.slice(0, 15) + "…" : n;
  }
  function label(a) {
    if (a.id === S.lead || (!a.parent && !a.team)) return "lead";
    return a.copy || a.member || String(a.task || "").trim().slice(0, 48) || a.role || a.id;
  }
  function lastLine(a) {
    var s = a.last_output || a.result || "";
    var lines = String(s).split("\n").map(function (x) { return x.trim(); }).filter(Boolean);
    return lines.length ? lines[lines.length - 1].slice(0, 140) : "";
  }
  function others() { return Object.keys(S.agents).filter(function (k) { return k !== S.lead; }); }

  function fetchTeam() {
    var sid = S.sid;
    if (!sid || !ctx) return;
    ctx.getJSON("/api/sessions/" + encodeURIComponent(sid) + "/team").then(function (r) {
      if (S.sid !== sid) return;
      var rows = (r.agents || []).slice();
      if (r.lead) { rows.unshift(r.lead); S.lead = r.lead.id; }
      var seen = {};
      rows.forEach(function (row) {
        var a = agent(row.id);
        seen[row.id] = 1;
        Object.keys(row).forEach(function (k) { a[k] = row[k]; });
      });
      Object.keys(S.agents).forEach(function (k) { if (!seen[k]) delete S.agents[k]; });
      S.teams = r.teams || [];
      if (S.msgs && !S.planLive) { var hp = scanHistory(S.msgs); if (hp != null) S.plan = hp; }   // lead known now: skip workers' plans
      (S.teams).forEach(function (t) {
        (t.copies || []).forEach(function (c) { if (c.agent && S.agents[c.agent]) { var a = S.agents[c.agent]; a.copy = a.copy || c.copy; a.team = a.team || t.name; } });
      });
      render();
    }, function () { /* the banner shows API errors */ });
  }
  function fetchSoon() {
    clearTimeout(S.fetchT);
    S.fetchT = setTimeout(fetchTeam, 300);
  }
  function startPoll() {
    if (S.pollT) return;
    S.pollT = setInterval(function () {
      if (!S.sid || document.hidden || !PANE_MODES[S.mode]) return;
      var busy = Date.now() < S.busyUntil || Object.keys(S.agents).some(function (k) { return ACTIVE[S.agents[k].activity]; });
      if (busy) fetchTeam();
    }, 2000);
  }

  /* ----- DOM ----- */
  function build() {
    var wb = document.querySelector(".work-body"), top = document.querySelector(".wh-top");
    if (!wb || !top || root) return;
    root = el("aside", "apane");
    root.id = "agent-pane";
    root.setAttribute("aria-label", "Plan and agents");
    root.hidden = true;
    var head = el("div", "ap-head");
    head.appendChild(el("b", "ap-title", "Plan and agents"));
    var x = el("button", "btn small ghost ap-x", "Hide");
    x.type = "button";
    x.onclick = function () { setOpen(false); };
    head.appendChild(x);
    root.appendChild(head);
    body = el("div", "ap-body");
    root.appendChild(body);
    wb.appendChild(root);
    chip = el("button", "btn small ap-chip");
    chip.type = "button";
    chip.hidden = true;
    chip.setAttribute("aria-controls", "agent-pane");
    chip.onclick = function () { setOpen(!isOpen()); };
    var anchor = document.getElementById("turn-pill");
    top.insertBefore(chip, anchor ? anchor.nextSibling : null);
    pop = el("div", "ap-pop");
    pop.hidden = true;
    document.body.appendChild(pop);
    document.addEventListener("keydown", function (e) { if (e.key === "Escape") closePop(); });
    document.addEventListener("mousedown", function (e) {
      if (!pop.hidden && !pop.contains(e.target) && !(e.target.closest && e.target.closest(".ap-row"))) closePop();
    });
  }
  function key() { return "judas.agentpane." + S.mode; }
  // desktop: open unless the user collapsed this mode; phone: sheet closed until the chip is tapped
  function isOpen() {
    if (S.userOpen != null) return S.userOpen;
    if (phone()) return false;
    return lsGet(key()) !== "closed";
  }
  function setOpen(v) {
    S.userOpen = v;
    if (!phone()) lsSet(key(), v ? "open" : "closed");
    if (!v) closePop();
    render();
  }

  function hasContent() { return !!S.plan.trim() || others().length > 0; }
  function chipText() {
    var n = others().length, steps = parsePlan(S.plan).filter(function (s) { return s.state !== "note"; });
    var bits = [];
    if (S.plan.trim()) bits.push(steps.length ? "Plan " + steps.filter(function (s) { return s.state === "done"; }).length + "/" + steps.length : "Plan");
    if (n) bits.push(n + (n === 1 ? " agent" : " agents"));
    return bits.join(" · ");
  }

  function render() {
    if (!root) return;
    var show = PANE_MODES[S.mode] && hasContent();
    chip.hidden = !show;
    root.hidden = !show;
    if (!show) { root.classList.remove("open"); closePop(); return; }
    var open = isOpen();
    chip.textContent = chipText();
    chip.setAttribute("aria-expanded", open ? "true" : "false");
    chip.title = open ? "Hide the plan and agents" : "Show the plan and agents";
    root.classList.toggle("open", open);
    root.classList.toggle("collapsed", !open);
    root.hidden = !open;
    if (!open) return;
    body.textContent = "";
    renderPlan();
    renderAgents();
    if (S.sel && pop && !pop.hidden) fillPop(S.sel);
  }

  function renderPlan() {
    var steps = parsePlan(S.plan);
    if (!steps.length) return;
    var sec = el("section", "ap-sec");
    var real = steps.filter(function (s) { return s.state !== "note"; });
    var done = real.filter(function (s) { return s.state === "done"; }).length;
    var h = el("div", "ap-sh");
    h.appendChild(el("span", "ap-cap", "Plan"));
    if (real.length) h.appendChild(el("span", "ap-count", done + "/" + real.length));
    sec.appendChild(h);
    if (real.length) {
      var bar = el("div", "ap-bar"), fill = el("i");
      fill.style.width = Math.round(100 * done / real.length) + "%";
      bar.appendChild(fill);
      sec.appendChild(bar);
    }
    var firstOpen = -1;
    steps.forEach(function (s, i) { if (firstOpen < 0 && s.state === "open") firstOpen = i; });
    var working = Object.keys(S.agents).some(function (k) { return WORKING[S.agents[k].activity]; });
    var ul = el("ul", "ap-plan");
    steps.forEach(function (s, i) {
      var st = s.state === "open" && i === firstOpen && working ? "doing" : s.state;
      var li = el("li", "ap-step st-" + st);
      li.appendChild(el("span", "ap-mark", st === "done" ? "✓" : st === "doing" ? "◐" : st === "skip" ? "–" : st === "note" ? "" : "○"));
      li.appendChild(el("span", "ap-text", s.text));
      ul.appendChild(li);
    });
    sec.appendChild(ul);
    body.appendChild(sec);
  }

  function order() {
    var ids = Object.keys(S.agents), kids = {};
    ids.forEach(function (k) {
      var p = S.agents[k].parent;
      if (!S.agents[p]) p = "";
      (kids[p] = kids[p] || []).push(k);
    });
    var out = [];
    (function walk(p, d) {
      (kids[p] || []).sort(function (a, b) { return (S.agents[a].created_ms || 0) - (S.agents[b].created_ms || 0); }).forEach(function (k) {
        if (k === p) return;
        out.push({ id: k, depth: d });
        walk(k, d + 1);
      });
    })("", 0);
    return out;
  }

  function renderAgents() {
    var rows = order();
    if (rows.length < 2 && !others().length) return;
    var sec = el("section", "ap-sec");
    var h = el("div", "ap-sh");
    h.appendChild(el("span", "ap-cap", "Agents"));
    if (S.teams.length) {
      var sw = el("button", "btn small ap-swarm", "Open in Swarm");
      sw.type = "button";
      sw.onclick = function () { if (ctx && ctx.openSwarm && S.sid) ctx.openSwarm(S.sid); };
      h.appendChild(sw);
    }
    sec.appendChild(h);
    var list = el("div", "ap-tree");
    rows.forEach(function (r) {
      var a = S.agents[r.id], s = act(a);
      var row = el("button", "ap-row tone-" + tone(s) + (S.sel === r.id ? " sel" : ""));
      row.type = "button";
      row.style.setProperty("--d", Math.min(r.depth, 3));
      row.setAttribute("aria-label", label(a) + ", " + (WORD[s] || s));
      var l1 = el("span", "ap-l1");
      l1.appendChild(el("span", "ap-g", glyph(s)));
      l1.appendChild(el("span", "ap-name", label(a)));
      l1.appendChild(el("span", "ap-word", WORD[s] || s));
      row.appendChild(l1);
      var l2 = el("span", "ap-l2");
      l2.appendChild(el("span", "ap-model", shortModel(a)));
      var p = a.ctx_pct == null ? null : Math.round(a.ctx_pct);
      l2.appendChild(el("span", "ap-ctx " + (p == null ? "" : p < 60 ? "ok" : p <= 80 ? "warn" : "bad"), "ctx " + (p == null ? "--" : p + "%")));
      row.appendChild(l2);
      var what = a.tool ? "tool: " + a.tool : lastLine(a);
      if (what) row.appendChild(el("span", "ap-l3", what));
      row.onclick = function (e) { e.stopPropagation(); openPop(r.id, row); };
      list.appendChild(row);
    });
    sec.appendChild(list);
    body.appendChild(sec);
  }

  /* ----- popover: latest output + pause/cancel ----- */
  function openPop(id, anchor) {
    S.sel = id;
    pop.hidden = false;
    fillPop(id);
    var r = anchor.getBoundingClientRect(), w = pop.offsetWidth, h = pop.offsetHeight;
    if (phone()) { pop.style.left = "8px"; pop.style.right = "8px"; pop.style.top = "auto"; pop.style.bottom = "8px"; pop.classList.add("sheet"); }
    else {
      pop.classList.remove("sheet"); pop.style.right = "auto"; pop.style.bottom = "auto";
      pop.style.left = Math.max(8, r.left - w - 8) + "px";
      pop.style.top = Math.max(8, Math.min(r.top, window.innerHeight - h - 8)) + "px";
    }
    Array.prototype.forEach.call(body.querySelectorAll(".ap-row"), function (n) { n.classList.toggle("sel", n === anchor); });
  }
  function closePop() {
    if (!pop) return;
    pop.hidden = true; S.sel = "";
    if (body) Array.prototype.forEach.call(body.querySelectorAll(".ap-row.sel"), function (n) { n.classList.remove("sel"); });
  }
  function fillPop(id) {
    var a = S.agents[id];
    if (!a) { closePop(); return; }
    var s = act(a);
    pop.textContent = "";
    var h = el("div", "ap-ph");
    h.appendChild(el("span", "ap-g tone-" + tone(s), glyph(s)));
    h.appendChild(el("b", "ap-pname", label(a)));
    h.appendChild(el("span", "ap-word", WORD[s] || s));
    var x = el("button", "btn small ghost", "Close");
    x.type = "button"; x.onclick = closePop;
    h.appendChild(x);
    pop.appendChild(h);
    var meta = [shortModel(a), a.ctx_pct == null ? "" : "ctx " + Math.round(a.ctx_pct) + "%", a.tool ? "tool: " + a.tool : ""].filter(Boolean).join(" · ");
    pop.appendChild(el("div", "ap-pmeta dim small", meta));
    if (a.task) pop.appendChild(el("div", "ap-ptask small", a.task));
    var out = a.tail || a.last_output || a.result || "";
    pop.appendChild(el("pre", "ap-pout", out.trim() ? out.slice(-1200) : "No output yet."));
    if (id !== S.lead) {
      var bar = el("div", "ap-pbtn");
      function call(what) {
        ctx.postJSON("/api/sessions/" + encodeURIComponent(S.sid) + "/agents/" + encodeURIComponent(id) + "/" + what).then(fetchSoon, function (e) {
          pop.appendChild(el("div", "ap-perr", "Could not " + what + ": " + e.message));
        });
      }
      var live = a.status === "running" || a.status === "paused";
      if (a.status === "running") { var p = el("button", "btn small", "Pause"); p.type = "button"; p.onclick = function () { call("pause"); }; bar.appendChild(p); }
      if (a.status === "paused" || a.status === "killed") { var r = el("button", "btn small", "Resume"); r.type = "button"; r.onclick = function () { call("resume"); }; bar.appendChild(r); }
      if (live) { var c = el("button", "btn small danger", "Cancel"); c.type = "button"; c.onclick = function () { call("cancel"); }; bar.appendChild(c); }
      if (bar.childNodes.length) pop.appendChild(bar);
    }
  }

  /* ----- hooks for app.js ----- */
  function init(c) {
    ctx = c;
    mq = window.matchMedia ? window.matchMedia("(max-width:700px)") : null;
    build();
    if (mq && mq.addEventListener) mq.addEventListener("change", function () { S.userOpen = null; render(); });
    window.addEventListener("resize", function () { if (pop && !pop.hidden) closePop(); });
    startPoll();
  }
  // a session was opened or a new one started ("" = none)
  function reset(sid) {
    clearTimeout(S.fetchT);
    S.sid = sid || ""; S.plan = ""; S.msgs = null; S.planLive = false; S.agents = {}; S.teams = []; S.lead = ""; S.sel = ""; S.userOpen = null; S.busyUntil = 0;
    closePop();
    render();
  }
  // history loaded: rebuild the plan from stored update_plan calls, fetch the agents
  function open(sid, msgs) {
    if (S.sid !== sid) reset(sid);
    S.msgs = msgs;
    var p = scanHistory(msgs);
    if (p != null && !S.planLive) S.plan = p;
    render();
    fetchTeam();
  }
  function event(ev) {
    if (!ev || !ctx || !S.sid) return;
    if (ev.session && ev.session !== S.sid) return;
    var d = ev.data || {}, lead = !ev.parent;
    S.busyUntil = Date.now() + 4000;
    if (ev.type === "tool_call" && d.name === "update_plan" && (lead || ev.agent === S.lead)) {
      var p = planFromArgs(d.args);
      if (p != null) { S.planLive = true; setPlan(p); }
      return;
    }
    if (ev.type === "team") { fetchSoon(); return; }
    if (!ev.agent) return;
    var known = !!S.agents[ev.agent], a;
    switch (ev.type) {
      case "spawned":
        a = agent(ev.agent);
        a.parent = ev.parent || ""; a.role = d.role || ev.role || a.role; a.task = d.task || a.task; a.status = "running";
        if (d.team) a.team = d.team;
        if (d.member) a.member = d.member;
        if (d.model) a.model = d.model;
        if (!ev.parent) S.lead = ev.agent;
        fetchSoon();
        break;
      case "activity":
        if (!known) { fetchSoon(); return; }
        a = S.agents[ev.agent];
        a.activity = d.activity || a.activity; a.tool = d.tool || "";
        if (ACTIVE[a.activity] && (a.status === "done" || a.status === "failed")) a.status = "running";
        break;
      case "delta":
        if (!known) return;
        a = S.agents[ev.agent];
        a.tail = ((a.tail || "") + (d.text || "")).slice(-1200);
        a.last_output = a.tail.slice(-600);
        break;
      case "tool_call":
        if (!known) return;
        S.agents[ev.agent].tool = d.name || "";
        break;
      case "status":
        if (!known) return;
        a = S.agents[ev.agent];
        if (d.error) { a.status = "failed"; a.activity = "failed"; } else if (d.status) a.status = d.status;
        break;
      case "done":
        if (!known) { fetchSoon(); return; }
        a = S.agents[ev.agent];
        a.status = d.status || "done";
        a.activity = d.status === "failed" ? "failed" : (d.status === "killed" || d.status === "paused") ? a.activity : "done";
        a.tool = "";
        fetchSoon();
        break;
      default: return;
    }
    renderSoon();
  }
  var rT = 0;
  function renderSoon() {
    if (rT) return;
    rT = setTimeout(function () { rT = 0; render(); }, 120);
  }
  function mode(m) {
    if (S.mode === m) { render(); return; }
    S.mode = m; S.userOpen = null;
    render();
  }

  window.JudasAgentPane = { init: init, reset: reset, open: open, event: event, mode: mode };
})();
