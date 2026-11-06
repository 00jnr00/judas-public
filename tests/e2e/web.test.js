// Browser end-to-end tests for the Judas web UI (desktop app and phone /m/).
// Run through run.sh: it starts the fake model server and a headless judas, then sets BASE.
"use strict";
const path = require("path");
const fs = require("fs");
const PW = process.env.PLAYWRIGHT_DIR || path.join(process.env.HOME, ".local/share/mise/installs/npm-playwright/latest/node_modules/playwright");
const { chromium } = require(PW);

const BASE = process.env.BASE || "http://127.0.0.1:18179";
const results = [];
let browser;

function findChromium() {
  for (const p of [process.env.CHROMIUM, "/usr/bin/chromium", "/usr/bin/chromium-browser"]) if (p && fs.existsSync(p)) return p;
  return undefined;
}
async function api(method, url, body, headers) {
  const r = await fetch(BASE + url, { method, headers: Object.assign(body && typeof body !== "string" && !(body instanceof Buffer) ? { "Content-Type": "application/json" } : {}, headers || {}),
    body: body === undefined ? undefined : (typeof body === "string" || body instanceof Buffer ? body : JSON.stringify(body)) });
  const t = await r.text();
  let j = null; try { j = JSON.parse(t); } catch (e) { /* not json */ }
  return { status: r.status, json: j, text: t };
}
function check(c, msg) { if (!c) throw new Error(msg); }

// A fresh page that records console errors and failed local requests.
async function open(kind, url) {
  const ctx = await browser.newContext(kind === "phone"
    ? { viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, deviceScaleFactor: 2 }
    : { viewport: { width: 1360, height: 860 } });
  const page = await ctx.newPage();
  page.errs = [];
  page.on("console", (m) => { if (m.type() === "error") page.errs.push(m.text()); });
  page.on("pageerror", (e) => page.errs.push("pageerror: " + e.message));
  // the remote image placeholder must never fetch by itself; example.invalid would show up as a failed request
  page.remoteHits = [];
  page.on("request", (r) => { if (/example\.invalid/.test(r.url())) page.remoteHits.push(r.url()); });
  await page.goto(BASE + url, { waitUntil: "load" });
  // the desktop app reopens the last session; start each test from an empty one
  if (kind === "desktop" && url === "/") { await page.waitForSelector("#new-session"); await page.click("#new-session"); await page.waitForTimeout(300); }
  return page;
}
function noErrors(page) {
  // a 404 for an optional asset (favicon, fonts probe) is not an app bug; JS errors are
  const bad = page.errs.filter((e) => !/favicon|Failed to load resource.*404/.test(e));
  check(!bad.length, "console errors: " + bad.join(" | "));
}
async function send(page, text) {
  await page.fill("#input", text);
  await page.click("#send");
}
const ASST = { desktop: ".msg.assistant", phone: ".msg.asst" };
function test(name, fn) { results.push({ name, fn }); }

