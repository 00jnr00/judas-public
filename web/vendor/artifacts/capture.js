/* Injected into every html artifact page by the server. Inside a Judas iframe it answers {type:"judas-capture"} from the
   parent with a PNG of the page (vendored html-to-image). The page itself cannot call /api (sandbox); the parent relays. */
(function () {
  "use strict";
  if (window.parent === window) return;
  function ready(cb) {
    if (window.htmlToImage) return cb();
    var s = document.createElement("script");
    s.src = "/vendor/artifacts/h2i/html-to-image.js";
    s.onload = function () { cb(); };
    s.onerror = function () { cb(new Error("capture library missing")); };
    document.head.appendChild(s);
  }
  function bg() {
    var c = getComputedStyle(document.body).backgroundColor, h = getComputedStyle(document.documentElement).backgroundColor;
    function solid(x) { return x && x !== "transparent" && !/rgba\(.*,\s*0\)$/.test(x); }
    return solid(c) ? c : solid(h) ? h : "#ffffff";
  }
  addEventListener("message", function (e) {
    if (e.source !== window.parent) return;
    var d = e.data;
    if (!d || d.type !== "judas-capture") return;
    function reply(m) { m.type = "judas-captured"; m.token = d.token; window.parent.postMessage(m, "*"); }
    ready(function (err) {
      if (err || !window.htmlToImage) return reply({ error: "capture library missing" });
      var w = Math.max(64, Math.min(2048, d.width | 0)), h = Math.max(64, Math.min(2048, d.height | 0));
      window.htmlToImage.toPng(document.documentElement, { width: w, height: h, canvasWidth: w, canvasHeight: h, pixelRatio: 1, backgroundColor: bg(), cacheBust: false })
        .then(function (url) { reply({ png: url }); }, function (ex) { reply({ error: String(ex && ex.message || ex) }); });
    });
  });
  function hello() { window.parent.postMessage({ type: "judas-artifact-ready" }, "*"); }
  if (document.readyState === "complete") hello(); else addEventListener("load", hello);
})();
