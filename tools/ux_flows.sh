#!/bin/bash
# How the browser feels in a person's common flows, measured on the real client: the macOS app (out/pion-app, its
# whole Shell tree drawn offscreen with Metal at real speed) against a real host tab (out/pion-host --tab) on public
# sites. The app's own driver (FV_APP_UX, pion/cmd/app/ux_flows.pion) points at a link as laid out in the client,
# presses and releases it with the events the window feeds `ui`, and times each frame: the press's feedback, the
# loading state, the new address, the new page's first paint, the page usable; Cmd+click and middle click (a new
# tab, this page kept), the wheel turned while a page loads, Back and Forward through the shell's buttons, the
# pointer's shape over a link, and whether the link under the pointer is the one meant (hit-test offsets).
#
#   tools/ux_flows.sh [flow...]        flows: tests/ux_flows/<name>.txt (default: all but record)
#   UX_VARIANTS="plain zoom sidebar"   window variants for the wikipedia flow (125% zoom; sidebar open)
#   UX_OUT=probe-out/ux                where timelines (<flow>.jsonl), logs and frames go; summary.txt there
#   record                             saves every drawn frame of one link click (probe-out/ux/record-plain/frames/)
#   UX_CHROME=0                        skip the Chrome baseline (tools/ux_chrome.mjs: the same flows in Chrome)
#
# Needs macOS (Metal) and built out/pion-host, out/pion-app. Network access to the sites. Exits non-zero only when
# a run could not be done (host or app failed); what the timelines say is for summary.txt to tell.
set -u
B=${FV_BIN_DIR:-$PWD/out}
O=${UX_OUT:-$PWD/probe-out/ux}
mkdir -p "$O"
W=${UX_W:-1280}
H=${UX_H:-800}
start_url() { sed -n 's/^# start: //p' "tests/ux_flows/$1.txt" | head -1; }
pids=()
cleanup() { for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; }
trap cleanup EXIT
port=${UX_PORT:-7610}
one() {
  local flow=$1 variant=$2
  local name=$flow-$variant
  local d=$O/$name
  rm -rf "$d"; mkdir -p "$d/state"
  local url; url=$(start_url "$flow")
  local script=$d/script.txt
  sed "s#FRAMES_DIR#$d/frames#" "tests/ux_flows/$flow.txt" > "$script"
  mkdir -p "$d/frames"
  for _ in $(seq 1 50); do nc -z 127.0.0.1 $port 2>/dev/null || break; sleep 0.2; done
  FV_STATE_DIR=$d/state FV_TAB_SIZE=${W}x${H} FV_HOST_LOG=$d/host.log "$B/pion-host" --tab $port "$url" > "$d/host.out" 2>&1 &
  local h=$!
  pids+=("$h")
  for _ in $(seq 1 100); do [ -s "$d/state/host.json" ] && break; sleep 0.1; done
  local env=(FV_APP_PORT=$port FV_APP_UX=$script FV_APP_SHOT=$d/last.png FV_APP_W=$W FV_APP_H=$H)
  case $variant in
    zoom) env+=(FV_APP_ZOOM=1.25) ;;
    sidebar) env+=(FV_APP_SIDEBAR=1) ;;
  esac
  local t0; t0=$(date +%s)
  env "${env[@]}" perl -e 'alarm 300; exec @ARGV' "$B/pion-app" > "$d/app.out" 2>&1
  local rc=$?
  kill "$h" 2>/dev/null; wait "$h" 2>/dev/null
  grep '^UX ' "$d/app.out" | cut -c4- | sed "s/^{/{\"flow\":\"$flow\",\"variant\":\"$variant\",/" > "$O/$name.jsonl"
  echo "$name: rc=$rc $(( $(date +%s) - t0 ))s $(wc -l < "$O/$name.jsonl") events"
  port=$((port + 10))
  [ $rc -eq 0 ] || return 1
}
flows=("$@")
[ ${#flows[@]} -gt 0 ] || flows=(wikipedia mdn hn github github_search)
failed=0
for f in "${flows[@]}"; do
  if [ "$f" = wikipedia ]; then
    for v in ${UX_VARIANTS:-plain zoom sidebar}; do one wikipedia "$v" || failed=1; done
  else
    one "$f" plain || failed=1
  fi
done
# Chrome (Arc's engine) doing the same flows, for a baseline (UX_CHROME=0 skips it)
if [ "${UX_CHROME:-1}" = 1 ]; then
  NODE=$(command -v node || echo deps/.mm/n24/bin/node)
  real=(); for f in "${flows[@]}"; do [ "$f" = record ] || real+=("$f"); done
  [ ${#real[@]} -gt 0 ] && "$NODE" tools/ux_chrome.mjs "${real[@]}" 2> "$O/chrome.err" | grep '^UX ' | cut -c4- > "$O/chrome-baseline.jsonl"
  echo "chrome: $(wc -l < "$O/chrome-baseline.jsonl") events"
fi
cat "$O"/*-*.jsonl > "$O/all.tmp" 2>/dev/null; mv "$O/all.tmp" "$O/all.jsonl"
python3 tools/ux_summary.py "$O/all.jsonl" | tee "$O/summary.txt"
exit $failed
