#!/bin/bash
# What a page's fonts and pictures are asked for with, against a built host (host --tab, pictures after the first frame)
# and wpt.exe (pictures before it): a server here answering each font 100 ms late. The page's six @font-face files
# asked for together (all six come in less than one delay; one after another they took six); a picture's sources of
# AVIF and WebP passed over for its img's PNG; pictures (img, background) asked for with an Accept with WebP and
# without AVIF, the document with Chrome's own and a script's fetch not with the pictures'. Fails on the first that is not so.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/fetchahead
rm -rf $O && mkdir -p $O/state $O/www
port=${FETCHAHEAD_PORT:-7790}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
WPT=${WPT:-$B/pion-wpt}
DELAY_MS=${DELAY_MS:-100}
fail() { echo "fetchahead: $1"; for f in $O/*.out $O/requests.log; do echo "-- $f"; tail -30 $f; done; exit 1; }
font=""
for f in /System/Library/Fonts/Supplemental/Arial.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
  /usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf; do
  [ -f "$f" ] && { font=$f; break; }
done
[ -n "$font" ] || fail "no TrueType font here to serve"
cp "$font" $O/www/font.ttf
{
  echo '<!doctype html><html><head><title>loading</title><style>'
  for i in 1 2 3 4 5 6; do
    echo "@font-face { font-family: W; font-weight: ${i}00; src: url(/f$i.ttf) format(\"truetype\"); }"
  done
  echo '#bg { width: 10px; height: 10px; background-image: url(/c.png) }'
  echo 'body { font-family: W }</style></head><body>'
  for i in 1 2 3 4 5 6; do echo "<p style=\"font-weight: ${i}00\">Weight ${i}00 text</p>"; done
  echo '<picture><source type="image/avif" srcset="/a.avif"><source type="image/webp" srcset="/a.webp"><img id="p" src="/a.png"></picture>'
  echo '<img src="/b.png"><div id="bg"></div>'
  echo '<script>fetch("/x.json").then(r => r.json()).then(j => { document.title = "ready " + j.ok })</script>'
  echo '</body></html>'
} > $O/www/page.html
# a server: each request's time, path and Accept in requests.log; the fonts late
python3 -I - $web $O/www $O/requests.log $DELAY_MS > $O/web.out 2>&1 <<'PY' & W=$!
import http.server, os, sys, time, zlib, struct
D, LOG, DELAY = sys.argv[2], sys.argv[3], int(sys.argv[4]) / 1000
def png():
    c = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return b"\x89PNG\r\n\x1a\n" + c(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) + \
        c(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff")) + c(b"IEND", b"")
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(s, *a): pass
    def answer(s, body, ctype):
        s.send_response(200); s.send_header("Content-Type", ctype); s.send_header("Content-Length", str(len(body)))
        s.send_header("Cache-Control", "no-store"); s.end_headers(); s.wfile.write(body)
    def do_GET(s):
        p = s.path.split("?")[0]
        with open(LOG, "a") as f:
            f.write("%.3f\t%s\t%s\n" % (time.time(), p, s.headers.get("Accept", "")))
        if p.startswith("/f") and p.endswith(".ttf"):
            time.sleep(DELAY)
            return s.answer(open(os.path.join(D, "font.ttf"), "rb").read(), "font/ttf")
        if p.endswith(".png"): return s.answer(png(), "image/png")
        if p.endswith(".avif") or p.endswith(".webp"): return s.answer(b"not drawn here", "image/" + p.rsplit(".", 1)[1])
        if p == "/x.json": return s.answer(b'{"ok": 1}', "application/json")
        if p == "/page.html": return s.answer(open(os.path.join(D, "page.html"), "rb").read(), "text/html")
        s.send_response(404); s.send_header("Content-Length", "0"); s.end_headers()
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
trap 'kill $W ${H:-} 2>/dev/null' EXIT
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$web/x.json" > /dev/null && break; sleep 0.1; done
PAGE="http://127.0.0.1:$web/page.html"
PICTURE_ACCEPT='image/webp,image/png,image/jpeg,image/gif,image/svg+xml,image/*;q=0.8,*/*;q=0.5'
# the requests of one run (after the server's own check), as asked for
check() {
  local run=$1 log=$O/req-$1.log pictures=$2
  awk -F'\t' 'NR > 1' $O/requests.log > $log
  : > $O/requests.log
  local doc
  doc=$(awk -F'\t' '$2 == "/page.html" { print $3; exit }' $log)
  case "$doc" in *text/html*image/avif*) ;; *) fail "$run: the document not asked for with Chrome's Accept: '$doc'" ;; esac
  for p in $pictures; do
    local a
    a=$(awk -F'\t' -v p=$p '$2 == p { print $3; exit }' $log)
    [ "$a" = "$PICTURE_ACCEPT" ] || fail "$run: $p asked for with '$a', not the pictures' Accept"
  done
  grep -qE $'\t/a\\.(avif|webp)\t' $log && fail "$run: a picture's AVIF or WebP source asked for: $(grep -E $'\t/a\\.(avif|webp)\t' $log)"
  local js
  js=$(awk -F'\t' '$2 == "/x.json" { print $3; exit }' $log)
  [ -n "$js" ] && [ "$js" != "$PICTURE_ACCEPT" ] || fail "$run: the script's fetch asked for with '$js'"
  local n spread
  n=$(awk -F'\t' '$2 ~ /^\/f[1-6]\.ttf$/' $log | wc -l | tr -d ' ')
  [ "$n" = 6 ] || fail "$run: $n of the 6 font files asked for"
  spread=$(awk -F'\t' '$2 ~ /^\/f[1-6]\.ttf$/ { if (!lo || $1 < lo) lo = $1; if ($1 > hi) hi = $1 } END { printf "%d", (hi - lo) * 1000 }' $log)
  echo "fetchahead: $run: the 6 fonts asked for within $spread ms (each answered $DELAY_MS ms late)"
  [ "$spread" -lt "$DELAY_MS" ] || fail "$run: the fonts asked for one after another ($spread ms from the first to the last)"
}
: > $O/requests.log
curl -sf "http://127.0.0.1:$web/x.json" > /dev/null
# pictures after the first frame: the host
FV_STATE_DIR=$O/state FV_HOST_LOG=$O/host.log $HOST --tab $port "$PAGE" >> $O/host.out 2>&1 & H=$!
for _ in $(seq 100); do [ -s $O/state/host.json ] && break; sleep 0.1; done
[ -s $O/state/host.json ] || fail "no host.json"
A() { $AGENT 127.0.0.1 $((port+1)) "$@"; }
for _ in $(seq 50); do A 'eval:document.title' 2>/dev/null | grep -qx '"ready 1"' && break; sleep 0.2; done
A 'eval:document.title' | grep -qx '"ready 1"' || fail "the page did not load"
# (drawn: laid out, then its pictures got)
A "shot:$O/shot1.png" 'wait:1000' "shot:$O/shot2.png" > $O/shot.out 2>&1
for _ in $(seq 50); do grep -q $'\t/c.png\t' $O/requests.log && grep -q $'\t/a.png\t' $O/requests.log && break; sleep 0.2; done
kill $H 2>/dev/null; wait $H 2>/dev/null; H=
check host "/a.png /b.png /c.png"
# pictures before the first frame: wpt.exe (laid out, not drawn: a background's picture is got when drawn)
curl -sf "http://127.0.0.1:$web/x.json" > /dev/null
FV_WAIT=500 $WPT "$PAGE" 'document.body.offsetHeight && document.title' > $O/wpt.out 2>&1 || fail "wpt.exe failed"
grep -q 'ready 1' $O/wpt.out || fail "wpt.exe: the page did not load"
check wpt "/a.png /b.png"
echo "fetchahead: ok"