// ---------------------------------------------------------------- tests
for (const kind of ["desktop", "phone"]) {
  const url = kind === "phone" ? "/m/" : "/";

  test(kind + ": page loads with no console errors", async () => {
    const page = await open(kind, url);
    await page.waitForSelector("#input");
    await page.waitForTimeout(800);
    noErrors(page);
    await page.context().close();
  });

  test(kind + ": send a message and see the streamed reply", async () => {
    const page = await open(kind, url);
    await page.waitForSelector("#input");
    await send(page, "e2e-hello please");
    await page.waitForFunction((s) => [...document.querySelectorAll(s)].some((n) => /Hello from the fake model/.test(n.textContent)), ASST[kind], { timeout: 15000 });
    noErrors(page);
    await page.context().close();
  });

  test(kind + ": a reply with thinking renders live and again from history after a reload", async () => {
    const page = await open(kind, url);
    await page.waitForSelector("#input");
    await send(page, "e2e-think please");
    const seen = (s) => [...document.querySelectorAll(s)].some((n) => /Answer after thinking/.test(n.textContent));
    await page.waitForFunction(seen, ASST[kind], { timeout: 15000 });
    await page.waitForTimeout(800);
    await page.reload();
    await page.waitForFunction(seen, ASST[kind], { timeout: 15000 });
    noErrors(page);
    await page.context().close();
  });

  test(kind + ": rich blocks render (swatch, cards, task list, remote image gate)", async () => {
    const page = await open(kind, url);
    await page.waitForSelector("#input");
    await send(page, "e2e-rich show me");
    await page.waitForSelector(".rc-swatches .rc-chip", { timeout: 15000 });
    check((await page.locator(".rc-swatches .rc-chip").count()) === 3, "expected 3 swatch chips");
    const bg = await page.locator(".rc-chip").first().evaluate((n) => getComputedStyle(n).backgroundColor);
    check(bg === "rgb(255, 136, 0)", "first chip color is " + bg);
    check((await page.locator(".rc-cards > *").count()) === 2, "expected 2 cards");
    check(/Alpha/.test(await page.locator(".rc-cards").innerText()), "card title missing");
    await page.waitForSelector(".rc-task");
    check((await page.locator(".rc-task").count()) === 3, "expected 3 tasks");
    check((await page.locator(".rc-task.done").count()) === 1, "expected 1 done task");
    const cnt = await page.locator(".rc-count").first().innerText();
    check(cnt.trim() === "1/3", "task counter is '" + cnt + "'");
    // remote image: click to load, never fetched on its own
    const btn = page.locator("button.rc-load");  // the second card's thumbnail
    check((await btn.count()) === 1, "expected one click-to-load placeholder");
    check(/example\.invalid/.test(await btn.innerText()), "placeholder should name the host");
    check(page.remoteHits.length === 0, "remote image was fetched before the click: " + page.remoteHits);
    await btn.click();
    await page.waitForTimeout(500);
    check(page.remoteHits.length >= 1, "click did not start the remote image load");
    await page.context().close();
  });

  test(kind + ": inline /api/attachments image loads", async () => {
    // 1x1 PNG
    const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==", "base64");
    const up = await api("POST", "/api/attachments?name=dot.png", png, { "Content-Type": "image/png" });
    check(up.status === 200 && up.json && up.json.id, "upload failed: " + up.status + " " + up.text);
    const page = await open(kind, url);
    await page.waitForSelector("#input");
    await send(page, "e2e-att " + up.json.id);
    await page.waitForSelector('img[src^="/api/attachments/"]', { timeout: 15000 });
    await page.waitForFunction(() => { const i = document.querySelector('img[src^="/api/attachments/"]'); return i && i.complete && i.naturalWidth > 0; }, null, { timeout: 8000 });
    await page.context().close();
  });

  test(kind + ": suggested prompts rotate and a click fills the composer", async () => {
    const page = await open(kind, url);
    await page.waitForSelector(".sg-chip", { timeout: 10000 });
    const first = await page.locator(".sg-chip b").allInnerTexts();
    check(first.length >= 3, "expected suggestion chips");
    await page.mouse.move(0, 0);
    await page.waitForTimeout(10500);
    const t1 = (await page.locator(".sg-chip b").allInnerTexts()).join("|");
    check(t1 !== first.join("|"), "suggestions did not rotate on their own within 10 s");
    let changed = false;
    for (let i = 0; i < 6 && !changed; i++) {
      await page.locator(".sg-shuffle").first().click();
      await page.waitForTimeout(700);
      changed = (await page.locator(".sg-chip b").allInnerTexts()).join("|") !== t1;
    }
    check(changed, "suggestions never changed after shuffling");
    const chip = page.locator(".sg-chip").first();
    const title = await chip.locator("b").innerText();
    await chip.click();
    await page.waitForTimeout(300);
    const v = await page.inputValue("#input");
    check(v.length > 10, "composer empty after clicking '" + title + "'");
    check(await page.locator(ASST[kind]).count() === 0, "clicking a suggestion must not send");
    await page.context().close();
  });
}

