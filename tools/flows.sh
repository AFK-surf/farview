#!/bin/bash
# Runs each site's main flow (tests/flows.txt) with a built host and probe at phone size, four at a time, and says
# per site whether the last page is what the flow wanted, how long it took and what it cost; pictures and logs in
# probe-out/flows/<name>/. Names as arguments run only those.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=$PWD/probe-out/flows
mkdir -p "$O"
one() {
  local name=$1 url=$2 steps=$3 want=$4 port=$5
  local d=$O/$name
  rm -rf "$d"; mkdir -p "$d"
  # a port a host of an earlier run may still hold (ending): wait until it is free
  for _ in $(seq 1 50); do nc -z 127.0.0.1 $port 2>/dev/null || break; sleep 0.2; done
  # (exec: $! is the host itself, so kill ends it rather than the subshell around it)
  ( cd "$d" && exec env FV_HOST_LOG=$d/host.log "$B/pion-host" $port "$url" > $d/host.out 2>&1 ) & local h=$!
  sleep 1
  local t0=$(date +%s)
  # shellcheck disable=SC2086
  FV_PROBE_WAIT=60000 perl -e 'alarm 180; exec @ARGV' "$B/pion-probe" 127.0.0.1 $port 412 900 "$d/shot.raw" $steps > "$d/probe.out" 2>&1
  local t1=$(date +%s)
  kill $h 2>/dev/null; wait $h 2>/dev/null
  local last=$(grep '^page ' "$d/probe.out" | tail -1 | cut -c6-)
  # (the title as the client was told it: the host's log may still be written by its page process as it ends)
  local title=$(grep '^title ' "$d/probe.out" | tail -1 | cut -c7-)
  local kind=${want%%~*} words=${want#*~} ok=FAIL
  case $kind in
    url) [[ "$last" == *"$words"* ]] && ok=PASS ;;
    # (a title's case is the site's to change: Amazon's search title went from "moonbit" to "Moonbit")
    title) [[ "$(echo "$title" | tr '[:upper:]' '[:lower:]')" == *"$(echo "$words" | tr '[:upper:]' '[:lower:]')"* ]] && ok=PASS ;;
  esac
  # and the page shows something: a page that comes up nearly blank (its title right, its content never drawn) fails
  # (what is drawn over it where the viewport is, a modal or a fixed bar, counts too)
  local page_ops=$(grep -o "^page: height [0-9.]*, [0-9]* ops" "$d/probe.out" | tail -1 | awk '{print $4}')
  local over_ops=$(grep -o "^overlay: [0-9]* ops" "$d/probe.out" | tail -1 | awk '{print $2}')
  local ops=$(( ${page_ops:-0} + ${over_ops:-0} ))
  if [ "${ops:-0}" -lt 15 ]; then ok=FAIL; title="$title (blank: ${ops:-0} ops)"; fi
  local bytes=$(grep -o 'received [0-9]*' "$d/probe.out" | awk '{print $2}')
  local errs=$(grep -c '^script error' "$d/host.log")
  printf '%s\t%s\t%ss\t%s B\t%s script errors\t%s | %s\n' "$ok" "$name" $((t1-t0)) "${bytes:-0}" "$errs" "${last:-no page}" "$title" > "$d/result"
}
i=0
ran=()
# ports of this run's own (an earlier run's hosts may still be ending on theirs)
base=$((20000 + ($$ % 400) * 80))
while IFS='|' read -r name url steps want; do
  name=$(echo $name); url=$(echo $url); steps=$(echo $steps); want=$(echo $want)
  [[ -z "$name" || "$name" == \#* ]] && continue
  if [ $# -gt 0 ] && [[ " $* " != *" $name "* ]]; then continue; fi
  ran+=("$name")
  # (two each: a host takes the next port for agents)
  one "$name" "$url" "$steps" "$want" $((base + 2 * i)) &
  i=$((i+1))
  if [ $((i % 4)) = 0 ]; then wait; fi
done < tests/flows.txt
wait
results=$(for n in "${ran[@]}"; do cat "$O/$n/result"; done | sort)
echo "$results"
echo "$(echo "$results" | grep -c ^PASS) of ${#ran[@]} flows passed"
