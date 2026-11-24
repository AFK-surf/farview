#!/bin/bash
# Stage 16A end-to-end integration check:
# 1. Inline PDF viewer (application/pdf without attachment): renders page JPEGs as fv-pdf://page/ Assets,
#    shows filename + page count + zero raw %PDF- text, and one-click "下载 PDF" matches original SHA-256 100%.
# 2. Attachment PDF (Content-Disposition: attachment): preserves direct download with 100% SHA-256 match.
# 3. Iframe PDF (<iframe src="/report.pdf">): renders inline PDF viewer inside sub-frame.
# 4. Real <video>/<audio> audio streaming: DocClient receives IMA-ADPCM HostMsg::Audio (tag 134) packets
#    whose decoded 48kHz stereo PCM16 has real non-silent RMS energy; mute/pause immediately sends
#    HostMsg::AudioCtl (tag 135, stop=true) and stops streaming; 0 lingering ffmpeg processes on exit.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/pdf_audio
rm -rf "$O" && mkdir -p "$O/state" "$O/www"
port=${PDF_AUDIO_PORT:-7710}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "pdf_audio: $1"; for f in "$O"/*.out "$O"/*.log; do [ -f "$f" ] && { echo "-- $f"; tail -25 "$f"; }; done; exit 1; }

# Generate a valid 2-page vector PDF in pure Python (works identically on macOS and Linux)
python3 -I - "$O/www/report.pdf" <<'PY' || fail "pdf gen failed"
import sys

def make_two_page_pdf():
    objs = []
    def add_obj(body):
        objs.append(body)
        return len(objs)
    c1 = b"BT /F1 20 Tf 50 720 Td (Farview Stage 16A Report - Page 1) Tj ET\n0.15 0.45 0.85 rg 50 600 500 80 re f\n"
    c2 = b"BT /F1 20 Tf 50 720 Td (Farview Stage 16A Report - Page 2) Tj ET\n0.10 0.65 0.45 rg 50 600 500 80 re f\n"
    add_obj(b"<</Type/Catalog/Pages 2 0 R>>")
    add_obj(b"<</Type/Pages/Count 2/Kids[3 0 R 4 0 R]>>")
    add_obj(b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 5 0 R>>>>/Contents 6 0 R>>")
    add_obj(b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 5 0 R>>>>/Contents 7 0 R>>")
    add_obj(b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>")
    add_obj(f"<</Length {len(c1)}>>\nstream\n".encode() + c1 + b"endstream")
    add_obj(f"<</Length {len(c2)}>>\nstream\n".encode() + c2 + b"endstream")
    add_obj(b"<</Title(Stage 16A Test PDF)>>")

    out = bytearray(b"%PDF-1.4\n")
    offs = [0]
    for i, body in enumerate(objs, 1):
        offs.append(len(out))
        out += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
    xref_pos = len(out)
    out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n".encode()
    for off in offs[1:]:
        out += f"{off:010d} 00000 n \n".encode()
    out += f"trailer\n<</Size {len(objs) + 1}/Root 1 0 R/Info 8 0 R>>\nstartxref\n{xref_pos}\n%%EOF\n".encode()
    return bytes(out)

open(sys.argv[1], "wb").write(make_two_page_pdf())
PY

if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -nostdin -y -v error -f lavfi -i "testsrc2=size=320x180:rate=24:duration=2" -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
    -c:v libx264 -pix_fmt yuv420p -crf 28 -c:a aac -b:a 96k -movflags +faststart "$O/www/tone.mp4" || fail "ffmpeg mp4 failed"
  ffmpeg -nostdin -y -v error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" -c:a pcm_s16le "$O/www/tone.wav" || fail "ffmpeg wav failed"
  HAS_FFMPEG=1
else
  HAS_FFMPEG=0
fi

HAS_PDF_RASTER=0
if [ "$(uname -s)" = "Darwin" ] || command -v pdftoppm >/dev/null 2>&1 || command -v mutool >/dev/null 2>&1 || command -v gs >/dev/null 2>&1; then
  HAS_PDF_RASTER=1
fi

cat > "$O/www/iframe.html" <<'HTML'
<!doctype html><html><head><title>iframe-pdf</title></head><body>
<h1>Embedded PDF</h1>
<iframe id="f" src="/report.pdf" width="600" height="400"></iframe>
</body></html>
HTML

cat > "$O/www/media.html" <<'HTML'
<!doctype html><html><head><title>stage16-audio</title></head><body>
<video id="v" controls src="/tone.mp4" width="320" height="180"></video>
<audio id="a" controls src="/tone.wav"></audio>
</body></html>
HTML

# Custom HTTP server supporting inline PDF and Content-Disposition: attachment PDF
python3 -I - "$web" "$O/www" > "$O/web.out" 2>&1 <<'PY' &
import http.server, os, sys
port, root = int(sys.argv[1]), sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = path.split("?", 1)[0]
        if path == "/download.pdf":
            return os.path.join(root, "report.pdf")
        return os.path.join(root, path.lstrip("/"))
    def end_headers(self):
        if self.path.split("?", 1)[0] == "/download.pdf":
            self.send_header("Content-Disposition", 'attachment; filename="attached-report.pdf"')
        super().end_headers()
    def log_message(self, fmt, *args):
        pass
http.server.HTTPServer(("127.0.0.1", port), H).serve_forever()
PY
W=$!
trap 'kill $H $W 2>/dev/null || true' EXIT
for _ in $(seq 80); do curl -sf "http://127.0.0.1:$web/report.pdf" > /dev/null && break; sleep 0.1; done

FV_NO_FORK=1 FV_STATE_DIR="$O/state" FV_HOST_LOG="$O/host.log" FV_TAB_SIZE=1280x800 \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/report.pdf" > "$O/host.out" 2>&1 & H=$!
for _ in $(seq 100); do [ -s "$O/state/host.json" ] && break; sleep 0.1; done
[ -s "$O/state/host.json" ] || fail "no host.json"

python3 -I - "$port" "$web" "$AGENT" "$O" "$HAS_FFMPEG" "$HAS_PDF_RASTER" > "$O/check.out" 2>&1 <<'PY' || fail "$(tail -15 "$O/check.out")"
import hashlib, json, math, os, socket, struct, subprocess, sys, time
port, web, agent, out_dir = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
has_ff, has_pdf_raster = int(sys.argv[5]), int(sys.argv[6])
orig_pdf_sha = hashlib.sha256(open(os.path.join(out_dir, "www", "report.pdf"), "rb").read()).hexdigest()

def run_agent(*cmds):
    r = subprocess.run([agent, "127.0.0.1", str(port + 1), *cmds], check=True, capture_output=True, text=True)
    return r.stdout

def eval_js(expr):
    out = run_agent("eval:" + expr)
    lines = [l for l in out.splitlines() if not l.startswith("== ")]
    return json.loads(lines[-1]) if lines else None

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
    def num(s):
        v = struct.unpack("<d", s.b[s.i:s.i + 8])[0]; s.i += 8; return v
    def flag(s):
        v = s.b[s.i] != 0; s.i += 1; return v
    def bytes(s): n = s.int(); v = s.b[s.i:s.i + n]; s.i += n; return v
    def string(s): return s.bytes().decode(errors="replace")

IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

def decode_ima_stereo(adpcm):
    assert len(adpcm) == 967, f"expected 967-byte ADPCM packet, got {len(adpcm)}"
    lp = struct.unpack_from("<h", adpcm, 0)[0]
    li = adpcm[2]
    rp = struct.unpack_from("<h", adpcm, 4)[0]
    ri = adpcm[6]
    assert 0 <= li <= 88 and 0 <= ri <= 88
    samples = [lp, rp]
    def step_one(nib, pred, idx):
        step = IMA_STEP[idx]
        d = step >> 3
        if nib & 4: d += step
        if nib & 2: d += step >> 1
        if nib & 1: d += step >> 2
        pred = pred - d if (nib & 8) else pred + d
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IMA_INDEX[nib & 0x0F]))
        return pred, idx
    for b in adpcm[8:]:
        lp, li = step_one(b & 0x0F, lp, li)
        rp, ri = step_one((b >> 4) & 0x0F, rp, ri)
        samples.append(lp)
        samples.append(rp)
    return samples

sock = socket.create_connection(("127.0.0.1", port))
sock.settimeout(0.05)
sock.sendall(framed(varint(120) + varint(1) + string("doc")) + framed(varint(121) + struct.pack("<ddd", 1280, 800, 1)))

buf = b""
pdf_assets = {}
downloads_wire = []
audio_pkts = []
audio_ctls = []

def drain(dur):
    global buf
    t0 = time.monotonic()
    while time.monotonic() - t0 < dur:
        try:
            chunk = sock.recv(1 << 20)
            if chunk:
                buf += chunk
        except socket.timeout:
            pass
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n:
                break
            body, buf = buf[4:4 + n], buf[4 + n:]
            r = R(body)
            tag = r.int()
            if tag == 103:
                k = r.string()
                _kind = r.int()
                r.i += 16
                data = r.bytes()
                if k.startswith("fv-pdf://"):
                    pdf_assets[k] = data
            elif tag == 108:
                name = r.string()
                url = r.string()
                mime = r.string()
                data = r.bytes()
                downloads_wire.append((name, url, mime, data))
            elif tag == 134:
                fmt = r.int()
                data = r.bytes()
                audio_pkts.append((fmt, data))
            elif tag == 135:
                vol = r.num()
                stop = r.flag()
                audio_ctls.append((vol, stop))

# 1. Verify inline PDF viewer rendered on initial load
drain(1.0)
pdf_info = json.loads(eval_js("""JSON.stringify({
  title: document.title,
  text: document.body.textContent,
  imgs: [...document.querySelectorAll('img')].map(i => i.getAttribute('src')),
  hasBtn: !!document.getElementById('fv-pdf-dl')
})"""))
print("inline pdf:", {k: v for k, v in pdf_info.items() if k != "text"}, "text_sample:", pdf_info["text"][:80])
assert "%PDF-" not in pdf_info["text"], "raw PDF bytes leaked into page text!"
assert "report.pdf" in pdf_info["text"], f"filename missing: {pdf_info['text']}"
assert "2 页" in pdf_info["text"], f"page count missing: {pdf_info['text']}"
assert pdf_info["hasBtn"], "download button missing"
if has_pdf_raster:
    assert len(pdf_assets) >= 2, f"expected >=2 fv-pdf:// JPEG assets, got {list(pdf_assets.keys())}"
    for k, jpg in pdf_assets.items():
        assert len(jpg) > 200 and jpg[0] == 0xFF and jpg[1] == 0xD8, f"invalid JPEG asset {k}"
    print(f"inline pdf raster OK: {len(pdf_assets)} JPEG page assets")

# 2. Click "下载 PDF" button -> verify downloaded file SHA-256 matches original 100%
eval_js("document.getElementById('fv-pdf-dl').click(); 'ok'")
drain(0.6)
dl_dir = os.path.join(out_dir, "state", "downloads")
dl_path = os.path.join(dl_dir, "report.pdf")
assert os.path.isfile(dl_path), f"downloaded report.pdf not found in {os.listdir(dl_dir) if os.path.isdir(dl_dir) else 'no dir'}"
dl_sha = hashlib.sha256(open(dl_path, "rb").read()).hexdigest()
assert dl_sha == orig_pdf_sha, f"downloaded PDF SHA mismatch: {dl_sha} != {orig_pdf_sha}"
assert any(d[0] == "report.pdf" and hashlib.sha256(d[3]).hexdigest() == orig_pdf_sha for d in downloads_wire), "DocClient did not receive Downloaded for inline PDF"
print("inline pdf download SHA-256 OK:", dl_sha[:16])

# 3. Verify Content-Disposition: attachment PDF still downloads directly without replacing page
run_agent(f"go:http://127.0.0.1:{web}/download.pdf")
drain(0.5)
att_path = os.path.join(dl_dir, "attached-report.pdf")
assert os.path.isfile(att_path), "attachment PDF was not saved"
assert hashlib.sha256(open(att_path, "rb").read()).hexdigest() == orig_pdf_sha, "attachment PDF SHA mismatch"
print("attachment pdf download OK")

# 4. Verify <iframe src="/report.pdf"> renders inline PDF viewer inside iframe
run_agent(f"go:http://127.0.0.1:{web}/iframe.html", "wait:500")
drain(0.5)
iframe_txt = eval_js("document.getElementById('f').contentDocument ? document.getElementById('f').contentDocument.body.textContent : ''")
assert "2 页" in iframe_txt and "%PDF-" not in iframe_txt, f"bad iframe PDF content: {iframe_txt[:100]}"
print("iframe pdf OK")

# 5. Verify real <video> & <audio> audio streaming over DocClient
if has_ff:
    run_agent(f"go:http://127.0.0.1:{web}/media.html", "wait:600")
    t_wait = time.monotonic()
    while time.monotonic() - t_wait < 5.0:
        rs = json.loads(eval_js("JSON.stringify({v: v.readyState, a: a.readyState})"))
        if rs["v"] == 4 and rs["a"] == 4:
            break
        time.sleep(0.05)
    assert rs["v"] == 4 and rs["a"] == 4, f"media not ready: {rs}"

    audio_pkts.clear()
    audio_ctls.clear()
    eval_js("v.play(); 'ok'")
    drain(0.8)
    assert len(audio_pkts) >= 8, f"expected >=8 Audio packets from <video>, got {len(audio_pkts)}"
    rms_vals = []
    for fmt, data in audio_pkts[:10]:
        assert fmt == 1, f"expected IMA-ADPCM fmt=1, got {fmt}"
        samples = decode_ima_stereo(data)
        assert len(samples) == 1920, f"expected 1920 samples (3840 bytes), got {len(samples)}"
        rms = math.sqrt(sum(x * x for x in samples) / len(samples))
        rms_vals.append(rms)
    max_rms = max(rms_vals)
    assert max_rms > 1000.0, f"decoded video audio looks silent (max RMS={max_rms:.1f})"
    print(f"video audio stream OK: {len(audio_pkts)} ADPCM pkts, max RMS={max_rms:.1f}")

    # Mute -> must immediately send AudioCtl(..., stop=True) and stop streaming
    audio_ctls.clear()
    eval_js("v.muted = true; 'ok'")
    drain(0.3)
    assert any(stop for (_, stop) in audio_ctls), f"expected AudioCtl stop=true on mute, got {audio_ctls}"
    cnt_after_mute = len(audio_pkts)
    drain(0.3)
    assert len(audio_pkts) == cnt_after_mute, f"audio continued streaming while muted: {cnt_after_mute} -> {len(audio_pkts)}"

    # Pause cleanup
    eval_js("v.muted = false; v.pause(); 'ok'")
    drain(0.2)

    # Test <audio> element streaming + pause stop
    audio_pkts.clear()
    audio_ctls.clear()
    eval_js("a.play(); 'ok'")
    drain(0.6)
    assert len(audio_pkts) >= 6, f"expected >=6 Audio packets from <audio>, got {len(audio_pkts)}"
    a_samples = decode_ima_stereo(audio_pkts[-1][1])
    a_rms = math.sqrt(sum(x * x for x in a_samples) / len(a_samples))
    assert a_rms > 1000.0, f"decoded <audio> looks silent (RMS={a_rms:.1f})"
    audio_ctls.clear()
    eval_js("a.pause(); 'ok'")
    drain(0.3)
    assert any(stop for (_, stop) in audio_ctls), f"expected AudioCtl stop=true on <audio> pause, got {audio_ctls}"
    print(f"audio element stream OK: {len(audio_pkts)} ADPCM pkts, RMS={a_rms:.1f}")

sock.close()
PY

# Verify zero lingering ffmpeg child processes of host
if pgrep -P "$H" ffmpeg >/dev/null 2>&1; then
  fail "lingering ffmpeg child process found under host PID $H"
fi

cat "$O/check.out"
echo "pdf_audio: PASS"
