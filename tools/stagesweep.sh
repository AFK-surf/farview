#!/bin/bash
# The new client's drawing of each site (fvclient/stage: what docshot draws of what a host's tab sends it over the doc
# wire) against the host's own picture of the same page, at a wide window (1280 × 800) and a phone's (390 × 844): each
# site of tests/flows.txt loaded, its main flow done by the agent, then both pictured. Per site and size: whether
# docshot drew it (and what it said went wrong), how much each picture holds (tools/pngink.py) and the flow's last
# address; pictures in probe-out/stage/<size>/<name>/ (host.png, client.png). Names as arguments run only those.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/stage
HOST=${HOST:-$B/pion-host}
AGENT=${AGENT:-$B/pion-agent}
SHOT=${SHOT:-$B/pion-docshot}
mkdir -p "$O"
one() {
  local size=$1 name=$2 url=$3 steps=$4
  local w=${size%x*} h=${size#*x} d=$O/$size/$name
  rm -rf "$d"; mkdir -p "$d/state"
  ( cd "$d" && exec env FV_STATE_DIR=$d/state FV_TAB_SIZE=$size FV_HOST_LOG=$d/host.log "$HOST" --tab 0 "$url" > $d/host.out 2>&1 ) & local hp=$!
  for _ in $(seq 150); do [ -s $d/state/host.json ] && break; sleep 0.2; done
  local port=$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))['port'])" $d/state/host.json 2>/dev/null)
  local agent=$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))['agent'])" $d/state/host.json 2>/dev/null)
  # the flow: probe's steps as the agent's
  local asteps=()
  for s in $steps; do
    case $s in
      idle:*) asteps+=("wait:${s#idle:}") ;;
      scrollto:*) ;;
      *) asteps+=("$s") ;;
    esac
  done
  perl -e 'alarm 120; exec @ARGV' "$AGENT" 127.0.0.1 $agent 'wait:4000' "${asteps[@]}" 'wait:2000' look "shot:$d/host.png" > $d/agent.out 2>&1
  local t0=$(date +%s)
  FV_SHOT_QUIET=2500 perl -e 'alarm 90; exec @ARGV' "$SHOT" $port "$d/client.png" $w $h > $d/docshot.out 2>&1
  local rc=$? t1=$(date +%s)
  kill $hp 2>/dev/null; wait $hp 2>/dev/null
  local last=$(grep '^url ' $d/agent.out | tail -1 | cut -c5-)
  local inks=($(python3 -I tools/pngink.py $d/host.png $d/client.png 2>/dev/null))
  local said=$(grep -E "fault|not steady|panic|abort|Error" $d/docshot.out | head -1 | cut -c1-120)
  local ok=OK
  [ $rc != 0 ] && ok=CRASH
  [ ! -s $d/client.png ] && ok=NOPIC
  printf '%s\t%s\t%s\thost %s%%\tclient %s%%\t%ss\t%s\t%s\n' "$ok" "$size" "$name" "${inks[0]:--}" "${inks[1]:--}" $((t1 - t0)) "${last:-no page}" "$said" > $d/result
}
ran=()
for size in ${SIZES:-1280x800 390x844}; do
  i=0
  while IFS='|' read -r name url steps want; do
    name=$(echo $name); url=$(echo $url); steps=$(echo $steps)
    [[ -z "$name" || "$name" == \#* ]] && continue
    if [ $# -gt 0 ] && [[ " $* " != *" $name "* ]]; then continue; fi
    ran+=("$size/$name")
    one "$size" "$name" "$url" "$steps" &
    i=$((i + 1))
    if [ $((i % ${PAR:-4})) = 0 ]; then wait; fi
  done < tests/flows.txt
  wait
done
for r in "${ran[@]}"; do cat "$O/$r/result"; done
