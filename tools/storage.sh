#!/bin/bash
# Stage 10A storage & cross-tab IPC checks against a built host:
# 1. IndexedDB persistence across host process restart (String, Number, Date, Uint8Array, index query).
# 2. Cross-tab BroadcastChannel: same-origin delivery across two host --tab processes, cross-origin isolation.
# 3. Sub-<iframe> window.open + window.opener.postMessage cross-tab OAuth flow.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/storage
rm -rf "$O" && mkdir -p "$O/profile" "$O/state1" "$O/state2" "$O/state3" "$O/www"
port=${STORAGE_PORT:-7940}
port2=$((port + 5))
port3=$((port + 10))
web=$((port + 15))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "storage: $1"; for f in "$O"/*.out "$O"/*.lines "$O"/*.log; do [ -f "$f" ] && { echo "-- $f"; tail -20 "$f"; }; done; exit 1; }

cat > "$O/www/app.html" << 'EOF'
<!doctype html><html><head><title>storage</title></head><body>ok</body></html>
EOF

cat > "$O/www/iframe-parent.html" << 'EOF'
<!doctype html><html><head><title>iframe-parent</title></head><body>
<iframe id="f" style="width:400px;height:200px"></iframe>
<script>
window.__parentGot = [];
window.addEventListener('message', e => window.__parentGot.push(e.data));
const p = location.search.slice(1);
document.getElementById('f').src = '/iframe-child.html?' + p;
</script>
</body></html>
EOF

cat > "$O/www/iframe-child.html" << 'EOF'
<!doctype html><html><head><title>iframe-child</title></head><body>
<button id="openBtn">open</button>
<script>
window.__res = { got: [], closed: false };
window.addEventListener('message', e => {
  window.__res.got.push({ data: e.data, origin: e.origin, sourceMatch: e.source === window.__pop });
});
document.getElementById('openBtn').onclick = () => {
  const popupUrl = location.search.slice(1);
  window.__pop = window.open(popupUrl);
};
</script>
</body></html>
EOF

cat > "$O/www/iframe-popup.html" << 'EOF'
<!doctype html><html><head><title>iframe-popup</title></head><body>
<script>
const targetOrigin = location.search.slice(1);
if (window.opener) {
  window.opener.postMessage({ token: 'iframe-oauth-ok' }, targetOrigin);
  window.close();
}
</script>
</body></html>
EOF

python3 -I -m http.server --bind 127.0.0.1 --directory "$O/www" "$web" > "$O/web.out" 2>&1 & W=$!
H1= H2= H3= L1=
trap 'kill $H1 $H2 $H3 $W $L1 2>/dev/null' EXIT
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$web/app.html" > /dev/null && break; sleep 0.05; done

export FV_CONNECT_TO=::127.0.0.1:

# --- 1. IndexedDB persistence across process restart ---
FV_PROFILE_DIR="$O/profile" FV_STATE_DIR="$O/state1" FV_HOST_LOG="$O/host1.log" \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/app.html" > "$O/host1.out" 2>&1 & H1=$!
for _ in $(seq 100); do [ -s "$O/state1/host.json" ] && break; sleep 0.05; done
[ -s "$O/state1/host.json" ] || fail "no host.json for IDB write tab"
A1() { "$AGENT" 127.0.0.1 $((port + 1)) "$@"; }
for _ in $(seq 50); do A1 'eval:document.title' 2>/dev/null | grep -qx '"storage"' && break; sleep 0.1; done

write_res=$(A1 'eval:new Promise((resolve, reject) => {
  const req = indexedDB.open("appdb", 1);
  req.onupgradeneeded = () => {
    const db = req.result;
    const st = db.createObjectStore("items", { keyPath: "id" });
    st.createIndex("by_tag", "tag", { unique: false });
  };
  req.onsuccess = () => {
    const db = req.result;
    const tx = db.transaction("items", "readwrite");
    tx.objectStore("items").put({
      id: 1,
      name: "alpha-rec",
      tag: "alpha",
      when: new Date(1700000000123),
      bytes: new Uint8Array([1, 2, 250, 255]),
    });
    tx.oncomplete = () => resolve("written");
    tx.onerror = () => reject(tx.error);
  };
  req.onerror = () => reject(req.error);
})' | tail -1)
[ "$write_res" = '"written"' ] || fail "IDB write failed: $write_res"

# Kill Tab 1 and start a brand-new host process on the same FV_PROFILE_DIR
kill "$H1" 2>/dev/null; wait "$H1" 2>/dev/null; H1=
rm -rf "$O/state1" && mkdir -p "$O/state1"

FV_PROFILE_DIR="$O/profile" FV_STATE_DIR="$O/state1" FV_HOST_LOG="$O/host1b.log" \
  "$HOST" --tab "$port" "http://127.0.0.1:$web/app.html" > "$O/host1b.out" 2>&1 & H1=$!
for _ in $(seq 100); do [ -s "$O/state1/host.json" ] && break; sleep 0.05; done
for _ in $(seq 50); do A1 'eval:document.title' 2>/dev/null | grep -qx '"storage"' && break; sleep 0.1; done

read_res=$(A1 'eval:new Promise((resolve, reject) => {
  indexedDB.databases().then(dbs => {
    const req = indexedDB.open("appdb", 1);
    req.onsuccess = () => {
      const db = req.result;
      const tx = db.transaction("items", "readonly");
      const r1 = tx.objectStore("items").get(1);
      const r2 = tx.objectStore("items").index("by_tag").get("alpha");
      tx.oncomplete = () => {
        const v = r1.result;
        resolve(JSON.stringify({
          dbs,
          id: v && v.id,
          name: v && v.name,
          isDate: v && (v.when instanceof Date),
          whenMs: v && v.when && v.when.getTime(),
          isU8: v && (v.bytes instanceof Uint8Array),
          bytes: v && v.bytes ? [...v.bytes] : null,
          byIdx: r2.result && r2.result.name,
        }));
      };
    };
    req.onerror = () => reject(req.error);
  });
})' | tail -1)
python3 -I -c 'import json, sys
r = json.loads(json.loads(sys.argv[1]))
assert r["dbs"] == [{"name": "appdb", "version": 1}], r
assert r["id"] == 1 and r["name"] == "alpha-rec", r
assert r["isDate"] is True and r["whenMs"] == 1700000000123, r
assert r["isU8"] is True and r["bytes"] == [1, 2, 250, 255], r
assert r["byIdx"] == "alpha-rec", r
print("storage: IndexedDB persistence ok (Date, Uint8Array, index query survived restart)")' "$read_res" || fail "IDB read failed: $read_res"

# --- 2. Cross-tab BroadcastChannel (same-origin delivered, cross-origin isolated) ---
FV_PROFILE_DIR="$O/profile" FV_STATE_DIR="$O/state2" FV_HOST_LOG="$O/host2.log" \
  "$HOST" --tab "$port2" "http://127.0.0.1:$web/app.html" > "$O/host2.out" 2>&1 & H2=$!
FV_PROFILE_DIR="$O/profile" FV_STATE_DIR="$O/state3" FV_HOST_LOG="$O/host3.log" \
  "$HOST" --tab "$port3" "http://localhost:$web/app.html" > "$O/host3.out" 2>&1 & H3=$!
for _ in $(seq 100); do [ -s "$O/state2/host.json" ] && [ -s "$O/state3/host.json" ] && break; sleep 0.05; done
A2() { "$AGENT" 127.0.0.1 $((port2 + 1)) "$@"; }
A3() { "$AGENT" 127.0.0.1 $((port3 + 1)) "$@"; }
for _ in $(seq 50); do A2 'eval:document.title' 2>/dev/null | grep -qx '"storage"' && break; sleep 0.1; done
for _ in $(seq 50); do A3 'eval:document.title' 2>/dev/null | grep -qx '"storage"' && break; sleep 0.1; done

# Tab 1 and Tab 3 (different origin) subscribe to "room1"; Tab 2 sends on "room1"
A1 'eval:(() => { window.__bc = []; const c = new BroadcastChannel("room1"); c.onmessage = e => window.__bc.push({ data: e.data, origin: e.origin, bytes: [...e.data.bytes] }); return "sub1"; })()' > /dev/null
A3 'eval:(() => { window.__bc = []; const c = new BroadcastChannel("room1"); c.onmessage = e => window.__bc.push({ data: e.data, origin: e.origin }); return "sub3"; })()' > /dev/null
A2 'eval:(() => { const c = new BroadcastChannel("room1"); c.postMessage({ msg: "hello-cross-tab", bytes: new Uint8Array([9, 8, 7]) }); return "sent2"; })()' > /dev/null

for _ in $(seq 40); do
  got1=$(A1 'eval:JSON.stringify(window.__bc)' 2>/dev/null | tail -1)
  echo "$got1" | grep -q 'hello-cross-tab' && break
  sleep 0.1
done
got1=$(A1 'eval:JSON.stringify(window.__bc)' | tail -1)
got3=$(A3 'eval:JSON.stringify(window.__bc)' | tail -1)
python3 -I -c 'import json, sys
r1 = json.loads(json.loads(sys.argv[1]))
r3 = json.loads(json.loads(sys.argv[2]))
assert len(r1) == 1, r1
assert r1[0]["data"]["msg"] == "hello-cross-tab", r1
assert r1[0]["bytes"] == [9, 8, 7], r1
assert r1[0]["origin"] == sys.argv[3], r1
assert len(r3) == 0, f"cross-origin leak: {r3}"
print("storage: BroadcastChannel ok (same-origin cross-tab delivered, cross-origin isolated)")' "$got1" "$got3" "http://127.0.0.1:$web" || fail "BroadcastChannel failed: got1=$got1 got3=$got3"

kill "$H1" "$H2" "$H3" 2>/dev/null; wait "$H1" "$H2" "$H3" 2>/dev/null; H1= H2= H3=

# --- 3. Sub-<iframe> window.open + window.opener.postMessage ---
rm -rf "$O/state1" "$O/state2" "$O/profile/ipc" && mkdir -p "$O/state1" "$O/state2"
OPENER_ORIGIN="http://127.0.0.1:$web"
POPUP="http://localhost:$web/iframe-popup.html?$OPENER_ORIGIN"
FV_NEW_TABS=1 FV_TAB_ID=ifparent FV_PROFILE_DIR="$O/profile" FV_STATE_DIR="$O/state1" FV_HOST_LOG="$O/host_if1.log" \
  "$HOST" --tab "$port" "$OPENER_ORIGIN/iframe-parent.html?$POPUP" > "$O/host_if1.out" 2>&1 & H1=$!
for _ in $(seq 100); do [ -s "$O/state1/host.json" ] && break; sleep 0.05; done

python3 -I -c 'import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
f = s.makefile("rb")
out = open(sys.argv[2], "ab", buffering=0)
for line in f: out.write(line)' $((port + 2)) "$O/frame1.lines" & L1=$!
for _ in $(seq 100); do grep -q '"size"' "$O/frame1.lines" 2>/dev/null && break; sleep 0.05; done

for _ in $(seq 50); do A1 'eval:document.getElementById("f").contentWindow && document.getElementById("f").contentWindow.document.getElementById("openBtn") ? "ready" : ""' 2>/dev/null | grep -qx '"ready"' && break; sleep 0.1; done
A1 'eval:(() => { document.getElementById("f").contentWindow.document.getElementById("openBtn").click(); return "clicked"; })()' > /dev/null

for _ in $(seq 50); do grep -q '"openTab"' "$O/frame1.lines" 2>/dev/null && break; sleep 0.1; done
open_line=$(grep '"openTab"' "$O/frame1.lines" | head -1)
[ -n "$open_line" ] || fail "iframe window.open produced no openTab"
read -r url win opener origin < <(python3 -I -c 'import json, sys; o = json.loads(sys.argv[1]); print(o["openTab"], o.get("window", ""), o.get("opener", ""), o.get("origin", ""))' "$open_line")
[ "$opener" = ifparent ] && [ -n "$win" ] || fail "bad openTab from iframe: $open_line"

FV_NEW_TABS=1 FV_TAB_ID="$win" FV_OPENER_ID="$opener" FV_OPENER_ORIGIN="$origin" FV_PROFILE_DIR="$O/profile" FV_STATE_DIR="$O/state2" \
  FV_HOST_LOG="$O/host_if2.log" "$HOST" --tab "$port2" "$url" > "$O/host_if2.out" 2>&1 & H2=$!
for _ in $(seq 100); do [ -s "$O/state2/host.json" ] && break; sleep 0.05; done
for _ in $(seq 50); do
  res=$(A1 'eval:JSON.stringify({ got: document.getElementById("f").contentWindow.__res.got, closed: !!(document.getElementById("f").contentWindow.__pop && document.getElementById("f").contentWindow.__pop.closed) })' 2>/dev/null | tail -1)
  echo "$res" | grep -q 'iframe-oauth-ok' && break
  sleep 0.1
done
res=$(A1 'eval:JSON.stringify({ got: document.getElementById("f").contentWindow.__res.got, closed: !!(document.getElementById("f").contentWindow.__pop && document.getElementById("f").contentWindow.__pop.closed), parentGot: window.__parentGot })' | tail -1)
python3 -I -c 'import json, sys
r = json.loads(json.loads(sys.argv[1]))
assert len(r["got"]) == 1, r
assert r["got"][0]["data"] == {"token": "iframe-oauth-ok"}, r
assert r["got"][0]["sourceMatch"] is True, r
assert r["closed"] is True, r
assert r["parentGot"] == [], f"popup message leaked to same-origin parent: {r}"
print("storage: iframe popup IPC ok (window.open + opener.postMessage + close, parent isolated)")' "$res" || fail "iframe popup IPC failed: $res"

echo "storage: all ok"
