#!/bin/bash
# An agent and a person on one tab, against a built host: the agent's steps show on the person's client, the agent
# reads the page it is on, and the person's tap takes the tab back. Fails on the first thing that is not so.
set -u
B=${FV_BIN_DIR:-out}
O=probe-out/agent
mkdir -p $O
port=7480
T=file://$PWD/tests/host
FV_HOST_LOG=$O/host.log $B/pion-host $port "$T/a.html" > $O/host.out 2>&1 & H=$!
trap 'kill $H 2>/dev/null' EXIT
sleep 1
# the person: the page, then nothing for a while, then a tap on the page
$B/pion-probe 127.0.0.1 $port 412 900 $O/person.raw idle:6000 tap:200,400 > $O/person.out 2>&1 & P=$!
sleep 2
$B/pion-agent 127.0.0.1 $((port+1)) "go:$T/b.html" look wait:20000 > $O/agent.out 2>&1
wait $P
fail() { echo "agent: $1"; echo "-- person"; cat $O/person.out; echo "-- agent"; cat $O/agent.out; exit 1; }
grep -q "^agent true" $O/person.out || fail "the person was not shown that an agent drives the tab"
grep -q "^page $T/b.html" $O/person.out || fail "the person did not see the agent's page"
grep -q "^url $T/b.html" $O/agent.out || fail "the agent was not told where it is"
grep -q '^\[link .*\] to b' $O/agent.out || fail "the agent did not read the page's link"
grep -q "the person took the tab back" $O/agent.out || fail "the person's tap did not take the tab back"
grep -q "^agent false" $O/person.out || fail "the person was not shown the tab is theirs again"
echo "agent: all steps did what they should"
