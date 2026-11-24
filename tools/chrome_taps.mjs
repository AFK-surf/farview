// What a tap costs Chrome, for the same pages and actions as tools/github.sh: headless Chrome at phone size (412x915,
// a phone's user agent), the page loaded and left to settle, then the action traced. For each tap: the time from the
// input to the first frame Chrome committed after it, and the main thread's time in it by kind (script, style, layout,
// paint), as the trace's events say. node tools/chrome_taps.mjs [names...]
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { homedir, tmpdir } from "node:os";
const CHROME = process.env.CHROME || homedir() + "/Library/Caches/ms-playwright/chromium-1228/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
const [W, H] = [412, 915];
const UA = "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36";
// the actions: (name, url, steps), steps as tools/github.sh writes them (click:sel, fill:sel=text, key:k)
const cases = readFileSync("tests/github.txt", "utf8").split("\n").filter(l => l && !l.startsWith("#")).map(l => l.split("|").map(x => x.trim()))
  .filter(c => c[2] !== "look").map(c => ({ name: c[0], url: c[1], steps: c[2].split(" ").filter(s => /^(click|fill|key):/.test(s)) }));
const want = process.argv.slice(2);
const dir = mkdtempSync(join(tmpdir(), "fv-chrome-taps-"));
const p = spawn(CHROME, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${dir}`, "--no-first-run", "--hide-scrollbars", `--window-size=${W},${H}`, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
const wsUrl = await new Promise((ok, no) => { let b = ""; p.stderr.on("data", d => { b += d; const m = b.match(/ws:\/\/\S+/); if (m) ok(m[0]); }); setTimeout(() => no(new Error(b)), 20000); });
const ws = new WebSocket(wsUrl); await new Promise(r => ws.onopen = r);
let id = 0; const wait = new Map(); const ls = [];
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { const w = wait.get(m.id); wait.delete(m.id); m.error ? w.no(new Error(JSON.stringify(m.error))) : w.ok(m.result); } else ls.forEach(l => l(m)); };
const send = (method, params = {}, sessionId) => new Promise((ok, no) => { const i = ++id; wait.set(i, { ok, no }); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
// main-thread events by kind (their own time: what is nested in them of another kind counted there)
const kind = n => {
  if (n === "UpdateLayoutTree" || n === "RecalculateStyles" || n === "ParseAuthorStyleSheet") return "style";
  if (n === "Layout") return "layout";
  if (n === "Paint" || n === "PrePaint" || n === "Layerize" || n === "Commit" || n === "UpdateLayerTree" || n === "PaintImage" || n === "CompositeLayers") return "paint";
  if (n === "FunctionCall" || n === "EvaluateScript" || n === "EventDispatch" || n === "TimerFire" || n === "FireAnimationFrame" || n === "RunMicrotasks" || n === "V8.Execute" || n === "v8.callFunction" || n === "XHRReadyStateChange" || n === "FireIdleCallback") return "script";
  return null;
};
for (const c of cases) {
  if (want.length && !want.includes(c.name)) continue;
  const { targetId } = await send("Target.createTarget", { url: "about:blank" });
  const { sessionId: s } = await send("Target.attachToTarget", { targetId, flatten: true });
  await send("Emulation.setDeviceMetricsOverride", { width: W, height: H, deviceScaleFactor: 2, mobile: true }, s);
  await send("Emulation.setUserAgentOverride", { userAgent: UA }, s);
  await send("Page.enable", {}, s);
  await send("Page.navigate", { url: c.url }, s);
  await sleep(12000);
  const results = [];
  for (const step of c.steps) {
    const [verb, rest] = [step.slice(0, step.indexOf(":")), step.slice(step.indexOf(":") + 1)];
    // (where the element is, before tracing: the trace holds only the action)
    let at = null, sel = rest, text = "";
    if (verb === "fill") { sel = rest.slice(0, rest.indexOf("=")); text = rest.slice(rest.indexOf("=") + 1); }
    if (verb !== "key") {
      const r = await send("Runtime.evaluate", { expression: `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; e.scrollIntoView({block: "center"}); const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`, returnByValue: true }, s);
      at = r.result.value;
      if (!at) { results.push({ step, missing: true }); continue; }
      await sleep(500);
    }
    const events = [];
    const got = new Promise(ok => { const l = m => { if (m.sessionId !== s) return; if (m.method === "Tracing.dataCollected") events.push(...m.params.value); if (m.method === "Tracing.tracingComplete") { ls.splice(ls.indexOf(l), 1); ok(); } }; ls.push(l); });
    await send("Tracing.start", { traceConfig: { includedCategories: ["devtools.timeline", "disabled-by-default-devtools.timeline", "blink.user_timing", "v8.execute"] }, transferMode: "ReportEvents" }, s);
    await sleep(300);
    const t0 = Date.now();
    if (verb === "click") {
      for (const type of ["mousePressed", "mouseReleased"]) await send("Input.dispatchMouseEvent", { type, x: at[0], y: at[1], button: "left", clickCount: 1 }, s);
    } else if (verb === "fill") {
      for (const type of ["mousePressed", "mouseReleased"]) await send("Input.dispatchMouseEvent", { type, x: at[0], y: at[1], button: "left", clickCount: 1 }, s);
      await send("Input.insertText", { text: text.slice(0, 1) }, s);
    } else {
      for (const type of ["keyDown", "keyUp"]) await send("Input.dispatchKeyEvent", { type, key: rest, text: type === "keyDown" ? rest : undefined, code: "Key" + rest.toUpperCase(), windowsVirtualKeyCode: rest.toUpperCase().charCodeAt(0) }, s);
    }
    await sleep(1500);
    await send("Tracing.end", {}, s);
    await got;
    // the renderer's main thread; the input's first event there; the first frame committed after it
    const main = events.find(e => e.name === "thread_name" && e.args && e.args.name === "CrRendererMain");
    const onMain = events.filter(e => main && e.pid === main.pid && e.tid === main.tid && e.ph === "X");
    const dispatches = onMain.filter(e => e.name === "EventDispatch").sort((a, b) => a.ts - b.ts);
    const start = dispatches.length ? dispatches[0].ts : undefined;
    if (start === undefined) { results.push({ step, noinput: true }); continue; }
    // the action's last event (a click after its mousedown and mouseup, the keyup after the keydown), and the first
    // frame committed after it was handled
    const type = e => e.args && e.args.data && e.args.data.type;
    const last = [...dispatches].reverse().find(e => ["click", "keyup", "input", "keypress", "textInput"].includes(type(e))) || dispatches[dispatches.length - 1];
    const handled = last.ts + (last.dur || 0);
    const commit = onMain.filter(e => e.name === "Commit" && e.ts > handled).sort((a, b) => a.ts - b.ts)[0];
    const end = commit ? commit.ts + (commit.dur || 0) : handled;
    // and the main thread's time by kind in the second after the input (what the action set going: renders a
    // framework puts off, network answers it waited for)
    const tail = start + 1000000;
    // each kind's own time in [start, end]: an event's time less what is nested in it of the kinds counted
    const kinds = (from, to) => {
      const inWin = onMain.filter(e => e.ts >= from && e.ts + (e.dur || 0) <= to && e.dur).sort((a, b) => a.ts - b.ts || b.dur - a.dur);
      const by = { script: 0, style: 0, layout: 0, paint: 0 };
      const stack = [];
      for (const e of inWin) {
        while (stack.length && stack[stack.length - 1].ts + stack[stack.length - 1].dur <= e.ts) stack.pop();
        const k = kind(e.name);
        if (k) {
          // the nearest counted parent loses this event's time to it
          const parent = [...stack].reverse().find(x => kind(x.name));
          if (parent) by[kind(parent.name)] -= e.dur;
          by[k] += e.dur;
        }
        stack.push(e);
      }
      return Object.fromEntries(Object.entries(by).map(([k, v]) => [k, Math.round(v / 100) / 10]));
    };
    results.push({ step, frame: (end - start) / 1000, ...kinds(start, end), second: kinds(start, tail) });
    await sleep(1000);
  }
  for (const r of results) console.log(`${c.name}\t${r.step}\t` + (r.missing ? "not found" : r.noinput ? "no input event" : `input to frame ${r.frame.toFixed(1)} ms: script ${r.script}, style ${r.style}, layout ${r.layout}, paint ${r.paint}; in the second after: script ${r.second.script}, style ${r.second.style}, layout ${r.second.layout}, paint ${r.second.paint}`));
  await send("Target.closeTarget", { targetId });
}
ws.close(); p.kill(); rmSync(dir, { recursive: true, force: true });
process.exit(0);
