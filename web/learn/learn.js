/* Judas Learn: question cards, plan, knowledge. One file shared by the desktop app and the phone UI.
   The host page calls JudasLearn.init({...}) once and a few small hooks (toolCall, noteUser, onEvent). */
(function () {
  "use strict";
  var L = window.JudasLearn = {};
  var H = { md: null, send: null, mode: null, sid: null, practice: null, area: null };

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }
  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  function md(s) { return H.md ? H.md(s) : "<p>" + esc(s).replace(/\n/g, "<br>") + "</p>"; }
  function clip(s, n) { s = String(s == null ? "" : s).replace(/\s+/g, " ").trim(); return s.length > n ? s.slice(0, n - 1) + "…" : s; }
  function parseArgs(a) {
    if (a && typeof a === "object") return a;
    try { var v = JSON.parse(a || "{}"); return v && typeof v === "object" ? v : {}; } catch (e) { return {}; }
  }
  function getJSON(url) {
    return fetch(url, { headers: { Accept: "application/json" } }).then(function (r) {
      if (!r.ok) throw new Error(url + " " + r.status);
      return r.json();
    });
  }

  L.init = function (opts) {
    for (var k in opts) if (Object.prototype.hasOwnProperty.call(opts, k)) H[k] = opts[k];
  };

  /* ---------- question cards (learn_ask) ---------- */
  var KINDS = { predict: "Predict", check: "Check", probe: "Probe", practice: "Practice" };
  var CONF = ["Guessing", "Unsure", "Fairly sure", "Certain"];

  function buildAnswer(state, a) {
    var lines = ["Answer: " + state.answer.trim()];
    if (a.want_confidence && state.conf >= 0) lines.push("Confidence: " + CONF[state.conf].toLowerCase() + " (" + (state.conf + 1) + " of 4)");
    if (a.want_thinking && state.think.trim()) lines.push("Thinking: " + state.think.trim());
    return lines.join("\n");
  }

  function askCard(raw) {
    var a = parseArgs(raw);
    var kind = KINDS[a.kind] ? a.kind : "check";
    var opts = Array.isArray(a.options) ? a.options.filter(function (o) { return typeof o === "string"; }) : [];
    var st = { answer: "", conf: -1, think: "" };
    var card = el("div", "lrn-card open k-" + kind);
    card.setAttribute("role", "group");
    card.setAttribute("aria-label", KINDS[kind] + " question");

    var head = el("button", "lrn-head");
    head.type = "button";
    head.appendChild(el("span", "lrn-kind", KINDS[kind]));
    var sum = el("span", "lrn-sum", clip(a.prompt, 70));
    head.appendChild(sum);
    head.setAttribute("aria-expanded", "true");
    head.onclick = function () {
      if (!card.classList.contains("done")) return;
      var o = card.classList.toggle("expanded");
      head.setAttribute("aria-expanded", String(o));
    };
    card.appendChild(head);

    var body = el("div", "lrn-body");
    var q = el("div", "lrn-q");
    q.innerHTML = md(String(a.prompt || ""));
    body.appendChild(q);

    var form = el("form", "lrn-form");
    var ta = null, optBtns = [];
    if (opts.length) {
      var list = el("div", "lrn-opts");
      list.setAttribute("role", "radiogroup");
      opts.forEach(function (o) {
        var b = el("button", "lrn-opt", o);
        b.type = "button";
        b.setAttribute("role", "radio");
        b.setAttribute("aria-checked", "false");
        b.onclick = function () {
          st.answer = o;
          optBtns.forEach(function (x) { x.setAttribute("aria-checked", String(x === b)); });
          upd();
        };
        optBtns.push(b);
        list.appendChild(b);
      });
      form.appendChild(list);
    } else {
      ta = el("textarea", "lrn-ans");
      ta.rows = 2;
      ta.placeholder = "Your answer";
      ta.setAttribute("aria-label", "Your answer");
      ta.oninput = function () { st.answer = ta.value; upd(); };
      form.appendChild(ta);
    }
    if (a.want_confidence) {
      var cw = el("div", "lrn-conf");
      cw.appendChild(el("div", "lrn-lbl", "How sure are you?"));
      var seg = el("div", "lrn-seg");
      seg.setAttribute("role", "radiogroup");
      seg.setAttribute("aria-label", "Confidence");
      var cb = [];
      CONF.forEach(function (c, i) {
        var b = el("button", "lrn-c" + (i + 1), c);
        b.type = "button";
        b.setAttribute("role", "radio");
        b.setAttribute("aria-checked", "false");
        b.onclick = function () { st.conf = i; cb.forEach(function (x, j) { x.setAttribute("aria-checked", String(j === i)); }); upd(); };
        cb.push(b);
        seg.appendChild(b);
      });
      cw.appendChild(seg);
      form.appendChild(cw);
    }
    if (a.want_thinking) {
      var tw = el("label", "lrn-think");
      tw.appendChild(el("span", "lrn-lbl", "Think out loud"));
      var tt = el("textarea");
      tt.rows = 2;
      tt.placeholder = "How are you getting there? Rough is fine.";
      tt.oninput = function () { st.think = tt.value; };
      tw.appendChild(tt);
      form.appendChild(tw);
    }
    var go = el("button", "lrn-go", "Send answer");
    go.type = "submit";
    go.disabled = true;
    form.appendChild(go);
    function upd() { go.disabled = !st.answer.trim() || (a.want_confidence && st.conf < 0); }
    form.onsubmit = function (e) {
      e.preventDefault();
      if (go.disabled) return;
      var text = buildAnswer(st, a);
      go.disabled = true;
      if (H.send) H.send(text);
    };
    body.appendChild(form);
    card.appendChild(body);
    card._lrn = { sum: sum, prompt: String(a.prompt || "") };
    return card;
  }

  // The learner's reply closes the newest open card in `box`: it folds to one line "question -> answer".
  L.noteUser = function (box, text) {
    if (!box) return;
    var open = box.querySelectorAll(".lrn-card.open");
    if (!open.length) return;
    var card = open[open.length - 1];
    var m = /^Answer:\s*([\s\S]*?)(?:\nConfidence:|\nThinking:|$)/.exec(String(text || ""));
    var ans = clip(m ? m[1] : text, 60);
    var cm = /\nConfidence:\s*([^\n(]+)/.exec(String(text || ""));
    card.classList.remove("open");
    card.classList.add("done");
    var s = card.firstChild.querySelector(".lrn-sum");
    s.textContent = clip(card._lrn.prompt, 40) + "  →  " + ans + (cm ? " (" + cm[1].trim() + ")" : "");
    card.firstChild.setAttribute("aria-expanded", "false");
  };

  // Host hook: returns the element it appended for a Learn tool call, or null for the host's usual row.
  L.toolCall = function (name, args, into) {
    if (name !== "learn_ask") return null;
    var c = askCard(args);
    into.appendChild(c);
    return c;
  };

  /* ---------- phase strip, plan, knowledge ---------- */
  var SVGNS = "http://www.w3.org/2000/svg";
  function svg(tag, attrs, cls) {
    var n = document.createElementNS(SVGNS, tag);
    for (var k in attrs) if (Object.prototype.hasOwnProperty.call(attrs, k)) n.setAttribute(k, attrs[k]);
    if (cls) n.setAttribute("class", cls);
    return n;
  }
  var PHASES = [["measure", "Measure"], ["plan", "Plan"], ["teach", "Teach"], ["practice", "Practice"]];
  var STATUS_WORD = { todo: "to do", now: "now", done: "done", repair: "repair" };
  var T = { topics: [], topic: null, mode: "", sid: null, open: true, openTouched: false, req: 0, top: null, home: null, timer: 0, last: "" };

  function phaseStrip(phase) {
    var ol = el("ol", "lrn-phases");
    var at = -1;
    PHASES.forEach(function (p, i) { if (p[0] === phase) at = i; });
    PHASES.forEach(function (p, i) {
      var li = el("li", i < at ? "past" : i === at ? "now" : "next");
      if (i === at) li.setAttribute("aria-current", "step");
      li.appendChild(el("i"));
      li.appendChild(el("span", null, p[1]));
      ol.appendChild(li);
    });
    return ol;
  }

  function stepper(steps) {
    var ol = el("ol", "lrn-steps");
    var byId = {};
    steps.forEach(function (s) { byId[s.id] = s; });
    steps.forEach(function (s) {
      var li = el("li", "st-" + s.status);
      li.appendChild(el("i", "dot"));
      var b = el("div", "lrn-st-body");
      b.appendChild(el("span", "lrn-st-title", s.title));
      var meta = STATUS_WORD[s.status] || s.status;
      if (s.depends_on && s.depends_on.length)
        meta += " \u00b7 after " + s.depends_on.map(function (d) { return byId[d] ? clip(byId[d].title, 22) : d; }).join(", ");
      b.appendChild(el("span", "lrn-st-meta", meta));
      li.appendChild(b);
      ol.appendChild(li);
    });
    return ol;
  }

  // Small layered node diagram: a step sits one column right of the deepest step it depends on.
  function diagram(steps) {
    var byId = {}, depth = {};
    steps.forEach(function (s) { byId[s.id] = s; });
    function dep(id, seen) {
      if (depth[id] != null) return depth[id];
      if (seen[id]) return 0;
      seen[id] = true;
      var d = 0;
      (byId[id].depends_on || []).forEach(function (x) { if (byId[x]) d = Math.max(d, dep(x, seen) + 1); });
      return (depth[id] = d);
    }
    steps.forEach(function (s) { dep(s.id, {}); });
    var cols = [], pos = {}, W = 150, Hn = 36, GX = 44, GY = 12;
    steps.forEach(function (s) {
      var c = depth[s.id];
      (cols[c] = cols[c] || []).push(s);
    });
    var rows = 0;
    cols.forEach(function (c) { rows = Math.max(rows, c ? c.length : 0); });
    cols.forEach(function (c, ci) {
      (c || []).forEach(function (s, ri) { pos[s.id] = { x: 8 + ci * (W + GX), y: 8 + ri * (Hn + GY) }; });
    });
    var w = 16 + cols.length * W + (cols.length - 1) * GX, h = 16 + rows * Hn + (rows - 1) * GY;
    var root = svg("svg", { viewBox: "0 0 " + w + " " + h, width: w, height: h, role: "img", "aria-label": "Plan: " + steps.length + " steps" }, "lrn-dia");
    steps.forEach(function (s) {
      (s.depends_on || []).forEach(function (d) {
        if (!pos[d]) return;
        var a = pos[d], b = pos[s.id], x1 = a.x + W, y1 = a.y + Hn / 2, x2 = b.x, y2 = b.y + Hn / 2, mx = (x1 + x2) / 2;
        root.appendChild(svg("path", { d: "M" + x1 + " " + y1 + " C" + mx + " " + y1 + " " + mx + " " + y2 + " " + x2 + " " + y2 }, "edge"));
      });
    });
    steps.forEach(function (s) {
      var p = pos[s.id], g = svg("g", { transform: "translate(" + p.x + " " + p.y + ")" }, "node st-" + s.status);
      var t = svg("title", {});
      t.textContent = s.title + " (" + (STATUS_WORD[s.status] || s.status) + ")";
      g.appendChild(t);
      g.appendChild(svg("rect", { width: W, height: Hn, rx: 9 }));
      var tx = svg("text", { x: 12, y: Hn / 2 + 4 });
      tx.textContent = clip(s.title, 20);
      g.appendChild(tx);
      root.appendChild(g);
    });
    return root;
  }

  function wide() { return window.matchMedia && window.matchMedia("(min-width: 900px)").matches; }

  function renderTop() {
    var box = T.top;
    if (!box) return;
    var show = T.mode === "learn";
    box.hidden = !show;
    if (!show) return;
    box.textContent = "";
    var t = T.topic;
    var wrap = el("div", "lrn-top" + (T.open ? " open" : ""));
    if (t) {
      var done = 0;
      (t.steps || []).forEach(function (s) { if (s.status === "done") done++; });
      var head = el("button", "lrn-top-head");
      head.type = "button";
      head.setAttribute("aria-expanded", String(T.open));
      head.appendChild(phaseStrip(t.phase));
      var sum = el("span", "lrn-top-sum");
      sum.appendChild(el("b", null, clip(t.name, 28)));
      if (t.steps && t.steps.length) sum.appendChild(el("span", null, done + " of " + t.steps.length));
      head.appendChild(sum);
      head.appendChild(el("span", "lrn-car", "\u25be"));
      head.onclick = function () { T.open = !T.open; T.openTouched = true; renderTop(); };
      wrap.appendChild(head);
      if (T.open && t.steps && t.steps.length) {
        var body = el("div", "lrn-top-body");
        body.appendChild(wide() ? diagram(t.steps) : stepper(t.steps));
        wrap.appendChild(body);
      }
    }
    var foot = el("div", "lrn-top-foot");
    var kb = el("button", "lrn-link", "Knowledge" + (T.topics.length ? " (" + T.topics.length + ")" : ""));
    kb.type = "button";
    kb.onclick = function () { L.openKnowledge(t ? t.id : null); };
    foot.appendChild(kb);
    wrap.appendChild(foot);
    box.appendChild(wrap);
  }

  function renderHome() {
    var box = T.home;
    if (!box) return;
    var show = T.mode === "learn" && T.topics.length > 0;
    box.hidden = !show;
    if (!show) return;
    box.textContent = "";
    box.appendChild(el("div", "lrn-lbl", "Your topics"));
    T.topics.slice(0, 4).forEach(function (t) { box.appendChild(topicRow(t, true)); });
    var all = el("button", "lrn-link", "All knowledge");
    all.type = "button";
    all.onclick = function () { L.openKnowledge(null); };
    box.appendChild(all);
  }

  function topicRow(t, practiceBtn) {
    var row = el("div", "lrn-trow");
    var b = el("button", "lrn-tmain");
    b.type = "button";
    b.appendChild(el("b", null, t.name));
    var bits = [PHASES.filter(function (p) { return p[0] === t.phase; }).map(function (p) { return p[1]; })[0] || t.phase];
    if (t.steps_total) bits.push(t.steps_done + " of " + t.steps_total + " steps");
    bits.push(t.props + (t.props === 1 ? " proposition" : " propositions"));
    b.appendChild(el("span", null, bits.join(" \u00b7 ")));
    b.onclick = function () { L.openKnowledge(t.id); };
    row.appendChild(b);
    if (practiceBtn && t.props > 0) {
      var p = el("button", "lrn-mini", "Practice");
      p.type = "button";
      p.onclick = function () { startPractice(t.id); };
      row.appendChild(p);
    }
    return row;
  }

  /* data */
  function load() {
    var my = ++T.req;
    return getJSON("/api/learn/topics").then(function (r) {
      if (my !== T.req) return;
      T.topics = r.topics || [];
      var mine = null;
      if (T.sid) T.topics.forEach(function (t) { if (!mine && t.session === T.sid) mine = t; });
      if (!mine) { T.topic = null; finish(); return; }
      return getJSON("/api/learn/topic/" + encodeURIComponent(mine.id)).then(function (d) {
        if (my !== T.req) return;
        T.topic = d;
        if (!T.openTouched && !wide() && d.steps && d.steps.length && !T.autoOpened) { T.open = false; T.autoOpened = true; }
        finish();
      });
    }).catch(function () { /* the Learn endpoints are optional; keep what is shown */ });
  }
  function finish() {
    renderTop();
    renderHome();
    if (K.el && !K.el.hidden) renderKnowledge();
  }
  L.refresh = function () { clearTimeout(T.timer); T.timer = setTimeout(load, 120); };

  // Host events (lead only matters): a plan or note tool finished, or a turn ended.
  L.onEvent = function (ev) {
    if (!ev || T.mode !== "learn") return;
    var d = ev.data || {};
    if (ev.type === "tool_result" && (d.name === "learn_plan" || d.name === "learn_note") && d.ok !== false) L.refresh();
    else if (ev.type === "done" && !ev.parent) L.refresh();
  };

  /* mounting: the host gives two empty containers, and a way to read its mode and session */
  L.mount = function (opts) {
    T.top = opts.top || null;
    T.home = opts.home || null;
    if (opts.practice) H.practice = opts.practice;
    if (opts.mode) H.mode = opts.mode;
    if (opts.sid) H.sid = opts.sid;
    setInterval(sync, 700);
    sync();
  };
  function sync() {
    var m = H.mode ? H.mode() : "", sid = H.sid ? H.sid() : null;
    var key = m + "|" + (sid || "");
    if (key === T.last) return;
    T.last = key;
    var was = T.mode;
    T.mode = m;
    T.sid = sid;
    T.topic = sid && T.topic && T.topic.session === sid ? T.topic : null;
    T.openTouched = false;
    T.autoOpened = false;
    T.open = true;
    renderTop();
    renderHome();
    if (m === "learn" || was === "learn") load();
  }

  /* ---------- knowledge view ---------- */
  var K = { el: null, id: null, detail: null, req: 0 };

  function practiceText(t) {
    var lines = ["Practice: " + t.name + ". Quiz me on what I have noted for this topic. Ask one varied problem at a time with learn_ask (kind practice, want_confidence), each different from the last, and never name the idea inside the problem. Start easy and make it harder only after I succeed.", "", "What I have noted:"];
    (t.props || []).forEach(function (p) {
      var s = "- " + p.text;
      if (p.premises && p.premises.length) s += " (because: " + p.premises.map(function (x) { return premiseText(t.props || [], x); }).join("; ") + ")";
      lines.push(s);
    });
    return lines.join("\n");
  }
  function startPractice(id) {
    getJSON("/api/learn/topic/" + encodeURIComponent(id)).then(function (t) {
      closeKnowledge();
      if (H.practice) H.practice(practiceText(t), t.name);
    }, function () { /* topic vanished */ });
  }

  // Matching form of a sentence (same rule as the server): lowercase letters and digits, other runs become one space.
  function norm(t) { return String(t == null ? "" : t).toLowerCase().replace(/[^a-z0-9]+/g, " ").replace(/^ | $/g, ""); }
  // Index of the proposition a premise names: short id ("p3", "#P3") or its words under norm(); -1 when none.
  function premiseIndex(props, pr) {
    var id = String(pr).trim().replace(/^#/, "").toLowerCase(), n = norm(pr), i;
    for (i = 0; i < props.length; i++) if (props[i].id === id) return i;
    if (n) for (i = 0; i < props.length; i++) if (norm(props[i].text) === n) return i;
    return -1;
  }
  function premiseText(props, pr) { var i = premiseIndex(props, pr); return i >= 0 ? props[i].text : pr; }
  L.norm = norm;

  // Ring graph: propositions on a ring (numbered), their premises as small outer dots, links where a premise names
  // another proposition by id or by its exact words.
  function ringGraph(props, onPick) {
    var n = props.length, S = 320, C = S / 2, R = n > 1 ? 92 : 0, RO = 140;
    var root = svg("svg", { viewBox: "0 0 " + S + " " + S, role: "img", "aria-label": "Graph of " + n + " propositions" }, "lrn-ring");
    function ang(i) { return (i / Math.max(n, 1)) * Math.PI * 2 - Math.PI / 2; }
    function at(i, r) { return [C + r * Math.cos(ang(i)), C + r * Math.sin(ang(i))]; }
    root.appendChild(svg("circle", { cx: C, cy: C, r: R || 1, fill: "none" }, "guide"));
    var leaves = {}, order = [];
    props.forEach(function (p, i) {
      (p.premises || []).forEach(function (pr) {
        var j = premiseIndex(props, pr);
        if (j >= 0 && j !== i) {
          var a = at(j, R), b = at(i, R);
          root.appendChild(svg("path", { d: "M" + a[0] + " " + a[1] + " Q" + C + " " + C + " " + b[0] + " " + b[1] }, "edge"));
        } else if (j < 0) {
          var k = norm(pr);
          if (!leaves[k]) { leaves[k] = { text: pr, users: [] }; order.push(k); }
          leaves[k].users.push(i);
        }
      });
    });
    order.forEach(function (k, li) {
      var lf = leaves[k], u = lf.users[0], base = ang(u) + (li % 3 - 1) * 0.16;
      var x = C + RO * Math.cos(base), y = C + RO * Math.sin(base);
      lf.users.forEach(function (ui) {
        var a = at(ui, R);
        root.appendChild(svg("line", { x1: a[0], y1: a[1], x2: x, y2: y }, "leaf-edge"));
      });
      var g = svg("g", {}, "leaf");
      var t = svg("title", {});
      t.textContent = lf.text;
      g.appendChild(t);
      g.appendChild(svg("circle", { cx: x, cy: y, r: 5 }));
      root.appendChild(g);
    });
    props.forEach(function (p, i) {
      var pt = n === 1 ? [C, C] : at(i, R), g = svg("g", { tabindex: 0, role: "button", "aria-label": "Proposition " + (i + 1) + ": " + p.text }, "pnode");
      var t = svg("title", {});
      t.textContent = p.text;
      g.appendChild(t);
      g.appendChild(svg("circle", { cx: pt[0], cy: pt[1], r: 15 }));
      var tx = svg("text", { x: pt[0], y: pt[1] + 4, "text-anchor": "middle" });
      tx.textContent = String(i + 1);
      g.appendChild(tx);
      g.onclick = function () { onPick(i); };
      g.onkeydown = function (e) { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onPick(i); } };
      root.appendChild(g);
    });
    return root;
  }

  function ensureKnowledge() {
    if (K.el) return;
    var o = el("div", "lrn-know");
    o.hidden = true;
    o.setAttribute("role", "dialog");
    o.setAttribute("aria-modal", "true");
    o.setAttribute("aria-label", "Knowledge");
    document.body.appendChild(o);
    K.el = o;
    document.addEventListener("keydown", function (e) { if (e.key === "Escape" && !o.hidden) closeKnowledge(); });
  }
  function closeKnowledge() { if (K.el) K.el.hidden = true; document.documentElement.classList.remove("lrn-lock"); }
  L.openKnowledge = function (id) {
    ensureKnowledge();
    K.id = id;
    K.el.hidden = false;
    document.documentElement.classList.add("lrn-lock");
    renderKnowledge();
    getJSON("/api/learn/topics").then(function (r) { T.topics = r.topics || []; renderKnowledge(); }, function () {});
  };

  function renderKnowledge() {
    var o = K.el;
    o.textContent = "";
    var inner = el("div", "lrn-know-in");
    var bar = el("div", "lrn-know-bar");
    if (K.id) {
      var back = el("button", "lrn-back", "\u2039 Topics");
      back.type = "button";
      back.onclick = function () { K.id = null; renderKnowledge(); };
      bar.appendChild(back);
    } else bar.appendChild(el("h2", null, "Knowledge"));
    var x = el("button", "lrn-x", "Close");
    x.type = "button";
    x.onclick = closeKnowledge;
    bar.appendChild(x);
    inner.appendChild(bar);
    var sc = el("div", "lrn-know-scroll");
    if (!K.id) {
      if (!T.topics.length) {
        sc.appendChild(el("p", "lrn-empty", "Nothing here yet. As you learn, the tutor writes down what you can use, and it shows up here."));
      } else {
        T.topics.forEach(function (t) { sc.appendChild(topicRow(t, true)); });
      }
      inner.appendChild(sc);
      o.appendChild(inner);
      return;
    }
    var my = ++K.req;
    var cached = K.detail && K.detail.id === K.id ? K.detail : null;
    if (cached) paintTopic(sc, cached);
    else sc.appendChild(el("p", "lrn-empty", "Loading..."));
    inner.appendChild(sc);
    o.appendChild(inner);
    getJSON("/api/learn/topic/" + encodeURIComponent(K.id)).then(function (d) {
      if (my !== K.req || !K.el || K.el.hidden) return;
      K.detail = d;
      sc.textContent = "";
      paintTopic(sc, d);
    }, function () {
      sc.textContent = "";
      sc.appendChild(el("p", "lrn-empty", "That topic is gone."));
    });
  }

  function paintTopic(sc, t) {
    var h = el("h2", "lrn-tname", t.name);
    sc.appendChild(h);
    sc.appendChild(phaseStrip(t.phase));
    var props = t.props || [];
    if (props.length) {
      var pr = el("button", "lrn-go lrn-practice", "Practice this topic");
      pr.type = "button";
      pr.onclick = function () { startPractice(t.id); };
      sc.appendChild(pr);
      var list = el("ol", "lrn-props");
      var lis = [];
      props.forEach(function (p) {
        var li = el("li");
        li.appendChild(el("span", "lrn-ptext", p.text));
        if (p.premises && p.premises.length) {
          var bc = el("div", "lrn-because");
          bc.appendChild(el("span", null, "because"));
          p.premises.forEach(function (x) { bc.appendChild(el("em", null, premiseText(props, x))); });
          li.appendChild(bc);
        }
        lis.push(li);
        list.appendChild(li);
      });
      sc.appendChild(ringGraph(props, function (i) {
        lis[i].scrollIntoView({ block: "center", behavior: "smooth" });
        lis.forEach(function (l, j) { l.classList.toggle("hit", j === i); });
      }));
      sc.appendChild(list);
    } else {
      sc.appendChild(el("p", "lrn-empty", "No propositions noted for this topic yet."));
    }
    if (t.steps && t.steps.length) {
      sc.appendChild(el("h3", "lrn-sub", "Plan"));
      sc.appendChild(stepper(t.steps));
    }
  }
})();
