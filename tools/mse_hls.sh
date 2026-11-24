#!/bin/bash
# Stage 17A end-to-end integration check:
# 1. MSE (MediaSource + SourceBuffer):
#    - MediaSource.isTypeSupported checks
#    - Dual-track fragmented MP4 (separate video avc1 + audio mp4a SourceBuffers) AND single muxed fMP4:
#      URL.createObjectURL(ms) -> sourceopen -> addSourceBuffer -> appendBuffer -> endOfStream() -> play()
#      verifies videoWidth/videoHeight/duration > 0, DocClient receives real MJPEG fvmedia: frames AND
#      non-silent 48kHz stereo IMA-ADPCM HostMsg::Audio packets (RMS > 1000), pause stops audio immediately.
# 2. Native HLS (.m3u8):
#    - canPlayType('application/vnd.apple.mpegurl') === 'maybe'
#    - <video src="/master.m3u8"> -> Master Playlist -> Media Playlist -> 2x .ts segment fetch -> play()
#      verifies videoWidth/duration > 0, DocClient receives real video frames & non-silent Audio packets.
# 3. Zero lingering ffmpeg child processes on exit.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/mse_hls
rm -rf "$O" && mkdir -p "$O/state" "$O/www/hls"
port=${MSE_HLS_PORT:-7730}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "mse_hls: $1"; for f in "$O"/*.out "$O"/*.log; do [ -f "$f" ] && { echo "-- $f"; tail -25 "$f"; }; done; exit 1; }

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "mse_hls: SKIP (ffmpeg not installed)"
  exit 0
fi

# Generate fragmented MP4 (dual-track separate video + audio, and single muxed track)
ffmpeg -nostdin -y -v error -f lavfi -i "testsrc2=size=320x180:rate=24:duration=2" \
  -c:v libx264 -pix_fmt yuv420p -crf 28 -movflags frag_keyframe+empty_moov+default_base_moof \
  "$O/www/vid.mp4" || fail "ffmpeg vid fMP4 failed"

ffmpeg -nostdin -y -v error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
  -c:a aac -b:a 96k -movflags frag_keyframe+empty_moov+default_base_moof \
  "$O/www/aud.mp4" || fail "ffmpeg aud fMP4 failed"

ffmpeg -nostdin -y -v error -f lavfi -i "testsrc2=size=320x180:rate=24:duration=2" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
  -c:v libx264 -pix_fmt yuv420p -crf 28 -c:a aac -b:a 96k \
  -movflags frag_keyframe+empty_moov+default_base_moof \
  "$O/www/muxed.mp4" || fail "ffmpeg muxed fMP4 failed"

# Generate 2-segment HLS (.m3u8 + .ts) + Master Playlist
ffmpeg -nostdin -y -v error -f lavfi -i "testsrc2=size=320x180:rate=24:duration=2" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
  -c:v libx264 -pix_fmt yuv420p -crf 28 -g 24 -c:a aac -b:a 96k \
  -f hls -hls_time 1 -hls_list_size 0 -hls_segment_filename "$O/www/hls/seg%d.ts" \
  "$O/www/hls/media.m3u8" || fail "ffmpeg hls failed"

cat > "$O/www/master.m3u8" <<'M3U8'
#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=320x180
hls/media.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=320x180
hls/media.m3u8
M3U8

cat > "$O/www/mse.html" <<'HTML'
<!doctype html><html><head><title>stage17-mse</title></head><body>
<video id="v" controls width="320" height="180"></video>
<script>
  window.__mseStatus = "init";
  window.__startDualMSE = async () => {
    const v = document.getElementById("v");
    const ms = new MediaSource();
    window.__ms = ms;
    const url = URL.createObjectURL(ms);
    const opened = new Promise(resolve => ms.addEventListener("sourceopen", resolve, { once: true }));
    v.src = url;
    await opened;
    const vsb = ms.addSourceBuffer('video/mp4; codecs="avc1.42E01E"');
    const asb = ms.addSourceBuffer('audio/mp4; codecs="mp4a.40.2"');
    const [vBuf, aBuf] = await Promise.all([
      fetch("/vid.mp4").then(r => r.arrayBuffer()),
      fetch("/aud.mp4").then(r => r.arrayBuffer()),
    ]);
    await new Promise(resolve => { vsb.addEventListener("updateend", resolve, { once: true }); vsb.appendBuffer(vBuf); });
    await new Promise(resolve => { asb.addEventListener("updateend", resolve, { once: true }); asb.appendBuffer(aBuf); });
    ms.endOfStream();
    window.__mseStatus = "ready";
    return JSON.stringify({
      rs: ms.readyState,
      vrs: v.readyState,
      vw: v.videoWidth,
      vh: v.videoHeight,
      dur: v.duration,
      sbs: ms.sourceBuffers.length,
      vbuf: vsb.buffered.length,
      abuf: asb.buffered.length,
      supMp4: MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E, mp4a.40.2"'),
      supWebm: MediaSource.isTypeSupported('video/webm; codecs="vp9, opus"'),
      supBad: MediaSource.isTypeSupported('video/x-unknown-codec-999'),
    });
  };
  window.__startMuxedMSE = async () => {
    const v2 = document.createElement("video");
    document.body.appendChild(v2);
    const ms = new MediaSource();
    const opened = new Promise(resolve => ms.addEventListener("sourceopen", resolve, { once: true }));
    v2.srcObject = ms;
    await opened;
    const sb = ms.addSourceBuffer('video/mp4; codecs="avc1.42E01E, mp4a.40.2"');
    const buf = await fetch("/muxed.mp4").then(r => r.arrayBuffer());
    await new Promise(resolve => { sb.addEventListener("updateend", resolve, { once: true }); sb.appendBuffer(buf); });
    ms.endOfStream();
    const out = { rs: ms.readyState, vrs: v2.readyState, vw: v2.videoWidth, vh: v2.videoHeight, dur: v2.duration };
    v2.remove();
    return JSON.stringify(out);
  };
</script>
</body></html>
HTML

cat > "$O/www/hls.html" <<'HTML'
<!doctype html><html><head><title>stage17-hls</title></head><body>
<video id="hv" controls src="/master.m3u8" width="320" height="180"></video>
</body></html>
HTML

python3 -I -m http.server "$web" --bind 127.0.0.1 --directory "$O/www" > "$O/web.out" 2>&1 & W=$!
trap 'kill $H $W 2>/dev/null || true' EXIT
for _ in $(seq 80); do curl -sf "http://127.0.0.1:$web/mse.html" > /dev/null && break; sleep 0.1; done

FV_NO_FORK=1 FV_STATE_DIR="$O/state" FV_HOST_LOG="$O/host.log" FV_TAB_SIZE=1280x800 \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/mse.html" > "$O/host.out" 2>&1 & H=$!
for _ in $(seq 100); do [ -s "$O/state/host.json" ] && break; sleep 0.1; done
[ -s "$O/state/host.json" ] || fail "no host.json"

python3 -I - "$port" "$web" "$AGENT" > "$O/check.out" 2>&1 <<'PY' || fail "$(tail -15 "$O/check.out")"
import json, math, socket, struct, subprocess, sys, time
port, web, agent = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]

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
    lp = struct.unpack_from("<h", adpcm, 0)[0]; li = adpcm[2]
    rp = struct.unpack_from("<h", adpcm, 4)[0]; ri = adpcm[6]
    samples = [lp, rp]
    def step_one(nib, pred, idx):
        step = IMA_STEP[idx]
        d = step >> 3
        if nib & 4: d += step
        if nib & 2: d += step >> 1
        if nib & 1: d += step >> 2
        pred = max(-32768, min(32767, pred - d if (nib & 8) else pred + d))
        idx = max(0, min(88, idx + IMA_INDEX[nib & 0x0F]))
        return pred, idx
    for b in adpcm[8:]:
        lp, li = step_one(b & 0x0F, lp, li)
        rp, ri = step_one((b >> 4) & 0x0F, rp, ri)
        samples.append(lp); samples.append(rp)
    return samples

def max_audio_rms(pkts):
    vals = []
    for fmt, data in pkts[:12]:
        assert fmt == 1, f"expected IMA-ADPCM fmt=1, got {fmt}"
        s = decode_ima_stereo(data)
        vals.append(math.sqrt(sum(x * x for x in s) / len(s)))
    return max(vals) if vals else 0.0

sock = socket.create_connection(("127.0.0.1", port))
sock.settimeout(0.05)
sock.sendall(framed(varint(120) + varint(1) + string("doc")) + framed(varint(121) + struct.pack("<ddd", 1280, 800, 1)))

buf = b""
video_frames = []
audio_pkts = []
audio_ctls = []

def drain(dur):
    global buf
    t0 = time.monotonic()
    while time.monotonic() - t0 < dur:
        try:
            chunk = sock.recv(1 << 20)
            if chunk: buf += chunk
        except socket.timeout:
            pass
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n: break
            body, buf = buf[4:4 + n], buf[4 + n:]
            r = R(body)
            tag = r.int()
            if tag == 103:
                k = r.string(); _kind = r.int(); r.i += 16; data = r.bytes()
                if k.startswith("fvmedia:") and len(data) >= 2 and data[0] == 0xFF and data[1] == 0xD8:
                    video_frames.append(k)
            elif tag == 134:
                audio_pkts.append((r.int(), r.bytes()))
            elif tag == 135:
                audio_ctls.append((r.num(), r.flag()))

drain(0.5)

# 1. Dual-track MSE (separate video & audio SourceBuffers)
mse_info = json.loads(eval_js("window.__startDualMSE()"))
print("dual MSE:", mse_info)
assert mse_info["rs"] == "ended" and mse_info["vrs"] == 4, f"bad MSE state: {mse_info}"
assert mse_info["vw"] == 320 and mse_info["vh"] == 180, f"bad MSE video size: {mse_info}"
assert 1.8 <= mse_info["dur"] <= 2.4, f"bad MSE duration: {mse_info}"
assert mse_info["sbs"] == 2 and mse_info["vbuf"] >= 1 and mse_info["abuf"] >= 1, f"bad SourceBuffers: {mse_info}"
assert mse_info["supMp4"] is True and mse_info["supWebm"] is True and mse_info["supBad"] is False, f"bad isTypeSupported: {mse_info}"

video_frames.clear(); audio_pkts.clear(); audio_ctls.clear()
eval_js("document.getElementById('v').play(); 'ok'")
drain(1.0)
assert len(video_frames) >= 8, f"expected >=8 MSE video frames, got {len(video_frames)}"
assert len(audio_pkts) >= 8, f"expected >=8 MSE audio pkts, got {len(audio_pkts)}"
mse_rms = max_audio_rms(audio_pkts)
assert mse_rms > 1000.0, f"MSE audio looks silent (RMS={mse_rms:.1f})"
print(f"dual MSE playback OK: {len(video_frames)} JPEG frames, {len(audio_pkts)} ADPCM pkts, RMS={mse_rms:.1f}")

audio_ctls.clear()
eval_js("document.getElementById('v').pause(); 'ok'")
drain(0.3)
assert any(stop for (_, stop) in audio_ctls), f"expected AudioCtl stop=true on MSE pause, got {audio_ctls}"
cnt_paused = len(audio_pkts)
drain(0.3)
assert len(audio_pkts) == cnt_paused, f"MSE audio continued after pause: {cnt_paused} -> {len(audio_pkts)}"

# 2. Single muxed-track MSE via srcObject
mux_info = json.loads(eval_js("window.__startMuxedMSE()"))
print("muxed MSE (srcObject):", mux_info)
assert mux_info["rs"] == "ended" and mux_info["vrs"] == 4 and mux_info["vw"] == 320 and mux_info["dur"] >= 1.8, f"bad muxed MSE: {mux_info}"

# 3. Native HLS (.m3u8 Master -> Media -> .ts segments)
run_agent(f"go:http://127.0.0.1:{web}/hls.html", "wait:500")
t_hls = time.monotonic()
hls_info = None
while time.monotonic() - t_hls < 6.0:
    hls_info = json.loads(eval_js("""(() => {
      const v = document.getElementById('hv');
      return JSON.stringify({
        cpt: v.canPlayType('application/vnd.apple.mpegurl'),
        cpt2: v.canPlayType('application/x-mpegurl'),
        rs: v.readyState, vw: v.videoWidth, vh: v.videoHeight, dur: v.duration
      });
    })()"""))
    if hls_info["rs"] == 4:
        break
    time.sleep(0.05)
print("HLS info:", hls_info)
assert hls_info["cpt"] == "maybe" and hls_info["cpt2"] == "maybe", f"bad HLS canPlayType: {hls_info}"
assert hls_info["rs"] == 4 and hls_info["vw"] == 320 and hls_info["vh"] == 180 and hls_info["dur"] >= 1.8, f"bad HLS meta: {hls_info}"

video_frames.clear(); audio_pkts.clear(); audio_ctls.clear()
eval_js("document.getElementById('hv').play(); 'ok'")
drain(1.0)
assert len(video_frames) >= 8, f"expected >=8 HLS video frames, got {len(video_frames)}"
assert len(audio_pkts) >= 8, f"expected >=8 HLS audio pkts, got {len(audio_pkts)}"
hls_rms = max_audio_rms(audio_pkts)
assert hls_rms > 1000.0, f"HLS audio looks silent (RMS={hls_rms:.1f})"
print(f"HLS playback OK: {len(video_frames)} JPEG frames, {len(audio_pkts)} ADPCM pkts, RMS={hls_rms:.1f}")

eval_js("document.getElementById('hv').pause(); 'ok'")
drain(0.25)
sock.close()
PY

if pgrep -P "$H" ffmpeg >/dev/null 2>&1; then
  fail "lingering ffmpeg child process found under host PID $H"
fi

cat "$O/check.out"
echo "mse_hls: PASS"
