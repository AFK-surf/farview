#!/bin/bash
# Logins stay on the host: a person signs in to a site through a client, closes it, and a new client finds the tab still
# signed in, while nothing the clients sent or got ever held the cookie. Fails on the first thing that is not so.
set -u
B=${FV_BIN_DIR:-out}
O=probe-out/boundary
rm -rf $O && mkdir -p $O
port=7520 site=7524 relay=7526
secret=fv$RANDOM$RANDOM$RANDOM
python3 -I tests/boundary/site.py $site $secret & S=$!
python3 -I tests/boundary/relay.py $relay $port $O/wire.bin & R=$!
FV_COOKIE_JAR=$O/jar.txt FV_HOST_LOG=$O/host.log $B/pion-host $port http://127.0.0.1:$site/ > $O/host.out 2>&1 & H=$!
trap 'kill $H $S $R 2>/dev/null' EXIT
# the site and the relay up before the first client asks
for _ in $(seq 1 100); do curl -s -o /dev/null http://127.0.0.1:$site/ && break; sleep 0.1; done
sleep 1
fail() { echo "boundary: $1"; for f in $O/*.out; do echo "-- $f"; cat $f; done; exit 1; }
# the first client signs in and goes away (its page process ends with it)
$B/pion-probe 127.0.0.1 $relay 412 900 $O/first.raw "fill:input[name=user]=tester" "submit:input[name=user]" > $O/first.out 2>&1
grep -q "^page http://127.0.0.1:$site/" $O/first.out || fail "the first client got no page"
grep -q "session	$secret" $O/jar.txt || fail "the host did not keep the site's cookie"
# a new client, and an agent reading what that client sees
$B/pion-probe 127.0.0.1 $relay 412 900 $O/second.raw idle:4000 > $O/second.out 2>&1 & P=$!
sleep 2
$B/pion-agent 127.0.0.1 $((port+1)) look > $O/agent.out 2>&1
wait $P
grep -q "signed in as tester" $O/agent.out || fail "the new client's tab is not signed in"
[ -s $O/wire.bin ] || fail "nothing went through the relay"
grep -qa "$secret" $O/wire.bin && fail "the cookie went over the wire to or from a client"
echo "boundary: signed in on the host, and no client ever held the cookie"
