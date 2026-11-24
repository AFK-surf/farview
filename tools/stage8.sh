#!/bin/bash
# What Weibo, YouTube, Taobao and Zhihu stopped on, against a built wpt.exe: window.toolbar and the other bars, new
# Audio(), console.clear and its kin, a WebGL context to read, a template's custom elements left alone until imported,
# and each Web Audio event's handler run once (tests/host/stage8.html sets its title to "ok" or what failed)
set -u
WPT=${WPT:-${FV_BIN_DIR:-$PWD/out}/pion-wpt}
out=$(FV_WAIT=500 "$WPT" "file://$PWD/tests/host/stage8.html" 'document.title' 2>&1)
echo "$out" | tail -1
[ "$(echo "$out" | tail -1)" = ok ] || { echo "stage8: $out" | tail -20; exit 1; }
