#!/bin/sh
# macOS: dist/Farview.app, the desktop browser as a Mac app the system can open links with.
#
#   Contents/MacOS/Farview    the launcher (CFBundleExecutable): starts a host tab for the profile, then becomes the
#                             client (exec, the same process), so the system's Apple Events (a link clicked in another
#                             app, `open -a Farview <url>`, Farview set as the default browser) reach the client's
#                             NSApplication, which opens each link in a new tab (SystemSaid::Link, app_dispatch.pion).
#   Contents/MacOS/pion-host, pion-app   the binaries (out/, built here unless FV_BIN_DIR names built ones)
#   Contents/Frameworks       what they load by @rpath (libnode: deps/node/lib, or FV_LIB_DIR)
#   Contents/Info.plist       http/https as its URL schemes (CFBundleURLTypes), so System Settings > Desktop & Dock >
#                             Default web browser lists it. No document types: a local HTML file would open as
#                             file://, and until a file page's origin is its own it could read the person's other
#                             files (proto::system_link_target refuses file:// for the same reason).
#
# Linux: dist/farview.desktop with MimeType x-scheme-handler/http;https (for `xdg-settings set default-web-browser
# farview.desktop`); its Exec runs FARVIEW_BIN_DIR's binaries.
#
# Environment: FV_BIN_DIR (built pion-host / pion-app, default: build them into out/), PIONC.
set -eu
PIONC=${PIONC:-pionc}
mkdir -p dist

plist() {
  cat <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Farview</string>
  <key>CFBundleDisplayName</key><string>Farview</string>
  <key>CFBundleIdentifier</key><string>surf.afk.farview</string>
  <key>CFBundleExecutable</key><string>Farview</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.productivity</string>
  <key>CFBundleURLTypes</key>
  <array>
    <dict>
      <key>CFBundleURLName</key><string>Web site URL</string>
      <key>CFBundleURLSchemes</key><array><string>http</string><string>https</string></array>
      <key>LSHandlerRank</key><string>Alternate</string>
    </dict>
  </array>
</dict>
</plist>
EOF
}

launcher() {
  cat <<'EOF'
#!/bin/sh
# Farview.app's launcher: a host tab for the profile on a port the system picks (it writes that port to
# $state/host.json), then the client in this process. Nothing but /bin/sh, sed and kill: no python3, whose stub on a
# stock Mac asks to install the Command Line Tools.
here=$(cd "$(dirname "$0")" && pwd)
state=${FV_STATE_DIR:-"$HOME/Library/Application Support/Farview"}
mkdir -p "$state"
rm -f "$state/host.json"
# (the first page is loaded before the client connects: shown dark if the browser will be, at a Retina ratio; the
# client says the window's own once it connects)
theme=$(sed -n 's/.*"chrome_theme":"\([a-z]*\)".*/\1/p' "${FV_PROFILE_DIR:-$state}/settings.json" 2>/dev/null)
sys_dark=0
[ "$(defaults read -g AppleInterfaceStyle 2>/dev/null)" = Dark ] && sys_dark=1
case "$theme" in
  dark) dark=1 ;;
  light) dark=0 ;;
  *) dark=$sys_dark ;;
esac
FV_STATE_DIR="$state" FV_PROFILE_DIR="${FV_PROFILE_DIR:-$state}" FV_NEW_TABS=1 FV_PREFERS_DARK=$dark FV_DPR=2 \
  "$here/pion-host" --tab 0 about:newtab > "$state/host.log" 2>&1 &
hpid=$!
port=
i=0
while [ $i -lt 150 ]; do
  if [ -f "$state/host.json" ]; then
    port=$(sed -n 's/.*"port":\([0-9][0-9]*\).*/\1/p' "$state/host.json")
    if [ -n "$port" ] && [ "$port" != 0 ]; then break; fi
    port=
  fi
  kill -0 "$hpid" 2>/dev/null || break
  sleep 0.1
  i=$((i + 1))
done
if [ -z "$port" ]; then
  kill "$hpid" 2>/dev/null
  msg="The page host did not start. Its log: $state/host.log"
  osascript -e "display alert \"Farview could not start\" message \"$msg\"" > /dev/null 2>&1 || echo "Farview: $msg" >&2
  exit 1
fi
FV_HOST_BIN="$here/pion-host" FV_APP_PORT=$port exec "$here/pion-app" "$@"
EOF
}

if [ "$(uname -s)" = Darwin ]; then
  B=${FV_BIN_DIR:-}
  if [ -z "$B" ]; then
    mkdir -p out
    "$PIONC" build -o out/pion-host pion/cmd/host
    "$PIONC" build -o out/pion-app pion/cmd/app
    B=out
  fi
  A=dist/Farview.app
  rm -rf "$A"
  mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources"
  plist > "$A/Contents/Info.plist"
  plutil -lint "$A/Contents/Info.plist" > /dev/null
  launcher > "$A/Contents/MacOS/Farview"
  chmod +x "$A/Contents/MacOS/Farview"
  cp "$B/pion-host" "$B/pion-app" "$A/Contents/MacOS/"
  # (the libraries they load by @rpath, libnode for the host's V8: into Contents/Frameworks, found from the binaries'
  # own place; a build's rpath is deps/node/lib relative to where it was built, so the app ran only from there)
  mkdir -p "$A/Contents/Frameworks"
  for lib in $(otool -L "$A/Contents/MacOS/pion-host" "$A/Contents/MacOS/pion-app" | awk '/@rpath\//{print $1}' | sort -u); do
    name=${lib#@rpath/}
    src=
    for d in "${FV_LIB_DIR:-}" deps/node/lib "$B"; do
      if [ -n "$d" ] && [ -f "$d/$name" ]; then src="$d/$name"; break; fi
    done
    [ -n "$src" ] || { echo "app_bundle: $name (loaded by @rpath) not found; set FV_LIB_DIR" >&2; exit 1; }
    cp -L "$src" "$A/Contents/Frameworks/$name"
  done
  for b in pion-host pion-app; do
    install_name_tool -add_rpath @executable_path/../Frameworks "$A/Contents/MacOS/$b" 2> /dev/null || true
  done
  codesign -f -s - --deep "$A" 2> /dev/null || true
  # (LaunchServices learns of its schemes now, not only once it is moved to /Applications)
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$A" 2> /dev/null || true
  echo "app_bundle: $A (http, https)"
else
  cat > dist/farview.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Farview
GenericName=Web Browser
Comment=Browse the web
Exec=sh -c 'exec "${FARVIEW_BIN_DIR:-$HOME/.local/lib/farview}/pion-app" "$@"' farview %U
Terminal=false
Categories=Network;WebBrowser;
MimeType=x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
EOF
  echo "app_bundle: dist/farview.desktop (x-scheme-handler/http, https)"
fi