test("phone: /m/?q=hello prefills the composer without sending", async () => {
  const page = await open("phone", "/m/?q=hello%20there");
  await page.waitForSelector("#input");
  await page.waitForFunction(() => document.querySelector("#input").value === "hello there", null, { timeout: 5000 });
  await page.waitForTimeout(500);
  check((await page.locator(".msg.user").count()) === 0, "q must not auto-send");
  noErrors(page);
  await page.context().close();
});

test("phone: font picker changes the font and persists across reload", async () => {
  const page = await open("phone", "/m/");
  await page.waitForSelector("#menu-btn");
  await page.click("#menu-btn");
  const sel = page.locator('select[aria-label="UI font"]');
  await sel.waitFor({ timeout: 8000 });
  const before = await page.evaluate(() => getComputedStyle(document.body).fontFamily);
  const opts = await sel.locator("option").evaluateAll((o) => o.map((x) => x.value));
  const pick = opts.find((v) => v && v !== "system");
  check(pick, "no selectable font in the picker");
  await sel.selectOption(pick);
  await page.waitForTimeout(500);
  const after = await page.evaluate(() => getComputedStyle(document.body).fontFamily);
  check(after !== before, "font-family did not change (" + before + ")");
  const stored = await page.evaluate(() => localStorage.getItem("judas.font.ui"));
  check(stored === pick, "storage holds '" + stored + "', expected '" + pick + "'");
  await page.reload({ waitUntil: "load" });
  await page.waitForTimeout(800);
  const again = await page.evaluate(() => getComputedStyle(document.body).fontFamily);
  check(again === after, "font did not persist: " + again + " vs " + after);
  noErrors(page);
  await page.context().close();
});

test("daily question endpoint returns JSON", async () => {
  const r = await api("GET", "/api/daily-question");
  check(r.status === 200 && r.json, "status " + r.status);
  check(typeof r.json.question === "string" && r.json.question.length > 5, "no question");
  check(/^\d{4}-\d\d-\d\d$/.test(r.json.date), "bad date " + r.json.date);
  const a = await api("GET", "/api/daily-question?date=2026-01-02");
  const b = await api("GET", "/api/daily-question?date=2026-01-02");
  check(a.json.question === b.json.question && a.json.date === "2026-01-02", "date param not stable");
  check((await api("GET", "/api/daily-question?date=nope")).status === 400, "bad date should be 400");
});

test("phone: an API-started session appears in the session list", async () => {
  const title = "api session " + Date.now();
  const s = await api("POST", "/api/sessions", { title, mode: "chat" });
  check(s.status === 200 && s.json && s.json.id, "create failed " + s.text);
  const m = await api("POST", "/api/sessions/" + s.json.id + "/messages", { content: "e2e-hello from api" });
  check(m.status < 300, "message failed " + m.status + " " + m.text);
  const page = await open("phone", "/m/");
  await page.waitForSelector("#drawer-btn");
  await page.waitForTimeout(800);
  await page.click("#drawer-btn");
  await page.waitForFunction((t) => document.querySelector("#d-list") && document.querySelector("#d-list").textContent.includes(t), title, { timeout: 8000 });
  noErrors(page);
  await page.context().close();
});

test("phone: GFM table renders in a scroller and the chat home never overlaps", async () => {
  const page = await open("phone", "/m/");
  await page.setViewportSize({ width: 412, height: 915 });
  await page.waitForSelector("#home-sg .sg-chip");
  // the hero avatar sits below the tab pills
  const tabs = await page.locator("#bar").boundingBox(), coin = await page.locator(".empty .coin").boundingBox();
  check(coin.y >= tabs.y + tabs.height - 1, "avatar overlaps the top bar: " + JSON.stringify([tabs, coin]));
  if (process.env.SHOT_DIR) await page.screenshot({ path: process.env.SHOT_DIR + "/phone-home.png" });
  // focusing the composer hides chips and shuffle
  await page.focus("#input");
  await page.waitForTimeout(200);
  check(!(await page.locator("#home-sg").isVisible()), "chips still visible while typing");
  await send(page, "e2e-table please");
  await page.waitForSelector(".msg.asst .md-table table", { timeout: 15000 });
  const rows = await page.locator(".msg.asst .md-table tbody tr").count();
  check(rows === 3, "expected 3 body rows, got " + rows);
  check((await page.locator(".msg.asst .md-table th").count()) === 4, "expected 4 header cells");
  check((await page.locator(".msg.asst .md-table td code").count()) === 1, "inline code in a cell");
  const al = await page.locator(".msg.asst .md-table th").nth(1).evaluate((e) => getComputedStyle(e).textAlign);
  check(al === "center", "alignment not honored: " + al);
  check(!(await page.locator(".msg.asst").innerText()).includes("|---"), "raw separator row leaked");
  const over = await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth + 1);
  check(!over, "page scrolls horizontally");
  if (process.env.SHOT_DIR) await page.screenshot({ path: process.env.SHOT_DIR + "/phone-table.png" });
  noErrors(page);
  await page.context().close();
});

