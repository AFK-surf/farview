// Takes a site as Chrome laid it out: tests/sites/<name>/{page.html, style.css, rects.json, shot.png}.
// The page is the DOM after load without its scripts; rects.json is every element's box in tree order, in CSS pixels
// from the top of the document.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { homedir, tmpdir } from "node:os";
const CHROME = process.env.CHROME || homedir() + "/Library/Caches/ms-playwright/chromium-1228/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
// FV_SNAP_PHONE=1: as Chrome on a phone (412x915, a phone's user agent and viewport rules), the size written to
// viewport.txt for the layout tool
const PHONE = !!process.env.FV_SNAP_PHONE;
const [W, H] = PHONE ? [412, 915] : [1280, 800];
const dir = mkdtempSync(join(tmpdir(), "fv-chrome-"));
const p = spawn(CHROME, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${dir}`, "--no-first-run", "--hide-scrollbars", `--window-size=${W},${H}`, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
const wsUrl = await new Promise((ok, no) => { let b = ""; p.stderr.on("data", d => { b += d; const m = b.match(/ws:\/\/\S+/); if (m) ok(m[0]); }); setTimeout(() => no(new Error(b)), 20000); });
const ws = new WebSocket(wsUrl); await new Promise(r => ws.onopen = r);
let id = 0; const wait = new Map(); const ls = [];
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { const w = wait.get(m.id); wait.delete(m.id); m.error ? w.no(new Error(JSON.stringify(m.error))) : w.ok(m.result); } else ls.forEach(l => l(m)); };
const send = (method, params = {}, sessionId) => new Promise((ok, no) => { const i = ++id; wait.set(i, { ok, no }); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const collect = String.raw`(async () => {
  const sheets = [];
  // a sheet a <link> or a script-filled <style> holds goes in the page where its element is, as a <style>: the order of
  // the rules stays the document's (style.css comes before every <style> for the layout tool)
  const placed = [];
  // url()s made whole against their own sheet, so the snapshot's fonts can be fetched
  const abs = (t, base) => t.replace(/url\(\s*(["']?)([^"')]*)\1\s*\)/g, (m, q, u) => { if (u.startsWith("data:") || (base && base.startsWith("file://"))) return m; try { const resolved = new URL(u, base).href; return resolved.startsWith("file://") ? m : 'url("' + resolved + '")'; } catch (e) { return m; } });
  for (const s of document.styleSheets) {
    // a <style> whose rules a script inserted (CSS-in-JS) has them only in its sheet: they go into style.css, the
    // element is emptied
    if (s.ownerNode && s.ownerNode.tagName === 'STYLE') { if (!s.ownerNode.textContent.trim() && s.cssRules.length) { placed.push([s.ownerNode, [...s.cssRules].map(r => r.cssText).join("\n")]); } continue; }
    const keep = t => s.ownerNode && s.ownerNode.tagName === 'LINK' ? placed.push([s.ownerNode, t]) : sheets.push(t);
    const text = sheet => [...sheet.cssRules].map(r => r.styleSheet ? text(r.styleSheet) : abs(r.cssText, sheet.href || location.href)).join("\n");
    const wrap = t => s.media && s.media.mediaText && s.media.mediaText !== 'all' ? "@media " + s.media.mediaText + " {\n" + t + "\n}" : t;
    // a sheet's own text where it has one: CSSOM's cssText drops shorthands holding var() (it writes "border-top-color: ;")
    if (s.href && !s.ownerRule) { try { const r = await fetch(s.href); if (r.ok) { keep(wrap(abs(await r.text(), s.href))); continue; } } catch (e) {} }
    try { keep(wrap(text(s))); }
    catch (e) { try { const r = await fetch(s.href); keep(abs(await r.text(), s.href)); } catch (e2) { keep("/* failed " + s.href + " */"); } }
  }
  // what scripts set as state, kept as markup so :checked and the like see it
  for (const i of document.querySelectorAll('input')) { if (i.checked) i.setAttribute('checked', ''); else i.removeAttribute('checked'); }
  for (const i of document.images) { i.setAttribute('data-fv-w', i.naturalWidth); i.setAttribute('data-fv-h', i.naturalHeight); }
  // boxes as laid out, before scripts scrolled any of them (a sidebar to its current entry)
  for (const e of document.querySelectorAll('*')) if (e.scrollTop || e.scrollLeft) e.scrollTo({ top: 0, left: 0, behavior: 'instant' });
  const rects = [];
  const sx = scrollX, sy = scrollY;
  const walk = e => {
    const r = e.getBoundingClientRect(), c = getComputedStyle(e);
    rects.push([e.tagName.toLowerCase(), +(r.x + sx).toFixed(2), +(r.y + sy).toFixed(2), +r.width.toFixed(2), +r.height.toFixed(2), c.display, c.fontFamily.slice(0, 40), c.fontSize, (e.id ? '#' + e.id : '') + (typeof e.className === 'string' && e.className ? '.' + e.className.split(/\s+/).slice(0, 2).join('.') : ''), c.position, c.float, e.parentElement ? 1 : 0, (e.innerText || '').slice(0, 24).replace(/\s+/g, ' ')]);
    for (const k of e.children) walk(k);
  };
  walk(document.documentElement);
  for (const s of [...document.querySelectorAll('script,link[rel=preload],link[rel=modulepreload]')]) s.setAttribute('data-fv-gone', '');
  placed.forEach(([n], k) => n.setAttribute('data-fv-sheet', k));
  // shadow roots go out as declarative ones, their adopted sheets as <style> inside them
  const roots = [];
  const sheetText = list => [...list].map(x => [...x.cssRules].map(r => r.cssText).join("\n")).join("\n");
  const findRoots = n => { for (const e of n.querySelectorAll('*')) if (e.shadowRoot) { roots.push(e.shadowRoot); findRoots(e.shadowRoot); } };
  findRoots(document);
  for (const r of roots) if (r.adoptedStyleSheets.length) { const st = document.createElement('style'); st.textContent = sheetText(r.adoptedStyleSheets); r.append(st); }
  if (document.adoptedStyleSheets.length) sheets.push(sheetText(document.adoptedStyleSheets));
  const html = document.documentElement.getHTML({ serializableShadowRoots: true, shadowRoots: roots })
    .replace(/<(link|style)\b[^>]*data-fv-sheet="(\d+)"[^>]*>(<\/style>)?/g, (m, tag, k) => '<style data-fv-sheet="' + k + '">' + placed[k][1].replace(/<\/style/gi, '<\\/style') + '</style>');
  const open = document.documentElement.outerHTML.slice(0, document.documentElement.outerHTML.indexOf('>') + 1);
  return JSON.stringify({ css: sheets.join("\n"), rects, html: (document.compatMode === 'CSS1Compat' ? '<!doctype html>' : '') + open + html + '</html>', title: document.title, url: location.href, roots: roots.length });
})()`;
for (const arg of process.argv.slice(2)) {
  const at = arg.indexOf("="); const [name, url] = [arg.slice(0, at), arg.slice(at + 1)];
  const isLocal = url.startsWith("file://");
  const outRoot = process.env.FV_SNAPSHOT_DIR || (isLocal ? "tests/sites" : "tmp/sites");
  if (isLocal) {
    const relCheck = relative(process.cwd(), fileURLToPath(url)).replace(/\\/g, "/");
    if (!relCheck || relCheck.startsWith("..")) {
      throw new Error(`Refusing to snapshot local file outside repository: ${url}`);
    }
  }
  const out = join(outRoot, name); mkdirSync(out, { recursive: true });
  const { targetId } = await send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  await send("Page.enable", {}, sessionId); await send("Page.bringToFront", {}, sessionId);
  await send("Emulation.setDeviceMetricsOverride", { width: W, height: H, deviceScaleFactor: 1, mobile: PHONE }, sessionId);
  if (PHONE) await send("Emulation.setUserAgentOverride", { userAgent: "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36" }, sessionId);
  await send("Page.navigate", { url }, sessionId); await sleep(5000);
  const r = await send("Runtime.evaluate", { expression: collect, awaitPromise: true, returnByValue: true }, sessionId);
  if (!r.result.value) { console.log(name, JSON.stringify(r).slice(0, 300)); continue; }
  const d = JSON.parse(r.result.value);
  let recordedUrl = d.url;
  if (recordedUrl.startsWith("file://")) {
    const rel = relative(process.cwd(), fileURLToPath(recordedUrl)).replace(/\\/g, "/");
    if (!rel || rel.startsWith("..")) {
      throw new Error(`Refusing to record local file URL outside repository: ${recordedUrl}`);
    }
    recordedUrl = rel;
  }
  writeFileSync(join(out, "page.html"), d.html); writeFileSync(join(out, "style.css"), d.css); writeFileSync(join(out, "rects.json"), JSON.stringify(d.rects)); writeFileSync(join(out, "url.txt"), recordedUrl); writeFileSync(join(out, "viewport.txt"), W + " " + H);
  const shot = await send("Page.captureScreenshot", { format: "png" }, sessionId); writeFileSync(join(out, "shot.png"), Buffer.from(shot.data, "base64"));
  await send("Target.closeTarget", { targetId });
  console.log(name, d.title.slice(0, 40), "elements", d.rects.length, "html KB", d.html.length >> 10, "css KB", d.css.length >> 10, "shadow roots", d.roots);
}
p.kill(); await sleep(300); rmSync(dir, { recursive: true, force: true }); process.exit(0);
