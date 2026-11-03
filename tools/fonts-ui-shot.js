/* Screenshots of Settings > Fonts and the Swarm screen with three font choices, plus document.fonts checks.
   Needs the fake daemon:  python3 tools/swarm-ui-shot.py --serve   (prints the URL)
   Run:  NODE_PATH=<dir with playwright> node tools/fonts-ui-shot.js http://127.0.0.1:PORT/ OUTDIR  */
const { chromium } = require("playwright");
const base = process.argv[2], out = process.argv[3];
const choices = [
  { tag: "inter-jetbrains", ui: "inter", mono: "jetbrains-mono", size: "100", lh: "" },
  { tag: "literata-spacemono", ui: "literata", mono: "space-mono", size: "115", lh: "relaxed" },
  { tag: "atkinson-ibmplexmono", ui: "atkinson", mono: "ibm-plex-mono", size: "90", lh: "compact" },
];
const families = { inter: "Inter", literata: "Literata", atkinson: "Atkinson Hyperlegible",
  "jetbrains-mono": "JetBrains Mono", "space-mono": "Space Mono", "ibm-plex-mono": "IBM Plex Mono" };
(async () => {
  const browser = await chromium.launch({ executablePath: "/usr/bin/chromium", args: ["--no-sandbox"] });
  let bad = 0;
  for (const c of choices) {
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
    await ctx.addInitScript((c) => {
      try {
        localStorage.setItem("judas.font.ui", c.ui); localStorage.setItem("judas.font.mono", c.mono);
        if (c.size !== "100") localStorage.setItem("judas.font.size", c.size);
        if (c.lh) localStorage.setItem("judas.font.lh", c.lh);
      } catch (e) {}
    }, c);
    const page = await ctx.newPage();
    const fails = [];
    page.on("requestfailed", (r) => fails.push(r.url()));
    page.on("response", (r) => { if (r.status() >= 400 && r.url().includes("/fonts")) fails.push(r.url()); });
    await page.goto(base + "#swarm", { waitUntil: "networkidle" });
    await page.waitForTimeout(800);
    await page.screenshot({ path: `${out}/swarm-${c.tag}.png` });
    await page.evaluate(() => document.querySelector('button[data-screen="settings"]').click());
    await page.waitForTimeout(800);
    await page.evaluate(() => document.getElementById("font-settings").scrollIntoView());
    await page.waitForTimeout(800);
    await page.screenshot({ path: `${out}/settings-fonts-${c.tag}.png` });
    const info = await page.evaluate(async (fam) => {
      await document.fonts.ready;
      const cs = getComputedStyle(document.documentElement);
      return { ui: cs.getPropertyValue("--font-ui"), mono: cs.getPropertyValue("--font-mono"),
        bodyFont: getComputedStyle(document.body).fontFamily, bodySize: getComputedStyle(document.body).fontSize,
        check: fam.map((f) => [f, document.fonts.check('16px "' + f + '"')]),
        loaded: [...document.fonts].filter((f) => f.status === "loaded").map((f) => f.family + " " + f.style) };
    }, [families[c.ui], families[c.mono]]);
    console.log(c.tag, JSON.stringify(info), "fails:", JSON.stringify(fails));
    for (const [f, ok] of info.check) if (!ok) bad++;
    if (!info.loaded.some((l) => l.includes(families[c.ui]))) bad++;
    await ctx.close();
  }
  // theme broadcast carries the choice
  const ctx = await browser.newContext({ viewport: { width: 900, height: 700 } });
  const page = await ctx.newPage();
  await page.addInitScript(() => { try { localStorage.setItem("judas.font.ui", "lora"); } catch (e) {} });
  await page.goto(base, { waitUntil: "networkidle" });
  const msg = await page.evaluate(() => new Promise((res) => {
    const fr = document.createElement("iframe"); fr.src = JudasTheme.decorate("/robots.txt"); document.body.appendChild(fr);
    JudasTheme.track(fr);
    window.addEventListener("message", () => {});
    const orig = fr.contentWindow;   // same-origin frame: listen inside it
    fr.addEventListener("load", () => {
      fr.contentWindow.addEventListener("message", (e) => res({ origin: e.origin, ft: e.data.fonts_css, ui: e.data.vars["--font-ui"] }));
      JudasTheme.refresh();
    });
    setTimeout(() => res("timeout"), 4000);
  }));
  console.log("broadcast", JSON.stringify(msg));
  await browser.close();
  if (bad) { console.log("FONT CHECK FAILED", bad); process.exit(1); }
  console.log("fonts ok");
})();