test("phone: drawer search filters titles and finds message text", async () => {
  const token = "zebrafish" + Date.now();
  const s = await api("POST", "/api/sessions", { title: "Quiet title " + Date.now(), mode: "chat" });
  await api("POST", "/api/sessions/" + s.json.id + "/messages", { content: "e2e-hello " + token + " in the body" });
  const api1 = await api("GET", "/api/sessions/search?q=" + encodeURIComponent(token));
  check(api1.status === 200 && api1.json.hits.length === 1, "api search " + api1.text);
  const page = await open("phone", "/m/");
  await page.waitForSelector("#drawer-btn");
  await page.click("#drawer-btn");
  await page.fill("#d-q", token);
  await page.waitForSelector("#d-list .d-hit", { timeout: 8000 });
  check((await page.locator("#d-list .d-hit mark").count()) >= 1, "snippet not highlighted");
  if (process.env.SHOT_DIR) await page.screenshot({ path: process.env.SHOT_DIR + "/phone-search.png" });
  await page.fill("#d-q", "quiet title");
  await page.waitForFunction(() => document.querySelector("#d-list").textContent.includes("Quiet title"));
  await page.click("#d-clear");
  check((await page.inputValue("#d-q")) === "", "clear did not empty the field");
  await page.fill("#d-q", '"( NEAR *');   // punctuation must not break anything
  await page.waitForTimeout(600);
  noErrors(page);
  await page.context().close();
});

test("desktop: Statistics daily grid renders cells", async () => {
  const page = await open("desktop", "/");
  await page.waitForSelector('[data-screen="stats"]', { state: "attached" });
  await page.evaluate(() => document.querySelector('[data-screen="stats"]').click());
  await page.waitForSelector("#stats .heat-grid .cell", { timeout: 10000 });
  const n = await page.locator("#stats .heat-grid .cell").count();
  check(n >= 300, "expected a year of cells, got " + n);
  check((await page.locator("#stats .heat-grid .cell.future").count()) < n, "every cell is future");
  noErrors(page);
  await page.context().close();
});

test("phone: a picked image shows a preview in the composer and a thumbnail in the sent bubble", async () => {
  const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==", "base64");
  const page = await open("phone", "/m/");
  await page.waitForSelector("#input");
  await page.setInputFiles("#file", { name: "dot.png", mimeType: "image/png", buffer: png });
  await page.waitForSelector("#atts .pill.has-pic img", { timeout: 8000 });
  await page.waitForFunction(() => !/uploading/.test(document.querySelector("#atts").textContent), null, { timeout: 8000 });
  await send(page, "e2e look at this");
  await page.waitForSelector('.msg.user .att-pic img[src^="/api/attachments/"]', { timeout: 10000 });
  await page.click(".msg.user .att-pic");
  await page.waitForSelector(".rc-lightbox img", { timeout: 4000 });
  noErrors(page);
  await page.context().close();
});

