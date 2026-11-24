#!/bin/bash
# What the host sends a client per step, with a built host and probe: `tools/bytes.sh` runs the pages below; output in
# probe-out/<name>.probe (bytes per step) and probe-out/<name>.host.log (each frame against the whole page).
set -u
B=${FV_BIN_DIR:-out}
mkdir -p probe-out
port=7330
run() {
  name=$1; url=$2; shift 2
  port=$((port+2))
  FV_HOST_LOG=out/$name.host.log $B/pion-host $port "$url" > out/$name.host.out 2>&1 & H=$!
  sleep 1
  $B/pion-probe 127.0.0.1 $port 412 900 out/$name.raw "$@" > out/$name.probe 2>&1
  kill $H; wait $H 2>/dev/null
  echo "== $name"; grep "^bytes\|no answer\|^field\|new pieces" out/$name.probe | uniq -c; grep "^frame\|^tap\|^input" out/$name.host.log | uniq -c | tail -30
}
run search 'https://en.m.wikipedia.org/w/index.php?title=Special:Search' tap:130,145 type:M type:Mo type:Moo idle:5000
run article 'https://en.m.wikipedia.org/wiki/Tokyo' idle:10000
run bbc 'https://www.bbc.com/news' idle:10000
run hn 'https://news.ycombinator.com/' idle:5000
