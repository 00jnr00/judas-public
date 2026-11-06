/* Share my location with Judas (desktop page and phone page). Off until the user turns it on in Settings; the toggle
   asks the browser (or the Android app) for permission. While on, a visible page reports its position to
   POST /api/location when it moved more than about 100 m, and at least every 5 minutes. The server keeps only the
   latest fix in memory (docs/API.md, Location). The switch is remembered in this browser only. */
(function () {
  "use strict";
  var KEY = "judas.shareLocation", MOVE_M = 100, EVERY_MS = 5 * 60 * 1000;
  var device = window.JudasHost ? "Judas Android app" : (window.matchMedia && matchMedia("(max-width:760px)").matches ? "Phone browser" : "Desktop browser");
  var watchId = null, timer = null, last = null, listeners = [], nativeWait = null;

  function lsGet() { try { return localStorage.getItem(KEY); } catch (e) { return null; } }
  function lsSet(v) { try { localStorage.setItem(KEY, v); } catch (e) { /* ignore */ } }
  function enabled() { return lsGet() === "1"; }
  function supported() { return !!(navigator.geolocation || (window.JudasHost && window.JudasHost.requestLocation)); }
  function emit() { listeners.forEach(function (f) { try { f(); } catch (e) { /* ignore */ } }); }

  function meters(a, b) {
    var R = 6371000, r = Math.PI / 180, dl = (b.lat - a.lat) * r, dn = (b.lon - a.lon) * r;
    var h = Math.sin(dl / 2) * Math.sin(dl / 2) + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dn / 2) * Math.sin(dn / 2);
    return 2 * R * Math.atan2(Math.sqrt(h), Math.sqrt(1 - h));
  }
  function send(fix) {
    var body = { lat: fix.lat, lon: fix.lon, accuracy: fix.accuracy, timestamp: fix.timestamp, device: device };
    last = { lat: fix.lat, lon: fix.lon, t: Date.now() };
    return fetch("/api/location", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) })
      .then(function () { emit(); }, function () { /* offline: the next fix tries again */ });
  }
  function fromPos(pos) {
    var c = pos.coords;
    return { lat: c.latitude, lon: c.longitude, accuracy: c.accuracy, timestamp: pos.timestamp };
  }
  function onPos(pos) {
    if (!enabled()) return;
    var f = fromPos(pos);
    if (!last || Date.now() - last.t >= EVERY_MS || meters(last, f) >= MOVE_M) send(f);
  }
  function errText(err) {
    if (err && err.code === 1) return "Location permission was refused. Allow it for this site in the browser (or Android) settings, then turn this on again.";
    if (err && err.code === 3) return "The device did not answer in time. Try again.";
    if (err && err.code === 2) return "The device could not work out where it is. Check that location is on.";
    return (err && err.message) || "Could not get the location.";
  }
  function insecureNote() {
    return window.isSecureContext === false && !window.JudasHost
      ? " Browsers only give location to https pages or localhost; open Judas that way (for example through tailscale serve) or use the Android app." : "";
  }

  /* one fix, from the browser, else from the Android app's native fallback */
  function getFix() {
    return new Promise(function (resolve, reject) {
      var viaNative = function (err) {
        if (!(window.JudasHost && window.JudasHost.requestLocation)) { reject(err); return; }
        var t = setTimeout(function () { nativeWait = null; reject(err); }, 25000);
        nativeWait = function (j) { clearTimeout(t); nativeWait = null; if (j && typeof j.lat === "number") resolve(j); else reject(err); };
        window.JudasHost.requestLocation();
      };
      if (!navigator.geolocation) { viaNative({ code: 0, message: "This browser has no location support." }); return; }
      navigator.geolocation.getCurrentPosition(function (p) { resolve(fromPos(p)); }, function (e) { viaNative(e); },
        { enableHighAccuracy: false, maximumAge: 60000, timeout: 20000 });
    });
  }

  function startWatch() {
    if (!enabled() || document.visibilityState !== "visible") return;
    if (navigator.geolocation && watchId == null) {
      try { watchId = navigator.geolocation.watchPosition(onPos, function () { /* the timer falls back to getFix */ }, { enableHighAccuracy: false, maximumAge: 60000, timeout: 60000 }); }
      catch (e) { watchId = null; }
    }
    if (!timer) timer = setInterval(tick, 60000);
  }
  function stopWatch() {
    if (watchId != null && navigator.geolocation) { try { navigator.geolocation.clearWatch(watchId); } catch (e) { /* ignore */ } }
    watchId = null;
    clearInterval(timer); timer = null;
  }
  function tick() {
    if (!enabled() || document.visibilityState !== "visible") return;
    if (last && Date.now() - last.t < EVERY_MS - 5000) return;
    getFix().then(function (f) { if (enabled()) send(f); }, function () { /* quiet: the status line shows the server's side */ });
  }
  document.addEventListener("visibilitychange", function () {
    if (document.visibilityState === "visible") { startWatch(); if (enabled() && (!last || Date.now() - last.t > 60000)) tick(); }
    else stopWatch();
  });

  /* turn sharing on: asks for permission, takes one fix, then keeps reporting. Rejects with a sentence for the user. */
  function enable() {
    if (!supported()) return Promise.reject(new Error("This browser has no location support." + insecureNote()));
    return getFix().then(function (f) {
      lsSet("1");
      return send(f).then(function () { startWatch(); emit(); });
    }, function (err) { throw new Error(errText(err) + insecureNote()); });
  }
  function disable() {
    lsSet("0");
    stopWatch();
    last = null;
    return fetch("/api/location", { method: "DELETE" }).then(function () { emit(); }, function () { emit(); });
  }
  function status() {
    return fetch("/api/location").then(function (r) { return r.json(); }).catch(function () { return null; });
  }
  function init() {
    if (!enabled()) return;
    var go = function () { startWatch(); tick(); };
    if (navigator.permissions && navigator.permissions.query) {
      navigator.permissions.query({ name: "geolocation" }).then(function (p) {
        if (p.state === "granted") go();
        else if (p.state === "denied") { lsSet("0"); emit(); }
        /* "prompt": never ask on page load; the toggle in Settings is where permission is requested */
      }, go);
    } else go();
  }

  window.JudasLocation = {
    enabled: enabled, supported: supported, enable: enable, disable: disable, status: status, init: init,
    onChange: function (f) { listeners.push(f); },
    native: function (j) { if (nativeWait) nativeWait(j); },
    describe: function (s) {   // one line for Settings from GET /api/location
      if (!s) return "The server did not answer.";
      if (!s.has_fix) return enabled() ? "Sharing is on in this browser; waiting for the first position." : "Not sharing. Judas cannot see where you are.";
      var age = s.age_s < 90 ? s.age_s + " s" : Math.round(s.age_s / 60) + " min";
      return "Latest position " + age + " ago" + (s.stale ? " (stale)" : "") + (s.device ? " from " + s.device : "") + (s.place ? ", near " + s.place : "") + ".";
    }
  };
  init();
})();