test("phone: create, run and delete an automated task", async () => {
  const name = "e2e task " + Date.now();
  const page = await open("phone", "/m/");
  await page.waitForSelector("#menu-btn");
  await page.click("#menu-btn");
  await page.getByText("Automated tasks").click();
  await page.click(".t-new");
  await page.fill('#sheet input.s-field[type="text"]', name);
  await page.fill("#sheet textarea", "e2e-hello from a task");
  await page.fill('#sheet input[type="number"]', "2");
  await page.selectOption("#sheet select", "3600");
  const d = new Date(); d.setDate(d.getDate() + 1); d.setHours(7, 30, 0, 0);
  const pad = (n) => String(n).padStart(2, "0");
  await page.fill("#sheet input.t-start", d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + "T07:30");
  await page.getByText("Save task").click();
  await page.waitForFunction((n) => [...document.querySelectorAll(".t-card")].some((c) => c.textContent.includes(n)), name, { timeout: 8000 });
  const r = await api("GET", "/api/tasks");
  const t = (r.json.tasks || []).find((x) => x.title === name);
  check(t && t.kind === "interval" && t.interval_s === 7200 && t.enabled && t.next_ms === d.getTime(), "task not saved right: " + JSON.stringify(t));
  const card = page.locator(".t-card", { hasText: name });
  // edit it: new prompt, every 3 hours; the schedule form opens prefilled
  await card.getByText("Edit", { exact: true }).click();
  await page.waitForSelector("#sheet h2:text('Edit task')");
  check((await page.inputValue('#sheet input[type="number"]')) === "2", "edit form not prefilled");
  await page.fill("#sheet textarea", "e2e-hello edited");
  await page.fill('#sheet input[type="number"]', "3");
  await page.getByText("Save changes").click();
  await page.waitForFunction(() => [...document.querySelectorAll(".t-card")].some((c) => c.textContent.includes("e2e-hello edited")), null, { timeout: 8000 });
  const t2 = ((await api("GET", "/api/tasks")).json.tasks || []).find((x) => x.title === name);
  check(t2 && t2.prompt === "e2e-hello edited" && t2.interval_s === 10800 && t2.id === t.id && t2.enabled, "task not edited right: " + JSON.stringify(t2));
  await card.locator(".t-del").click();
  await card.locator(".t-del").click();
  await page.waitForFunction((n) => ![...document.querySelectorAll(".t-card")].some((c) => c.textContent.includes(n)), name, { timeout: 8000 });
  check(!((await api("GET", "/api/tasks")).json.tasks || []).some((x) => x.title === name), "task not deleted");
  noErrors(page);
  await page.context().close();
});

test("desktop: a new project made in the header holds the session, its folder and instructions", async () => {
  const name = "E2E Proj " + Date.now();
  const page = await open("desktop", "/");
  await page.click('#rail button[data-screen="code"]');
  await page.selectOption("#proj-select", "__new");
  await page.fill("#wh-name", name);
  await page.fill("#wh-ins", "e2e instruction text");
  await page.click("#proj-new button[type=submit]");
  await page.waitForFunction(() => /^p-/.test(document.querySelector("#proj-select").value), null, { timeout: 8000 });
  check(!(await page.locator("#cwd").isVisible()), "the folder input should hide while a project is picked");
  await send(page, "e2e-hello project");
  await page.waitForFunction((s) => [...document.querySelectorAll(s)].some((n) => /Hello from the fake model/.test(n.textContent)), ASST.desktop, { timeout: 15000 });
  const pr = ((await api("GET", "/api/projects")).json.projects || []).find((x) => x.name === name);
  check(pr && pr.kind === "made" && pr.instructions === "e2e instruction text" && fs.existsSync(pr.folder), "project not made right: " + JSON.stringify(pr));
  const ss = (await api("GET", "/api/sessions?project=" + pr.id)).json.sessions || [];
  check(ss.length === 1 && ss[0].cwd === pr.folder && ss[0].project_name === name, "session not attached: " + JSON.stringify(ss));
  // the Projects screen lists it with the session; deleting keeps the folder
  await page.click('#rail button[data-screen="projects"]');
  await page.waitForFunction((n) => [...document.querySelectorAll(".proj-name")].some((c) => c.textContent === n), name, { timeout: 8000 });
  const del = await api("DELETE", "/api/projects/" + pr.id);
  check(del.status === 200 && fs.existsSync(pr.folder), "delete removed the folder or failed: " + del.text);
  check(((await api("GET", "/api/sessions/" + ss[0].id)).json.session || {}).project_id === "", "session still points at the deleted project");
  noErrors(page);
  await page.context().close();
});

