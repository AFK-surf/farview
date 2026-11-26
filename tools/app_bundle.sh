#!/bin/sh
# macOS: dist/Farview.app, the desktop browser as a Mac app the system can open links with.
#
#   Contents/MacOS/Farview    the launcher (CFBundleExecutable): starts a host tab for the profile, then becomes the
#                             client (exec, the same process), so the system's Apple Events (a link clicked in another
#                             app, `open -a Farview <url>`, Farview set as the default browser) reach the client's
#                             NSApplication, which opens each link in a new tab (SystemSaid::Link, app_dispatch.pion).
#   Contents/MacOS/pion-host, pion-app   the binaries (out/, built here unless FV_BIN_DIR names built ones)
#   Contents/Info.plist       http/https as its URL schemes (CFBundleURLTypes) and HTML/XHTML/SVG/PDF files as
#                             documents it views (CFBundleDocumentTypes), so System Settings > Desktop & Dock >
#                             Default web browser lists it.
#
# Linux: dist/farview.desktop with MimeType x-scheme-handler/http;https and text/html (for `xdg-settings set
# default-web-browser farview.desktop`); its Exec runs FARVIEW_BIN_DIR's binaries.
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
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Web page</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>LSHandlerRank</key><string>Alternate</string>
      <key>LSItemContentTypes</key>
      <array>
        <string>public.html</string>
        <string>public.xhtml</string>
        <string>public.svg-image</string>
        <string>com.adobe.pdf</string>
      </array>
    </dict>
  </array>
</dict>
</plist>
EOF
}

launcher() {
  cat <<'EOF'
#!/bin/sh
# Farview.app's launcher: a host tab for the profile on a free port, then the client in this process.
here=$(cd "$(dirname "$0")" && pwd)
state=${FV_STATE_DIR:-"$HOME/Library/Application Support/Farview"}
mkdir -p "$state"
port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])' 2>/dev/null || echo 7420)
FV_STATE_DIR="$state" FV_PROFILE_DIR="${FV_PROFILE_DIR:-$state}" FV_NEW_TABS=1 \
  "$here/pion-host" --tab "$port" about:newtab > "$state/host.log" 2>&1 &
i=0
while [ $i -lt 50 ] && ! nc -z 127.0.0.1 "$port" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
FV_HOST_BIN="$here/pion-host" FV_APP_PORT=$port exec "$here/pion-app"
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
  codesign -f -s - --deep "$A" 2> /dev/null || true
  # (LaunchServices learns of its schemes now, not only once it is moved to /Applications)
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$A" 2> /dev/null || true
  echo "app_bundle: $A (http, https; html, xhtml, svg, pdf)"
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
MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;application/xhtml+xml;
StartupNotify=true
EOF
  echo "app_bundle: dist/farview.desktop (x-scheme-handler/http, https; text/html)"
fi
