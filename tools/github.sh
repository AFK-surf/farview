#!/bin/bash
# GitHub's everyday pages (tests/github.txt), signed out, at phone size: each page, its scripts run, must say its texts
# in what it renders (innerText: what is laid out and not hidden). Names as arguments run only those.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/github
mkdir -p "$O"
grep -v '^#' tests/github.txt | grep -v '^\s*$' | while IFS='|' read -r name url steps want; do
  name=$(echo $name); url=$(echo $url); want=$(echo "$want" | sed 's/^ *//;s/ *$//')
  if [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$name"; then continue; fi
  d=$O/$name; rm -rf "$d"; mkdir -p "$d"
  # (steps beyond a look: a person's client and an agent that acts on the page, the texts read from what the agent
  # reads after its steps, the address among them)
  if [ "$(echo $steps)" != "look" ]; then
    port=$(( ${port:-7600} + 4 ))
    ( cd "$d" && exec env FV_HOST_LOG=$d/host.log "$B/pion-host" $port "$url" > $d/host.out 2>&1 < /dev/null ) & h=$!
    sleep 1; t0=$(date +%s)
    perl -e 'alarm 150; exec @ARGV' "$B/pion-probe" 127.0.0.1 $port 412 900 "$d/shot.raw" idle:100000 > "$d/person.out" 2>&1 < /dev/null & p=$!
    sleep 2
    # shellcheck disable=SC2086
    perl -e 'alarm 130; exec @ARGV' "$B/pion-agent" 127.0.0.1 $((port + 1)) wait:20000 $steps > "$d/out" 2>&1 < /dev/null
    # (the host goes first: the person's client, its connection closed, ends by itself and says all it was told)
    t1=$(date +%s); kill $h 2>/dev/null; wait $h 2>/dev/null; wait $p 2>/dev/null
    ok=PASS; missing=""
    IFS=';' read -ra ts <<< "$want"
    for t in "${ts[@]}"; do t=$(echo "$t" | sed 's/^ *//;s/ *$//'); cat "$d/out" "$d/person.out" | grep -qF -- "$t" || { ok=FAIL; missing="$missing [$t]"; }; done
    errs=$(grep -c '^script error' "$d/host.log" 2>/dev/null); errs=${errs:-0}
    printf '%s\t%s\t%ss\t%s script errors\t%s\n' "$ok" "$name" $((t1 - t0)) "$errs" "${missing:+missing$missing}"
    continue
  fi
  texts=$(python3 -c 'import json,sys; print(json.dumps([t.strip() for t in sys.argv[1].split(";")]))' "$want")
  t0=$(date +%s)
  FV_WIDTH=412 FV_WAIT=8000 perl -e 'alarm 120; exec @ARGV' "$B/pion-wpt" "$url" "JSON.stringify({ missing: $texts.filter(t => !document.body.innerText.includes(t)), title: document.title })" > "$d/out" 2>&1 < /dev/null
  t1=$(date +%s)
  r=$(grep '^{"missing"' "$d/out" | tail -1)
  errs=$(grep -c '^!error\|^script error' "$d/out"); errs=${errs:-0}
  ok=FAIL; [ -n "$r" ] && echo "$r" | grep -q '"missing":\[\]' && ok=PASS
  printf '%s\t%s\t%ss\t%s script errors\t%s\n' "$ok" "$name" $((t1 - t0)) "$errs" "${r:-no answer}"
done