test("phone: new project from the project sheet, then a session in it", async () => {
  const name = "Phone Proj " + Date.now();
  const page = await open("phone", "/m/#code");
  await page.waitForSelector("#folder");
  await page.click("#folder");
  await page.click("#sheet .s-item:has-text('New project')");
  await page.fill("#sheet input[aria-label='Project name']", name);
  await page.click("#sheet .primary");
  await page.waitForFunction((n) => document.querySelector("#folder").textContent.includes(n), name, { timeout: 8000 });
  await send(page, "e2e-hello phone project");
  await page.waitForFunction((s) => [...document.querySelectorAll(s)].some((n) => /Hello from the fake model/.test(n.textContent)), ASST.phone, { timeout: 15000 });
  const pr = ((await api("GET", "/api/projects")).json.projects || []).find((x) => x.name === name);
  check(pr && fs.existsSync(pr.folder), "project missing");
  const ss = (await api("GET", "/api/sessions?project=" + pr.id)).json.sessions || [];
  check(ss.length === 1 && ss[0].cwd === pr.folder, "session not attached: " + JSON.stringify(ss));
  await api("DELETE", "/api/projects/" + pr.id);
  noErrors(page);
  await page.context().close();
});

test("desktop: Share my location turns on, reports a fix, names the nearest city, turns off", async () => {
  const ctx = await browser.newContext({ viewport: { width: 1360, height: 860 }, permissions: ["geolocation"], geolocation: { latitude: 40.0150, longitude: -105.2705, accuracy: 30 } });
  const page = await ctx.newPage();
  page.errs = [];
  page.on("pageerror", (e) => page.errs.push("pageerror: " + e.message));
  await page.goto(BASE + "/#settings", { waitUntil: "load" });
  const sw = page.locator("#location-settings .switch");
  await sw.waitFor();
  check((await sw.getAttribute("aria-checked")) === "false", "sharing should start off");
  check(!(await api("GET", "/api/location")).json.has_fix, "no fix before the toggle");
  await sw.click();
  await page.waitForFunction(() => document.querySelector("#location-settings .switch").getAttribute("aria-checked") === "true", null, { timeout: 8000 });
  const on = (await api("GET", "/api/location")).json;
  check(on.sharing && on.has_fix && on.device === "Desktop browser" && /Boulder, Colorado/.test(on.place || "") && on.lat === undefined, "status after turning on: " + JSON.stringify(on));
  await sw.click();
  await page.waitForFunction(() => document.querySelector("#location-settings .switch").getAttribute("aria-checked") === "false", null, { timeout: 8000 });
  const off = (await api("GET", "/api/location")).json;
  check(!off.sharing && !off.has_fix, "fix survived turning sharing off: " + JSON.stringify(off));
  check(!page.errs.length, "page errors: " + page.errs.join(" | "));
  await ctx.close();
});

