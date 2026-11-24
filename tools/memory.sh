#!/bin/bash
# What a page laid out again after small changes keeps: tests/sites/absnest's .f box toggled 200 times, then
# 5,000 times, each laid out again (an attribute, then a style); the peak memory of the second may not be more than
# a few MB above the first's (a box that held itself was never freed: 10 KB a change). Exits 1 when it is.
set -euo pipefail
BIN_DIR=${FV_BIN_DIR:-$PWD/out}
X="$BIN_DIR/pion-layout"
d=tests/sites/absnest
[ -x "$X" ] || { echo "memory: missing executable $X (build with: pionc build pion/cmd/layout -o out/pion-layout)" >&2; exit 1; }
[ -f "$d/page.html" ] || { echo "memory: missing fixture $d/page.html" >&2; exit 1; }
peak() {
  local iters="$1"
  local st="${2:-}"
  python3 - "$X" "$d/page.html" "$d/style.css" "$iters" "$st" <<'PY'
import os, resource, subprocess, sys
exe, html, css, iters, st = sys.argv[1:6]
env = os.environ.copy()
env["FV_BENCH_SMALL"] = iters
env["FV_BENCH_SMALL_CLASS"] = "f"
env["FV_NO_WEB_FONTS"] = "1"
if st:
    env["FV_BENCH_SMALL_STYLE"] = st
else:
    env.pop("FV_BENCH_SMALL_STYLE", None)
res = subprocess.run([exe, html, css, "/dev/null"], env=env, capture_output=True, text=True)
if res.returncode != 0:
    sys.stderr.write(f"memory: {exe} failed (rc={res.returncode}): {res.stdout}\n{res.stderr}\n")
    sys.exit(res.returncode)
if "bench small change:" not in res.stdout:
    sys.stderr.write(f"memory: missing bench small change output: {res.stdout}\n")
    sys.exit(1)
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
# macOS reports ru_maxrss in bytes; Linux reports KiB
rss_bytes = ru.ru_maxrss if sys.platform == "darwin" else ru.ru_maxrss * 1024
if rss_bytes <= 0:
    sys.stderr.write(f"memory: invalid RSS {rss_bytes}\n")
    sys.exit(1)
print(rss_bytes)
PY
}
bad=0
for style in "" "display: block"; do
  a=$(peak 200 "$style")
  b=$(peak 5000 "$style")
  grew=$(( (b - a) / 1048576 ))
  echo "memory: ${style:-an attribute}: 200 changes $((a / 1048576)) MB, 5000 changes $((b / 1048576)) MB (+$grew MB)"
  if [ "$grew" -gt 8 ]; then bad=1; fi
done
[ "$bad" -eq 0 ] && echo "memory: nothing kept by changes laid out again"
exit "$bad"
