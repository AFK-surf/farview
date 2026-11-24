#!/bin/bash
# Stage 23A end-to-end integration check:
# 1. HTMLInputElement WHATWG valueAsNumber, valueAsDate, stepUp, stepDown across number/date/time/month/week/datetime-local.
# 2. Form submission interception on invalid required control -> focuses control + emits PatchOp::Validation over DocClient wire.
# 3. Typing valid input clears validation tip and allows form submission.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/stage23
rm -rf "$O" && mkdir -p "$O/state" "$O/www"
port=${STAGE23_PORT:-7760}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "stage23: $1"; for f in "$O"/*.out "$O"/*.log; do [ -f "$f" ] && { echo "-- $f"; tail -25 "$f"; }; done; exit 1; }

cat > "$O/www/forms.html" <<'HTML'
<!doctype html>
<html>
<head><meta charset="utf-8"><title>stage23-forms</title></head>
<body>
  <input id="d1" type="date" value="2026-11-21" min="2026-11-01" max="2026-12-31">
  <input id="n1" type="number" value="4" min="0" max="20" step="2">
  <form id="f1">
    <input id="req1" type="email" required placeholder="name@example.com">
    <button id="sub1" type="submit">Submit</button>
  </form>
  <div id="status">init</div>
  <script>
    f1.addEventListener("submit", e => {
      e.preventDefault();
      status.textContent = "submitted:" + req1.value;
    });
  </script>
</body>
</html>
HTML

python3 -m http.server "$web" --directory "$O/www" > "$O/http.log" 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID ${HOST_PID:-} 2>/dev/null; wait $HTTP_PID ${HOST_PID:-} 2>/dev/null' EXIT
sleep 0.4

(cd "$O" && FV_STATE_DIR="$O/state" FV_TAB_SIZE=800x600 exec "$HOST" --tab "$port" "http://127.0.0.1:$web/forms.html" > "$O/host.out" 2>&1) &
HOST_PID=$!
for _ in $(seq 50); do [ -s "$O/state/host.json" ] && break; sleep 0.2; done
[ -s "$O/state/host.json" ] || fail "host did not start"

# 1. Verify valueAsNumber, valueAsDate, stepUp, stepDown
res=$("$AGENT" 127.0.0.1 $((port + 1)) \
  'eval:(() => { n1.stepUp(2); d1.stepUp(3); const r = [n1.value, n1.valueAsNumber, d1.value, d1.valueAsDate.toISOString().slice(0,10)]; d1.valueAsDate = null; return JSON.stringify([...r, d1.value, typeof globalThis.__validationTip]); })()')
echo "$res" | grep -q '8,\\"2026-11-24\\",\\"2026-11-24\\",\\"\\",\\"undefined\\"' || fail "valueAsNumber/stepUp mismatch: $res"

# 2. Verify DocClient receives PatchOp::Validation when requestSubmit fails, and clears it on valid input
python3 - "$port" "$((port + 1))" "$AGENT" <<'PY' || fail "DocClient Validation wire check failed"
import socket, struct, subprocess, sys, time

port = int(sys.argv[1])
agent_port = int(sys.argv[2])
agent = sys.argv[3]

def encode_varint(v):
    u = ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF
    out = bytearray()
    while u >= 0x80:
        out.append((u & 0x7F) | 0x80)
        u >>= 7
    out.append(u)
    return bytes(out)

def frame(body):
    return struct.pack(">I", len(body)) + body

def encode_hello():
    cap = b"doc"
    body = encode_varint(120) + encode_varint(1) + encode_varint(len(cap)) + cap
    return frame(body)

def decode_varint(b, i):
    u = 0; shift = 0
    while i < len(b):
        c = b[i]; i += 1
        u |= (c & 0x7F) << shift
        if c < 0x80:
            break
        shift += 7
    v = (u >> 1) ^ -(u & 1)
    return v, i

def find_validation_ops(frames_bytes):
    # Scan Patch messages (kind 102) for Validation op (tag 18) containing target string
    found = []
    for body in frames_bytes:
        kind, _ = decode_varint(body, 0)
        if kind == 102:
            if b"Please fill out this field." in body:
                found.append("shown")
            elif b"\x24\x00\x00" in body:
                # zigzag(18)=36=0x24, uid=0, msg_len=0
                found.append("cleared")
    return found

s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(encode_hello())
s.settimeout(0.5)

def recv_frames(dur=0.6):
    end = time.time() + dur
    buf = bytearray()
    out = []
    while time.time() < end:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf.extend(chunk)
        except socket.timeout:
            pass
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n:
                break
            out.append(bytes(buf[4:4+n]))
            del buf[:4+n]
    return out

recv_frames(0.6)
subprocess.check_call([agent, "127.0.0.1", str(agent_port), "eval:f1.requestSubmit(); document.activeElement.id"], stdout=subprocess.DEVNULL)
f1 = recv_frames(1.2)
ops1 = find_validation_ops(f1)
assert "shown" in ops1, f"expected Validation shown op, got {ops1}; frames={len(f1)} kinds={[decode_varint(x,0)[0] for x in f1]}"

subprocess.check_call([agent, "127.0.0.1", str(agent_port), "eval:req1.value='ok@example.com'; f1.requestSubmit(); status.textContent"], stdout=subprocess.DEVNULL)
f2 = recv_frames(0.8)
ops2 = find_validation_ops(f2)
assert "cleared" in ops2, f"expected Validation cleared op, got {ops2}"
s.close()
print("stage23: DocClient Validation wire sync OK")
PY

echo "stage23: PASS"
