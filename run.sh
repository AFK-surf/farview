#!/bin/bash
# Compares out/pion-layout against Chrome layout snapshots: ./run.sh [failures to list] [sites...]
# Builds pion/cmd/layout with pionc if out/pion-layout is missing or PIONC is available.
# tests/pages holds the hand-made pages; snapshot one with: node tools/snapshot.mjs name=file://$PWD/tests/pages/name.html
set -euo pipefail
n=${1:-0}; shift || true
sites=${@:-$(ls tests/sites | tr '\n' ' ')}
PIONC=${PIONC:-pionc}
mkdir -p out
E=${FV_LAYOUT_BIN:-out/pion-layout}
if command -v "$PIONC" >/dev/null 2>&1; then
  "$PIONC" build pion/cmd/layout -o "$E"
elif [ ! -x "$E" ]; then
  echo "run.sh: $E not found and pionc not in PATH. Build it with: pionc build pion/cmd/layout -o out/pion-layout" >&2
  exit 1
fi
for s in $sites; do
  out_log="/tmp/fv-$s.log"
  if ! perl -e 'alarm 40; exec @ARGV' "$E" "tests/sites/$s/page.html" "tests/sites/$s/style.css" "/tmp/fv-$s.txt" >"$out_log" 2>&1; then
    echo "run.sh: layout failed for site '$s':" >&2
    cat "$out_log" >&2
    rm -f "$out_log" "/tmp/fv-$s.txt"
    exit 1
  fi
  grep -v "laid out" "$out_log" || true
  rm -f "$out_log"
  python3 tools/compare.py "$s" "/tmp/fv-$s.txt" "$n"
  rm -f "/tmp/fv-$s.txt"
done
