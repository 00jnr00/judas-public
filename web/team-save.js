/* Swarm team maps: a "Save map" button in the team bar and a "Saved maps" list in the empty Swarm state.
   Self-contained; app.js calls TeamSave.render(sessionId, teamData) after it draws the map (one hook line). */
(function () {
  "use strict";
  var state = { sid: "", saved: null, loadedAt: 0 };

  function css() {
    if (document.getElementById("team-save-css")) return;
    var s = document.createElement("style");
    s.id = "team-save-css";
    s.textContent =
      ".ts-bar{display:inline-flex;gap:var(--s2);align-items:center;margin-left:auto}" +
      ".ts-saved{margin:var(--s3) 0 0;padding:var(--s3);display:flex;flex-direction:column;gap:var(--s2);border:1px dashed var(--border);border-radius:var(--radius,8px)}" +
      ".ts-saved h3{margin:0;font-size:var(--fs-sm);font-weight:600}" +
      ".ts-row{display:flex;gap:var(--s3);align-items:baseline;flex-wrap:wrap;text-align:left}" +
      ".ts-row .ts-name{font-weight:600}" +
      ".ts-row .ts-out{flex:1;min-width:12ch}";
    document.head.appendChild(s);
  }
  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }
  function toast(msg) {
    var sub = document.getElementById("team-sub");
    if (sub) sub.textContent = msg;
  }

  function save(sid, team, name, overwrite) {
    return fetch("/api/sessions/" + encodeURIComponent(sid) + "/teams/" + encodeURIComponent(team) + "/save", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: name, overwrite: !!overwrite })
    }).then(function (r) {
      return r.json().catch(function () { return {}; }).then(function (j) { return { status: r.status, body: j }; });
    });
  }
  function ask(sid, team) {
    var name = window.prompt("Save the map of team " + team + " as (a-z, 0-9, -):", team.toLowerCase().replace(/[^a-z0-9-]+/g, "-"));
    if (!name) return;
    name = name.trim();
    save(sid, team, name, false).then(function (r) {
      if (r.status === 409 && window.confirm("A saved map named " + name + " exists. Replace it? (the old one is kept as .bak)"))
        return save(sid, team, name, true);
      return r;
    }).then(function (r) {
      if (!r) return;
      if (r.body && r.body.ok) { toast("Saved map " + name); state.saved = null; }
      else toast((r.body && r.body.error && (r.body.error.message || r.body.error)) || "Could not save the map");
    });
  }

  function bar(sid, teams) {
    var host = document.getElementById("team-sub");
    if (!host || !host.parentNode) return;
    var old = document.getElementById("team-save-bar");
    var live = (teams || []).filter(function (t) { return !t.disbanded; });
    if (!live.length) { if (old) old.remove(); return; }
    var key = live.map(function (t) { return t.name; }).join("|");
    if (old && old.dataset.key === key && old.dataset.sid === sid) return;
    if (old) old.remove();
    var b = el("span", "ts-bar");
    b.id = "team-save-bar";
    b.dataset.key = key;
    b.dataset.sid = sid;
    live.forEach(function (t) {
      var btn = el("button", "btn small ghost", live.length === 1 ? "Save map" : "Save map: " + t.name);
      btn.type = "button";
      btn.title = "Save this team's map to your teams folder so a later job can start from it";
      btn.addEventListener("click", function () { ask(sid, t.name); });
      b.appendChild(btn);
    });
    host.parentNode.appendChild(b);
  }

  function prefill(name) {
    var input = document.getElementById("input");
    if (!input) return;
    input.value = "use the saved map " + name + " for: ";
    input.focus();
    input.dispatchEvent(new Event("input", { bubbles: true }));
  }
  function list(box) {
    if (box.querySelector(".ts-saved")) box.querySelector(".ts-saved").remove();
    var maps = state.saved || [];
    if (!maps.length) return;
    var wrap = el("div", "ts-saved");
    wrap.appendChild(el("h3", null, "Saved maps"));
    maps.forEach(function (m) {
      var row = el("div", "ts-row");
      row.appendChild(el("span", "ts-name", m.name));
      row.appendChild(el("span", "dim small ts-out", [m.title, m.outcome].filter(Boolean).join(" | ")));
      if (m.date) row.appendChild(el("span", "dim small", m.date));
      var use = el("button", "btn small", "Use");
      use.type = "button";
      use.addEventListener("click", function () { prefill(m.name); });
      row.appendChild(use);
      wrap.appendChild(row);
    });
    box.appendChild(wrap);
  }
  function saved(box) {
    var fresh = state.saved && Date.now() - state.loadedAt < 10000;
    if (fresh) return list(box);
    fetch("/api/teams/saved").then(function (r) { return r.json(); }).then(function (j) {
      state.saved = Array.isArray(j) ? j : [];
      state.loadedAt = Date.now();
      var b = document.getElementById("team-map");
      if (b && b.querySelector(".team-empty")) list(b);
    }).catch(function () {});
  }

  window.TeamSave = {
    render: function (sid, data) {
      // after app.js has drawn the map for this tick
      setTimeout(function () {
        css();
        var teams = (data && data.teams) || [];
        bar(sid, teams);
        var box = document.getElementById("team-map");
        if (box && box.querySelector(".team-empty") && !teams.length) saved(box);
      }, 0);
    }
  };
})();
