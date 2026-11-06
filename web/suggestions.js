/* Suggested prompts, shared by the desktop app (web/app.js) and the phone UI (web/m/m.js).
   Suggest.mount(host, {mode, input, onPick}) shows a few random {title, prompt} chips,
   rotates them every ~8 s with a fade, pauses while the user types or hovers, and
   offers a shuffle button. Tapping a chip fills the composer; it never sends.
   Items with file:true also ask the host to open its file picker (onPick gets the item). */
(function () {
  "use strict";

  var POOLS = {
chat: [
    { title: "If I were a color", prompt: "If I were a color, which would I be? Ask me three odd questions first." },
    { title: "Plan my day", prompt: "Plan my day. I will list what I need to get done and when I have to be somewhere; build a realistic schedule with breaks." },
    { title: "Summarize this file", prompt: "Summarize the attached file in plain language. Key points first, then anything I need to act on.", file: true },
    { title: "Find barefoot shoes under $50", prompt: "Find barefoot-style shoes under $50 with a wide toe box and a thin flexible sole. Compare the best three." },
    { title: "Explain it like I am ten", prompt: "Explain something to me like I am ten. I will tell you the topic next." },
    { title: "Proofread my writing", prompt: "Proofread the following. Fix spelling, grammar and clarity, keep my voice, and list what you changed.\n\n" },
    { title: "Draft an email", prompt: "Help me write an email. I will tell you who it is to and what I need to say; ask for anything missing, then write the draft.\n\n" },
    { title: "Solve a problem from a photo", prompt: "Solve the problem in the attached photo. Show every step and check the answer at the end.", file: true },
    { title: "Study guide and flashcards", prompt: "Make a study guide and 15 flashcards from the attached material.", file: true },
    { title: "Analyze a spreadsheet", prompt: "Look at the attached spreadsheet. Tell me what it contains, point out anything that looks off, and summarize the main trends.", file: true },
    { title: "Translate something", prompt: "Translate the following into English. Tell me which language it was written in, and note anything that does not translate cleanly.\n\n" },
    { title: "Dinner from what I have", prompt: "Suggest a dinner I can cook with what is in my fridge. I will list the ingredients next." },
    { title: "Pick a movie tonight", prompt: "Suggest a movie for tonight. Ask me two quick questions about the mood first." },
    { title: "Cheer me on", prompt: "I have a hard task ahead and I am dreading it. Give me a short, honest pep talk and a first tiny step." },
    { title: "Settle a debate", prompt: "Help me settle a friendly debate. I will give both sides; argue each fairly, then tell me who has the better case." },
    { title: "Teach me a word", prompt: "Teach me one interesting word I probably do not know, with a sentence to use it in." },
    { title: "Rewrite this message nicer", prompt: "Rewrite this message so it is warm but still direct.\n\n" },
    { title: "Weekend trip ideas", prompt: "Give me five low-cost weekend trip ideas within a few hours of Austin, Texas." },
    { title: "Quiz me on trivia", prompt: "Quiz me with five trivia questions, one at a time, and keep score." },
    { title: "Help me decide", prompt: "I am stuck between two options. Ask me what matters most, then help me choose." },
    { title: "Birthday gift ideas", prompt: "Help me find a thoughtful gift under $40. Ask me about the person first." }
],
cowork: [
    { title: "Break the job into tasks", prompt: "Break this bigger job into clear, ordered tasks with the dependencies marked. I will describe it next." },
    { title: "Plan my week", prompt: "Plan my week. I will list my goals; sort them by what unblocks the rest and flag if it is too much." },
    { title: "Research and compare", prompt: "Research the options for this and give me a short comparison table with a recommendation." },
    { title: "Write a project brief", prompt: "Write a one-page brief for this project: goal, scope, risks, next steps." },
    { title: "Summarize these notes", prompt: "Read my notes folder and give me the five things that matter most right now." },
    { title: "Prep a meeting agenda", prompt: "Prepare an agenda and talking points for my next meeting. I will tell you the topic." },
    { title: "Spot the risks", prompt: "Point out the risks most likely to slow this down and how to head them off." },
    { title: "Draft a status update", prompt: "Draft a short status update from what has been done so far." },
    { title: "Triage my backlog", prompt: "Triage this backlog and pick the next three things to do, with reasons." },
    { title: "Estimate the effort", prompt: "Give a rough effort estimate for each task and say where you are least sure." },
    { title: "Organize these files", prompt: "Look at the working folder and propose a cleaner layout. Do not move anything until I agree." },
    { title: "Find the quickest win", prompt: "What is the smallest piece of this job that would give me a visible result today?" },
    { title: "Split it across workers", prompt: "Split this job across a few workers, one clear deliverable each, then merge the results." },
    { title: "Write the weekly recap", prompt: "Write a short recap of the week: done, blocked, and what carries over." },
    { title: "Make a checklist", prompt: "Turn this into a checklist I can tick off, in the order I should do it." },
    { title: "Review before I send", prompt: "Review this document before I send it. Flag gaps, unclear parts and anything risky." },
    { title: "Cut the scope", prompt: "This is too big. Suggest what to cut or defer so it fits in a week." },
    { title: "Gather sources", prompt: "Gather the sources I need for this and summarize each in two lines." },
    { title: "Compare two plans", prompt: "I have two plans. Lay out the tradeoffs and tell me which you would pick." },
    { title: "What am I avoiding", prompt: "Look at my task list and tell me which item I am probably avoiding, and why it might be stuck." }
],
code: [
    { title: "Summarize this file", prompt: "Read this file and give me a plain-language summary of what it does and how it fits in." },
    { title: "Explain the flow", prompt: "Trace how a request moves through this project, step by step, with file names." },
    { title: "Find a bug", prompt: "Look through this module for bugs and propose a minimal fix." },
    { title: "Add a test", prompt: "Write a regression test for this behavior and run it." },
    { title: "Refactor a function", prompt: "Suggest how to make this function clearer without changing behavior." },
    { title: "Review my diff", prompt: "Review my uncommitted changes and flag anything risky or unfinished." },
    { title: "Write a commit message", prompt: "Write a clear commit message for my staged changes." },
    { title: "Update the docs", prompt: "Update the docs so they match what the code does now." },
    { title: "Why is this slow", prompt: "Find where this is slow, measure it, and suggest the smallest fix." },
    { title: "Fix the build", prompt: "The build is failing. Find the cause and fix it." },
    { title: "Run the tests", prompt: "Run the test suite and report what fails and why." },
    { title: "Scaffold a script", prompt: "Write a small script to automate this repeated task." },
    { title: "Map the repo", prompt: "Give me a map of this repo: main directories, entry points, and where to start reading." },
    { title: "Rename safely", prompt: "Rename this symbol everywhere in the project and check nothing breaks." },
    { title: "Check for leaks", prompt: "Look for resource or memory leaks in this code." },
    { title: "Plan a feature", prompt: "Plan a small feature end to end before writing any code. I will describe it next." },
    { title: "Handle the edge cases", prompt: "List the edge cases this code misses and cover the likeliest ones." },
    { title: "Clean up the TODOs", prompt: "Collect the TODO comments and tell me which are worth doing now." },
    { title: "Explain this error", prompt: "Explain this error message and what is most likely causing it.\n\n" },
    { title: "Add a config option", prompt: "Add a config option for this behavior, with a sensible default and docs." }
],
learn: [
    { title: "Explain it simply", prompt: "Explain this to me like I am a beginner. I will tell you the topic next." },
    { title: "Show a worked example", prompt: "Show me a worked example so it clicks, then give me one to try." },
    { title: "Quiz me", prompt: "Ask me three questions to check what I understand so far." },
    { title: "Teach me from zero", prompt: "I know nothing about this. Ask what I know, then build up from there." },
    { title: "Why does it work", prompt: "Explain why this works before the mechanics." },
    { title: "Build intuition", prompt: "Help me build intuition for this instead of memorizing formulas." },
    { title: "Common mistakes", prompt: "What mistakes do beginners make here and how do I avoid them?" },
    { title: "Use an analogy", prompt: "Explain this with an analogy from everyday life." },
    { title: "Check my work", prompt: "Grade my attempt and tell me what to fix.\n\n" },
    { title: "Compare two ideas", prompt: "Compare these two ideas and tell me when to use each." },
    { title: "Draw it out", prompt: "Describe a diagram that makes this obvious, then draw it." },
    { title: "Give me a mini project", prompt: "Give me a small hands-on project to practice this." },
    { title: "What should I learn next", prompt: "Based on what we covered, what should I learn next and why?" },
    { title: "Recap and review", prompt: "Recap what we covered so I can review it." },
    { title: "Key terms", prompt: "Teach me the key terms for this topic and what each one means." },
    { title: "Real-world use", prompt: "Show me a real-world use for this idea." },
    { title: "Fill my gaps", prompt: "Find what I am unsure about and fill the gaps." },
    { title: "Teach me the physics", prompt: "Explain the physics of something I noticed today. I will describe it." },
    { title: "Study plan", prompt: "Make me a two-week study plan for this topic, 30 minutes a day." },
    { title: "Go one level deeper", prompt: "Go one level deeper on this concept, assuming I followed the basics." }
]
  };
  POOLS.engineering = [
    { title: "Size a bracket", prompt: "Design a bolted steel bracket that carries 2 kN at 150 mm offset with a safety factor of 3. State assumptions, calculate bolt and plate stresses with units, model it, render it and write the calc sheet." },
    { title: "Check a shaft", prompt: "I need a 25 mm steel shaft transmitting 15 kW at 3000 rpm. Check torsional stress, deflection and the keyway, then write the design note." },
    { title: "Model from a sketch", prompt: "I will describe a part with dimensions. Ask what is missing, then model it parametrically if CAD is available, and compare the measured volume to my numbers.\n\n" },
    { title: "Wall thickness for a printed part", prompt: "Find the minimum wall thickness for a PA12-CF printed housing holding 6 bar internal pressure, with a stated safety factor and a check against layer adhesion." },
    { title: "Report on a part", prompt: "Take the part in my project folder, measure it and write a report with the numbers and open risks." },
    { title: "Tolerance stack", prompt: "Work a worst-case and RSS tolerance stack for the dimensions I list next, and say which tolerance to tighten first.\n\n" }
  ];
  POOLS.design = [
    { title: "Cross-section render", prompt: "Sketch a labeled cross-section of the mechanism I describe next, as an SVG artifact, then render it and fix anything that reads wrong.\n\n" },
    { title: "Three concepts", prompt: "Give me three quick concept sketches for the part I describe next, each as a version of one artifact, with one line on the tradeoff of each.\n\n" },
    { title: "Exploded view", prompt: "Draw an exploded view of the assembly in my project folder, parts labeled in assembly order." },
    { title: "Rough 3D blockout", prompt: "Block out a rough 3D shape for the idea I describe, no precision, just proportions, and render it from three angles.\n\n" },
    { title: "Landing page mockup", prompt: "Mock up a one-page product page for the product I describe as an HTML artifact, then render it and tighten the layout.\n\n" },
    { title: "Explain it visually", prompt: "Make a diagram that explains how the system I describe works, step by step, as an SVG artifact.\n\n" }
  ];
  POOLS.research = [
    { title: "Compare options", prompt: "Compare the options I list next on cost, availability and the specs that matter, with sources for every number.\n\n" },
    { title: "What is known", prompt: "What is actually known about this topic, what is disputed, and what nobody has measured? Cite everything.\n\n" },
    { title: "Material data", prompt: "Find published data for the material I name next: strength, modulus, heat deflection and creep, with sources and test conditions.\n\n" },
    { title: "Prior art check", prompt: "Look for patents, products and papers close to the idea I describe, and say how each one differs.\n\n" },
    { title: "Supplier search", prompt: "Find suppliers for the part I describe that ship to Texas, with price breaks and lead times where listed.\n\n" },
    { title: "Explain a field", prompt: "Give me a sourced primer on this field: the main ideas, the key papers and who is working on it now.\n\n" }
  ];
  POOLS.write = [
    { title: "Spelling pass", prompt: "Check my document for spelling and clear typos only. Leave everything else alone." },
    { title: "Structural read", prompt: "Read my document and give me notes on structure: where the lede is, what drags, what is missing. No rewrites." },
    { title: "Does it land", prompt: "Read it as a first-time reader would and tell me in notes where you got lost or bored." },
    { title: "Outline first", prompt: "Make an outline in a new file for the piece I describe, so I can write it myself.\n\n" },
    { title: "Fact check", prompt: "Mark every factual claim in my document that needs a source, as notes. Do not change the text." },
    { title: "Title ideas", prompt: "Read my document and give me five title options in a note, in my voice, not marketing voice." }
  ];
  var ALIAS = { swarm: "cowork" };
  /* The large pool lives in /prompts.json ({chat:[...], cowork:[...], code:[...], learn:[...]}); the lists
     above are the fallback until it loads, or if it never does. */
  var loaded = false, waiting = [];
  function loadPool() {
    if (loaded || !window.fetch) return;
    loaded = true;
    fetch("/prompts.json", { cache: "no-cache" }).then(function (r) { return r.ok ? r.json() : null; })
      .then(function (j) {
        if (!j || typeof j !== "object") return;
        Object.keys(j).forEach(function (k) {
          var ok = (Array.isArray(j[k]) ? j[k] : []).filter(function (it) {
            return it && typeof it.title === "string" && typeof it.prompt === "string";
          });
          if (ok.length >= 6) POOLS[k] = ok;
        });
        waiting.forEach(function (f) { try { f(); } catch (e) { /* ignore */ } });
      }).catch(function () { /* keep the built-in lists */ });
  }
  var ROTATE_MS = 8000, FADE_MS = 350, PAUSE_AFTER_TYPING_MS = 6000;

  function poolFor(mode) { return POOLS[mode] || POOLS[ALIAS[mode]] || POOLS.chat; }

  /* n random distinct items, avoiding any in `avoid` when the pool is big enough */
  function pick(mode, n, avoid) {
    var pool = poolFor(mode).slice(), bad = avoid || [];
    var fresh = pool.filter(function (it) { return bad.indexOf(it) < 0; });
    if (fresh.length >= n) pool = fresh;
    for (var i = pool.length - 1; i > 0; i--) {
      var j = Math.floor(Math.random() * (i + 1)), t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    return pool.slice(0, n);
  }

  var css = ".sg{width:100%;max-width:640px;margin:14px auto 0;display:flex;flex-direction:column;gap:8px;text-align:left}" +
    ".sg-list{display:flex;flex-direction:column;gap:8px;transition:opacity .35s ease}" +
    ".sg-list.sg-out{opacity:0}" +
    ".sg-chip{display:flex;flex-direction:column;gap:2px;width:100%;box-sizing:border-box;padding:10px 14px;min-height:44px;background:var(--surface2,#1f1f26);color:var(--text,#d7d7de);border:1px solid var(--border,#44444c);border-radius:12px;cursor:pointer;font:inherit;text-align:left}" +
    ".sg-chip:hover,.sg-chip:focus-visible{border-color:var(--accent,#c0c0c8)}" +
    ".sg-chip b{color:var(--title,#e8e8f0);font-weight:600;line-height:1.3;font-size:.9375rem}" +
    ".sg-chip span{color:var(--dim,#878791);font-size:.8125rem;line-height:1.35;display:-webkit-box;-webkit-line-clamp:1;-webkit-box-orient:vertical;overflow:hidden}" +
    ".sg-bar{display:flex;justify-content:center}" +
    ".sg-shuffle{display:inline-flex;align-items:center;gap:6px;min-height:36px;padding:4px 14px;background:none;color:var(--dim,#878791);border:1px solid transparent;border-radius:999px;cursor:pointer;font:inherit;font-size:.8125rem}" +
    ".sg-shuffle:hover,.sg-shuffle:focus-visible{color:var(--title,#e8e8f0);border-color:var(--border,#44444c)}" +
    ".sg-shuffle svg{width:15px;height:15px}" +
    "@media (min-width:720px){.sg-list{display:grid;grid-template-columns:1fr 1fr}}" +
    "@media (prefers-reduced-motion:reduce){.sg-list{transition:none}}";
  function injectCss() {
    if (document.getElementById("sg-css")) return;
    var s = document.createElement("style"); s.id = "sg-css"; s.textContent = css; document.head.appendChild(s);
  }

  function reduced() { return !!(window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches); }

  function mount(host, opts) {
    injectCss();
    var mode = opts.mode || "chat", input = opts.input, count = opts.count || 3;
    var root = document.createElement("div"); root.className = "sg"; root.setAttribute("role", "group"); root.setAttribute("aria-label", "Suggested prompts");
    var list = document.createElement("div"); list.className = "sg-list";
    var bar = document.createElement("div"); bar.className = "sg-bar";
    var sh = document.createElement("button"); sh.type = "button"; sh.className = "sg-shuffle"; sh.setAttribute("aria-label", "Shuffle suggestions");
    sh.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M16 3h5v5M4 20 21 3M21 16v5h-5M15 15l6 6M4 4l5 5"/></svg><span>Shuffle</span>';
    bar.appendChild(sh); root.appendChild(list); root.appendChild(bar); host.appendChild(root);

    var shown = [], hovering = false, lastType = 0, timer = null, fadeT = null, dead = false;

    function paint() {
      shown = pick(mode, count, shown);
      list.textContent = "";
      shown.forEach(function (it) {
        var b = document.createElement("button"); b.type = "button"; b.className = "sg-chip";
        var t = document.createElement("b"); t.textContent = it.title;
        var p = document.createElement("span"); p.textContent = it.prompt.replace(/\s+/g, " ").trim();
        b.appendChild(t); b.appendChild(p);
        b.onclick = function () {
          if (input) {
            input.value = it.prompt; input.dispatchEvent(new Event("input", { bubbles: true }));
            input.focus(); try { input.setSelectionRange(input.value.length, input.value.length); } catch (e) { /* ignore */ }
          }
          if (opts.onPick) opts.onPick(it);
        };
        list.appendChild(b);
      });
    }
    function swap(animate) {
      clearTimeout(fadeT);
      if (!animate || reduced()) { paint(); list.classList.remove("sg-out"); return; }
      list.classList.add("sg-out");
      fadeT = setTimeout(function () { if (dead) return; paint(); list.classList.remove("sg-out"); }, FADE_MS);
    }
    function busy() {
      return hovering || (input && (input.value.length > 0 || Date.now() - lastType < PAUSE_AFTER_TYPING_MS)) || document.hidden;
    }
    function tick() {
      if (dead) return;
      if (!root.isConnected) { destroy(); return; }
      if (!busy()) swap(true);
    }
    function onType() { lastType = Date.now(); }
    function destroy() {
      dead = true; clearInterval(timer); clearTimeout(fadeT);
      if (input) input.removeEventListener("input", onType);
      if (root.parentNode) root.parentNode.removeChild(root);
    }
    root.addEventListener("mouseenter", function () { hovering = true; });
    root.addEventListener("mouseleave", function () { hovering = false; });
    root.addEventListener("focusin", function () { hovering = true; });
    root.addEventListener("focusout", function () { hovering = false; });
    sh.onclick = function () { swap(true); };
    if (input) input.addEventListener("input", onType);
    paint();
    /* when the big pool arrives (usually within a few ms), redraw once unless the user is already engaged */
    waiting.push(function () { if (!dead && !busy()) swap(false); });
    loadPool();
    /* with reduced motion the set still changes, only the fade is dropped; nothing flashes because
       rotation is paused while the pointer or focus is inside and while typing */
    timer = setInterval(tick, ROTATE_MS);
    return { destroy: destroy, setMode: function (m) { if (m !== mode) { mode = m; shown = []; swap(false); } }, el: root };
  }

  window.Suggest = { POOLS: POOLS, pick: pick, mount: mount };
})();
