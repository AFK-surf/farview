#!/bin/sh
# A local WPT server, so runs neither wait on nor depend on wpt.live: tools/wpt-serve.sh <dir to keep the checkout in>
# Then: FV_CONNECT_TO=::127.0.0.1: FV_WPT_BASE=http://web-platform.test:8000 python3 tools/wpt.py ...
set -e
D=${1:?checkout directory}
if [ ! -d "$D/.git" ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/web-platform-tests/wpt.git "$D"
  git -C "$D" sparse-checkout set --no-cone '/*' '!/*/' /resources/ /fonts/ /common/ /tools/ /docs/commands.json \
    /infrastructure/ /css/support/ /css/css-flexbox/ /css/selectors/ /css/fonts/ /dom/ /html/syntax/ /html/resources/ \
    /images/ /media/ /support/ /interfaces/
fi
# our testdriver: a test's clicks, keys and pointer actions done as the page's input (in place of the empty vendor file)
cp "$(dirname "$0")/testdriver-vendor.js" "$D/resources/testdriver-vendor.js"
# bound to loopback: WPT's host names are not in /etc/hosts here; the client sends every request to 127.0.0.1
cat > "$D/fv-config.json" <<'JSON'
{"server_host": "127.0.0.1", "check_subdomains": false,
 "ports": {"http": [8000, "auto"], "https": [8443, "auto"], "ws": ["auto"], "wss": ["auto"], "h2": [], "webtransport-h3": []}}
JSON
cd "$D" && exec python3 wpt serve --no-h2 --config fv-config.json
