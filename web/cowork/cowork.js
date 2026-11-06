/* Cowork sub-tab layouts (desktop). Each work mode gets a stage built for its job, beside the chat column:
   Engineering: 3D viewport + calc sheet. Design: canvas, version filmstrip, reference board.
   Research: numbered sources + live report. Write: the document with margin notes.
   Contract: docs/COWORK-MODES.md. app.js calls init(host), mode(m), session(id), event(ev).
   Every endpoint may be missing (older server): each pane then shows its empty text and nothing throws. */
(function () {
  "use strict";
  var H = null;                       // host object from app.js
  var stage = null, root = null;      // #cw-stage, #screen-work
  var S = { mode: "", sid: "", gen: 0 };
  var timers = {};

  function $(s, r) { return (r || document).querySelector(s); }
  function el(tag, cls, text) { var n = document.createElement(tag); if (cls) n.className = cls; if (text != null) n.textContent = text; return n; }
  function btn(label, cls, fn) { var b = el("button", "btn small " + (cls || ""), label); b.type = "button"; if (fn) b.onclick = fn; return b; }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } }
  function later(key, fn, ms) { clearTimeout(timers[key]); timers[key] = setTimeout(fn, ms); }
  function enc(s) { return encodeURIComponent(s); }
  function host(u) { try { return new URL(u).hostname.replace(/^www\./, ""); } catch (e) { return ""; } }
  function isDark() {
    var m = /rgba?\((\d+),\s*(\d+),\s*(\d+)/.exec(getComputedStyle(document.body).backgroundColor || "");
    return m ? (0.299 * m[1] + 0.587 * m[2] + 0.114 * m[3]) < 140 : false;
  }
  /* fetch that never rejects: {ok, status, text, json} */
  function fx(url, opts) {
    return fetch(url, opts).then(function (r) {
      return r.text().then(function (t) {
        var j = null; try { j = JSON.parse(t); } catch (e) { /* not json */ }
        return { ok: r.ok, status: r.status, text: t, json: j, hash: r.headers.get("X-Judas-Hash") || "" };
      });
    }, function () { return { ok: false, status: 0, text: "", json: null, hash: "" }; });
  }
  function sidURL(suffix) { return "/api/sessions/" + enc(S.sid) + suffix; }
  function empty(text) { return el("p", "cw-empty", text); }
  function head(title, count) {
    var h = el("div", "cw-head");
    h.appendChild(el("b", null, title));
    if (count != null) h.appendChild(el("span", "cw-count", String(count)));
    return h;
  }
  function relTime(ms) {
    var s = Math.max(0, Math.round((Date.now() - ms) / 1000));
    return s < 60 ? "just now" : s < 3600 ? Math.round(s / 60) + " min ago" : s < 86400 ? Math.round(s / 3600) + " h ago" : Math.round(s / 86400) + " d ago";
  }

  var LAYOUTS = { engineering: buildEng, design: buildDes, research: buildRes, write: buildWr };
  var L = null;   // the active layout: { refresh(kind), reset(), destroy() }

  /* ---------- host entry points ---------- */
  function init(h) { H = h; stage = h.stage || $("#cw-stage"); root = h.root || $("#screen-work"); }

  function mode(m) {
    if (!stage) return;
    if (S.mode === m && L) { return; }
    if (L && L.destroy) L.destroy();
    L = null; stage.textContent = ""; S.mode = m;
    root.classList.remove("cw-chat-collapsed");
    var build = LAYOUTS[m];
    stage.hidden = !build;
    root.classList.toggle("cw-staged", !!build);
    if (!build) return;
    S.gen++;
    L = build(stage);
    if (H.phone) stage.classList.add("cw-phone");
    S.sid = H.sid();
    L.reset();
    if (S.sid) L.refresh("all");
  }
  function session(id) {
    if (!L) return;
    var was = S.sid;
    S.sid = id || "";
    if (was === S.sid) { L.refresh("all"); return; }
    S.gen++;
    L.reset(!was && !!S.sid);   // true: the session was only just created for what is on screen
    if (S.sid) L.refresh("all");
  }
  var PING = { calcs: "calcs", sources: "sources", suggestions: "suggestions", cad: "cad", artifact: "artifact" };
  function event(ev) {
    if (!L || !S.sid) return;
    var k = PING[ev.type];
    if (k) { later("p" + k, function () { if (L) L.refresh(k); }, 150); return; }
    /* servers without pings: a finished tool call or turn is a good moment to look again */
    if (ev.type === "tool_result" || ev.type === "done") later("pall", function () { if (L) L.refresh("all"); }, ev.type === "done" ? 100 : 700);
  }

  /* =====================================================================================================
     ENGINEERING: chat narrow on the left, 3D viewport over a calc sheet on the right
     ===================================================================================================== */
  function buildEng(box) {
    box.className = "cw-stage cw-eng";
    var vp = el("section", "cw-pane cw-view");
    var vh = head("Model"); var vtag = el("span", "cw-sub"); vh.appendChild(vtag);
    var vbox = el("div", "cw-viewbox");
    vp.appendChild(vh); vp.appendChild(vbox);
    var strip = el("div", "cw-cadstrip");
    var cs = el("section", "cw-pane cw-calcs");
    var ch = head("Calculations", 0);
    var cbody = el("div", "cw-tablewrap");
    cs.appendChild(ch); cs.appendChild(cbody);
    box.appendChild(vp); box.appendChild(strip); box.appendChild(cs);
    var blobURL = "", cadSig = "";

    function drawCalcs(rows) {
      ch.lastChild.textContent = String(rows.length);
      cbody.textContent = "";
      if (!rows.length) { cbody.appendChild(empty("No calculations yet. Every number the model relies on lands here with its expression and unit.")); return; }
      var t = el("table", "cw-table");
      var th = el("thead"), hr = el("tr");
      [["#", "num"], ["Name", ""], ["Expression", ""], ["Value", "num"], ["Unit", ""], ["Note", ""]].forEach(function (c) { hr.appendChild(el("th", c[1], c[0])); });
      th.appendChild(hr); t.appendChild(th);
      var tb = el("tbody");
      rows.forEach(function (r) {
        var tr = el("tr");
        tr.appendChild(el("td", "num dim", String(r.n)));
        tr.appendChild(el("td", "nm", r.name));
        tr.appendChild(el("td", "mono ex", r.expr));
        tr.appendChild(el("td", "num mono val", r.value == null ? "" : String(r.value)));
        tr.appendChild(el("td", "unit", r.unit || ""));
        tr.appendChild(el("td", "dim cmt", r.note || ""));
        tb.appendChild(tr);
      });
      t.appendChild(tb); cbody.appendChild(t);
    }
    function num(v, d) { return typeof v === "number" ? (Math.abs(v) >= 100 ? Math.round(v) : Math.round(v * Math.pow(10, d)) / Math.pow(10, d)) : v; }
    function drawStrip(c) {
      strip.textContent = "";
      var st = c && c.available ? c.state : null;
      if (typeof st === "string") { try { st = JSON.parse(st); } catch (e) { st = null; } }
      if (!st) { strip.appendChild(el("span", "cw-chip dim", c && c.available === false ? "CAD not available in this build" : "No CAD part yet")); return; }
      var parts = st.parts || [], p = parts.filter(function (x) { return x.current || x.name === st.current; })[0] || parts[0] || {};
      var feats = (p.timeline || p.features || []).filter(function (x) { return !x.kind || x.kind === "feature"; }).length;
      var bodies = p.bodies || [], vol = 0;
      bodies.forEach(function (b) { vol += Number(b.volume) || 0; });
      function chip(k, v) { var s = el("span", "cw-chip"); s.appendChild(el("span", "k", k)); s.appendChild(el("b", "mono", String(v))); strip.appendChild(s); }
      chip("Part", p.name || st.current || "part");
      chip("Features", feats);
      if (bodies.length) chip("Volume", num(vol, 1) + " mm³");
      var bb = bodies[0] && bodies[0].bbox;
      if (bb && bb.size) chip("Size", bb.size.map(function (x) { return num(x, 1); }).join(" × ") + " mm");
      else if (bb && bb.max && bb.min) chip("Size", [0, 1, 2].map(function (i) { return num(bb.max[i] - bb.min[i], 1); }).join(" × ") + " mm");
      chip("Units", st.units || "mm");
    }
    function loadModel() {
      var gen = S.gen;
      return fetch(sidURL("/cad/model.stl")).then(function (r) {
        if (!r.ok) throw new Error("none");
        return r.blob();
      }).then(function (b) {
        if (gen !== S.gen) return;
        if (blobURL) URL.revokeObjectURL(blobURL);
        blobURL = URL.createObjectURL(b);
        vbox.textContent = "";
        var f = el("iframe", "cw-frame"); f.title = "3D model";
        f.src = "/cowork/viewport.html?dark=" + (isDark() ? 1 : 0) + "&src=" + enc(blobURL);
        vbox.appendChild(f);
        vtag.textContent = "drag to orbit, scroll to zoom";
      }, function () {
        if (gen !== S.gen) return;
        if (blobURL) { URL.revokeObjectURL(blobURL); blobURL = ""; }
        vbox.textContent = "";
        vbox.appendChild(empty("No model yet. When the model builds a part with CAD it appears here, and updates after each change."));
        vtag.textContent = "";
      });
    }
    function refresh(kind) {
      if (!S.sid) return;
      var gen = S.gen;
      if (kind === "all" || kind === "calcs") fx(sidURL("/calcs")).then(function (r) { if (gen === S.gen) drawCalcs(r.ok && r.json && r.json.calcs || []); });
      if (kind === "all" || kind === "cad") {
        fx(sidURL("/cad")).then(function (r) {
          if (gen !== S.gen) return;
          var j = r.ok ? r.json : null;
          drawStrip(j);
          var sig = JSON.stringify(j && j.state || "");
          if (kind === "all" || sig !== cadSig) { cadSig = sig; if (j && j.available !== false) loadModel(); else { vbox.textContent = ""; vbox.appendChild(empty(r.ok ? "CAD is not built into this server." : "No model yet. When the model builds a part with CAD it appears here, and updates after each change.")); } }
        });
      }
    }
    function reset() {
      cadSig = "";
      drawCalcs([]); drawStrip(null);
      vbox.textContent = ""; vbox.appendChild(empty("No model yet. When the model builds a part with CAD it appears here, and updates after each change."));
      vtag.textContent = "";
    }
    return { refresh: refresh, reset: reset, destroy: function () { if (blobURL) URL.revokeObjectURL(blobURL); } };
  }

  /* =====================================================================================================
     artifacts of this session (Design canvas, Research report)
     ===================================================================================================== */
  function listArtifacts() {
    return fx("/api/artifacts?session=" + enc(S.sid)).then(function (r) {
      var a = r.ok && r.json && r.json.artifacts || [];
      return a.slice().sort(function (x, y) { return (y.updated_ms || 0) - (x.updated_ms || 0); });
    });
  }
  function artMeta(id) { return fx("/api/artifacts/" + enc(id)).then(function (r) { return r.ok ? r.json : null; }); }
  function artURL(id, ver) { return "/a/" + enc(id) + "/v" + ver + "/"; }
  function thumbURL(id, ver) { return "/api/artifacts/" + enc(id) + "/thumb?v=" + ver; }

  /* =====================================================================================================
     DESIGN: the canvas first; filmstrip and reference board under it; chat is a collapsible side panel
     ===================================================================================================== */
  function buildDes(box) {
    box.className = "cw-stage cw-des";
    var top = el("div", "cw-head cw-deshead");
    var sel = el("select", "cw-artsel"); sel.setAttribute("aria-label", "Design"); sel.hidden = true;
    var title = el("b", "cw-destitle", "Canvas");
    var verTag = el("span", "cw-count");
    var spacer = el("span", "cw-spacer");
    var cmp = btn("Compare", "ghost", function () { D.compare = !D.compare; D.right = 0; draw(); });
    var tab = el("a", "btn small ghost", "Open in tab"); tab.target = "_blank"; tab.rel = "noopener";
    var chatBtn = btn("Chat", "ghost cw-chatbtn", function () {
      var c = root.classList.toggle("cw-chat-collapsed"); chatBtn.setAttribute("aria-pressed", c ? "false" : "true");
    });
    chatBtn.setAttribute("aria-pressed", "true");
    [title, verTag, sel, spacer, cmp, tab, chatBtn].forEach(function (n) { top.appendChild(n); });
    var canvas = el("div", "cw-canvas");
    var film = el("div", "cw-film");
    var refs = el("div", "cw-refs");
    box.appendChild(top); box.appendChild(canvas); box.appendChild(film); box.appendChild(refs);
    var D = { arts: [], id: "", meta: null, ver: 0, compare: false, right: 0, imgs: [] };

    function pins() { try { var a = JSON.parse(lsGet("judas.cw.pins." + S.sid) || "[]"); return Array.isArray(a) ? a : []; } catch (e) { return []; } }
    function setPins(a) { lsSet("judas.cw.pins." + S.sid, JSON.stringify(a)); }

    function frame(id, ver, label) {
      var w = el("figure", "cw-fig");
      var f = el("iframe", "cw-frame"); f.title = label;
      f.setAttribute("sandbox", "allow-scripts");
      f.src = artURL(id, ver);
      w.appendChild(f);
      if (D.compare) w.appendChild(el("figcaption", null, label));
      return w;
    }
    function drawCanvas() {
      canvas.textContent = "";
      canvas.classList.toggle("cmp", D.compare && !!D.meta);
      if (!D.meta) { canvas.appendChild(empty(S.sid ? "Nothing on the canvas yet. Describe what to sketch; the result appears here as soon as it exists." : "Describe what to sketch below. The result appears here, and every change adds a version to the filmstrip.")); return; }
      var m = D.meta;
      canvas.appendChild(frame(m.id, D.ver, m.title + " v" + D.ver));
      if (D.compare) {
        if (D.right) canvas.appendChild(frame(m.id, D.right, m.title + " v" + D.right));
        else canvas.appendChild(empty("Pick a second version in the filmstrip to compare."));
      }
    }
    function drawFilm() {
      film.textContent = "";
      var m = D.meta;
      if (!m) { film.appendChild(el("span", "dim small cw-filmempty", "Versions appear here, newest on the right.")); return; }
      var vs = (m.versions || []).slice().sort(function (a, b) { return a.n - b.n; });
      vs.forEach(function (v) {
        var b = el("button", "cw-ver" + (v.n === D.ver ? " on" : "") + (D.compare && v.n === D.right ? " on2" : "")); b.type = "button";
        b.title = v.note || ("Version " + v.n);
        var img = new Image(); img.alt = ""; img.src = thumbURL(m.id, v.n);
        img.onerror = function () { img.remove(); b.classList.add("nothumb"); };
        b.appendChild(img);
        b.appendChild(el("span", "n", "v" + v.n));
        b.onclick = function () {
          if (D.compare && v.n !== D.ver) D.right = v.n; else { D.ver = v.n; if (D.right === v.n) D.right = 0; }
          draw();
          if (H.pane && !D.compare) H.pane("canvas");
        };
        film.appendChild(b);
      });
      film.scrollLeft = film.scrollWidth;
    }
    function drawRefs() {
      refs.textContent = "";
      refs.appendChild(el("span", "cw-reflabel", "References"));
      var any = false;
      function add(src, label, onRemove, href) {
        any = true;
        var a = el(href ? "a" : "span", "cw-ref"); if (href) { a.href = href; a.target = "_blank"; a.rel = "noopener"; }
        var img = new Image(); img.alt = label; img.src = src; img.title = label;
        a.appendChild(img);
        if (onRemove) { var x = el("button", "cw-refx", "×"); x.type = "button"; x.title = "Unpin"; x.setAttribute("aria-label", "Unpin " + label); x.onclick = function (e) { e.preventDefault(); onRemove(); }; a.appendChild(x); }
        refs.appendChild(a);
      }
      D.imgs.forEach(function (im) { add("/api/attachments/" + enc(im.id), im.name, null, "/api/attachments/" + enc(im.id)); });
      pins().forEach(function (p) {
        var a = D.arts.filter(function (x) { return x.id === p.id; })[0];
        if (!a) return;
        add(thumbURL(a.id, p.v || a.latest), a.title + " v" + (p.v || a.latest), function () { setPins(pins().filter(function (q) { return q.id !== p.id; })); drawRefs(); });
      });
      if (!any) refs.appendChild(el("span", "dim small", "Images you attach and artifacts you pin show up here."));
      var pinSel = el("select", "cw-pin"); pinSel.setAttribute("aria-label", "Pin an artifact");
      var o0 = el("option", null, "+ Pin an artifact"); o0.value = ""; pinSel.appendChild(o0);
      D.arts.forEach(function (a) { var o = el("option", null, a.title); o.value = a.id; pinSel.appendChild(o); });
      pinSel.hidden = !D.arts.length;
      pinSel.onchange = function () {
        var a = D.arts.filter(function (x) { return x.id === pinSel.value; })[0];
        if (a && !pins().some(function (p) { return p.id === a.id; })) setPins(pins().concat([{ id: a.id, v: D.id === a.id ? D.ver : a.latest }]));
        drawRefs();
      };
      refs.appendChild(pinSel);
    }
    function draw() {
      var m = D.meta;
      title.textContent = m ? m.title : "Canvas";
      verTag.textContent = m ? "v" + D.ver + " of " + m.latest : "";
      verTag.hidden = !m;
      sel.hidden = D.arts.length < 2;
      title.hidden = !sel.hidden;
      cmp.hidden = !m || (m.versions || []).length < 2;
      cmp.setAttribute("aria-pressed", D.compare ? "true" : "false");
      cmp.classList.toggle("on", D.compare);
      tab.hidden = !m; if (m) tab.href = artURL(m.id, D.ver);
      drawCanvas(); drawFilm(); drawRefs();
    }
    function loadMeta(id, follow) {
      var gen = S.gen;
      return artMeta(id).then(function (m) {
        if (gen !== S.gen) return;
        var keep = D.meta && D.meta.id === id;
        D.meta = m; D.id = id;
        if (m) {
          if (!keep || follow || !D.ver) D.ver = m.latest;
          if (D.right > m.latest) D.right = 0;
        }
        draw();
      });
    }
    sel.onchange = function () { D.compare = false; D.right = 0; D.userPick = true; loadMeta(sel.value, true); };
    function refreshImgs() {
      var gen = S.gen;
      fx(sidURL("?limit=200")).then(function (r) {
        if (gen !== S.gen || !r.ok || !r.json) return;
        var seen = {}, out = [];
        (r.json.messages || []).forEach(function (mm) {
          var t = typeof mm.body === "string" ? mm.body : JSON.stringify(mm.body || "");
          var re = /\[\[image:([A-Za-z0-9_-]+)\|([^\]]*)\]\]/g, m;
          while ((m = re.exec(t))) if (!seen[m[1]]) { seen[m[1]] = 1; out.push({ id: m[1], name: m[2] || "image" }); }
          var re2 = /<attachment[^>]*\bid=\\?"([A-Za-z0-9_-]+)\\?"[^>]*\bkind=\\?"image\\?"/g;
          while ((m = re2.exec(t))) if (!seen[m[1]]) { seen[m[1]] = 1; out.push({ id: m[1], name: "attachment" }); }
        });
        D.imgs = out; drawRefs();
      });
    }
    function refresh(kind) {
      if (!S.sid) return;
      var gen = S.gen;
      if (kind === "all" || kind === "artifact") {
        listArtifacts().then(function (arts) {
          if (gen !== S.gen) return;
          D.arts = arts;
          sel.textContent = "";
          arts.forEach(function (a) { var o = el("option", null, a.title); o.value = a.id; sel.appendChild(o); });
          if (!arts.length) { D.meta = null; D.id = ""; draw(); return; }
          var keepPick = D.userPick && arts.some(function (a) { return a.id === D.id; });
          var want = keepPick ? D.id : arts[0].id;
          sel.value = want;
          /* a new version of the viewed design moves the canvas along to it, unless the user is comparing */
          loadMeta(want, !D.compare && !(D.meta && D.meta.id === want && D.ver !== D.meta.latest && D.userVer));
        });
      }
      if (kind === "all") refreshImgs();
    }
    /* clicking a filmstrip version counts as a deliberate pick: stay on it while new versions arrive */
    film.addEventListener("click", function () { D.userVer = true; });
    function reset() {
      D.arts = []; D.id = ""; D.meta = null; D.ver = 0; D.compare = false; D.right = 0; D.imgs = []; D.userPick = false; D.userVer = false;
      sel.textContent = ""; draw();
    }
    return { refresh: refresh, reset: reset };
  }

  /* =====================================================================================================
     RESEARCH: numbered sources on the left, the live report in the middle, chat on the right
     ===================================================================================================== */
  function buildRes(box) {
    box.className = "cw-stage cw-res";
    var side = el("aside", "cw-pane cw-sources");
    var sh = head("Sources", 0);
    var slist = el("ol", "cw-srclist");
    side.appendChild(sh); side.appendChild(slist);
    var rep = el("article", "cw-pane cw-report");
    var rh = head("Report");
    var rtitle = rh.firstChild;
    var rspacer = el("span", "cw-spacer");
    var rsel = el("select", "cw-ver-sel"); rsel.setAttribute("aria-label", "Report version"); rsel.hidden = true;
    var rtab = el("a", "btn small ghost", "Open in tab"); rtab.target = "_blank"; rtab.rel = "noopener"; rtab.hidden = true;
    rh.appendChild(rspacer); rh.appendChild(rsel); rh.appendChild(rtab);
    var rbody = el("div", "cw-md");
    rep.appendChild(rh); rep.appendChild(rbody);
    box.appendChild(side); box.appendChild(rep);
    var R = { sources: [], meta: null, id: "", ver: 0, sig: "" };

    function drawSources() {
      sh.lastChild.textContent = String(R.sources.length);
      slist.textContent = "";
      if (!R.sources.length) { slist.appendChild(empty("No sources yet. Each page the model reads is numbered here, and the report cites it as [n].")); return; }
      R.sources.forEach(function (s) {
        var li = el("li", "cw-src"); li.id = "cw-src-" + s.n; li.dataset.n = s.n;
        var top = el("div", "cw-srctop");
        top.appendChild(el("span", "cw-n", String(s.n)));
        var t = el("a", "cw-srctitle", s.title || host(s.url) || s.url);
        t.href = s.url; t.target = "_blank"; t.rel = "noopener noreferrer";
        top.appendChild(t);
        li.appendChild(top);
        var meta = el("div", "cw-srcmeta");
        meta.appendChild(el("span", "mono", host(s.url)));
        if (s.auto) { var a = el("span", "cw-badge", "auto"); a.title = "Recorded automatically when the page was fetched"; meta.appendChild(a); }
        li.appendChild(meta);
        if (s.quote) li.appendChild(el("blockquote", "cw-quote", s.quote));
        if (s.note) li.appendChild(el("p", "cw-srcnote", s.note));
        slist.appendChild(li);
      });
    }
    function linkCites(rootEl) {
      var w = document.createTreeWalker(rootEl, NodeFilter.SHOW_TEXT, null), nodes = [], n;
      while ((n = w.nextNode())) { if (/\[\d{1,3}\]/.test(n.nodeValue) && !n.parentNode.closest("a,code,pre")) nodes.push(n); }
      nodes.forEach(function (tn) {
        var frag = document.createDocumentFragment(), last = 0, re = /\[(\d{1,3})\]/g, m, txt = tn.nodeValue;
        while ((m = re.exec(txt))) {
          frag.appendChild(document.createTextNode(txt.slice(last, m.index)));
          var a = el("a", "cw-cite", "[" + m[1] + "]"); a.href = "#cw-src-" + m[1]; a.dataset.n = m[1];
          var src = R.sources.filter(function (s) { return String(s.n) === m[1]; })[0];
          if (src) a.title = src.title || src.url; else a.classList.add("missing");
          frag.appendChild(a); last = m.index + m[0].length;
        }
        frag.appendChild(document.createTextNode(txt.slice(last)));
        tn.parentNode.replaceChild(frag, tn);
      });
    }
    rbody.addEventListener("click", function (e) {
      var a = e.target.closest("a.cw-cite");
      if (!a) return;
      e.preventDefault();
      var row = document.getElementById("cw-src-" + a.dataset.n);
      if (!row) return;
      row.scrollIntoView({ block: "center", behavior: "smooth" });
      row.classList.remove("flash"); void row.offsetWidth; row.classList.add("flash");
    });
    function drawReport(text) {
      var keep = rbody.scrollTop, had = !!rbody.querySelector("h2,h3,h4,p");
      var atEnd = had && rbody.clientHeight > 0 && rbody.scrollTop > 0 && rbody.scrollHeight - rbody.scrollTop - rbody.clientHeight < 40;
      rbody.innerHTML = H.md(text, false);
      linkCites(rbody);
      rbody.scrollTop = !had ? 0 : atEnd && R.following ? rbody.scrollHeight : keep;
    }
    function loadReport() {
      var gen = S.gen, m = R.meta;
      if (!m) { rbody.textContent = ""; rbody.appendChild(empty(S.sid ? "The report appears here once the model starts writing it, and updates as findings land." : "Ask a question below. Sources fill in on the left, and the report grows here.")); rtitle.textContent = "Report"; rsel.hidden = true; rtab.hidden = true; return; }
      var v = (m.versions || []).filter(function (x) { return x.n === R.ver; })[0];
      var f = v && (v.files || []).filter(function (x) { return /\.(md|markdown)$/i.test(x.path); })[0];
      rtitle.textContent = m.title;
      rtab.hidden = false; rtab.href = artURL(m.id, R.ver);
      rsel.hidden = (m.versions || []).length < 2; rsel.textContent = "";
      (m.versions || []).slice().reverse().forEach(function (x) { var o = el("option", null, "v" + x.n); o.value = x.n; if (x.n === R.ver) o.selected = true; rsel.appendChild(o); });
      if (!f) { rbody.textContent = ""; rbody.appendChild(empty("This report has no markdown file yet.")); return; }
      fx("/a/" + enc(m.id) + "/v" + R.ver + "/" + f.path.split("/").map(enc).join("/")).then(function (r) {
        if (gen !== S.gen) return;
        if (!r.ok) { rbody.textContent = ""; rbody.appendChild(empty("Could not load the report.")); return; }
        var sig = m.id + ":" + R.ver + ":" + r.text.length + ":" + R.sources.length;
        if (sig === R.sig) return;
        R.sig = sig; drawReport(r.text);
      });
    }
    rsel.onchange = function () { R.ver = +rsel.value; R.following = R.ver === R.meta.latest; R.sig = ""; loadReport(); };
    function refresh(kind) {
      if (!S.sid) return;
      var gen = S.gen;
      if (kind === "all" || kind === "sources") {
        fx(sidURL("/sources")).then(function (r) {
          if (gen !== S.gen) return;
          R.sources = r.ok && r.json && r.json.sources || [];
          drawSources();
          if (R.meta) { R.sig = ""; loadReport(); }
        });
      }
      if (kind === "all" || kind === "artifact") {
        listArtifacts().then(function (arts) {
          if (gen !== S.gen) return;
          var md = arts.filter(function (a) { return a.kind === "markdown"; })[0];
          if (!md) { R.meta = null; loadReport(); return; }
          artMeta(md.id).then(function (m) {
            if (gen !== S.gen || !m) return;
            var follow = !R.meta || R.meta.id !== m.id || R.ver === R.meta.latest;
            R.meta = m;
            if (follow) { R.ver = m.latest; R.following = true; }
            loadReport();
          });
        });
      }
    }
    function reset() { R.sources = []; R.meta = null; R.id = ""; R.ver = 0; R.sig = ""; drawSources(); loadReport(); }
    return { refresh: refresh, reset: reset };
  }

  /* =====================================================================================================
     WRITE: the document fills the page; the model's suggestions sit in the margin, never auto-applied
     ===================================================================================================== */
  function buildWr(box) {
    box.className = "cw-stage cw-wr";
    var doc = el("section", "cw-doc");
    var bar = el("div", "cw-head cw-docbar");
    var fsel = el("select", "cw-filesel"); fsel.setAttribute("aria-label", "Document");
    var fin = el("input", "cw-pathin"); fin.type = "text"; fin.placeholder = "folder/file.md"; fin.setAttribute("aria-label", "File path in the project"); fin.spellcheck = false;
    var openb = btn("Open", "", function () { openPath(fin.value.trim()); });
    var newb = btn("New document", "", function () { newDoc(); });
    var state = el("span", "cw-state dim small");
    var chatBtn = btn("Chat", "ghost cw-chatbtn", function () { var c = root.classList.toggle("cw-chat-collapsed"); chatBtn.setAttribute("aria-pressed", c ? "false" : "true"); });
    chatBtn.setAttribute("aria-pressed", "true");
    [fsel, fin, openb, newb, el("span", "cw-spacer"), state, chatBtn].forEach(function (n) { bar.appendChild(n); });
    fin.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); openPath(fin.value.trim()); } });
    var banner = el("div", "cw-banner"); banner.hidden = true;
    var page = el("div", "cw-page");
    var hl = el("div", "cw-hl"); hl.setAttribute("aria-hidden", "true");
    var ta = el("textarea", "cw-text"); ta.spellcheck = true; ta.setAttribute("aria-label", "Document text");
    ta.placeholder = "Open a file or start a new document. Your text stays exactly as you type it.";
    ta.disabled = true;
    page.appendChild(hl); page.appendChild(ta);
    doc.appendChild(bar); doc.appendChild(banner); doc.appendChild(page);
    var margin = el("aside", "cw-margin");
    var mh = head("Margin notes", 0);
    var mlist = el("div", "cw-notes");
    margin.appendChild(mh); margin.appendChild(mlist);
    box.appendChild(doc); box.appendChild(margin);

    var W = { path: "", hash: "", saved: "", sugg: [], files: [], dirty: false, saving: false, conflict: false, ranges: {} };

    function recentsKey() { return "judas.cw.files." + (H.cwd() || "none"); }
    function recents() { try { var a = JSON.parse(lsGet(recentsKey()) || "[]"); return Array.isArray(a) ? a : []; } catch (e) { return []; } }
    function pushRecent(p) { lsSet(recentsKey(), JSON.stringify([p].concat(recents().filter(function (x) { return x !== p; })).slice(0, 12))); }
    function drawFiles() {
      var set = [], seen = {};
      [W.path].concat(recents(), W.sugg.map(function (g) { return g.path; }), W.files).forEach(function (p) { if (p && !seen[p]) { seen[p] = 1; set.push(p); } });
      fsel.textContent = "";
      var o0 = el("option", null, set.length ? "Choose a document" : "No documents yet"); o0.value = ""; fsel.appendChild(o0);
      set.forEach(function (p) { var o = el("option", null, p); o.value = p; fsel.appendChild(o); });
      fsel.value = W.path || "";
    }
    fsel.onchange = function () { if (fsel.value) openPath(fsel.value); };
    function setState(t, warn) { state.textContent = t; state.classList.toggle("warn", !!warn); }

    /* ---- load / save ---- */
    function flush() { return W.dirty ? save() : Promise.resolve(true); }
    function openPath(p) {
      if (!p) return Promise.resolve();
      return flush().then(function () {
        return H.ensureSession();
      }).then(function () {
        S.sid = H.sid();
        return load(p);
      }, function (e) { showBanner("Could not start a session: " + e.message); });
    }
    function load(p) {
      var gen = S.gen;
      return fx(sidURL("/file?path=" + enc(p) + "&meta=1")).then(function (r) {
        if (gen !== S.gen) return;
        if (!r.ok || !r.json || typeof r.json.text !== "string") {
          showBanner(r.status === 404 ? "No file “" + p + "” in this project. Use New document to create it." : "Could not open “" + p + "”" + (r.json && r.json.error ? ": " + r.json.error : r.status ? " (" + r.status + ")" : "."));
          return;
        }
        hideBanner();
        W.path = p; W.hash = r.json.hash || r.hash || ""; W.saved = r.json.text; W.dirty = false; W.conflict = false;
        ta.value = r.json.text; ta.disabled = false; fin.value = p;
        pushRecent(p); drawFiles(); setState("Saved"); paint();
        refreshSugg();
      });
    }
    function newDoc() {
      var name = window.prompt("Name of the new document (relative to the project folder)", "draft.md");
      if (!name) return;
      name = name.trim().replace(/^\/+/, "");
      if (!name) return;
      flush().then(function () { return H.ensureSession(); }).then(function () {
        S.sid = H.sid();
        return fx(sidURL("/file?path=" + enc(name)), { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ text: "", base_hash: "" }) });
      }).then(function (r) {
        if (!r.ok) { showBanner("Could not create “" + name + "”" + (r.json && r.json.error ? ": " + r.json.error : r.status ? " (" + r.status + ")" : ".")); return; }
        return load(name).then(function () { ta.focus(); });
      }, function (e) { showBanner("Could not start a session: " + e.message); });
    }
    function save(overwrite) {
      if (!W.path || !S.sid) return Promise.resolve(false);
      if (W.saving) return Promise.resolve(false);
      var text = ta.value, gen = S.gen, path = W.path;
      W.saving = true; setState("Saving...");
      var go = overwrite ? fx(sidURL("/file?path=" + enc(path) + "&meta=1")).then(function (r) { return r.json && r.json.hash || ""; }) : Promise.resolve(W.hash);
      return go.then(function (base) {
        return fx(sidURL("/file?path=" + enc(path)), { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ text: text, base_hash: base }) });
      }).then(function (r) {
        W.saving = false;
        if (gen !== S.gen || path !== W.path) return false;
        if (r.status === 409) { W.conflict = true; setState("Conflict", true); showConflict(); return false; }
        if (!r.ok) { setState("Not saved", true); showBanner("Could not save" + (r.json && r.json.error ? ": " + r.json.error : r.status ? " (" + r.status + ")" : ".")); return false; }
        W.conflict = false; hideBanner();
        W.saved = text;
        var h = r.json && r.json.hash || r.hash;
        if (h) W.hash = h;
        W.dirty = ta.value !== text;
        setState(W.dirty ? "Editing..." : "Saved");
        if (!h) return fx(sidURL("/file?path=" + enc(path) + "&meta=1")).then(function (g) { if (g.json && g.json.hash) W.hash = g.json.hash; return true; });
        if (W.dirty) later("save", save, 600);
        return true;
      });
    }
    function showBanner(t) { banner.hidden = false; banner.className = "cw-banner"; banner.textContent = t; }
    function hideBanner() { banner.hidden = true; banner.textContent = ""; }
    function showConflict() {
      banner.hidden = false; banner.className = "cw-banner conflict"; banner.textContent = "";
      banner.appendChild(el("span", null, "This file changed on disk since you opened it. Your edits are not saved yet."));
      banner.appendChild(btn("Load the disk version", "", function () { W.dirty = false; load(W.path); }));
      banner.appendChild(btn("Keep mine", "primary", function () { save(true); }));
    }
    ta.addEventListener("input", function () {
      W.dirty = ta.value !== W.saved;
      if (W.dirty) setState("Editing...");
      paint();
      if (!W.conflict) later("save", save, 1200);
    });
    ta.addEventListener("scroll", syncScroll);

    /* ---- highlight backdrop + margin notes ---- */
    function syncScroll() { hl.scrollTop = ta.scrollTop; mlist.style.transform = "translateY(" + (-ta.scrollTop) + "px)"; }
    function paint() {
      var text = ta.value, taken = [], items = [];
      W.sugg.filter(function (g) { return g.status === "open" && g.path === W.path && g.find; }).forEach(function (g) {
        var i = text.indexOf(g.find);
        if (i < 0 || text.indexOf(g.find, i + 1) >= 0 && g.kind === "spelling") { W.ranges[g.id] = null; return; }
        var j = i + g.find.length;
        if (taken.some(function (t) { return i < t[1] && j > t[0]; })) { W.ranges[g.id] = null; return; }
        taken.push([i, j]); items.push({ g: g, i: i, j: j });
      });
      items.sort(function (a, b) { return a.i - b.i; });
      hl.textContent = "";
      var pos = 0;
      items.forEach(function (it) {
        hl.appendChild(document.createTextNode(text.slice(pos, it.i)));
        var m = el("mark", "cw-anchor k-" + it.g.kind, text.slice(it.i, it.j)); m.dataset.g = it.g.id;
        hl.appendChild(m); pos = it.j; W.ranges[it.g.id] = m;
      });
      hl.appendChild(document.createTextNode(text.slice(pos) + "\n"));
      drawNotes();
      syncScroll();
    }
    function drawNotes() {
      mlist.textContent = "";
      var open = W.sugg.filter(function (g) { return g.status === "open" || g.status === "stale"; });
      mh.lastChild.textContent = String(open.length);
      if (!open.length) {
        mlist.style.height = "";
        mlist.appendChild(empty(W.path ? "No suggestions. Ask in the chat for a read, and the model leaves spelling fixes and notes here." : "Suggestions from the model appear next to the text they refer to."));
        return;
      }
      var cards = [];
      open.forEach(function (g) {
        var c = el("div", "cw-note k-" + g.kind + (g.status === "stale" ? " stale" : "")); c.dataset.g = g.id;
        var top = el("div", "cw-notetop");
        top.appendChild(el("span", "cw-badge k-" + g.kind, g.kind === "spelling" ? "spelling" : "note"));
        if (g.path !== W.path) { var pl = el("a", "cw-notepath mono", g.path); pl.href = "#"; pl.onclick = function (e) { e.preventDefault(); openPath(g.path); }; top.appendChild(pl); }
        c.appendChild(top);
        if (g.kind === "spelling") {
          var d = el("div", "cw-diff");
          d.appendChild(el("del", null, g.find)); d.appendChild(document.createTextNode(" → ")); d.appendChild(el("ins", null, g.replace));
          c.appendChild(d);
        }
        if (g.why) c.appendChild(el("p", "cw-why", g.why));
        if (g.status === "stale") c.appendChild(el("p", "cw-why warn", "The text changed, so this no longer applies."));
        var acts = el("div", "cw-noteacts");
        if (g.status === "open") acts.appendChild(btn(g.kind === "spelling" ? "Accept" : "Done", "primary", function () { act(g, "accept"); }));
        acts.appendChild(btn(g.kind === "spelling" ? "Reject" : "Dismiss", "ghost", function () { act(g, "reject"); }));
        c.appendChild(acts);
        c.addEventListener("mouseenter", function () { var m = W.ranges[g.id]; if (m) m.classList.add("hot"); });
        c.addEventListener("mouseleave", function () { var m = W.ranges[g.id]; if (m) m.classList.remove("hot"); });
        c.addEventListener("click", function (e) { if (e.target.closest("button,a")) return; var m = W.ranges[g.id]; if (m) { ta.scrollTop = Math.max(0, m.offsetTop - 80); syncScroll(); } });
        mlist.appendChild(c); cards.push({ c: c, g: g });
      });
      /* align each card to its anchor line, pushing down to avoid overlaps; unanchored ones go last */
      var y = 0, tops = 0;
      cards.forEach(function (x) { var m = W.ranges[x.g.id]; x.top = m ? m.offsetTop : Infinity; });
      cards.sort(function (a, b) { return a.top - b.top; });
      cards.forEach(function (x) {
        var t = x.top === Infinity ? y : Math.max(x.top, y);
        x.c.style.top = t + "px"; y = t + x.c.offsetHeight + 8; tops = y;
        x.c.classList.toggle("unanchored", x.top === Infinity);
      });
      mlist.style.height = Math.max(tops, page.clientHeight) + "px";
    }
    function act(g, what) {
      var go = what === "accept" ? flush() : Promise.resolve();
      go.then(function () {
        return fx(sidURL("/suggestions/" + enc(g.id) + "/" + what), { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
      }).then(function (r) {
        if (r.status === 409) { g.status = "stale"; paint(); return; }
        if (!r.ok) { showBanner("Could not " + what + " that suggestion" + (r.json && r.json.error ? ": " + r.json.error : ".")); return; }
        var s = r.json && r.json.suggestion;
        if (s) g.status = s.status; else g.status = what === "accept" ? "accepted" : "rejected";
        if (what === "accept" && g.kind === "spelling" && g.path === W.path) return load(W.path);   // the server changed the file
        paint();
      });
    }
    function refreshSugg() {
      if (!S.sid) return;
      var gen = S.gen;
      return fx(sidURL("/suggestions")).then(function (r) {
        if (gen !== S.gen) return;
        W.sugg = r.ok && r.json && r.json.suggestions || [];
        drawFiles(); paint();
        /* a session that already has suggestions or a document from before opens it by itself */
        if (!W.path && !W.opening) {
          var cand = (W.sugg[0] && W.sugg[0].path) || recents()[0];
          if (cand) { W.opening = true; load(cand).then(function () { W.opening = false; }); }
        }
      });
    }
    function refreshFiles() {
      if (!S.sid) return;
      var gen = S.gen;
      return fx(sidURL("/files")).then(function (r) {
        if (gen !== S.gen || !r.ok || !r.json) return;
        W.files = (r.json.files || []).map(function (f) { return f.path; });
        drawFiles();
      });
    }
    function refresh(kind) {
      if (!S.sid) return;
      if (kind === "all" || kind === "suggestions") refreshSugg();
      if (kind === "all") refreshFiles();
    }
    function reset(adopt) {
      if (adopt && W.path) { return; }        // the session was just created for the open document: keep it
      W.path = ""; W.hash = ""; W.saved = ""; W.sugg = []; W.files = []; W.dirty = false; W.conflict = false; W.ranges = {};
      ta.value = ""; ta.disabled = true; fin.value = ""; hl.textContent = "";
      hideBanner(); setState(""); drawFiles(); drawNotes();
    }
    window.addEventListener("resize", function () { if (L) drawNotes(); });
    return { refresh: refresh, reset: reset, destroy: function () { clearTimeout(timers.save); } };
  }

  window.JudasCowork = { init: init, mode: mode, session: session, event: event };
})();
