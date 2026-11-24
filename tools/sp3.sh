#!/bin/bash
# Speedometer 3 (github.com/WebKit/Speedometer, cloned beside the tree in ../sp3) in the host's page runner and in
# headless Chrome on the same machine: each suite's and each of its steps' mean time, ms, side by side.
# SUITES=a,b (default TodoMVC-React), N iterations (default 3); FV_ONLY=1: the host only. The page runner renders the
# frames a display would show (FV_RENDER), as Chrome does between its tasks: the two times count the same work.
set -u
SP=${SP3:-$PWD/../sp3}
[ -d "$SP" ] || git clone -q --depth 1 https://github.com/WebKit/Speedometer.git "$SP"
SUITES=${SUITES:-TodoMVC-React}
N=${N:-3}
port=${SP3_PORT:-8765}
( cd "$SP" && exec python3 -m http.server $port --bind 127.0.0.1 > /dev/null 2>&1 ) & srv=$!
sleep 1
URL="http://127.0.0.1:$port/index.html?startAutomatically=true&developerMode&iterationCount=$N&suites=$SUITES"
EXPR='JSON.stringify({ state: globalThis.benchmarkClient && benchmarkClient._state, error: (document.getElementById("invalid-score-text") || {}).textContent, m: Object.values((globalThis.benchmarkClient && benchmarkClient._metrics) || {}).filter(m => !m.name.startsWith("Iteration-") && m.name.split("/").length <= 2).map(m => [m.name, Math.round(m.mean * 10) / 10]) })'
if [ -z "${WPT:-}" ]; then
  WPT="$PWD/out/pion-wpt"
  mkdir -p "$PWD/out"
  "${PIONC:-pionc}" build pion/cmd/wpt -o "$WPT" >/dev/null
fi
host=$(FV_RENDER=1 FV_WIDTH=800 FV_WAIT=${FV_WAIT:-600000} "$WPT" "$URL" "$EXPR" 2>/dev/null | grep '^{"state"' | tail -1)
chrome=""
[ -z "${FV_ONLY:-}" ] && chrome=$(node tools/sp3_chrome.mjs "$URL" "$EXPR" 2>/dev/null | tail -1)
kill $srv 2>/dev/null
python3 - "$host" "$chrome" <<'PY'
import json, sys
def load(s):
    try: return json.loads(s)
    except Exception: return {"state": "no answer", "m": []}
h, c = load(sys.argv[1]), load(sys.argv[2]) if sys.argv[2] else {"state": "-", "m": []}
print("host:", h.get("state"), (h.get("error") or "").strip()[:200], "| chrome:", c.get("state"))
cm = dict(c.get("m", []))
for name, v in h.get("m", []):
    w = cm.get(name)
    print(f"{name:60s} {v:9.1f} ms" + (f"  chrome {w:8.1f} ms  x{v / w:5.1f}" if w else ""))
if not h.get("m"):
    for name, w in c.get("m", []): print(f"{name:60s}  chrome {w:8.1f} ms")
PY
