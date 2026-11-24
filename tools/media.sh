#!/bin/bash
# Stage 12A host integration check:
# 1. Real <video>/<audio> loading, metadata probing (videoWidth/videoHeight/duration), canPlayType,
#    event sequence (loadstart -> loadedmetadata -> loadeddata -> canplay -> canplaythrough),
#    play() advancing currentTime + streaming continuous fvmedia: frames to DocClient, seek, pause, ended.
# 2. Chrome 152 anti-bot JS fingerprint consistency (16 assertions).
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/media
rm -rf "$O" && mkdir -p "$O/state" "$O/www"
port=${MEDIA_PORT:-7690}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "media: $1"; for f in "$O"/*.out; do [ -f "$f" ] && { echo "-- $f"; tail -25 "$f"; }; done; exit 1; }

if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -nostdin -y -v error -f lavfi -i "testsrc2=size=320x180:rate=24:duration=2" -c:v libx264 -pix_fmt yuv420p -crf 28 -movflags +faststart "$O/www/test.mp4" || fail "ffmpeg mp4 failed"
  ffmpeg -nostdin -y -v error -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=2" -c:a pcm_s16le "$O/www/test.wav" || fail "ffmpeg wav failed"
  ffmpeg -nostdin -y -v error -f lavfi -i "color=c=#1e293b:s=320x180:d=1" -frames:v 1 "$O/www/poster.png" || fail "ffmpeg poster failed"
  HAS_FFMPEG=1
else
  # Minimal valid 1-second silent WAV fallback if ffmpeg is absent
  python3 -I - "$O/www/test.wav" <<'PY'
import struct, sys
sr, dur = 8000, 1
data = b"\x00\x00" * (sr * dur)
hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16) + b"data" + struct.pack("<I", len(data))
open(sys.argv[1], "wb").write(hdr + data)
PY
  HAS_FFMPEG=0
fi

cat > "$O/www/page.html" <<'HTML'
<!doctype html><html><head><title>stage12-media</title></head><body>
<video id="v" controls poster="/poster.png" src="/test.mp4" width="320" height="180"></video>
<audio id="a" controls src="/test.wav"></audio>
<script>
  const v = document.getElementById('v');
  const a = document.getElementById('a');
  window.__evs = [];
  for (const n of ['loadstart','loadedmetadata','loadeddata','canplay','canplaythrough','play','playing','timeupdate','seeking','seeked','pause','ended']) {
    v.addEventListener(n, () => window.__evs.push(n));
  }
</script>
</body></html>
HTML

python3 -I -m http.server "$web" --bind 127.0.0.1 --directory "$O/www" > "$O/web.out" 2>&1 & W=$!
trap 'kill $H $W 2>/dev/null || true' EXIT
for _ in $(seq 80); do curl -sf "http://127.0.0.1:$web/page.html" > /dev/null && break; sleep 0.1; done

FV_NO_FORK=1 FV_STATE_DIR="$O/state" FV_HOST_LOG="$O/host.log" FV_TAB_SIZE=1280x800 \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/page.html" > "$O/host.out" 2>&1 & H=$!
for _ in $(seq 100); do [ -s "$O/state/host.json" ] && break; sleep 0.1; done
[ -s "$O/state/host.json" ] || fail "no host.json"

python3 -I - "$port" "$AGENT" "$HAS_FFMPEG" > "$O/check.out" 2>&1 <<'PY' || fail "$(tail -10 "$O/check.out")"
import json, socket, struct, subprocess, sys, time
port, agent, has_ff = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])

def eval_js(expr):
    r = subprocess.run([agent, "127.0.0.1", str(port + 1), "eval:" + expr], check=True, capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if not l.startswith("== ")]
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
    def bytes(s): n = s.int(); v = s.b[s.i:s.i + n]; s.i += n; return v
    def string(s): return s.bytes().decode(errors="replace")

sock = socket.create_connection(("127.0.0.1", port)); sock.settimeout(0.05)
sock.sendall(framed(varint(120) + varint(1) + string("doc")) + framed(varint(121) + struct.pack("<ddd", 1280, 800, 1)))
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

# Part 1: verify async media metadata & canPlayType (wait up to 5s for async readyState === 4)
t_meta = time.monotonic()
info = None
while time.monotonic() - t_meta < 5.0:
    info = json.loads(eval_js("""JSON.stringify({
      mp4: v.canPlayType('video/mp4'),
      mp4c: v.canPlayType('video/mp4; codecs="avc1.42E01E"'),
      wav: a.canPlayType('audio/wav'),
      vw: v.videoWidth, vh: v.videoHeight, vdur: v.duration, vrs: v.readyState,
      adur: a.duration, ars: a.readyState, evs: window.__evs,
      vbuf: v.buffered.length, abuf: a.buffered.length
    })"""))
    if info["ars"] == 4 and (not has_ff or info["vrs"] == 4):
        break
    time.sleep(0.05)
print("media info:", info)
assert info["mp4"] == "maybe" and info["mp4c"] == "probably", f"bad canPlayType mp4: {info}"
assert info["wav"] == "maybe", f"bad canPlayType wav: {info}"
assert info["ars"] == 4 and info["adur"] >= 0.9, f"bad audio meta: {info}"
if has_ff:
    assert info["vw"] == 320 and info["vh"] == 180, f"bad video size: {info}"
    assert 1.8 <= info["vdur"] <= 2.3, f"bad video duration: {info}"
    assert info["vrs"] == 4 and info["vbuf"] >= 1, f"bad video readyState: {info}"
    for ev in ("loadstart", "loadedmetadata", "loadeddata", "canplay", "canplaythrough"):
        assert ev in info["evs"], f"missing event {ev}: {info['evs']}"

    # Part 2: play() -> advance currentTime & receive persistent MJPEG stream fvmedia: frames over DocClient
    eval_js("v.play(); 'ok'")
    media_keys = []
    jpeg_frames = 0
    t1 = time.monotonic()
    while time.monotonic() - t1 < 1.5:
        try: buf += sock.recv(1 << 20)
        except socket.timeout: pass
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n: break
            body, buf = buf[4:4 + n], buf[4 + n:]
            r = R(body)
            if r.int() == 103:
                k = r.string()
                _kind = r.int()
                r.i += 16  # skip w, h doubles
                data = r.bytes()
                if k.startswith("fvmedia:"):
                    media_keys.append(k)
                    if len(data) >= 2 and data[0] == 0xFF and data[1] == 0xD8:
                        jpeg_frames += 1
    assert len(media_keys) >= 15, f"expected >=15 streaming fvmedia frames in 1.5s, got {len(media_keys)}: {media_keys[:5]}"
    assert jpeg_frames == len(media_keys), f"expected all MJPEG frames, got {jpeg_frames}/{len(media_keys)}"
    st_play = json.loads(eval_js("JSON.stringify({ct: v.currentTime, paused: v.paused, evs: window.__evs})"))
    assert st_play["ct"] > 0.1, f"currentTime did not advance: {st_play}"
    print(f"media playback OK: {len(media_keys)} JPEG frames in 1.5s ({media_keys[0]}..{media_keys[-1]}), ct={st_play['ct']:.2f}s")

    # Part 3: seek & pause
    eval_js("v.pause(); v.currentTime = 1.2; 'ok'")
    st_seek = json.loads(eval_js("JSON.stringify({ct: v.currentTime, paused: v.paused})"))
    assert st_seek["paused"] is True and abs(st_seek["ct"] - 1.2) < 0.05, f"bad seek/pause: {st_seek}"

    # Part 4: play to end -> ended event
    eval_js("v.currentTime = 1.85; v.play(); 'ok'")
    t2 = time.monotonic()
    ended_ok = False
    while time.monotonic() - t2 < 3.0:
        time.sleep(0.15)
        st_end = json.loads(eval_js("JSON.stringify({ct: v.currentTime, paused: v.paused, ended: v.ended, evs: window.__evs})"))
        if st_end["ended"] and st_end["paused"] and "ended" in st_end["evs"]:
            ended_ok = True
            break
    assert ended_ok, f"video did not reach ended: {st_end}"
    print("media seek/pause/ended OK")

# Part 5: Chrome 152 anti-bot JS fingerprint (16 assertions)
fp = json.loads(eval_js("""(() => {
  const checks = [];
  const ok = (name, cond, detail) => checks.push({ name, pass: !!cond, detail: detail || "" });
  const wdDesc = Object.getOwnPropertyDescriptor(Navigator.prototype, "webdriver");
  ok("navigator_proto_webdriver", !!wdDesc && typeof wdDesc.get === "function" && !Object.prototype.hasOwnProperty.call(navigator, "webdriver") && navigator.webdriver === false);
  const navProps = ["plugins", "mimeTypes", "languages", "hardwareConcurrency", "deviceMemory", "platform", "vendor", "userAgent", "maxTouchPoints", "pdfViewerEnabled"];
  ok("navigator_proto_chain", navProps.every(k => !Object.prototype.hasOwnProperty.call(navigator, k) && !!Object.getOwnPropertyDescriptor(Navigator.prototype, k)));
  ok("plugins_instance", typeof PluginArray === "function" && navigator.plugins instanceof PluginArray && navigator.plugins.length === 5);
  let pIter = 0; for (const p of navigator.plugins) pIter++;
  const pdfP = navigator.plugins.namedItem("Chrome PDF Viewer");
  ok("plugins_api", pIter === 5 && !!pdfP && pdfP instanceof Plugin && navigator.plugins[0].name === "PDF Viewer");
  const mt = navigator.mimeTypes;
  const pdfM = mt.namedItem("application/pdf");
  ok("mimeTypes_api", mt instanceof MimeTypeArray && mt.length === 2 && !!pdfM && pdfM.enabledPlugin === navigator.plugins[0]);
  const ch = window.chrome;
  ok("window_chrome", !!ch && !!ch.runtime && !!ch.runtime.OnInstalledReason && !!ch.runtime.PlatformOs && typeof ch.loadTimes === "function" && typeof ch.csi === "function" && !!ch.app);
  ok("permissions_exists", !!(navigator.permissions && typeof navigator.permissions.query === "function"));
  const gl = document.createElement("canvas").getContext("webgl");
  const dbg = gl && gl.getExtension("WEBGL_debug_renderer_info");
  ok("webgl_unmasked", !!dbg && gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL).includes("Google Inc.") && gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL).includes("ANGLE"));
  const exts = gl.getSupportedExtensions();
  const prec = gl.getShaderPrecisionFormat(gl.VERTEX_SHADER, gl.HIGH_FLOAT);
  ok("webgl_exts_prec", Array.isArray(exts) && exts.length >= 20 && !!prec && prec.precision === 23);
  ok("outer_dimensions", window.outerWidth >= window.innerWidth && window.outerHeight >= window.innerHeight + 70);
  ok("screen_props", screen.width >= window.innerWidth && screen.height >= window.innerHeight && screen.availWidth > 0 && screen.availHeight > 0 && screen.colorDepth === 24 && screen.pixelDepth === 24);
  const conn = navigator.connection;
  ok("navigator_connection", !!conn && conn.effectiveType === "4g" && conn.rtt === 50 && conn.downlink === 10 && conn.saveData === false);
  const nativeFns = [navigator.javaEnabled, window.chrome.csi, navigator.permissions.query, Object.getOwnPropertyDescriptor(Navigator.prototype, "webdriver").get];
  ok("fn_toString_native", nativeFns.every(fn => /^function\s+[\w\s]*\(\)\s*\{\s*\[native code\]\s*\}$/.test(Function.prototype.toString.call(fn))));
  ok("ua_chrome152", /Chrome\/152\.0\.0\.0/.test(navigator.userAgent) && navigator.vendor === "Google Inc.");
  ok("ua_data_chrome152", !!navigator.userAgentData && navigator.userAgentData.brands.some(b => b.brand === "Google Chrome" && b.version === "152"));
  ok("host_only_hidden", !("__mediaAutoLoad" in window) && !("__mediaReady" in window) && !("__mediaTick" in window));
  return Promise.all(["notifications", "clipboard-read", "clipboard-write", "geolocation", "camera", "microphone"].map(name =>
    navigator.permissions.query({ name }).then(st => !!st && typeof st.state === "string" && typeof st.addEventListener === "function")
  )).then(rs => {
    ok("permissions_query_all", rs.length === 6 && rs.every(Boolean));
    return JSON.stringify({ passed: checks.filter(c => c.pass).length, total: checks.length, failed: checks.filter(c => !c.pass) });
  });
})()"""))
print(f"chrome152 fingerprint: {fp['passed']}/{fp['total']}")
assert fp["passed"] == fp["total"], f"fingerprint failures: {fp['failed']}"

# Part 6: bad/404 media URL must load asynchronously without blocking and fire error
bad = json.loads(eval_js("""new Promise(resolve => {
  const b = document.createElement('video');
  let errFired = false;
  b.addEventListener('error', () => {
    errFired = true;
    resolve(JSON.stringify({ rs: b.readyState, ns: b.networkState, err: errFired }));
  });
  b.setAttribute('src', '/does-not-exist-404.mp4');
  b.load();
  setTimeout(() => resolve(JSON.stringify({ rs: b.readyState, ns: b.networkState, err: errFired })), 2000);
})"""))
print("bad media check:", bad)
assert bad["rs"] == 0 and bad["err"] is True, f"404 media should not succeed: {bad}"

# Part 7: async play() + seek before download finishes must auto-start at seek target
if has_ff:
    race = json.loads(eval_js("""new Promise(resolve => {
      const r = document.createElement('video');
      document.body.appendChild(r);
      r.setAttribute('src', '/test.mp4?race=1');
      r.load();
      r.currentTime = 0.6;
      r.play();
      r.addEventListener('canplaythrough', () => {
        setTimeout(() => {
          const out = { rs: r.readyState, paused: r.paused, ct: r.currentTime };
          r.pause();
          r.remove();
          resolve(JSON.stringify(out));
        }, 120);
      });
      setTimeout(() => resolve(JSON.stringify({ rs: r.readyState, paused: r.paused, ct: r.currentTime, timeout: true })), 3000);
    })"""))
    print("async play/seek race check:", race)
    assert race["rs"] == 4 and race["paused"] is False and race["ct"] >= 0.55, f"bad async play/seek race: {race}"
sock.close()
PY

# Verify zero lingering ffmpeg child processes of host
if pgrep -P "$H" ffmpeg >/dev/null 2>&1; then
  fail "lingering ffmpeg child process found under host PID $H"
fi

cat "$O/check.out"
echo "media: PASS"
