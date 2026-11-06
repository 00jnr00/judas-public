"use strict";
/* Judas rich chat helpers shared by the desktop app and the phone page: the working indicator, inline images,
   product and swatch cards, task lists. Plain ES5, no dependencies. Everything that reaches HTML is escaped
   here; URLs are checked against a short allowlist first. */
(function () {
  var R = window.JudasRich = {};

  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
  R.esc = esc;
  function reduced() { return !!(window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches); }

  /* ---------- URL allowlists (the argument is the raw, unescaped text) ---------- */
  function okLink(u) { return /^https?:\/\/[^\s"'<>]+$/i.test(u); }
  function okImg(u) {
    return /^https?:\/\/[^\s"'<>]+$/i.test(u) || /^\/api\/attachments\/[\w.%-]+$/.test(u) ||
      /^data:image\/(png|jpe?g|gif|webp);base64,[A-Za-z0-9+\/=]+$/.test(u);
  }
  function isRemote(u) { return /^https?:\/\//i.test(u); }
  function hostOf(u) { var m = /^https?:\/\/([^\/?#:]+)/i.exec(u); return m ? m[1] : "remote site"; }
  /* A remote image never loads by itself (the URL could carry data out); this placeholder asks first. */
  function loadBtn(u, alt, cls) {
    return '<button type="button" class="rc-load ' + cls + '" data-src="' + esc(u) + '" data-alt="' + alt + '">' +
      '<span class="rc-lh">' + esc(hostOf(u)) + '</span><span class="rc-lb">Load image</span></button>';
  }
  function imgTag(u, alt) {
    return '<img loading="lazy" decoding="async" referrerpolicy="no-referrer" alt="' + alt + '" src="' + esc(u) + '">';
  }
  function unesc(s) { return String(s).replace(/&quot;/g, '"').replace(/&gt;/g, ">").replace(/&lt;/g, "<").replace(/&amp;/g, "&"); }

  /* ---------- inline images: ![alt](url) ---------- */
  /* Takes the alt and url of an already HTML-escaped line; returns an <img> button or null when the URL is not allowed. */
  R.imgInline = function (altEsc, urlEsc) {
    var u = unesc(urlEsc);
    if (!okImg(u)) return null;
    if (isRemote(u)) return loadBtn(u, altEsc, "rc-load-img");
    return '<button type="button" class="rc-img" data-full="' + esc(u) + '" title="' + altEsc + '">' +
      imgTag(u, altEsc) + '</button>';
  };
  /* Lift every ![alt](url) out of an escaped string; the callback puts a placeholder in, restore() swaps them back. */
  var IMG_MD = /!\[([^\]\n]*)\]\(([^\s)]+)\)/g;
  R.liftImages = function (s, stash) {
    return s.replace(IMG_MD, function (m, alt, url) {
      var h = R.imgInline(alt, url);
      return h ? stash(h) : m;
    });
  };

  /* ---------- cards ---------- */
  var KEYS = { title: "title", name: "title", url: "url", link: "url", image: "image", img: "image", thumbnail: "image",
    price: "price", badge: "badge", tag: "badge", note: "note", description: "note", desc: "note", hex: "hex", color: "hex" };
  function parseItems(body) {
    var items = [], cur = null;
    String(body).split("\n").forEach(function (ln) {
      var m = /^(\s*-\s+)?\s*([A-Za-z_]+)\s*:\s*(.*)$/.exec(ln);
      if (!m || !KEYS[m[2].toLowerCase()]) return;
      var k = KEYS[m[2].toLowerCase()];
      /* a new "- " item, or a title key arriving when the card already has one, starts the next card */
      if (m[1] || !cur || (k === "title" && cur.title !== undefined)) { cur = {}; items.push(cur); }
      var v = m[3].trim().replace(/^(["'])([\s\S]*)\1$/, "$2");
      cur[k] = v;
    });
    return items;
  }
  function cardHTML(it) {
    var title = it.title || it.url || "";
    if (!title) return "";
    var link = it.url && okLink(it.url) ? it.url : "";
    var h = '<div class="rc-card">';
    if (it.image && okImg(it.image)) {
      var img = isRemote(it.image) ? loadBtn(it.image, "", "rc-load-thumb") : imgTag(it.image, "");
      h += link && !isRemote(it.image) ? '<a class="rc-thumb" href="' + esc(link) + '" target="_blank" rel="noopener noreferrer" tabindex="-1">' + img + "</a>"
                : '<span class="rc-thumb">' + img + "</span>";
    }
    h += '<div class="rc-body"><div class="rc-head">';
    h += link ? '<a class="rc-title" href="' + esc(link) + '" target="_blank" rel="noopener noreferrer">' + esc(title) + "</a>"
              : '<span class="rc-title">' + esc(title) + "</span>";
    if (it.badge) h += '<span class="rc-badge">' + esc(it.badge) + "</span>";
    h += "</div>";
    if (it.price) h += '<div class="rc-price">' + esc(it.price) + "</div>";
    if (it.note) h += '<p class="rc-note">' + esc(it.note) + "</p>";
    if (link) h += '<a class="rc-view" href="' + esc(link) + '" target="_blank" rel="noopener noreferrer">View <span aria-hidden="true">↗</span></a>';
    return h + "</div></div>";
  }
  function swatchHTML(it) {
    var hex = String(it.hex || "").trim();
    if (!/^#[0-9a-fA-F]{3,8}$/.test(hex)) return "";
    return '<div class="rc-swatch"><span class="rc-chip" style="background:' + hex + '" role="img" aria-label="' + esc(it.title || hex) + '"></span>' +
      '<div class="rc-body"><div class="rc-title">' + esc(it.title || hex) + '</div><div class="rc-hex">' + esc(hex.toUpperCase()) + "</div>" +
      (it.note ? '<p class="rc-note">' + esc(it.note) + "</p>" : "") + "</div></div>";
  }
  /* A fenced block with language "cards" or "swatch"; returns HTML, or null to render it as ordinary code. */
  R.fence = function (lang, body) {
    lang = String(lang || "").toLowerCase();
    if (lang !== "cards" && lang !== "swatch") return null;
    var items = parseItems(body);
    if (lang === "cards") {
      var h = items.map(cardHTML).join("");
      return h ? '<div class="rc-cards">' + h + "</div>" : null;
    }
    /* one color is a big swatch; 2 to 4 make a palette row (anything past 4 is dropped) */
    var chips = items.map(swatchHTML).filter(Boolean).slice(0, 4);
    if (!chips.length) return null;
    return '<div class="rc-swatches' + (chips.length > 1 ? " rc-palette rc-n" + chips.length : "") + '">' + chips.join("") + "</div>";
  };

  /* ---------- task lists ---------- */
  var TASK = /^\[([ xX])\]\s+(.*)$/;
  var STORE_KEY = "judas.tasks";
  function loadChecks() { try { return JSON.parse(localStorage.getItem(STORE_KEY) || "{}") || {}; } catch (e) { return {}; } }
  function saveChecks(o) {
    try {
      var ks = Object.keys(o);
      if (ks.length > 600) ks.slice(0, ks.length - 600).forEach(function (k) { delete o[k]; });
      localStorage.setItem(STORE_KEY, JSON.stringify(o));
    } catch (e) { /* storage may be unavailable */ }
  }
  /* The page sets R.sid to return the open chat's id so ticks belong to one chat. */
  R.sid = function () { return ""; };
  /* Read and write use the same key: chat id plus the item's raw markdown text. */
  function taskKey(text) { return String(R.sid() || "") + "|" + String(text).trim().slice(0, 160); }
  /* localStorage is parsed once per render burst, not once per item. */
  var checksCache = null;
  function checks() {
    if (!checksCache) { checksCache = loadChecks(); setTimeout(function () { checksCache = null; }, 0); }
    return checksCache;
  }
  /* Returns {text, checked, key} when the list item text starts with [ ] or [x], else null. Local state (a tap) wins over the markdown. */
  R.taskMatch = function (itemText) {
    var m = TASK.exec(itemText);
    if (!m) return null;
    var key = taskKey(m[2]), st = checks()[key];
    return { text: m[2], key: key, checked: st === undefined ? m[1] !== " " : !!st };
  };
  R.taskLi = function (innerHtml, checked, key) {
    return '<li class="rc-task' + (checked ? " done" : "") + '" data-key="' + esc(key || "") + '" role="checkbox" aria-checked="' + (checked ? "true" : "false") +
      '" tabindex="0"><span class="rc-box" aria-hidden="true"></span><span class="rc-tt">' + innerHtml + "</span></li>";
  };
  /* The counter that sits in the heading above a task list. */
  R.taskCount = function (done, total) {
    return '<span class="rc-count" data-total="' + total + '">' + done + "/" + total + "</span>";
  };
  R.taskBar = function (done, total) {
    return '<div class="rc-bar" aria-hidden="true"><i style="width:' + (total ? Math.round(100 * done / total) : 0) + '%"></i></div>';
  };
  function toggleTask(li) {
    var on = !li.classList.contains("done");
    li.classList.toggle("done", on);
    li.setAttribute("aria-checked", on ? "true" : "false");
    var all = loadChecks();
    all[li.getAttribute("data-key") || ""] = on;
    saveChecks(all);
    checksCache = null;
    var ul = li.parentNode, items = ul.querySelectorAll(":scope > .rc-task");
    var done = ul.querySelectorAll(":scope > .rc-task.done").length;
    var h = ul.previousElementSibling;
    if (h && h.classList.contains("rc-bar")) { h.firstChild.style.width = Math.round(100 * done / items.length) + "%"; h = h.previousElementSibling; }
    var c = h && h.querySelector(".rc-count");
    if (c) c.textContent = done + "/" + items.length;
  }

  /* ---------- full-size image viewer ---------- */
  function openFull(src, alt) {
    var o = document.createElement("div");
    o.className = "rc-lightbox"; o.setAttribute("role", "dialog"); o.setAttribute("aria-label", alt || "Image");
    var im = document.createElement("img"); im.src = src; im.alt = alt || "";
    o.appendChild(im);
    var close = function () { o.remove(); document.removeEventListener("keydown", onKey); };
    var onKey = function (e) { if (e.key === "Escape") close(); };
    o.addEventListener("click", close);
    document.addEventListener("keydown", onKey);
    document.body.appendChild(o);
  }

  function loadRemote(btn) {
    var u = btn.getAttribute("data-src") || "", alt = esc(btn.getAttribute("data-alt") || "");
    if (!isRemote(u)) return;
    var tmp = document.createElement("div");
    tmp.innerHTML = btn.classList.contains("rc-load-thumb") ? imgTag(u, alt) :
      '<button type="button" class="rc-img" data-full="' + esc(u) + '" title="' + alt + '">' + imgTag(u, alt) + "</button>";
    btn.parentNode.replaceChild(tmp.firstChild, btn);
  }

  var wired = false;
  R.wire = function () {
    if (wired) return; wired = true;
    document.addEventListener("click", function (e) {
      var t = e.target;
      if (!t.closest) return;
      var ld = t.closest(".rc-load");
      if (ld) { e.preventDefault(); loadRemote(ld); return; }
      var b = t.closest(".rc-img");
      if (b) { e.preventDefault(); var im = b.querySelector("img"); openFull(b.getAttribute("data-full"), im ? im.alt : ""); return; }
      var li = t.closest("li.rc-task");
      if (li && !t.closest("a")) toggleTask(li);
    });
    document.addEventListener("keydown", function (e) {
      if ((e.key === " " || e.key === "Enter") && e.target.classList && e.target.classList.contains("rc-task")) { e.preventDefault(); toggleTask(e.target); }
    });
    /* a thumbnail that fails to load is dropped, not shown as a broken icon */
    document.addEventListener("error", function (e) {
      var t = e.target;
      if (!t || t.tagName !== "IMG" || !t.closest) return;
      var th = t.closest(".rc-thumb");
      if (th) { th.remove(); return; }
      var ib = t.closest(".rc-img");
      if (ib) ib.classList.add("broken");
    }, true);
  };

  /* ---------- working indicator ---------- */
  var WORDS = ["Thinking", "Pondering", "Tinkering", "Scheming", "Plotting", "Brewing", "Untangling", "Conspiring", "Mulling it over",
    "Hatching a plan", "Sleuthing", "Simmering", "Rummaging", "Sharpening the daggers", "Connecting dots", "Whispering to the weights"];
  var TOOLS = [
    [/web_?search|search_web|searx/, "Searching the web"], [/web_?fetch|fetch|browse|url/, "Reading a web page"],
    [/read|view|cat/, "Reading file"], [/write|create|append/, "Writing file"], [/edit|patch|replace/, "Editing file"],
    [/grep|glob|find|search/, "Searching files"], [/list|ls|tree/, "Looking around"], [/shell|bash|exec|run/, "Running a command"],
    [/spawn|team|wait_agent/, "Rallying the helpers"], [/mach12|plan|task/, "Checking the planner"],
    [/screenshot|image/, "Looking at the picture"], [/memory|recall|session_search/, "Remembering"], [/snapshot/, "Checking snapshots"]];
  R.toolLabel = function (name) {
    var n = String(name || "").toLowerCase();
    for (var i = 0; i < TOOLS.length; i++) if (TOOLS[i][0].test(n)) return TOOLS[i][1];
    return n ? "Using " + n.replace(/_/g, " ") : "Working";
  };

  /* One indicator per chat log. show(label) with no label rotates the friendly words; hide() removes it;
     last() moves it back to the bottom of the log after new rows were added. */
  R.work = function (log) {
    var node = null, word = null, timer = 0, idx = Math.floor(Math.random() * WORDS.length), fixed = null;
    function build() {
      node = document.createElement("div"); node.className = "rc-work"; node.setAttribute("role", "status");
      var l = document.createElement("span"); l.className = "rc-loader"; l.setAttribute("aria-hidden", "true");
      l.innerHTML = '<i class="rc-eye"></i><i class="rc-eye"></i>';
      word = document.createElement("span"); word.className = "rc-word";
      node.appendChild(l); node.appendChild(word);
    }
    function setWord(t) {
      if (reduced()) { word.textContent = t; return; }
      word.classList.add("out");
      setTimeout(function () { if (!word) return; word.textContent = t; word.classList.remove("out"); }, 220);
    }
    function next() { idx = (idx + 1) % WORDS.length; setWord(WORDS[idx] + "…"); }
    function stop() { if (timer) { clearInterval(timer); timer = 0; } }
    return {
      show: function (label) {
        if (!node) build();
        if (node.parentNode !== log || log.lastChild !== node) log.appendChild(node);
        if (label) {
          stop();
          if (fixed !== label) { fixed = label; word.classList.remove("out"); word.textContent = label + "…"; }
          return;
        }
        if (fixed !== null || !word.textContent) { fixed = null; word.classList.remove("out"); word.textContent = WORDS[idx] + "…"; }
        if (!timer) timer = setInterval(next, 2500);
      },
      hide: function () { stop(); fixed = null; if (node && node.parentNode) node.parentNode.removeChild(node); },
      active: function () { return !!(node && node.parentNode); }
    };
  };

  /* ---------- GFM pipe tables ---------- */
  var SEP_RE = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/;
  function splitRow(ln) {
    var t = ln.trim().replace(/^\|/, "").replace(/\|$/, ""), cells = [], cur = "";
    for (var i = 0; i < t.length; i++) {
      if (t[i] === "\\" && t[i + 1] === "|") { cur += "|"; i++; }
      else if (t[i] === "|") { cells.push(cur.trim()); cur = ""; }
      else cur += t[i];
    }
    cells.push(cur.trim());
    return cells;
  }
  /* If lines[at] starts a table (a header row with a pipe, then a |---|:-:| separator row), returns
     {html, next} where next is the index after the last body row; otherwise null. Cells go through cellHtml
     (the page's own inline renderer, which escapes). The table sits in a horizontal scroller. */
  R.tableAt = function (lines, at, cellHtml) {
    var head = lines[at], sep = lines[at + 1];
    if (head == null || sep == null || head.indexOf("|") < 0 || sep.indexOf("-") < 0 || !SEP_RE.test(sep)) return null;
    var hc = splitRow(head), sc = splitRow(sep);
    if (hc.length !== sc.length || !hc.length) return null;
    var al = sc.map(function (c) { return /^:-*:$/.test(c) ? "center" : /-:$/.test(c) ? "right" : /^:/.test(c) ? "left" : ""; });
    function cell(tag, txt, i) {
      return "<" + tag + (al[i] ? ' style="text-align:' + al[i] + '"' : "") + ">" + cellHtml(txt) + "</" + tag + ">";
    }
    var html = '<div class="md-table"><table><thead><tr>' + hc.map(function (c, i) { return cell("th", c, i); }).join("") + "</tr></thead><tbody>";
    var j = at + 2;
    for (; j < lines.length && lines[j].trim() !== "" && lines[j].indexOf("|") >= 0; j++) {
      var rc = splitRow(lines[j]), tr = "";
      for (var i = 0; i < hc.length; i++) tr += cell("td", rc[i] == null ? "" : rc[i], i);
      html += "<tr>" + tr + "</tr>";
    }
    return { html: html + "</tbody></table></div>", next: j };
  };

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", R.wire); else R.wire();
})();
