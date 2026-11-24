#!/bin/bash
# Stage 20A end-to-end integration check:
# 1. HTML5 Drag & Drop Kanban board driven over the real DocClient wire (Pointer DOWN -> MOVE -> UP):
#    verifies dragstart -> dragenter -> dragover -> drop -> dragend, DataTransfer payload, card moved to Done, no spurious click.
# 2. <canvas> interactive mouse drag drawing over DocClient wire:
#    verifies continuous pointerdown/mousedown -> pointermove/mousemove (buttons=1) -> pointerup/mouseup and painted canvas pixels.
# 3. @media print + window.print() (beforeprint/afterprint) + pure Pion `agent.exe pdf:<out.pdf>`:
#    verifies valid multi-page PDF 1.4 (>= 2 pages), screen viewport/media restored after export,
#    non-blank decoded page pixels (plus pdftoppm raster check when available).
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/stage20
rm -rf "$O" && mkdir -p "$O/state" "$O/www"
port=${STAGE20_PORT:-7740}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "stage20: $1"; for f in "$O"/*.out "$O"/*.log; do [ -f "$f" ] && { echo "-- $f"; tail -25 "$f"; }; done; exit 1; }

cat > "$O/www/dnd.html" <<'HTML'
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>stage20-dnd</title>
<style>
  body { margin: 0; font-family: sans-serif; }
  .board { display: flex; gap: 20px; padding: 20px; }
  .col { width: 200px; height: 200px; border: 2px solid #cbd5e1; padding: 10px; box-sizing: border-box; }
  .card { width: 160px; height: 50px; background: #2563eb; color: #fff; line-height: 50px; text-align: center; }
  canvas { display: block; margin: 10px 20px; border: 1px solid #94a3b8; }
</style>
</head>
<body>
  <div class="board">
    <div id="todo" class="col">
      <div id="card1" class="card" draggable="true">Task Alpha</div>
    </div>
    <div id="done" class="col"></div>
  </div>
  <canvas id="pad" width="240" height="120"></canvas>
  <script>
    window.dndLog = [];
    window.droppedText = "";
    const card = document.getElementById("card1");
    const done = document.getElementById("done");
    card.addEventListener("dragstart", e => {
      window.dndLog.push("dragstart");
      e.dataTransfer.setData("text/plain", "card1");
    });
    card.addEventListener("dragend", () => window.dndLog.push("dragend"));
    card.addEventListener("click", () => window.dndLog.push("spurious-click"));
    done.addEventListener("dragenter", () => window.dndLog.push("dragenter"));
    done.addEventListener("dragover", e => {
      e.preventDefault();
      if (!window.dndLog.includes("dragover")) window.dndLog.push("dragover");
    });
    done.addEventListener("drop", e => {
      e.preventDefault();
      window.dndLog.push("drop");
      window.droppedText = e.dataTransfer.getData("text/plain");
      done.appendChild(card);
    });

    const cv = document.getElementById("pad");
    const ctx = cv.getContext("2d");
    window.cvEvents = [];
    let drawing = false;
    cv.addEventListener("pointerdown", e => {
      drawing = true;
      window.cvEvents.push("pd:" + e.buttons);
      const r = cv.getBoundingClientRect();
      ctx.strokeStyle = "#dc2626";
      ctx.lineWidth = 6;
      ctx.beginPath();
      ctx.moveTo(e.clientX - r.left, e.clientY - r.top);
    });
    cv.addEventListener("mousemove", e => {
      if (!drawing) return;
      window.cvEvents.push("mm:" + e.buttons);
      const r = cv.getBoundingClientRect();
      ctx.lineTo(e.clientX - r.left, e.clientY - r.top);
      ctx.stroke();
    });
    cv.addEventListener("mouseup", e => {
      if (!drawing) return;
      drawing = false;
      window.cvEvents.push("mu:" + e.buttons);
    });
  </script>
</body>
</html>
HTML

cat > "$O/www/print.html" <<'HTML'
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>stage20-print</title>
<style>
  body { margin: 0; font-family: sans-serif; background: #ffffff; color: #0f172a; }
  .no-print { display: block; height: 60px; background: #ef4444; color: #fff; padding: 16px; }
  .print-only { display: none; height: 48px; background: #16a34a; color: #fff; padding: 12px; }
  .sec { height: 1050px; padding: 40px; box-sizing: border-box; border-bottom: 4px solid #2563eb; }
  .banner { width: 400px; height: 120px; background: #1d4ed8; }
  @media print {
    .no-print { display: none; }
    .print-only { display: block; }
  }
</style>
</head>
<body>
  <div id="nav" class="no-print">Screen Only Navigation</div>
  <div id="phdr" class="print-only">Print Only Header</div>
  <div class="sec"><div class="banner"></div><h1>Page 1 Content</h1></div>
  <div class="sec"><div class="banner"></div><h1>Page 2 Content</h1></div>
  <script>
    window.printEvents = [];
    window.addEventListener("beforeprint", () => window.printEvents.push("beforeprint"));
    window.addEventListener("afterprint", () => window.printEvents.push("afterprint"));
  </script>
</body>
</html>
HTML

python3 -I -m http.server "$web" --bind 127.0.0.1 --directory "$O/www" > "$O/web.out" 2>&1 &
W=$!
trap 'kill $H $W 2>/dev/null || true' EXIT
for _ in $(seq 80); do curl -sf "http://127.0.0.1:$web/dnd.html" > /dev/null && break; sleep 0.1; done

FV_NO_FORK=1 FV_STATE_DIR="$O/state" FV_HOST_LOG="$O/host.log" FV_TAB_SIZE=1280x800 \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/dnd.html" > "$O/host.out" 2>&1 & H=$!
for _ in $(seq 100); do [ -s "$O/state/host.json" ] && break; sleep 0.1; done
[ -s "$O/state/host.json" ] || fail "no host.json"

python3 -I - "$port" "$web" "$AGENT" "$O" > "$O/check.out" 2>&1 <<'PY' || fail "$(tail -20 "$O/check.out")"
import json, os, re, socket, struct, subprocess, sys, time

port, web, agent, out_dir = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]

def run_agent(*cmds):
    r = subprocess.run([agent, "127.0.0.1", str(port + 1), *cmds], check=True, capture_output=True, text=True)
    return r.stdout

def eval_js(expr):
    out = run_agent(f"eval:{expr}")
    lines = [l for l in out.splitlines() if not l.startswith("== ")]
    return json.loads("\n".join(lines).strip())

# Minimal fvclient/proto encoder (zigzag LEB128 int, little-endian float64, big-endian 4-byte frame len)
def enc_int(v):
    u = ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF
    out = bytearray()
    while u >= 0x80:
        out.append((u & 0x7F) | 0x80)
        u >>= 7
    out.append(u)
    return bytes(out)

def enc_str(s):
    b = s.encode("utf-8")
    return enc_int(len(b)) + b

def enc_num(v):
    return struct.pack("<d", float(v))

def send_doc_msg(sock, payload):
    sock.sendall(struct.pack(">I", len(payload)) + payload)

def enc_hello():
    # DocClient tag 120: Hello(ver: Int, caps: String)
    return enc_int(120) + enc_int(1) + enc_str("doc")

def enc_pointer(seq, kind, button, mods, target, rx, ry, x, y):
    # DocClient tag 122: Pointer(seq, kind, button, mods, target, rx, ry, x, y)
    return (
        enc_int(122)
        + enc_int(seq) + enc_int(kind) + enc_int(button) + enc_int(mods) + enc_int(target)
        + enc_num(rx) + enc_num(ry) + enc_num(x) + enc_num(y)
    )

# 1. Connect as a real DocClient and drag #card1 -> #done, then drag across <canvas id="pad">
s = socket.create_connection(("127.0.0.1", port), timeout=5)
send_doc_msg(s, enc_hello())
time.sleep(0.25)

# Drag #card1 (center ~110, 55) to #done (center ~320, 100)
seq = 1
send_doc_msg(s, enc_pointer(seq, 0, 0, 0, 0, 0.0, 0.0, 110.0, 55.0)); seq += 1
for i in range(1, 7):
    t = i / 6.0
    send_doc_msg(s, enc_pointer(seq, 2, 0, 0, 0, 0.0, 0.0, 110.0 + 210.0 * t, 55.0 + 45.0 * t)); seq += 1
    time.sleep(0.02)
send_doc_msg(s, enc_pointer(seq, 1, 0, 0, 0, 0.0, 0.0, 320.0, 100.0)); seq += 1
time.sleep(0.15)

# Drag across <canvas id="pad"> (x: 40..200, y: 270..300)
send_doc_msg(s, enc_pointer(seq, 0, 0, 0, 0, 0.0, 0.0, 40.0, 270.0)); seq += 1
for i in range(1, 6):
    t = i / 5.0
    send_doc_msg(s, enc_pointer(seq, 2, 0, 0, 0, 0.0, 0.0, 40.0 + 160.0 * t, 270.0 + 30.0 * t)); seq += 1
    time.sleep(0.02)
send_doc_msg(s, enc_pointer(seq, 1, 0, 0, 0, 0.0, 0.0, 200.0, 300.0)); seq += 1
time.sleep(0.2)
s.close()

dnd = eval_js('({log: window.dndLog, text: window.droppedText, parent: document.getElementById("card1").parentElement.id})')
print("dnd:", dnd)
assert dnd["log"] == ["dragstart", "dragenter", "dragover", "drop", "dragend"], f"bad dnd log: {dnd}"
assert dnd["text"] == "card1", f"bad droppedText: {dnd}"
assert dnd["parent"] == "done", f"card did not move to #done: {dnd}"

cv = eval_js('({events: window.cvEvents, ink: (() => { const d = pad.getContext("2d").getImageData(0, 0, pad.width, pad.height).data; let c = 0; for (let i = 0; i < d.length; i += 4) if (d[i+3] > 0 && d[i] > 150) c++; return c; })()})')
print("canvas:", cv)
assert cv["events"][0] == "pd:1", f"missing pointerdown: {cv}"
assert any(e == "mm:1" for e in cv["events"]), f"missing mousemove with buttons=1: {cv}"
assert cv["events"][-1] == "mu:0", f"missing mouseup: {cv}"
assert cv["ink"] > 200, f"canvas did not draw stroke: {cv}"

# 2. Navigate to print.html, verify window.print() + @media print + agent.exe pdf:<out.pdf>
run_agent(f"go:http://127.0.0.1:{web}/print.html", "wait:200")
pe = eval_js('(() => { window.print(); return window.printEvents; })()')
print("printEvents:", pe)
assert pe == ["beforeprint", "afterprint"], f"bad printEvents: {pe}"

pdf_path = os.path.join(out_dir, "out.pdf")
pdf_out = run_agent(f"pdf:{pdf_path}")
print("pdf cmd:", pdf_out.strip())
assert os.path.isfile(pdf_path), "out.pdf not written"
pdf_bytes = open(pdf_path, "rb").read()
assert pdf_bytes.startswith(b"%PDF-1.4\n"), "missing %PDF-1.4 header"
assert pdf_bytes.endswith(b"%%EOF\n"), "missing %%EOF trailer"
m = re.search(rb"/Type /Pages /Count (\d+)", pdf_bytes)
assert m and int(m.group(1)) >= 2, f"expected multi-page PDF, got {m}"
page_count = int(m.group(1))
print("pdf pages:", page_count, "bytes:", len(pdf_bytes))

# Verify screen media & viewport restored after PDF export
restored = eval_js('({screen: matchMedia("screen").matches, print: matchMedia("print").matches, nav: getComputedStyle(nav).display, phdr: getComputedStyle(phdr).display})')
print("restored:", restored)
assert restored == {"screen": True, "print": False, "nav": "block", "phdr": "none"}, f"not restored: {restored}"

# Decode every PDF RunLengthDecode image stream in pure Python and verify non-blank content on pages 1 & 2
def rle_decode(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]; i += 1
        if b == 128:
            break
        elif b < 128:
            out.extend(data[i:i + b + 1]); i += b + 1
        else:
            out.extend([data[i]] * (257 - b)); i += 1
    return out

streams = re.findall(rb"/Subtype /Image /Width (\d+) /Height (\d+) .*?/Length (\d+)>>\nstream\n", pdf_bytes, re.S)
assert len(streams) == page_count, f"expected {page_count} image streams, got {len(streams)}"
for idx, match in enumerate(re.finditer(rb"/Subtype /Image /Width (\d+) /Height (\d+) .*?/Length (\d+)>>\nstream\n", pdf_bytes, re.S)):
    pw, ph, slen = int(match.group(1)), int(match.group(2)), int(match.group(3))
    raw = pdf_bytes[match.end():match.end() + slen]
    rgb = rle_decode(raw)
    assert len(rgb) == pw * ph * 3, f"page {idx+1} decoded size mismatch: {len(rgb)} != {pw*ph*3}"
    non_white = sum(1 for k in range(0, len(rgb), 3) if rgb[k] < 245 or rgb[k+1] < 245 or rgb[k+2] < 245)
    print(f"page {idx+1}: {pw}x{ph}, non_white={non_white}")
    assert non_white > 10000, f"page {idx+1} appears blank ({non_white} non-white pixels)"
    # Verify .no-print red banner (#ef4444) is NOT present on page 1, and .print-only green banner (#16a34a) IS present!
    if idx == 0:
        red_px = sum(1 for k in range(0, len(rgb), 3) if rgb[k] > 210 and rgb[k+1] < 100 and rgb[k+2] < 100)
        green_px = sum(1 for k in range(0, len(rgb), 3) if rgb[k] < 60 and rgb[k+1] > 140 and rgb[k+2] < 100)
        print(f"page 1 @media print check: red(.no-print)={red_px}, green(.print-only)={green_px}")
        assert red_px == 0, f".no-print red banner leaked into PDF ({red_px} px)"
        assert green_px > 5000, f".print-only green banner missing in PDF ({green_px} px)"

# Also verify via pdftoppm when installed (Linux CI has poppler-utils)
if subprocess.run(["sh", "-c", "command -v pdftoppm >/dev/null 2>&1"]).returncode == 0:
    subprocess.run(["pdftoppm", "-png", "-r", "72", pdf_path, os.path.join(out_dir, "rendered")], check=True)
    pngs = sorted(f for f in os.listdir(out_dir) if f.startswith("rendered-") and f.endswith(".png"))
    print("pdftoppm pages:", pngs)
    assert len(pngs) == page_count, f"pdftoppm page count mismatch: {pngs}"

print("STAGE20 OK")
PY
echo "stage20: PASS"
