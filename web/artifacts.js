"use strict";
/* Artifacts screen (desktop): grid of everything the model made, a viewer with versions, and the compact card shown in
   chat when a tool made or changed an artifact. Pages open in a sandboxed iframe (no allow-same-origin), so an artifact
   cannot reach this app's API. app.js only lists the screen and calls JudasArtifacts.open() / cardsFor(). */
(function () {
  var A = window.JudasArtifacts = {};
  var items = [], cur = null, pending = null;

  function $(s) { return document.querySelector(s); }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function jget(p) { return fetch(p).then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); }); }
  var KIND = { html: "Web page", svg: "Diagram", markdown: "Document", model3d: "3D model", image: "Image" };
  var ICON = {
    html: "M4 5h16v14H4zM4 9h16M8 7h.01",
    svg: "M4 18l5-7 4 4 3-4 4 7zM7 7a1.6 1.6 0 100 .01",
    markdown: "M6 3.5h9l3 3V20.5H6zM9 11h6M9 14h6M9 17h4",
    model3d: "M12 3l8 4.5v9L12 21l-8-4.5v-9zM12 12l8-4.5M12 12v9M12 12L4 7.5",
    image: "M4 5h16v14H4zM4 16l5-5 4 4 3-3 4 4"
  };
  function icon(kind) {
    var s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    s.setAttribute("viewBox", "0 0 24 24");
    var p = document.createElementNS("http://www.w3.org/2000/svg", "path");
    p.setAttribute("d", ICON[kind] || ICON.html);
    s.appendChild(p);
    return s;
  }
  function rel(ms) {
    var s = Math.max(0, Math.round((Date.now() - ms) / 1000));
    if (s < 60) return "just now";
    if (s < 3600) return Math.round(s / 60) + " min ago";
    if (s < 86400) return Math.round(s / 3600) + " h ago";
    return Math.round(s / 86400) + " days ago";
  }
  function thumb(m, ver) {
    var t = el("div", "art-thumb");
    t.appendChild(icon(m.kind));
    var img = new Image();
    img.alt = "";
    img.onload = function () { t.textContent = ""; t.appendChild(img); };
    img.src = "/api/artifacts/" + encodeURIComponent(m.id) + "/thumb?v=" + (ver || m.latest) + "&t=" + Math.floor(Date.now() / 60000);
    return t;
  }

  function loadList() {
    var q = ($("#art-q") || {}).value || "";
    return jget("/api/artifacts" + (q ? "?q=" + encodeURIComponent(q) : "")).then(function (r) {
      items = r.artifacts || [];
      drawGrid();
      if (window.JudasArtifactCapture) JudasArtifactCapture.ensure(items);
    }, function (e) {
      var g = $("#art-grid"); g.textContent = "";
      g.appendChild(el("p", "dim", "Could not load artifacts: " + e.message));
    });
  }
  function drawGrid() {
    var g = $("#art-grid");
    g.textContent = "";
    if (!items.length) {
      g.appendChild(el("p", "dim", "Nothing here yet. Ask Judas to make a page, a diagram, a document or a 3D view and it will show up here."));
      return;
    }
    items.forEach(function (m) {
      var c = el("button", "art-card"); c.type = "button";
      c.appendChild(thumb(m));
      var meta = el("div", "art-meta");
      meta.appendChild(el("b", null, m.title));
      meta.appendChild(el("span", null, (KIND[m.kind] || m.kind) + " · v" + m.latest + " · " + rel(m.updated_ms)));
      c.appendChild(meta);
      c.onclick = function () { view(m.id); };
      g.appendChild(c);
    });
  }

  function view(id, ver) {
    return jget("/api/artifacts/" + encodeURIComponent(id)).then(function (m) {
      cur = { m: m, ver: ver || m.latest };
      drawView();
    }, function () {
      cur = null;
      $("#screen-artifacts").classList.remove("viewing");
      loadList();
    });
  }
  function drawView() {
    var m = cur.m, root = $("#screen-artifacts");
    root.classList.add("viewing");
    var bar = $("#art-vbar");
    bar.textContent = "";
    var back = el("button", "btn small ghost", "Back"); back.type = "button";
    back.onclick = function () { root.classList.remove("viewing"); $("#art-frame").textContent = ""; cur = null; loadList(); };
    bar.appendChild(back);
    bar.appendChild(el("h2", null, m.title));
    bar.appendChild(el("span", "tag", KIND[m.kind] || m.kind));
    var sel = el("select"); sel.setAttribute("aria-label", "Version");
    m.versions.slice().reverse().forEach(function (v) {
      var o = el("option", null, "v" + v.n + (v.note ? " · " + v.note : ""));
      o.value = v.n;
      if (v.n === cur.ver) o.selected = true;
      sel.appendChild(o);
    });
    sel.onchange = function () { cur.ver = +sel.value; drawFrame(); };
    bar.appendChild(sel);
    var tab = el("a", "btn small", "Open in tab"); tab.id = "art-tab"; tab.target = "_blank"; tab.rel = "noopener";
    bar.appendChild(tab);
    var dl = el("a", "btn small", "Download"); dl.id = "art-dl";
    bar.appendChild(dl);
    var pv = el("button", "btn small ghost", "Refresh preview"); pv.type = "button";
    pv.onclick = function () { refreshPreview(pv); };
    bar.appendChild(pv);
    var del = el("button", "btn small danger", "Delete"); del.type = "button";
    del.onclick = function () {
      if (!confirm("Delete “" + m.title + "” and all " + m.versions.length + " version(s) for good?")) return;
      fetch("/api/artifacts/" + encodeURIComponent(m.id), { method: "DELETE" }).then(function () {
        root.classList.remove("viewing"); $("#art-frame").textContent = ""; cur = null; loadList();
      });
    };
    bar.appendChild(del);
    drawFrame();
  }
  function drawFrame() {
    var m = cur.m, box = $("#art-frame");
    box.textContent = "";
    var url = "/a/" + encodeURIComponent(m.id) + "/v" + cur.ver + "/";
    var f = document.createElement("iframe");
    f.setAttribute("sandbox", "allow-scripts");   // no allow-same-origin: the page is a null-origin document
    f.setAttribute("title", m.title);
    f.src = url;
    box.appendChild(f);
    $("#art-tab").href = url;
    var v = m.versions.filter(function (x) { return x.n === cur.ver; })[0];
    var dl = $("#art-dl");
    dl.href = "/api/artifacts/" + encodeURIComponent(m.id) + "/download?v=" + cur.ver;
    dl.setAttribute("download", "");
    $("#art-note").textContent = v ? v.files.length + " file(s)" + (v.files.length > 1 ? ": " + v.files.map(function (x) { return x.path; }).join(", ") : "") : "";
  }
  function refreshPreview(btn) {
    var m = cur.m;
    btn.disabled = true;
    var done = function () { btn.disabled = false; };
    if (m.kind === "html" || m.kind === "markdown") {
      JudasArtifactCapture.run(m.id, cur.ver, 960, 720).then(function () { btn.textContent = "Preview saved"; setTimeout(function () { btn.textContent = "Refresh preview"; }, 2000); }, function (e) { alert("Could not capture: " + e.message); }).then(done);
    } else {
      fetch("/api/artifacts/" + encodeURIComponent(m.id) + "/render", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ version: cur.ver }) })
        .then(function (r) { btn.textContent = r.ok ? "Preview saved" : "Could not render"; setTimeout(function () { btn.textContent = "Refresh preview"; }, 2000); }).then(done, done);
    }
  }

  /* ---------- chat card for a tool result ---------- */
  A.cardsFor = function (output) {
    var out = [], re = /\[\[artifact:([a-z0-9]+)\|([^|\]]*)\|([a-z0-9]+)\|(\d+)\]\]/g, m;
    while ((m = re.exec(String(output || "")))) {
      (function (id, title, kind, ver) {
        var c = el("button", "art-chat"); c.type = "button";
        c.appendChild(thumb({ id: id, kind: kind, latest: ver }, ver));
        var meta = el("div", "art-meta");
        meta.appendChild(el("b", null, title));
        meta.appendChild(el("span", null, (KIND[kind] || kind) + " · v" + ver + " · open in Artifacts"));
        c.appendChild(meta);
        c.onclick = function () { A.openId(id, +ver); };
        out.push(c);
      })(m[1], m[2], m[3], m[4]);
    }
    return out;
  };
  A.openId = function (id, ver) { pending = { id: id, ver: ver }; window.JudasShow && window.JudasShow("artifacts"); };

  A.open = function () {
    if (pending) { var p = pending; pending = null; loadList(); view(p.id, p.ver); return; }
    $("#screen-artifacts").classList.remove("viewing");
    $("#art-frame").textContent = "";
    loadList();
  };
  if (window.JudasArtifactCapture) JudasArtifactCapture.refresh = function (id) { if (!cur) loadList(); };
  var q = $("#art-q");
  if (q) {
    var t = null;
    q.addEventListener("input", function () { clearTimeout(t); t = setTimeout(loadList, 250); });
  }
  var rf = $("#art-refresh");
  if (rf) rf.onclick = loadList;
})();
