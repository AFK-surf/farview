#!/bin/bash
# Each page of tests/sites laid out again after a change at each of 40 of its elements (an attribute no rule reads,
# its box made anew and the rest kept; then a style each element is given, one its children do not read and one
# they do): every box must land where laying the page out whole puts it, and the page drawn again (what did not
# change taken as drawn before) must draw what drawing it anew does. Prints the pages where some change does
# not, and exits 1 then.
set -u
X=${FV_BIN_DIR:-$PWD/out}/pion-layout
bad=0
for style in "" "padding-left: 3px; background-color: rgb(1, 2, 3)" "font-size: 21px"; do
  for d in tests/sites/*/; do
    [ -f "$d/page.html" ] || continue
    if [ -n "$style" ]; then export FV_BENCH_SMALL_STYLE="$style"; else unset FV_BENCH_SMALL_STYLE; fi
    r=$(FV_BENCH_SMALL_CHECK=1 FV_NO_WEB_FONTS=1 FV_BENCH_SMALL=1 "$X" "$d/page.html" "$d/style.css" /dev/null 2>&1 | grep '^check\|^paint check')
    if ! echo "$r" | grep -q '^check: 0 of 40' || ! echo "$r" | grep -q '^paint check: 0 of 40'; then
      bad=1
      echo "== $d ${style:-(an attribute)}"
      echo "$r" | head -4 | cut -c1-240
    fi
  done
done
[ $bad -eq 0 ] && echo "incremental layout: every page, every change, as laid out and drawn whole"
exit $bad
