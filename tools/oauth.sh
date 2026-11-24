#!/bin/bash
# An OAuth popup across two tabs of one profile (host --tab, FV_PROFILE_DIR), against a built host, as a station runs
# them: the page's window.open('') then its address set gives a handle and asks for a tab ({"openTab", "window",
# "opener", "origin"} on the frame port); the popup's tab, started as a station starts it (FV_TAB_ID, FV_OPENER_ID,
# FV_OPENER_ORIGIN), posts the token to window.opener with the opener's origin (and one for another origin, dropped)
# and closes; the opener's page gets the message (its data, the popup's origin, the handle as its source) and sees its
# handle closed; the popup's tab says it closed ({"closeTab":true}, host.json). Fails on the first thing that is not so.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/oauth
rm -rf $O && mkdir -p $O/profile $O/state1 $O/state2 $O/www
port=${OAUTH_PORT:-7890}
port2=$((port + 5))
web=$((port + 10))
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
fail() { echo "oauth: $1"; for f in $O/*.out $O/*.lines; do echo "-- $f"; tail -20 $f; done; exit 1; }
cp tests/host/oauth.html tests/host/oauth-popup.html $O/www/
python3 -I -m http.server --bind 127.0.0.1 --directory $O/www $web > $O/web.out 2>&1 & W=$!
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$web/oauth.html" > /dev/null && break; sleep 0.1; done
# (the popup on another origin than the page's: localhost, to the server here)
export FV_CONNECT_TO=::127.0.0.1:
OPENER_ORIGIN="http://127.0.0.1:$web"
POPUP="http://localhost:$web/oauth-popup.html?$OPENER_ORIGIN"
FV_NEW_TABS=1 FV_TAB_ID=parent FV_PROFILE_DIR=$O/profile FV_STATE_DIR=$O/state1 FV_HOST_LOG=$O/host1.log \
  $HOST --tab $port "$OPENER_ORIGIN/oauth.html?$POPUP" >> $O/host1.out 2>&1 & H1=$!
H2=
L1=
L2=
trap 'kill $H1 $W $H2 $L1 $L2 2>/dev/null' EXIT
for _ in $(seq 100); do [ -s $O/state1/host.json ] && break; sleep 0.1; done
[ -s $O/state1/host.json ] || fail "no host.json for the opener"
# a frame port's lines, kept (what a station hears of a tab)
lines() { python3 -I -c 'import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
f = s.makefile("rb")
out = open(sys.argv[2], "ab", buffering=0)
for line in f: out.write(line)' "$1" "$2"; }
lines $((port + 2)) $O/frame1.lines & L1=$!
for _ in $(seq 100); do grep -q '"size"' $O/frame1.lines 2>/dev/null && break; sleep 0.05; done
A1() { $AGENT 127.0.0.1 $((port+1)) "$@"; }
for _ in $(seq 50); do A1 'eval:document.title' 2>/dev/null | grep -qx '"oauth"' && break; sleep 0.2; done
A1 'eval:document.title' | grep -qx '"oauth"' || fail "the opener's page did not load"
A1 'click:#go' 'wait:300' 'eval:JSON.stringify(window.__r.early)' > $O/click.out 2>&1
tail -1 $O/click.out | grep -q 'closed\\":false,\\"hasPost\\":\\"function' || fail "window.open gave no handle: $(tail -1 $O/click.out)"
for _ in $(seq 50); do grep -q '"openTab"' $O/frame1.lines 2>/dev/null && break; sleep 0.1; done
open_line=$(grep '"openTab"' $O/frame1.lines | head -1)
[ -n "$open_line" ] || fail "no openTab on the frame port"
read -r url win opener origin < <(python3 -I -c 'import json, sys; o = json.loads(sys.argv[1]); print(o["openTab"], o.get("window", ""), o.get("opener", ""), o.get("origin", ""))' "$open_line")
[ "$url" = "$POPUP" ] || fail "the popup's address was not the one set after it opened: $open_line"
[ "$opener" = parent ] && [ "$origin" = "$OPENER_ORIGIN" ] && [ -n "$win" ] || fail "openTab does not say the window, its opener and origin: $open_line"
# the popup's tab, as a station starts it
FV_NEW_TABS=1 FV_TAB_ID=$win FV_OPENER_ID=$opener FV_OPENER_ORIGIN=$origin FV_PROFILE_DIR=$O/profile FV_STATE_DIR=$O/state2 \
  FV_HOST_LOG=$O/host2.log $HOST --tab $port2 "$url" >> $O/host2.out 2>&1 & H2=$!
for _ in $(seq 100); do [ -s $O/state2/host.json ] && break; sleep 0.1; done
[ -s $O/state2/host.json ] || fail "no host.json for the popup"
lines $((port2 + 2)) $O/frame2.lines & L2=$!
for _ in $(seq 100); do grep -q '"closeTab":true' $O/frame2.lines 2>/dev/null && break; sleep 0.1; done
grep -q '"closeTab":true' $O/frame2.lines || fail "the popup's tab did not say it closed"
grep -q '"closed":true' $O/state2/host.json || fail "the popup's host.json does not say it closed: $(cat $O/state2/host.json)"
for _ in $(seq 50); do A1 'eval:window.__r.closed' 2>/dev/null | tail -1 | grep -qx true && break; sleep 0.1; done
A1 'eval:JSON.stringify(window.__r)' > $O/result.out 2>&1
r=$(tail -1 $O/result.out)
python3 -I -c 'import json, sys
r = json.loads(json.loads(sys.argv[1]))
got = r["got"]
assert len(got) == 1, "messages: %r" % got
m = got[0]
assert m["data"] == {"token": "ok-123"}, m
assert m["origin"] == sys.argv[2], m
assert m["sourceIsPopup"] is True, m
assert r["closed"] is True, r' "$r" "http://localhost:$web" || fail "the opener did not get the token from its popup and see it closed: $r"
# no file left behind in the profile's ipc
left=$(find $O/profile/ipc -name '*.msg' | wc -l | tr -d ' ')
[ "$left" = 0 ] || fail "messages left unread: $(find $O/profile/ipc -name '*.msg')"
echo "oauth: ok (window.open handle, address set after, openTab with window/opener/origin; opener.postMessage token, origin and source checked, a message for another origin dropped; window.close: closed handle, closeTab, host.json)"
