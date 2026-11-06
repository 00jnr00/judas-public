"use strict";
/* Artifact previews made by the browser. html and markdown artifacts need a browser engine to draw, and judas has none,
   so an open Judas page does it: it loads the artifact in a hidden sandboxed iframe, asks the page (capture.js, injected by
   the server) for a PNG, and posts it to /api/artifacts/<id>/render-result. The sandboxed page never calls /api itself.
   Used by the desktop app and the phone page; they call JudasArtifactCapture.onFeed(event) for "artifact.render". */
(function () {
  var C = window.JudasArtifactCapture = {};
  var busy = Promise.resolve(), token = 0;

  function capture(id, ver, w, h) {
    return new Promise(function (resolve, reject) {
      var f = document.createElement("iframe");
      f.setAttribute("sandbox", "allow-scripts");
      f.setAttribute("aria-hidden", "true");
      f.tabIndex = -1;
      f.style.cssText = "position:fixed;left:0;top:0;z-index:-1;opacity:0;border:0;width:" + w + "px;height:" + h + "px;pointer-events:none";
      var tk = "c" + (++token) + "-" + Date.now(), done = false, timer;
      function finish(err, png) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        window.removeEventListener("message", onMsg);
        f.remove();
        if (err) reject(err); else resolve(png);
      }
      function onMsg(e) {
        if (e.source !== f.contentWindow || !e.data) return;
        if (e.data.type === "judas-artifact-ready") {
          setTimeout(function () { try { f.contentWindow.postMessage({ type: "judas-capture", token: tk, width: w, height: h }, "*"); } catch (x) { finish(x); } }, 700);
        } else if (e.data.type === "judas-captured" && e.data.token === tk) {
          if (e.data.png) finish(null, e.data.png); else finish(new Error(e.data.error || "capture failed"));
        }
      }
      window.addEventListener("message", onMsg);
      timer = setTimeout(function () { finish(new Error("the page did not answer")); }, 20000);
      f.src = "/a/" + encodeURIComponent(id) + "/v" + ver + "/";
      document.body.appendChild(f);
    });
  }

  function post(id, ver, dataUrl) {
    return fetch(dataUrl).then(function (r) { return r.blob(); }).then(function (b) {
      return fetch("/api/artifacts/" + encodeURIComponent(id) + "/render-result?version=" + ver, { method: "POST", headers: { "Content-Type": "image/png" }, body: b });
    });
  }

  /* one capture at a time so several requests do not pile up hidden iframes */
  C.run = function (id, ver, w, h) {
    var job = busy.then(function () { return capture(id, ver, w || 960, h || 720); }).then(function (png) { return post(id, ver, png); });
    busy = job.catch(function () { /* keep the queue going */ });
    return job;
  };

  C.onFeed = function (ev) {
    if (!ev || ev.type !== "artifact.render" || !ev.id) return;
    if (ev.deadline_ms && Date.now() > ev.deadline_ms) return;   // an old request replayed after a reconnect
    C.run(ev.id, ev.version, ev.width, ev.height).catch(function () { /* the waiting tool times out with a clear message */ });
  };

  /* Make sure html and markdown artifacts in a list have a preview image (thumbnail request 404s until one exists). */
  C.ensure = function (metas) {
    var todo = (metas || []).filter(function (m) { return m.kind === "html" || m.kind === "markdown"; }).slice(0, 8);
    todo.forEach(function (m) {
      fetch("/api/artifacts/" + encodeURIComponent(m.id) + "/thumb?v=" + m.latest, { method: "HEAD" }).then(function (r) {
        if (r.status === 404) return C.run(m.id, m.latest, 960, 720).then(function () { if (C.refresh) C.refresh(m.id); });
      }).catch(function () { /* optional */ });
    });
  };
})();
