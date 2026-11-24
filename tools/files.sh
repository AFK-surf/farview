#!/bin/bash
# Files in and out of a tab (host --tab) against a built host, as an agent's browser must: files chosen for a file
# input (agent.exe upload:) read by the page as chosen ones (input.files, their bytes, FormData, input and change) and
# sent in a form's multipart POST byte for byte; and what the page asks to have saved, not shown, saved in the tab's
# downloads directory and listed by agent.exe's downloads: a data: link with download, a blob: one a script makes and
# lets go of at once, a link answered with Content-Disposition: attachment (its filename* name), a link with download to
# the server; the page staying where it is. Fails on the first thing that is not so.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/files
rm -rf $O && mkdir -p $O/state $O/www
port=${FILES_PORT:-7590}
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "files: $1"; for f in $O/*.out; do echo "-- $f"; tail -20 $f; done; exit 1; }
cp tests/host/files.html $O/www/
printf 'Attachment one: \xe4\xbd\xa0\xe5\xa5\xbd world\n' > $O/note.txt
head -c 5000 /dev/urandom > $O/blob.bin
# a server: the page, a report answered as an attachment, a CSV, and what a POST sent kept
python3 -I - $web $O/www > $O/web.out 2>&1 <<'PY' & W=$!
import http.server, os, sys
D = sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s, *a, **k): super().__init__(*a, directory=D, **k)
    def answer(s, body, ctype, more=()):
        s.send_response(200); s.send_header("Content-Type", ctype); s.send_header("Content-Length", str(len(body)))
        for k, v in more: s.send_header(k, v)
        s.end_headers(); s.wfile.write(body)
    def do_GET(s):
        if s.path == "/report":
            return s.answer(b"%PDF-1.4 fake report body\n", "application/pdf", [("Content-Disposition", "attachment; filename=\"report.pdf\"; filename*=UTF-8''%E6%8A%A5%E5%91%8A.pdf")])
        if s.path == "/plain.csv":
            return s.answer(b"a,b\n3,4\n", "text/csv")
        return super().do_GET()
    def do_POST(s):
        body = s.rfile.read(int(s.headers.get("Content-Length", 0)))
        open(os.path.join(D, "post.bin"), "wb").write(body)
        s.answer(b"<html><body><pre id=r>got %d bytes</pre></body></html>" % len(body), "text/html")
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$web/files.html" > /dev/null && break; sleep 0.1; done
FV_STATE_DIR=$O/state FV_HOST_LOG=$O/host.log $HOST --tab $port "http://127.0.0.1:$web/files.html" >> $O/host.out 2>&1 & H=$!
trap 'kill $H $W 2>/dev/null' EXIT
for _ in $(seq 100); do [ -s $O/state/host.json ] && break; sleep 0.1; done
[ -s $O/state/host.json ] || fail "no host.json"
A() { $AGENT 127.0.0.1 $((port+1)) "$@"; }
for _ in $(seq 50); do A 'eval:document.title' 2>/dev/null | grep -qx '"files"' && break; sleep 0.2; done
A 'eval:document.title' | grep -qx '"files"' || fail "the page did not load"
# upload: the page reads the files as chosen ones
A "upload:#up=$O/note.txt,$O/blob.bin" 'wait:300' 'eval:document.getElementById("out").textContent' > $O/up.out 2>&1
sum() { python3 -I -c 'import sys; d=open(sys.argv[1],"rb").read(); print(len(d), sum(d) % 65536)' "$1"; }
read nn ns < <(sum $O/note.txt); read bn bs < <(sum $O/blob.bin)
grep -q '"input,change\\n' $O/up.out || fail "no input and change events: $(tail -1 $O/up.out)"
grep -q "note.txt|text/plain|$nn|$ns|Attachment o" $O/up.out || fail "note.txt not read as chosen: $(tail -1 $O/up.out)"
grep -q "blob.bin||$bn|$bs|" $O/up.out || fail "blob.bin's bytes not the file's: $(tail -1 $O/up.out)"
grep -q "formdata:note.txt:$nn,blob.bin:$bn" $O/up.out || fail "FormData does not hold them: $(tail -1 $O/up.out)"
# the form sent: the files' bytes in its multipart body
A 'click:#send' 'wait:1500' 'eval:document.body.textContent' > $O/post.out 2>&1
python3 -I -c 'import sys; p=open(sys.argv[1],"rb").read(); assert open(sys.argv[2],"rb").read() in p and open(sys.argv[3],"rb").read() in p and b"filename=\"blob.bin\"" in p' \
  $O/www/post.bin $O/note.txt $O/blob.bin || fail "the POST did not carry the files"
# downloads: saved, listed, the page staying
A "go:http://127.0.0.1:$web/files.html" 'click:#d1' 'click:#d2' 'click:#d3' 'wait:500' 'click:#d4' 'wait:300' 'eval:location.href' downloads > $O/down.out 2>&1
grep -qx "\"http://127.0.0.1:$web/files.html\"" $O/down.out || fail "the page did not stay: $(grep -A1 eval: $O/down.out)"
D=$O/state/downloads
[ "$(cat "$D/hello.txt")" = "Hello from a data link 你好" ] || fail "the data: link was not saved"
[ "$(printf 'col1,col2\n1,2\n')" = "$(cat "$D/made-by-script.csv")" ] || fail "the blob: link was not saved"
grep -q "fake report body" "$D/报告.pdf" || fail "the attachment was not saved under its filename*: $(ls $D)"
[ "$(cat "$D/plain.csv")" = "$(printf 'a,b\n3,4')" ] || fail "the server's download was not saved"
for n in hello.txt made-by-script.csv 报告.pdf plain.csv; do grep -q "\"name\":\"$n\"" $O/down.out || fail "downloads does not list $n"; done
# a person's device (a client of the document's wire, fvclient/proto): a hidden file input a page's button clicks asks
# it for its picker (PickFile), what it chose is the input's (UploadFiles), a picker shut with nothing is the input's
# cancel; what the page saves is sent it (Downloaded)
python3 -I - $port "$AGENT" "http://127.0.0.1:$web/files.html" > $O/doc.out 2>&1 <<'PY' || fail "a doc client's picker and download: $(tail -3 $O/doc.out)"
import socket, struct, subprocess, sys, time
port, agent, page = int(sys.argv[1]), sys.argv[2], sys.argv[3]
def varint(v):
    u = ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF; out = b""
    while u >= 0x80: out += bytes([(u & 0x7F) | 0x80]); u >>= 7
    return out + bytes([u])
def string(s): b = s if isinstance(s, bytes) else s.encode(); return varint(len(b)) + b
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
    def string(s): return s.bytes().decode()
    def flag(s): v = s.b[s.i]; s.i += 1; return v != 0
sock = socket.create_connection(("127.0.0.1", port)); sock.settimeout(0.1)
sock.sendall(framed(varint(120) + varint(1) + string("doc")) + framed(varint(121) + struct.pack("<ddd", 1280, 800, 1)))
buf = b""
def until(kind, secs=10):
    global buf
    t = time.time()
    while time.time() - t < secs:
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n: break
            body, buf = buf[4:4 + n], buf[4 + n:]
            r = R(body)
            if r.int() == kind: return r
        try: buf += sock.recv(1 << 20)
        except socket.timeout: pass
    sys.exit("no message of kind %d" % kind)
def A(*steps): return subprocess.run([agent, "127.0.0.1", str(port + 1), *steps], capture_output=True, text=True).stdout.strip().splitlines()[-1]
A("click:#pick")
r = until(107); wid, accept, many = r.int(), r.string(), r.flag()
assert accept == "image/*,.txt" and many, (accept, many)
print("PickFile", wid, accept, many)
files = [("a.txt", "text/plain", b"hi there"), ("b.txt", "text/plain", "你好".encode())]
body = varint(128) + varint(wid) + varint(len(files)) + b"".join(string(n) + string(t) + string(d) for n, t, d in files)
sock.sendall(framed(body))
time.sleep(0.5)
got = A("eval:document.getElementById('hidout').textContent")
assert got == '"change\\na.txt|text/plain|8|hi there\\nb.txt|text/plain|6|你好"', got
A("click:#pick")
r = until(107); assert r.int() == wid
sock.sendall(framed(varint(128) + varint(wid) + varint(0)))
time.sleep(0.5)
got = A("eval:document.getElementById('hidout').textContent")
assert got.startswith('"change,cancel'), got
A("click:#d1")
r = until(108); name, url, mime, data = r.string(), r.string(), r.string(), r.bytes()
assert name.startswith("hello") and mime == "text/plain" and data.decode() == "Hello from a data link 你好", (name, mime, data)
print("Downloaded", name, mime, len(data))
# a link the person opens in a new tab (a menu's): Nav(NAV_NEW_TAB), told back as the page's own new tab is
sock.sendall(framed(varint(127) + varint(4) + string(page + "#other")))
r = until(106); u = r.string()
assert u == page + "#other", u
print("OpenTab", u)
PY
echo "files: ok (upload: 2 files, input+change, FormData, multipart POST; downloads: data:, blob:, attachment, download attr; a doc client: a hidden input's picker, files given, cancel, the download sent, a link opened in a new tab)"
