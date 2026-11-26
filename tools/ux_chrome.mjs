// The same flows as tools/ux_flows.sh (tests/ux_flows/<name>.txt) in Chrome — the engine Arc runs — for a baseline:
// headless Chrome at the client's page size (1264x752), the page loaded and left to settle, then each step done with
// real input events (Input.dispatchMouseEvent) on the first link whose address has the needle and that is on top at
// its point. For each: ms from the pointer down until loading started (what Arc shows as its progress at once), the
// address changed, the new page's first contentful paint, DOMContentLoaded, load; a new tab for Cmd+click and middle
// click. Prints UX-like JSON lines, one a step. node tools/ux_chrome.mjs [flow...]
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { homedir, tmpdir } from "node:os";
const CHROME = process.env.CHROME || homedir() + "/Library/Caches/ms-playwright/chromium-1228/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
const [W, H] = [1264, 752];
const flows = process.argv.slice(2).length ? process.argv.slice(2) : ["wikipedia", "mdn", "hn", "github", "github_search"];
const dir = mkdtempSync(join(tmpdir(), "fv-ux-chrome-"));
const p = spawn(CHROME, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${dir}`, "--no-first-run", "--hide-scrollbars", `--window-size=${W},${H}`, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
const wsUrl = await new Promise((ok, no) => { let b = ""; p.stderr.on("data", d => { b += d; const m = b.match(/ws:\/\/\S+/); if (m) ok(m[0]); }); setTimeout(() => no(new Error(b)), 20000); });
const ws = new WebSocket(wsUrl); await new Promise(r => ws.onopen = r);
let id = 0; const wait = new Map(); const ls = [];
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { const w = wait.get(m.id); wait.delete(m.id); m.error ? w.no(new Error(JSON.stringify(m.error))) : w.ok(m.result); } else ls.forEach(l => l(m)); };
const send = (method, params = {}, sessionId) => new Promise((ok, no) => { const i = ++id; wait.set(i, { ok, no }); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const say = o => console.log("UX " + JSON.stringify(o));
await send("Target.setDiscoverTargets", { discover: true });
let tabs = 0;
ls.push(m => { if (m.method === "Target.targetCreated" && m.params.targetInfo.type === "page") tabs++; });
const pick = `(needles) => { for (const n of needles.split("|")) { for (const a of document.querySelectorAll("a[href]")) {
  if (!a.href.includes(n) || !/^https?:/.test(a.href) || a.href.split("#")[0] === location.href.split("#")[0]) continue;
  const r = a.getClientRects()[0]; if (!r || r.width < 4 || r.height < 4) continue;
  const x = r.left + Math.min(r.width / 2, 24), y = r.top + Math.min(r.height / 2, 9);
  if (x < 0 || y < 8 || r.right > innerWidth || r.bottom > innerHeight - 8) continue;
  const top = document.elementFromPoint(x, y); if (!top || !(top === a || a.contains(top) || top.closest("a") === a)) continue;
  return { x, y, href: a.href }; } } return null; }`;
for (const flow of flows) {
  const lines = readFileSync(`tests/ux_flows/${flow}.txt`, "utf8").split("\n");
  const start = lines.find(l => l.startsWith("# start: ")).slice(9).trim();
  const { targetId } = await send("Target.createTarget", { url: "about:blank" });
  const { sessionId: s } = await send("Target.attachToTarget", { targetId, flatten: true });
  await send("Emulation.setDeviceMetricsOverride", { width: W, height: H, deviceScaleFactor: 2, mobile: false }, s);
  await send("Page.enable", {}, s); await send("Runtime.enable", {}, s);
  await send("Page.setLifecycleEventsEnabled", { enabled: true }, s);
  const evs = [];
  ls.push(m => { if (m.sessionId === s) evs.push({ t: performance.now(), m }); });
  const ev = async (expr) => (await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true }, s)).result.value;
  const t0s = performance.now();
  await send("Page.navigate", { url: start }, s);
  for (let i = 0; i < 200 && !evs.some(e => e.m.method === "Page.loadEventFired"); i++) await sleep(50);
  say({ flow, variant: "chrome", ev: "ready", t: Math.round(performance.now() - t0s), url: start });
  await sleep(1500);
  for (const raw of lines) {
    const l = raw.trim(); if (!l || l.startsWith("#")) continue;
    const [verb, arg0 = "", label0] = l.split(/\s+/);
    const nav = verb === "back" || verb === "forward";
    const arg = nav ? "" : arg0, label = nav ? (arg0 || verb) : (label0 || verb + " " + arg0);
    if (!/^(click|cmdclick|middleclick|longclick|clickscroll|back|forward)$/.test(verb)) continue;
    let target = null;
    if (!nav) {
      target = await ev(`(${pick})(${JSON.stringify(arg)})`);
      for (let k = 0; !target && k < 40; k++) { await send("Input.dispatchMouseEvent", { type: "mouseWheel", x: W / 2, y: H / 2, deltaX: 0, deltaY: 300 }, s); await sleep(30); target = await ev(`(${pick})(${JSON.stringify(arg)})`); }
      if (!target) { say({ flow, variant: "chrome", ev: "error", label, why: "no link " + arg }); continue; }
      await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: target.x, y: target.y }, s); await sleep(150);
    }
    const url0 = await ev("location.href"); const tabs0 = tabs; evs.length = 0;
    const t0 = performance.now();
    if (target) {
      const button = verb === "middleclick" ? "middle" : "left", modifiers = verb === "cmdclick" ? 4 : 0;
      await send("Input.dispatchMouseEvent", { type: "mousePressed", x: target.x, y: target.y, button, clickCount: 1, modifiers }, s);
      await sleep(verb === "longclick" ? 650 : 80);
      await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: target.x, y: target.y, button, clickCount: 1, modifiers }, s);
      if (verb === "clickscroll") for (let k = 0; k < 5; k++) { await sleep(50); await send("Input.dispatchMouseEvent", { type: "mouseWheel", x: W / 2, y: H / 2, deltaX: 0, deltaY: 40 }, s); }
    } else {
      const h = await send("Page.getNavigationHistory", {}, s); const k = h.currentIndex + (verb === "back" ? -1 : 1);
      if (k < 0 || k >= h.entries.length) { say({ flow, variant: "chrome", ev: "error", label, why: "no history there" }); continue; }
      await send("Page.navigateToHistoryEntry", { entryId: h.entries[k].id }, s);
    }
    const wantsTab = verb === "cmdclick" || verb === "middleclick";
    let done = -1;
    for (let i = 0; i < 300; i++) {
      await sleep(50);
      if (wantsTab ? tabs > tabs0 : evs.some(e => e.m.method === "Page.loadEventFired" || e.m.method === "Page.navigatedWithinDocument")) { done = performance.now() - t0; break; }
    }
    await sleep(wantsTab ? 300 : 500);
    const first = name => { const e = evs.find(x => x.m.method === name || (x.m.method === "Page.lifecycleEvent" && x.m.params.name === name)); return e ? Math.round(e.t - t0) : -1; };
    const navEv = evs.find(e => (e.m.method === "Page.frameNavigated" && !e.m.params.frame.parentId) || e.m.method === "Page.navigatedWithinDocument");
    say({ flow, variant: "chrome", ev: "timeline", label, loading: first("Page.frameStartedLoading"), url: navEv ? Math.round(navEv.t - t0) : -1,
      paint: first("firstContentfulPaint"), dcl: first("DOMContentLoaded"), loaded: first("Page.loadEventFired"), done: Math.round(done),
      new_tab: wantsTab ? (tabs > tabs0 ? Math.round(done) : -1) : -1, from: url0, to: await ev("location.href"), href: target && target.href });
  }
  await send("Target.closeTarget", { targetId });
}
ws.close(); p.kill(); await sleep(300); rmSync(dir, { recursive: true, force: true });
