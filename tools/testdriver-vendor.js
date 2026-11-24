// WPT's testdriver for Farview: what a test asks the browser's user to do (click, keys, pointer and key actions) done
// through the host's input entry points (__mouse, __keyev, which make trusted events as the host's input does),
// which the WPT runner gives the page as window.__fvTestInput.
// tools/wpt-serve.sh puts it in the checkout's resources/ in place of the empty vendor file.
(function () {
  const ti = window.test_driver_internal;
  // (the host's input entry points, which the WPT runner gives tests: a page's scripts otherwise cannot reach them)
  const I = window.__fvTestInput;
  ti.in_automation = true;
  const frame = () => new Promise(r => requestAnimationFrame(() => r()));
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  // WebDriver's key codes (U+E000 on) as the key names a page sees
  const special = {
    "": "Backspace", "": "Tab", "": "Enter", "": "Enter", "": "Shift", "": "Control",
    "": "Alt", "": "Escape", "": " ", "": "PageUp", "": "PageDown", "": "End",
    "": "Home", "": "ArrowLeft", "": "ArrowUp", "": "ArrowRight", "": "ArrowDown",
    "": "Insert", "": "Delete", "": "Meta", "": "Shift", "": "Control", "": "Alt",
    "": "Meta",
  };
  const keyName = k => special[k] || k;
  const codeOf = k => k.length === 1 ? (/[a-z]/i.test(k) ? "Key" + k.toUpperCase() : /[0-9]/.test(k) ? "Digit" + k : k === " " ? "Space" : "") : k === "Shift" ? "ShiftLeft" : k === "Control" ? "ControlLeft" : k === "Alt" ? "AltLeft" : k === "Meta" ? "MetaLeft" : k;
  const key = (kind, k) => { const n = keyName(k); if (n === "Shift") window.__shiftDown = kind === "down"; I.__keyev(kind, n, codeOf(n)); };
  ti.click = async (el, coords) => {
    I.__mouse("move", coords.x, coords.y, 0, 0, "mouse");
    I.__mouse("down", coords.x, coords.y, 0, 1, "mouse");
    I.__mouse("up", coords.x, coords.y, 0, 0, "mouse");
    await frame();
  };
  ti.send_keys = async (el, keys) => {
    if (el && el !== document.activeElement && typeof el.focus === "function") el.focus();
    for (const k of keys) { key("down", k); if (keyName(k) !== "Shift") key("up", k); }
    window.__shiftDown = false;
    await frame();
  };
  ti.action_sequence = async (sources) => {
    const pos = {}, down = {};
    const ticks = Math.max(0, ...sources.map(s => s.actions.length));
    for (let i = 0; i < ticks; i++) {
      let wait = 0;
      for (const s of sources) {
        const a = s.actions[i]; if (!a) continue;
        const id = s.id || s.type;
        if (a.type === "pause") { wait = Math.max(wait, a.duration || 0); continue; }
        if (s.type === "key") { if (a.type === "keyDown") key("down", a.value); else if (a.type === "keyUp") key("up", a.value); continue; }
        if (s.type === "pointer") {
          const pt = (s.parameters && s.parameters.pointerType) || "mouse";
          const p = pos[id] || (pos[id] = { x: 0, y: 0 });
          if (a.type === "pointerMove") {
            let x = a.x || 0, y = a.y || 0;
            if (a.origin && typeof a.origin === "object") { const r = a.origin.getBoundingClientRect(); x += r.left + r.width / 2; y += r.top + r.height / 2; }
            else if (a.origin === "pointer") { x += p.x; y += p.y; }
            p.x = x; p.y = y;
            I.__mouse("move", x, y, 0, down[id] ? 1 : 0, pt);
            wait = Math.max(wait, a.duration || 0);
          } else if (a.type === "pointerDown") { down[id] = true; I.__mouse("down", p.x, p.y, a.button || 0, 1, pt); }
          else if (a.type === "pointerUp") { down[id] = false; I.__mouse("up", p.x, p.y, a.button || 0, 0, pt); }
          continue;
        }
        if (s.type === "wheel" && a.type === "scroll") {
          let x = a.x || 0, y = a.y || 0;
          if (a.origin && typeof a.origin === "object") { const r = a.origin.getBoundingClientRect(); x += r.left + r.width / 2; y += r.top + r.height / 2; }
          const t = document.elementFromPoint(x, y) || document.documentElement;
          if (t.dispatchEvent(new WheelEvent("wheel", { bubbles: true, cancelable: true, composed: true, clientX: x, clientY: y, deltaX: a.deltaX || 0, deltaY: a.deltaY || 0 }))) {
            for (let n = t; n; n = n.parentElement) { if (n.scrollHeight > n.clientHeight || n.scrollWidth > n.clientWidth) { const top = n.scrollTop, left = n.scrollLeft; n.scrollTop += a.deltaY || 0; n.scrollLeft += a.deltaX || 0; if (n.scrollTop !== top || n.scrollLeft !== left) break; } }
            if (window.scrollBy) window.scrollBy(a.deltaX || 0, a.deltaY || 0);
          }
        }
      }
      if (wait) await sleep(wait);
    }
    window.__shiftDown = false;
    await frame();
  };
  ti.set_permission = async () => {};
  ti.get_computed_role = async el => el.getAttribute("role") || "";
  ti.get_computed_label = async el => el.getAttribute("aria-label") || (el.textContent || "").trim();
})();
