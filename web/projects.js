/* Projects (desktop): the project list cache, the "new project" form shared by the work header and the Projects
   screen, and the Projects screen itself. A project is a name + folder + instructions; sessions attach to one and
   use its folder as their working directory (docs/API.md, Projects). app.js sets JudasProjects.host. */
(function () {
  "use strict";
  var $ = function (s, r) { return (r || document).querySelector(s); };
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }

  var state = { list: [], root: "", loaded: false }, subs = [];
  var P = { host: null };

  function call(method, path, body) {
    var init = { method: method, headers: {} };
    if (body !== undefined) { init.headers["Content-Type"] = "application/json"; init.body = JSON.stringify(body); }
    return fetch(path, init).then(function (r) {
      return r.text().then(function (t) {
        var j = null; try { j = t ? JSON.parse(t) : null; } catch (e) { /* not json */ }
        if (!r.ok) throw new Error((j && j.error && j.error.message) || t || ("HTTP " + r.status));
        return j;
      });
    });
  }
  function notify() { subs.forEach(function (f) { try { f(); } catch (e) { /* ignore */ } }); }
  function load() {
    return call("GET", "/api/projects").then(function (r) {
      state.list = r.projects || []; state.root = r.root || ""; state.loaded = true; notify(); return state.list;
    });
  }
  function byId(id) {
    for (var i = 0; i < state.list.length; i++) if (state.list[i].id === id) return state.list[i];
    return null;
  }
  function recents() { try { var r = JSON.parse(lsGet("judas.recentDirs") || "[]"); return Array.isArray(r) ? r : []; } catch (e) { return []; } }
  function slug(n) {
    var s = String(n).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48).replace(/-+$/, "");
    return s || "project";
  }
  function when(ms) { return ms ? new Date(ms).toLocaleDateString() : ""; }

  /* ---------- the new-project form ---------- */
  /* opts: onCreated(project), onCancel(), idp (unique id prefix) */
  function buildForm(opts) {
    var idp = opts.idp || "np", form = el("form", "proj-form");
    form.autocomplete = "off";
    function field(label, ctl, id) {
      var f = el("div", "field"), l = el("label", null, label); if (id) l.htmlFor = id;
      f.appendChild(l); f.appendChild(ctl); return f;
    }
    var name = el("input"); name.type = "text"; name.id = idp + "-name"; name.placeholder = "Kitchen remodel"; name.maxLength = 120;
    form.appendChild(field("Name", name, name.id));

    var kinds = el("div", "proj-kinds"), makeId = idp + "-make", exId = idp + "-ex";
    function radio(id, val, text, checked) {
      var w = el("label", "proj-kind"), r = el("input"); r.type = "radio"; r.name = idp + "-kind"; r.id = id; r.value = val; r.checked = checked;
      w.appendChild(r); w.appendChild(el("span", null, text)); return [w, r];
    }
    var rm = radio(makeId, "make", "Make a new folder", true), re = radio(exId, "existing", "Use an existing folder", false);
    kinds.appendChild(rm[0]); kinds.appendChild(re[0]);
    var hint = el("div", "dim small proj-hint");
    var path = el("input", "cwd"); path.type = "text"; path.id = idp + "-path"; path.placeholder = "/home/you/project"; path.spellcheck = false;
    var pathWrap = field("Folder", path, path.id), chips = el("div", "chips");
    pathWrap.appendChild(chips);
    recents().slice(0, 5).forEach(function (d) {
      var b = el("button", "chip", d); b.type = "button"; b.title = d; b.onclick = function () { path.value = d; }; chips.appendChild(b);
    });
    var kf = el("div", "field"); kf.appendChild(el("label", null, "Folder")); kf.appendChild(kinds); kf.appendChild(hint);
    form.appendChild(kf); form.appendChild(pathWrap);
    function sync() {
      var mk = rm[1].checked;
      pathWrap.hidden = mk;
      hint.hidden = !mk;
      hint.textContent = "Created for you at " + (state.root || "~/Judas/Projects") + "/" + slug(name.value || "project") + "/. Judas never deletes it.";
    }
    rm[1].onchange = re[1].onchange = sync; name.addEventListener("input", sync); sync();

    var ins = el("textarea"); ins.id = idp + "-ins"; ins.rows = 4; ins.maxLength = 8000;
    ins.placeholder = "Optional. Added to the instructions of every session in this project, for example: answer in French; this is a Rust workspace, run cargo test before saying done.";
    form.appendChild(field("Instructions", ins, ins.id));

    var err = el("div", "field-err"); err.hidden = true; form.appendChild(err);
    var acts = el("div", "actions"), ok = el("button", "btn primary", "Create project"); ok.type = "submit";
    acts.appendChild(ok);
    if (opts.onCancel) { var c = el("button", "btn", "Cancel"); c.type = "button"; c.onclick = opts.onCancel; acts.appendChild(c); }
    form.appendChild(acts);
    form.addEventListener("submit", function (e) {
      e.preventDefault();
      err.hidden = true;
      var body = { name: name.value.trim(), mode: rm[1].checked ? "make" : "existing", instructions: ins.value };
      if (!body.name) { err.textContent = "Give the project a name."; err.hidden = false; name.focus(); return; }
      if (body.mode === "existing") {
        body.folder = path.value.trim();
        if (!body.folder) { err.textContent = "Enter the folder, an absolute path."; err.hidden = false; path.focus(); return; }
      }
      ok.disabled = true;
      call("POST", "/api/projects", body).then(function (p) {
        ok.disabled = false; return load().then(function () { if (opts.onCreated) opts.onCreated(p); });
      }, function (er) { ok.disabled = false; err.textContent = er.message; err.hidden = false; });
    });
    form.focusFirst = function () { name.focus(); };
    return form;
  }

  /* ---------- the Projects screen ---------- */
  var MODE_CHOICES = [["chat", "Chat"], ["code", "Code"], ["cowork", "Cowork"], ["engineering", "Engineering"], ["swarm", "Swarm"]];
  var openCards = {};

  function projectCard(p, detail) {
    var card = el("div", "card proj-card");
    var head = el("div", "proj-head");
    head.appendChild(el("h3", "proj-name", p.name));
    head.appendChild(el("span", "tag", p.kind === "made" ? "folder made by Judas" : "existing folder"));
    if (!p.folder_exists) { var miss = el("span", "tag failed", "folder missing"); miss.title = "The folder is not on disk any more."; head.appendChild(miss); }
    card.appendChild(head);
    card.appendChild(el("div", "proj-path mono small dim", p.folder));
    if (p.instructions) {
      var line = p.instructions.split("\n")[0];
      card.appendChild(el("p", "proj-ins dim small", "Instructions: " + (line.length > 140 ? line.slice(0, 140) + "..." : line)));
    }

    /* sessions */
    var sess = el("div", "proj-sessions");
    var ss = (detail && detail.sessions) || [];
    if (!ss.length) sess.appendChild(el("p", "dim small", p.session_count ? "Loading sessions..." : "No sessions yet."));
    ss.slice(0, 8).forEach(function (s) {
      var b = el("button", "proj-sess", ""); b.type = "button";
      b.appendChild(el("span", "t", s.title || s.id));
      b.appendChild(el("span", "tag", s.mode)); b.appendChild(el("span", "dim small", when(s.updated_ms)));
      b.onclick = function () { if (P.host) P.host.openSession(s.id); };
      sess.appendChild(b);
    });
    if (ss.length > 8) sess.appendChild(el("p", "dim small", (ss.length - 8) + " older sessions are in each mode's list."));
    card.appendChild(sess);

    /* start a session */
    var row = el("div", "proj-row");
    var sel = el("select"); sel.setAttribute("aria-label", "Mode for a new session in " + p.name);
    MODE_CHOICES.forEach(function (m) { var o = el("option", null, m[1]); o.value = m[0]; sel.appendChild(o); });
    sel.value = "code";
    var go = el("button", "btn small primary", "New session"); go.type = "button";
    go.onclick = function () { if (P.host) P.host.newSession(sel.value, p.id); };
    row.appendChild(sel); row.appendChild(go);
    card.appendChild(row);

    /* edit + delete */
    var det = el("details", "proj-edit"); det.open = !!openCards[p.id];
    det.addEventListener("toggle", function () { openCards[p.id] = det.open; });
    det.appendChild(el("summary", null, "Edit"));
    var form = el("form", "proj-form"); form.autocomplete = "off";
    function fld(label, ctl) { var f = el("div", "field"); f.appendChild(el("label", null, label)); f.appendChild(ctl); form.appendChild(f); }
    var nm = el("input"); nm.type = "text"; nm.value = p.name; nm.maxLength = 120; fld("Name", nm);
    var fo = el("input", "cwd"); fo.type = "text"; fo.value = p.folder; fo.spellcheck = false;
    if (p.kind === "made") { fo.readOnly = true; fo.title = "A folder Judas made cannot be moved here."; }
    fld("Folder", fo);
    var ins = el("textarea"); ins.rows = 5; ins.maxLength = 8000; ins.value = p.instructions || ""; fld("Instructions", ins);
    var msg = el("div", "field-err"); msg.hidden = true; form.appendChild(msg);
    var acts = el("div", "actions"), save = el("button", "btn primary", "Save"); save.type = "submit"; acts.appendChild(save);
    form.appendChild(acts);
    form.addEventListener("submit", function (e) {
      e.preventDefault(); msg.hidden = true; save.disabled = true;
      var body = { name: nm.value.trim(), instructions: ins.value };
      if (p.kind !== "made" && fo.value.trim() !== p.folder) body.folder = fo.value.trim();
      call("PATCH", "/api/projects/" + encodeURIComponent(p.id), body).then(function () {
        save.disabled = false; return load();
      }, function (er) { save.disabled = false; msg.textContent = er.message; msg.hidden = false; });
    });
    det.appendChild(form);

    var del = el("div", "proj-del");
    var delBtn = el("button", "btn small danger", "Delete project"); delBtn.type = "button";
    var panel = el("div", "proj-del-panel"); panel.hidden = true;
    delBtn.onclick = function () { panel.hidden = !panel.hidden; if (!panel.hidden) buildDelete(); };
    function buildDelete() {
      panel.textContent = "";
      panel.appendChild(el("p", null, "Delete the project “" + p.name + "”? Its sessions stay (they just leave the project). Judas does not touch the folder " + p.folder + "."));
      var lab = el("label", "proj-also"), cb = el("input"); cb.type = "checkbox";
      lab.appendChild(cb); lab.appendChild(el("span", null, "Also delete the folder and everything in it"));
      panel.appendChild(lab);
      var r = el("div", "actions"), yes = el("button", "btn danger", "Delete project"); yes.type = "button";
      var no = el("button", "btn", "Cancel"); no.type = "button"; no.onclick = function () { panel.hidden = true; };
      r.appendChild(yes); r.appendChild(no); panel.appendChild(r);
      var emsg = el("div", "field-err"); emsg.hidden = true; panel.appendChild(emsg);
      cb.onchange = function () { yes.textContent = cb.checked ? "Delete project and folder..." : "Delete project"; };
      var armed = false;
      yes.onclick = function () {
        if (cb.checked && !armed) {   // the second ask, naming the folder
          armed = true;
          yes.textContent = "Yes, permanently delete " + p.folder;
          emsg.className = "field-err"; emsg.textContent = "This cannot be undone: every file in that folder goes. Click again to confirm, or Cancel."; emsg.hidden = false;
          return;
        }
        yes.disabled = true;
        var q = cb.checked ? "?delete_folder=1&confirm_folder=1" : "";
        call("DELETE", "/api/projects/" + encodeURIComponent(p.id) + q).then(function (r) {
          if (P.host && P.host.projectGone) P.host.projectGone(p.id);
          return load();
        }, function (er) { yes.disabled = false; emsg.textContent = er.message; emsg.hidden = false; });
      };
    }
    del.appendChild(delBtn); del.appendChild(panel);
    det.appendChild(del);
    card.appendChild(det);
    return card;
  }

  var screenHost = null, details = {};
  function renderScreen(host) {
    if (host) screenHost = host;
    if (!screenHost) return;
    var box = screenHost;
    box.textContent = "";
    var hd = el("header", "page-head"); hd.appendChild(el("h2", null, "Projects"));
    hd.appendChild(el("p", "desc", "A project is a name, a folder and optional instructions. Pick one in any mode and the session works in its folder with its instructions."));
    box.appendChild(hd);
    var np = el("details", "card form-card proj-new"); np.id = "proj-new-details";
    np.appendChild(el("summary", null, "New project"));
    np.appendChild(buildForm({ idp: "pg", onCreated: function () { np.open = false; } }));
    box.appendChild(np);
    if (!state.list.length) {
      box.appendChild(el("p", "dim", state.loaded ? "No projects yet. Make one above, or pick “New project” in a mode's header." : "Loading..."));
      return;
    }
    state.list.forEach(function (p) {
      var card = projectCard(p, details[p.id]);
      box.appendChild(card);
      if (!details[p.id] || details[p.id].stamp !== p.updated_ms + ":" + p.session_count) {
        call("GET", "/api/projects/" + encodeURIComponent(p.id)).then(function (d) {
          d.stamp = p.updated_ms + ":" + p.session_count; details[p.id] = d;
          var fresh = projectCard(p, d); card.replaceWith(fresh);
        }, function () { /* the list still shows */ });
      }
    });
  }
  subs.push(function () { if (screenHost && $("#screen-projects") && $("#screen-projects").classList.contains("active")) renderScreen(); });

  P.load = load; P.byId = byId; P.list = function () { return state.list; }; P.root = function () { return state.root; };
  P.onChange = function (f) { subs.push(f); }; P.buildForm = buildForm; P.renderScreen = renderScreen;
  P.patchSession = function (sid, project) { return call("PATCH", "/api/sessions/" + encodeURIComponent(sid), { project: project }); };
  P.invalidate = function () { details = {}; };
  window.JudasProjects = P;
})();