// Cowork sub-tabs: each one opens its own layout (docs/COWORK-MODES.md). The new mode endpoints may be missing from the
// server under test (backend lands on its own branch), so these tests only need the layout and a quiet console.
const COWORK = {
  engineering: { cls: ".cw-eng", parts: [".cw-view", ".cw-cadstrip", ".cw-calcs .cw-table, .cw-calcs .cw-empty"], seg: ["Chat", "Model", "Calcs"] },
  design: { cls: ".cw-des", parts: [".cw-canvas", ".cw-film", ".cw-refs"], seg: ["Canvas", "Versions", "Chat"] },
  research: { cls: ".cw-res", parts: [".cw-sources", ".cw-report .cw-md"], seg: ["Report", "Sources", "Chat"] },
  write: { cls: ".cw-wr", parts: [".cw-doc .cw-text", ".cw-margin .cw-notes"], seg: ["Document", "Notes", "Chat"] },
};
for (const [m, c] of Object.entries(COWORK)) {
  test("desktop: Cowork > " + m + " opens its layout, the project picker and the composer", async () => {
    const page = await open("desktop", "/#" + m);
    await page.waitForSelector("#cw-tabs .cw-tab");
    const tabs = await page.$$eval("#cw-tabs .cw-tab", (n) => n.map((x) => x.textContent));
    check(tabs.join(",") === "General,Engineering,Design,Research,Write", "pills: " + tabs);
    check((await page.$eval("#cw-tabs .cw-tab[aria-selected=true]", (n) => n.dataset.sub)) === m, "wrong pill selected");
    check((await page.$('#rail button[data-screen="engineering"]')) === null, "Engineering still has a rail entry");
    check(await page.$eval('#rail button[data-screen="cowork"]', (n) => n.classList.contains("active")), "Cowork rail entry should be active");
    await page.waitForSelector("#cw-stage" + c.cls.replace(/^\./, ".") , { state: "visible" });
    for (const sel of c.parts) check(await page.locator("#cw-stage " + sel).first().count(), "missing part " + sel);
    check(await page.locator("#proj-select").isVisible() && await page.locator("#input").isVisible(), "project picker or composer hidden");
    check(!(await page.locator("#panel-toggle").isVisible()), "agent panel toggle should be gone in a staged tab");
    // start a session in a project; an older server may refuse the mode, the layout must survive either way
    const name = "E2E CW " + m + " " + Date.now();
    const pr = await api("POST", "/api/projects", { name });
    check(pr.status === 200 || pr.status === 201, "project create: " + pr.text);
    await page.reload(); await page.waitForSelector("#cw-stage" + c.cls, { state: "visible" });
    await page.selectOption("#proj-select", pr.json.id || (pr.json.project || {}).id);
    await send(page, "e2e-hello " + m);
    await page.waitForTimeout(1500);
    check(await page.locator("#cw-stage" + c.cls).isVisible(), "layout vanished after sending");
    check(!page.errs.some((e) => /^pageerror/.test(e)), "page errors: " + page.errs.join(" | "));
    await api("DELETE", "/api/projects/" + (pr.json.id || (pr.json.project || {}).id));
    await page.context().close();
  });
  test("phone: Cowork > " + m + " has the sub-tab pills and a pane switch", async () => {
    const page = await open("phone", "/m/#" + m);
    await page.waitForSelector("#cw-sub .cw-subpills");
    const seg = await page.$$eval("#cw-sub .cw-seg button", (n) => n.map((x) => x.textContent));
    check(seg.join(",") === c.seg.join(","), "pane switch: " + seg);
    const other = c.seg.find((x) => x !== "Chat" && x !== c.seg[0]) || c.seg[1];
    await page.click("#cw-sub .cw-seg button:has-text('" + other + "')");
    await page.waitForSelector("#v-cw #cw-stage" + c.cls, { state: "visible" });
    check(await page.locator("#dock").isHidden(), "composer should be hidden outside the chat pane");
    await page.click("#cw-sub .cw-seg button:has-text('Chat')");
    await page.waitForFunction(() => document.querySelector("#v-cw").hidden, null, { timeout: 4000 });
    check(await page.locator("#input").isVisible(), "composer back in the chat pane");
    check(!page.errs.some((e) => /^pageerror/.test(e)), "page errors: " + page.errs.join(" | "));
    await page.context().close();
  });
}

// ---------------------------------------------------------------- runner
(async () => {
  browser = await chromium.launch({ executablePath: findChromium(), args: ["--no-sandbox"] });
  let fail = 0;
  const only = process.env.ONLY;
  for (const t of results) {
    if (only && !t.name.includes(only)) continue;
    try { await t.fn(); console.log("PASS " + t.name); }
    catch (e) { fail++; console.log("FAIL " + t.name + "\n     " + String(e.message).split("\n")[0]); }
  }
  await browser.close();
  console.log(fail ? fail + " failed" : "all passed");
  process.exit(fail ? 1 : 0);
})();
