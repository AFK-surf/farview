#!/bin/bash
# A tab of its own (host --tab) against a built host: an agent drives it with no one showing it (eval, a screenshot,
# cookies), the frame port gives its picture and takes a tap and typing (which takes the tab from the agent), a
# person's client joins and leaves, every port listens on 127.0.0.1 only, and what the tab holds (page, localStorage,
# cookies) is there again after SIGTERM and a new run. Fails on the first thing that is not so.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/tab
rm -rf $O && mkdir -p $O/state
port=${TAB_PORT:-7490}
T=file://$PWD/tests/host
# (HOST, AGENT: other builds of them, as tools/package_pion.sh packs them)
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "tab: $1"; for f in $O/*.out; do echo "-- $f"; tail -20 $f; done; exit 1; }
start() {
  FV_STATE_DIR=$O/state FV_TAB_SIZE=800x600 FV_HOST_LOG=$O/host.log $HOST --tab $port "$T/tab.html" >> $O/host.out 2>&1 & H=$!
  for _ in $(seq 100); do [ -s $O/state/host.json ] && return; sleep 0.1; done
  fail "no host.json"
}
trap 'kill $H 2>/dev/null' EXIT
start
grep -q "\"port\":$port,\"agent\":$((port+1)),\"frame\":$((port+2))" $O/state/host.json || fail "host.json: $(cat $O/state/host.json)"
for p in $port $((port+1)) $((port+2)); do
  l=$(lsof -nP -iTCP:$p -sTCP:LISTEN 2>/dev/null | tail -n +2)
  echo "$l" | grep -q "127.0.0.1:$p" || fail "port $p does not listen on 127.0.0.1 only: $l"
done
A() { $AGENT 127.0.0.1 $((port+1)) "$@"; }
# an agent with no one showing the tab
A look 'eval:document.title' 'eval:Promise.resolve(6 * 7)' 'eval:(() => { throw new Error("boom") })()' "shot:$O/shot.png" \
  'cookies:set=[{"name":"k","value":"v1","url":"https://example.com/"}]' 'cookies:get=https://example.com/x' > $O/agent1.out 2>&1
grep -q '^title tab seen 1' $O/agent1.out || fail "the agent did not see the page"
grep -qx '"tab seen 1"' $O/agent1.out || fail "eval gave no title"
grep -qx '42' $O/agent1.out || fail "eval did not wait for the promise"
grep -q '^error: Error: boom' $O/agent1.out || fail "eval did not say what was thrown"
[ "$(head -c 8 $O/shot.png | od -An -tx1 | tr -d ' \n')" = "89504e470d0a1a0a" ] || fail "the shot is not a PNG"
grep -q '"name":"k","value":"v1","domain":"example.com"' $O/agent1.out || fail "the cookie set is not there"
# the frame port: its picture, then a tap in the field and typing (the person takes the tab)
A 'wait:6000' > $O/agent2.out 2>&1 & AG=$!
sleep 0.5
perl -e 'alarm 30; exec @ARGV' python3 -u - "$port" "$O" > $O/frame.out 2>&1 <<'PY' || fail "the frame port"
import json, socket, sys, time
port, out = int(sys.argv[1]) + 2, sys.argv[2]
s = socket.create_connection(("127.0.0.1", port))
s.settimeout(10)
f = s.makefile("rb")
def line():
    return json.loads(f.readline())
def send(o):
    s.sendall((json.dumps(o) + "\n").encode())
def frame(fmt):
    send({"frame": {"format": fmt, "quality": 70}})
    while True:
        m = line()
        if "frame" in m:
            return m["frame"], f.read(m["frame"]["bytes"])
first = [line() for _ in range(4)]
print(first)
assert first[0] == {"size": {"width": 800, "height": 600}}, first
assert first[2] == {"taken": "agent"}, first
send({"size": {"width": 800, "height": 600, "scale": 2}})
info, data = frame("jpeg")
print(info)
assert info["width"] == 1600 and info["height"] == 1200 and data[:3] == b"\xff\xd8\xff", info
open(out + "/frame.jpg", "wb").write(data)
send({"pointer": {"type": "down", "x": 100, "y": 80, "button": 0, "buttons": 1}})
send({"pointer": {"type": "up", "x": 100, "y": 80, "button": 0, "buttons": 0}})
send({"text": "hi"})
send({"key": {"type": "down", "key": "Backspace", "code": "Backspace", "mods": 0}})
send({"text": "o!"})
send({"wheel": {"x": 10, "y": 10, "dx": 0, "dy": 500}})
seen = []
end = time.time() + 5
while time.time() < end:
    m = line()
    seen.append(m)
    if m.get("page", {}).get("title") == "typed ho!":
        break
print(seen)
assert {"taken": "person"} in seen, "the tap did not take the tab"
assert any(m.get("page", {}).get("title") == "typed ho!" for m in seen), "typing did not reach the field"
info, data = frame("png")
assert data[:4] == b"\x89PNG", info
print("frame port ok")
PY
wait $AG
grep -q "the person took the tab back" $O/agent2.out || fail "the agent was not told the person took the tab"
# a person's client joins the running tab, and leaves it as it was
$B/pion-probe 127.0.0.1 $port 800 600 $O/person.raw idle:500 > $O/person.out 2>&1
grep -q "^page $T/tab.html" $O/person.out || fail "a person's client joining was not shown the page"
A look > $O/agent3.out 2>&1
grep -q '^title typed ho!' $O/agent3.out || fail "the tab was not as left after a client came and went"
# SIGTERM: what it holds written, host.json gone; a new run has it all again
kill -TERM $H; wait $H
[ ! -e $O/state/host.json ] || fail "host.json left after the tab ended"
grep -q "example.com.*k.*v1" $O/state/cookies.txt || fail "the cookies were not kept: $(cat $O/state/cookies.txt)"
grep -q '"seen":"1"' $O/state/storage.json || fail "localStorage was not kept: $(cat $O/state/storage.json)"
start
A look 'cookies:get=example.com' 'cookies:clear=example.com' 'cookies:get=example.com' > $O/agent4.out 2>&1
grep -q '^title tab seen 2' $O/agent4.out || fail "a new run did not start from the page with its localStorage"
grep -q '"value":"v1"' $O/agent4.out || fail "a new run lost the cookies"
tail -1 $O/agent4.out | grep -qx '\[\]' || fail "cookies:clear left cookies"
echo "tab: all steps did what they should"
