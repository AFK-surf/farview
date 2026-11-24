#!/bin/bash
# Stage 10A realtime network checks against a built host:
# 1. Native WebSocket (RFC 6455): text & ArrayBuffer binary echo, subprotocol negotiation, clean CloseEvent(code, reason, wasClean).
# 2. True streaming EventSource (SSE): verifies first chunk arrives well before stream end (incremental, not buffered) + auto-reconnect with Last-Event-ID.
# 3. Streaming fetch().body.getReader() incremental chunks + AbortController.abort() immediate cancellation.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/realtime
rm -rf "$O" && mkdir -p "$O/state"
port=${REALTIME_PORT:-7920}
web=$((port + 10))
wsport=$((port + 11))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "realtime: $1"; for f in "$O"/*.out "$O"/*.log; do [ -f "$f" ] && { echo "-- $f"; tail -25 "$f"; }; done; exit 1; }

cat > "$O/server.py" << 'PYEOF'
import base64, hashlib, http.server, socket, struct, sys, threading, time

HTTP_PORT = int(sys.argv[1])
WS_PORT = int(sys.argv[2])

# --- Minimal RFC 6455 WebSocket server ---
def ws_recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        c = conn.recv(n - len(buf))
        if not c:
            return None
        buf.extend(c)
    return bytes(buf)

def ws_send_frame(conn, opcode, payload):
    hdr = bytearray([0x80 | (opcode & 0x0F)])
    n = len(payload)
    if n < 126:
        hdr.append(n)
    elif n < 65536:
        hdr.append(126)
        hdr.extend(struct.pack("!H", n))
    else:
        hdr.append(127)
        hdr.extend(struct.pack("!Q", n))
    conn.sendall(bytes(hdr) + payload)

def ws_client(conn):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            c = conn.recv(4096)
            if not c:
                return
            req += c
        lines = req.decode("latin1").split("\r\n")
        hdrs = {}
        for l in lines[1:]:
            if ":" in l:
                k, v = l.split(":", 1)
                hdrs[k.strip().lower()] = v.strip()
        key = hdrs.get("sec-websocket-key", "")
        accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        proto = hdrs.get("sec-websocket-protocol", "").split(",")[0].strip()
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
        )
        if proto:
            resp += f"Sec-WebSocket-Protocol: {proto}\r\n"
        resp += "\r\n"
        conn.sendall(resp.encode("latin1"))

        while True:
            h2 = ws_recv_exact(conn, 2)
            if not h2:
                break
            opcode = h2[0] & 0x0F
            masked = (h2[1] & 0x80) != 0
            plen = h2[1] & 0x7F
            if plen == 126:
                plen = struct.unpack("!H", ws_recv_exact(conn, 2))[0]
            elif plen == 127:
                plen = struct.unpack("!Q", ws_recv_exact(conn, 8))[0]
            mask = ws_recv_exact(conn, 4) if masked else b""
            data = ws_recv_exact(conn, plen) if plen else b""
            if data is None:
                break
            if masked:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if opcode == 0x1:  # text
                txt = data.decode("utf-8", errors="replace")
                if txt == "close-me":
                    ws_send_frame(conn, 0x8, struct.pack("!H", 1000) + b"bye-server")
                    break
                ws_send_frame(conn, 0x1, ("echo:" + txt).encode("utf-8"))
            elif opcode == 0x2:  # binary
                ws_send_frame(conn, 0x2, bytes([b ^ 0xFF for b in data]))
            elif opcode == 0x8:  # close
                ws_send_frame(conn, 0x8, data[:2] if len(data) >= 2 else struct.pack("!H", 1000))
                break
            elif opcode == 0x9:  # ping
                ws_send_frame(conn, 0xA, data)
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass

def run_ws():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", WS_PORT))
    s.listen(16)
    while True:
        c, _ = s.accept()
        threading.Thread(target=ws_client, args=(c,), daemon=True).start()

threading.Thread(target=run_ws, daemon=True).start()

# --- Streaming HTTP / SSE server ---
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        if self.path == "/" or self.path == "/page.html":
            body = b"<!doctype html><html><head><title>realtime</title></head><body>ok</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/sse":
            last_id = self.headers.get("Last-Event-ID", "")
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.flush()
            if last_id == "5":
                msg = b"id: 6\nevent: resumed\ndata: after-5\n\n"
                self.wfile.write(msg)
                self.wfile.flush()
                time.sleep(0.5)
                return
            self.wfile.write(b": comment\nretry: 120\n\n")
            self.wfile.flush()
            for i in range(1, 6):
                self.wfile.write(f"id: {i}\ndata: chunk-{i}\n\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.08)
            return
        if self.path == "/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.flush()
            for i in range(5):
                self.wfile.write(f"part-{i}\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.08)
            return
        if self.path == "/slow-abort":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"first-chunk\n")
            self.wfile.flush()
            try:
                for _ in range(100):
                    time.sleep(0.2)
            except Exception:
                pass
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):
        pass

srv = http.server.ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
srv.serve_forever()
PYEOF

python3 -I "$O/server.py" "$web" "$wsport" > "$O/srv.out" 2>&1 & S=$!
H=
trap 'kill $H $S 2>/dev/null' EXIT
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$web/" > /dev/null && break; sleep 0.05; done

FV_STATE_DIR="$O/state" FV_HOST_LOG="$O/host.log" \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/" > "$O/host.out" 2>&1 & H=$!
for _ in $(seq 100); do [ -s "$O/state/host.json" ] && break; sleep 0.05; done
[ -s "$O/state/host.json" ] || fail "no host.json"

A() { "$AGENT" 127.0.0.1 $((port + 1)) "$@"; }
for _ in $(seq 50); do A 'eval:document.title' 2>/dev/null | grep -qx '"realtime"' && break; sleep 0.1; done
A 'eval:document.title' | grep -qx '"realtime"' || fail "page did not load"

# 1. WebSocket test (text, >64KB large frame, binary ArrayBuffer, subprotocol, clean close)
ws_res=$(A "eval:new Promise((resolve, reject) => {
  const ws = new WebSocket('ws://127.0.0.1:$wsport/chat', ['chat.v1', 'chat.v2']);
  ws.binaryType = 'arraybuffer';
  const bigStr = 'A'.repeat(40000) + 'B'.repeat(40000) + 'END';
  const out = { proto: '', msgs: [], bigOk: false, bin: null, close: null, rttMs: 0 };
  let tSend = 0, step = 0;
  ws.onopen = () => {
    out.proto = ws.protocol;
    tSend = Date.now();
    ws.send('hello-ws');
  };
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      if (step === 0) {
        out.rttMs = Date.now() - tSend;
        out.msgs.push(e.data);
        step = 1;
        ws.send(bigStr);
      } else if (step === 1) {
        out.bigOk = (e.data === 'echo:' + bigStr);
        step = 2;
        ws.send(new Uint8Array([1, 2, 3, 250]).buffer);
      }
    } else if (e.data instanceof ArrayBuffer) {
      out.bin = [...new Uint8Array(e.data)];
      ws.send('close-me');
    }
  };
  ws.onclose = (e) => {
    out.close = { code: e.code, reason: e.reason, wasClean: e.wasClean, state: ws.readyState };
    resolve(JSON.stringify(out));
  };
  ws.onerror = () => reject(new Error('ws error'));
})" | tail -1)
python3 -I -c 'import json, sys
r = json.loads(json.loads(sys.argv[1]))
assert r["proto"] == "chat.v1", r
assert r["msgs"] == ["echo:hello-ws"], r
assert r["bigOk"] is True, r
assert r["bin"] == [254, 253, 252, 5], r
assert r["close"] == {"code": 1000, "reason": "bye-server", "wasClean": True, "state": 3}, r
print("realtime: ws ok (proto=%s, >80KB frame ok, rtt=%sms)" % (r["proto"], r["rttMs"]))' "$ws_res" || fail "ws failed: $ws_res"

# 2. EventSource (SSE) true incremental streaming + Last-Event-ID reconnect
sse_res=$(A "eval:new Promise((resolve, reject) => {
  const es = new EventSource('/sse');
  const times = [], datas = [];
  let opened = false;
  es.onopen = () => { opened = true; };
  es.onmessage = (e) => {
    times.push(Date.now());
    datas.push(e.lastEventId + ':' + e.data);
  };
  es.addEventListener('resumed', (e) => {
    es.close();
    resolve(JSON.stringify({
      opened,
      datas,
      spanMs: times.length >= 2 ? times[times.length - 1] - times[0] : 0,
      resumed: e.lastEventId + ':' + e.data,
      closedState: es.readyState,
    }));
  });
  setTimeout(() => { es.close(); reject(new Error('sse timeout: ' + JSON.stringify(datas))); }, 8000);
})" | tail -1)
python3 -I -c 'import json, sys
r = json.loads(json.loads(sys.argv[1]))
assert r["opened"] is True, r
assert r["datas"] == ["1:chunk-1", "2:chunk-2", "3:chunk-3", "4:chunk-4", "5:chunk-5"], r
assert r["spanMs"] >= 200, "SSE was buffered, not streaming! spanMs=%s" % r["spanMs"]
assert r["resumed"] == "6:after-5", r
assert r["closedState"] == 2, r
print("realtime: sse ok (5 chunks incremental span=%sms, Last-Event-ID reconnect ok)" % r["spanMs"])' "$sse_res" || fail "sse failed: $sse_res"

# 3. fetch().body.getReader() true streaming + AbortController.abort()
fetch_res=$(A "eval:(async () => {
  const res = await fetch('/stream');
  const rd = res.body.getReader();
  const dec = new TextDecoder();
  const chunks = [], times = [];
  for (;;) {
    const { value, done } = await rd.read();
    if (done) break;
    times.push(Date.now());
    chunks.push(dec.decode(value));
  }
  const ac = new AbortController();
  const tAbort0 = Date.now();
  const res2 = await fetch('/slow-abort', { signal: ac.signal });
  const rd2 = res2.body.getReader();
  const first = await rd2.read();
  ac.abort();
  let aborted = false;
  try { await rd2.read(); } catch (e) { aborted = true; }
  const abortMs = Date.now() - tAbort0;
  return JSON.stringify({
    text: chunks.join(''),
    chunkCount: chunks.length,
    spanMs: times.length >= 2 ? times[times.length - 1] - times[0] : 0,
    firstChunk: new TextDecoder().decode(first.value),
    aborted,
    abortMs,
  });
})()" | tail -1)
python3 -I -c 'import json, sys
r = json.loads(json.loads(sys.argv[1]))
assert r["text"] == "part-0\npart-1\npart-2\npart-3\npart-4\n", r
assert r["chunkCount"] >= 2 and r["spanMs"] >= 200, f"fetch body was not streaming! {r}"
assert r["firstChunk"] == "first-chunk\n", r
assert r["aborted"] is True and r["abortMs"] < 2000, r
print("realtime: fetch stream ok (%s chunks span=%sms, abort in %sms)" % (r["chunkCount"], r["spanMs"], r["abortMs"]))' "$fetch_res" || fail "fetch stream failed: $fetch_res"

echo "realtime: all ok"
