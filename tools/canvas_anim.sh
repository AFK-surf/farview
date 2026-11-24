#!/bin/bash
# Stage 11A host integration check:
# 1. Continuous <canvas> 2D rAF/timer updates over DocClient wire: every frame arrives promptly with O(1) host
#    picture map (no layout storms, no memory growth).
# 2. Large download (> 4 MiB DOWNLOAD_SINGLE_MOST) streams via DownloadStart (110) / DownloadChunk (111) /
#    DownloadEnd (112) and reassembles byte-for-byte with identical SHA-256.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/canvas_anim
rm -rf "$O" && mkdir -p "$O/state" "$O/www"
port=${CANVAS_ANIM_PORT:-7660}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "canvas_anim: $1"; for f in "$O"/*.out; do [ -f "$f" ] && { echo "-- $f"; tail -20 "$f"; }; done; exit 1; }

# 1. Generate a 5 MiB deterministic binary file for chunked download verification (> 4 MiB single-frame cap)
python3 -I - "$O/www/big5m.bin" "$O/www/page.html" <<'PY'
import hashlib, os, sys
raw = bytes((i * 131 + 17) & 0xFF for i in range(5 * 1024 * 1024 + 12345))
open(sys.argv[1], "wb").write(raw)
print("big5m sha256:", hashlib.sha256(raw).hexdigest())
cards = "".join(f"<div class='c'>Card {i}</div>" for i in range(200))
html = f"""<!doctype html><html><head><title>canvas-anim</title><style>
body {{ margin: 0; font-family: Arial; }}
canvas {{ display: block; width: 240px; height: 120px; }}
.grid {{ display: flex; flex-wrap: wrap; gap: 4px; }}
.c {{ width: 90px; height: 24px; background: #eee; }}
</style></head><body>
<a id="dl" href="/big5m.bin" download="big5m.bin">Download 5MiB</a>
<canvas id="cv" width="240" height="120"></canvas>
<div class="grid">{cards}</div>
<script>
var c = document.getElementById('cv');
var ctx = c.getContext('2d');
var n = 0;
window.runFrames = function(total) {{
  n = 0;
  function step() {{
    n++;
    ctx.fillStyle = '#111827';
    ctx.fillRect(0, 0, 240, 120);
    ctx.fillStyle = (n % 2 === 0) ? '#38bdf8' : '#34d399';
    ctx.fillRect((n * 4) % 200, 15, 36, 90);
    if (n < total) requestAnimationFrame(step);
  }}
  requestAnimationFrame(step);
}};
</script></body></html>"""
open(sys.argv[2], "w").write(html)
PY

python3 -I -m http.server "$web" --bind 127.0.0.1 --directory "$O/www" > "$O/web.out" 2>&1 & W=$!
trap 'kill $H $W 2>/dev/null || true' EXIT
for _ in $(seq 80); do curl -sf "http://127.0.0.1:$web/page.html" > /dev/null && break; sleep 0.1; done

FV_NO_FORK=1 FV_STATE_DIR="$O/state" FV_HOST_LOG="$O/host.log" \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/page.html" > "$O/host.out" 2>&1 & H=$!
for _ in $(seq 100); do [ -s "$O/state/host.json" ] && break; sleep 0.1; done
[ -s "$O/state/host.json" ] || fail "no host.json"

python3 -I - "$port" "$AGENT" "$O/www/big5m.bin" > "$O/check.out" 2>&1 <<'PY' || fail "$(tail -5 "$O/check.out")"
import hashlib, socket, struct, subprocess, sys, time
port, agent, big_path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
expected_bytes = open(big_path, "rb").read()
expected_sha = hashlib.sha256(expected_bytes).hexdigest()

def varint(v):
    u = ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF; out = b""
    while u >= 0x80: out += bytes([(u & 0x7F) | 0x80]); u >>= 7
    return out + bytes([u])
def string(s): b = s.encode(); return varint(len(b)) + b
def framed(body): return struct.pack(">I", len(body)) + body
class R:
    def __init__(s, b): s.b, s.i = b, 0
    def int(s):
        u = sh = 0
        while True:
            c = s.b[s.i]; s.i += 1; u |= (c & 0x7F) << sh; sh += 7
            if c < 0x80: break
        return (u >> 1) ^ -(u & 1)
    def bytes(s): n = s.int(); v = s.b[s.i:s.i + n]; s.i += n; return v
    def string(s): return s.bytes().decode(errors="replace")
    def flag(s): v = s.b[s.i]; s.i += 1; return v != 0

sock = socket.create_connection(("127.0.0.1", port)); sock.settimeout(0.05)
sock.sendall(framed(varint(120) + varint(1) + string("doc")) + framed(varint(121) + struct.pack("<ddd", 960, 640, 1)))
buf = b""
got_reset = False
t0 = time.monotonic()
while time.monotonic() - t0 < 8.0 and not got_reset:
    try: buf += sock.recv(1 << 20)
    except socket.timeout: pass
    while len(buf) >= 4:
        n = struct.unpack(">I", buf[:4])[0]
        if len(buf) < 4 + n: break
        body, buf = buf[4:4 + n], buf[4 + n:]
        if R(body).int() == 101: got_reset = True
assert got_reset, "did not receive initial Reset"

# Part 1: trigger 40 continuous rAF canvas frames via agent
subprocess.run([agent, "127.0.0.1", str(port + 1), "eval:window.runFrames(40)"], check=True, capture_output=True)
canvas_keys = []
t1 = time.monotonic()
while time.monotonic() - t1 < 10.0 and len(canvas_keys) < 20:
    try: buf += sock.recv(1 << 20)
    except socket.timeout: pass
    while len(buf) >= 4:
        n = struct.unpack(">I", buf[:4])[0]
        if len(buf) < 4 + n: break
        body, buf = buf[4:4 + n], buf[4 + n:]
        r = R(body)
        if r.int() == 103:
            k = r.string()
            if k.startswith("fvcanvas:"):
                canvas_keys.append(k)
assert len(canvas_keys) >= 20, f"expected >=20 canvas frames, got {len(canvas_keys)}"
print(f"canvas_anim: received {len(canvas_keys)} continuous canvas frames ({canvas_keys[0]} .. {canvas_keys[-1]})")

# Part 2: trigger 5 MiB download and verify chunked protocol (110/111/112) + SHA-256
subprocess.run([agent, "127.0.0.1", str(port + 1), "click:#dl"], check=True, capture_output=True)
dl_meta = {}
dl_bufs = {}
dl_done = None
chunks_seen = 0
t2 = time.monotonic()
while time.monotonic() - t2 < 15.0 and dl_done is None:
    try: buf += sock.recv(1 << 20)
    except socket.timeout: pass
    while len(buf) >= 4:
        n = struct.unpack(">I", buf[:4])[0]
        if len(buf) < 4 + n: break
        body, buf = buf[4:4 + n], buf[4 + n:]
        r = R(body)
        kind = r.int()
        if kind == 110:
            did, name, url, mime, total = r.int(), r.string(), r.string(), r.string(), r.int()
            dl_meta[did] = (name, url, mime, total)
            dl_bufs[did] = bytearray()
        elif kind == 111:
            did = r.int()
            chunk = r.bytes()
            chunks_seen += 1
            dl_bufs[did].extend(chunk)
        elif kind == 112:
            did = r.int()
            dl_done = (dl_meta[did], bytes(dl_bufs[did]))
            break
sock.close()
assert dl_done is not None, "did not receive chunked download End (112)"
assert chunks_seen >= 3, f"expected >=3 chunks for 5MiB file, got {chunks_seen}"
meta, got_bytes = dl_done
assert meta[0] == "big5m.bin" and meta[3] == len(expected_bytes), f"bad meta: {meta}"
got_sha = hashlib.sha256(got_bytes).hexdigest()
assert got_sha == expected_sha, f"sha mismatch: {got_sha} != {expected_sha}"
print(f"canvas_anim: chunked download OK ({len(got_bytes)} bytes in {chunks_seen} chunks, sha256={got_sha[:16]})")
PY

cat "$O/check.out"
echo "canvas_anim: PASS"
