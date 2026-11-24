// Speedometer 3 in headless Chrome, for comparing with the host (tools/sp3.sh): the URL given run to its end, then
// the expression given evaluated and printed. node tools/sp3_chrome.mjs <url> <expression>
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { join } from "node:path";
import { homedir, tmpdir } from "node:os";
const CHROME = process.env.CHROME || homedir() + "/Library/Caches/ms-playwright/chromium-1228/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
const [url, expr] = process.argv.slice(2);
const dir = mkdtempSync(join(tmpdir(), "fv-sp3-"));
const p = spawn(CHROME, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${dir}`, "--no-first-run", "--window-size=800,900", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
const wsUrl = await new Promise((ok, no) => { let b = ""; p.stderr.on("data", d => { b += d; const m = b.match(/ws:\/\/\S+/); if (m) ok(m[0]); }); setTimeout(() => no(new Error(b)), 20000); });
const ws = new WebSocket(wsUrl); await new Promise(r => ws.onopen = r);
let id = 0; const wait = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { const w = wait.get(m.id); wait.delete(m.id); m.error ? w.no(new Error(JSON.stringify(m.error))) : w.ok(m.result); } };
const send = (method, params = {}, sessionId) => new Promise((ok, no) => { const i = ++id; wait.set(i, { ok, no }); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const { targetId } = await send("Target.createTarget", { url: "about:blank" });
const { sessionId: s } = await send("Target.attachToTarget", { targetId, flatten: true });
await send("Page.navigate", { url }, s);
const ev = async e => (await send("Runtime.evaluate", { expression: e, returnByValue: true }, s)).result.value;
for (let i = 0; i < 900; i++) {
  await sleep(1000);
  const st = await ev("globalThis.benchmarkClient && benchmarkClient._state");
  if (st === "DONE" || st === "ERROR") break;
}
console.log(await ev(expr));
ws.close(); p.kill(); rmSync(dir, { recursive: true, force: true });
process.exit(0);
