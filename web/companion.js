"use strict";
/* Pixel companions (Jude and Pip). Frames come from /sprites.json, the single source shared with the TUI.
   Cheap by design: one 16x16 canvas, redrawn only at the state's own fps, stopped when the tab is hidden,
   when the companion is off, or when the user prefers reduced motion (then a single still idle frame). */
(function () {
  var KEY_ON = "judas.mascot", KEY_CH = "judas.mascot.char";
  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } }

  var dock, root, canvas, bubble, ctx;
  var data = null, pal = {}, cache = {};
  var reduce = window.matchMedia ? window.matchMedia("(prefers-reduced-motion: reduce)") : { matches: false };
  var st = { state: "idle", frame: 0, char: lsGet(KEY_CH) || "jude", on: lsGet(KEY_ON) !== "0", server: true,
             timer: 0, revert: 0, hide: 0 };
  var HOLD = { happy: 3000, sad: 5000 };

  function charData() { return data && data.characters && (data.characters[st.char] || data.characters.jude); }
  function frames() { var c = charData(); return c && c.frames[st.state] ? c.frames[st.state] : []; }

  function image(ch, state, i) {
    var k = ch + "/" + state + "/" + i;
    if (cache[k]) return cache[k];
    var c = data.characters[ch], f = c.frames[state][i];
    var img = ctx.createImageData(c.w, c.h);
    for (var y = 0; y < c.h; y++) {
      for (var x = 0; x < c.w; x++) {
        var col = pal[f[y].charAt(x)];
        if (!col) continue;
        var o = (y * c.w + x) * 4;
        img.data[o] = col[0]; img.data[o + 1] = col[1]; img.data[o + 2] = col[2]; img.data[o + 3] = 255;
      }
    }
    return (cache[k] = img);
  }
  function draw() {
    if (!data || !ctx) return;
    var ch = charData(); if (!ch) return;
    var name = data.characters[st.char] ? st.char : "jude";
    var state = reduce.matches ? "idle" : st.state;
    var list = ch.frames[state] || ch.frames.idle;
    var i = reduce.matches ? 0 : Math.min(st.frame, list.length - 1);
    ctx.clearRect(0, 0, ch.w, ch.h);
    ctx.putImageData(image(name, state, i), 0, 0);
  }
  function running() { return data && st.on && st.server && !document.hidden && !reduce.matches && frames().length > 1; }
  function schedule() {
    clearTimeout(st.timer);
    if (!running()) return;
    var fps = charData().fps[st.state] || 2;
    st.timer = setTimeout(function () { requestAnimationFrame(step); }, 1000 / fps);
  }
  function step() {
    if (!running()) return;
    st.frame = (st.frame + 1) % frames().length;
    draw(); schedule();
  }
  function applyVisible() {
    if (!root) return;
    root.hidden = !(data && st.on && st.server);
    var w = document.getElementById("screen-work");
    if (w) w.classList.toggle("has-companion", !root.hidden);
    if (!root.hidden) { draw(); schedule(); } else clearTimeout(st.timer);
  }
  function say(line, keep) {
    if (!bubble) return;
    clearTimeout(st.hide);
    bubble.textContent = line || "";
    bubble.hidden = !line;
    if (line && !keep) st.hide = setTimeout(function () { bubble.hidden = true; }, 4500);
  }
  function set(state, line) {
    if (!data) { st.pending = [state, line]; return; }
    if (!charData().frames[state]) state = "idle";
    var busy = state === "think" || state === "work";
    if (state !== st.state) {
      st.state = state; st.frame = 0;
      clearTimeout(st.revert);
      if (HOLD[state]) st.revert = setTimeout(function () { set("idle"); }, HOLD[state]);
      draw(); schedule();
    }
    if (line !== undefined && line !== bubble.textContent) say(line, busy);
    else if (line === undefined && state === "idle") say("");
  }
  function settle() { if (st.state === "think" || st.state === "work") set("idle", ""); }

  function build() {
    dock = document.getElementById("companion-dock");
    root = document.getElementById("companion");
    canvas = document.getElementById("comp-canvas");
    bubble = document.getElementById("comp-bubble");
    if (!dock || !root || !canvas) return false;
    ctx = canvas.getContext("2d");
    canvas.addEventListener("click", function () { set("happy", "Hi!"); });
    document.addEventListener("visibilitychange", function () { if (document.hidden) clearTimeout(st.timer); else { draw(); schedule(); } });
    if (reduce.addEventListener) reduce.addEventListener("change", function () { draw(); schedule(); });
    return true;
  }
  function load() {
    fetch("/sprites.json").then(function (r) { return r.json(); }).then(function (j) {
      data = j;
      Object.keys(j.palette || {}).forEach(function (k) {
        var v = j.palette[k];
        if (/^#[0-9a-f]{6}$/i.test(v)) pal[k] = [parseInt(v.slice(1, 3), 16), parseInt(v.slice(3, 5), 16), parseInt(v.slice(5, 7), 16)];
      });
      if (!j.characters[st.char]) st.char = "jude";
      applyVisible();
      if (st.pending) { var p = st.pending; st.pending = null; set(p[0], p[1]); }
    }).catch(function () { /* no sprites: the companion stays hidden */ });
  }

  window.JudasCompanion = {
    set: function (s, l) { try { set(s, l); } catch (e) { /* never break the app */ } },
    settle: function () { try { settle(); } catch (e) { /* ignore */ } },
    isOn: function () { return st.on; },
    setOn: function (b) { st.on = !!b; lsSet(KEY_ON, b ? "1" : "0"); applyVisible(); },
    setServer: function (b) { if (st.server !== !!b) { st.server = !!b; applyVisible(); } },
    serverAllows: function () { return st.server; },
    char: function () { return st.char; },
    chars: function () { return data ? Object.keys(data.characters) : ["jude", "pip"]; },
    setChar: function (n) {
      if (data && !data.characters[n]) return;
      st.char = n; st.frame = 0; lsSet(KEY_CH, n); applyVisible();
    }
  };
  function boot() { if (build()) { applyVisible(); load(); } }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
})();
