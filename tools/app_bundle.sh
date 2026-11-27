#!/bin/sh
# macOS: dist/Farview.app (and optional dist/Farview-<version>-macos-<arch>.dmg), the desktop browser as a self-contained
# Mac app the system can open links with.
#
#   Contents/MacOS/Farview    the launcher (CFBundleExecutable): starts a host tab for the profile, then becomes the
#                             client (exec, the same process), so the system's Apple Events (a link clicked in another
#                             app, `open -a Farview <url>`, Farview set as the default browser) reach the client's
#                             NSApplication, which opens each link in a new tab (SystemSaid::Link, app_dispatch.pion).
#   Contents/MacOS/pion-host, pion-app   the binaries (out/, built here unless FV_BIN_DIR names built ones)
#   Contents/Frameworks       transitive non-system dylibs (libnode + its deps), rewritten to @loader_path
#   Contents/Resources/AppIcon.icns      generated from FV_ICON_SRC (default: assets/icon.png)
#   Contents/Resources/LICENSE*          first-party & third-party licenses
#   Contents/Info.plist       http/https as its URL schemes (CFBundleURLTypes), so System Settings > Desktop & Dock >
#                             Default web browser lists it. No document types: a local HTML file would open as
#                             file://, and until a file page's origin is its own it could read the person's other
#                             files (proto::system_link_target refuses file:// for the same reason).
#
# Linux: dist/farview.desktop with MimeType x-scheme-handler/http;https (for `xdg-settings set default-web-browser
# farview.desktop`); its Exec runs FARVIEW_BIN_DIR's binaries.
#
# Environment:
#   FV_BIN_DIR          prebuilt pion-host / pion-app dir (default: build into out/)
#   FV_LIB_DIR          optional extra search dir for @rpath dylibs
#   FV_ICON_SRC         source square PNG for AppIcon.icns (default: assets/icon.png)
#   FV_VERSION          bundle short version (default: 0.1.0; leading 'v' stripped)
#   FV_BUILD            bundle build number (default: 1)
#   FV_SIGN_IDENTITY    codesign identity (default: "-" ad-hoc)
#   FV_MAKE_DMG         set to 1 to package dist/Farview-<version>-macos-<arch>.dmg
#   FV_DMG_OUT          optional explicit output path for the .dmg
#   FV_SKIP_LSREGISTER  set to 1 to skip LaunchServices registration (e.g. in CI)
#   FV_REQUIRE_LICENSES set to 1 to fail if third-party dep licenses are missing
#   PIONC               pionc binary path (default: pionc)
set -eu
PIONC=${PIONC:-pionc}
FV_VERSION=${FV_VERSION:-0.1.0}
FV_VERSION=${FV_VERSION#v}
FV_BUILD=${FV_BUILD:-1}
FV_ICON_SRC=${FV_ICON_SRC:-assets/icon.png}
FV_SIGN_IDENTITY=${FV_SIGN_IDENTITY:--}
mkdir -p dist

plist() {
  has_icon="$1"
  cat <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Farview</string>
  <key>CFBundleDisplayName</key><string>Farview</string>
  <key>CFBundleIdentifier</key><string>surf.afk.farview</string>
  <key>CFBundleExecutable</key><string>Farview</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleShortVersionString</key><string>${FV_VERSION}</string>
  <key>CFBundleVersion</key><string>${FV_BUILD}</string>
$(if [ "$has_icon" = 1 ]; then printf '  <key>CFBundleIconFile</key><string>AppIcon</string>\n'; fi)  <key>LSMinimumSystemVersion</key><string>13.0</string>
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
# Stop the background host once the exec'd client exits (exec preserves $$).
app_pid=$$
(
  while kill -0 "$app_pid" 2>/dev/null; do sleep 1; done
  kill "$hpid" 2>/dev/null || true
) </dev/null >/dev/null 2>&1 &
FV_HOST_BIN="$here/pion-host" FV_APP_PORT=$port exec "$here/pion-app" "$@"
EOF
}

copy_licenses() {
  dest="$1"
  strict="$2"
  cp LICENSE "$dest/LICENSE"
  cp THIRD_PARTY_NOTICES.md "$dest/THIRD_PARTY_NOTICES.md"
  cp -R LICENSES "$dest/LICENSES"

  if [ -f deps/node/LICENSE ]; then
    cp deps/node/LICENSE "$dest/LICENSES/Node.js-LICENSE.txt"
  elif [ "$strict" = 1 ]; then
    echo "app_bundle: deps/node/LICENSE missing" >&2; exit 1
  fi
  if [ -f deps/invar/NOTICE ] && [ -f deps/invar/THIRD_PARTY_NOTICES.md ] && [ -d deps/invar/LICENSES ]; then
    cp deps/invar/NOTICE "$dest/LICENSES/invar-NOTICE.txt"
    cp deps/invar/THIRD_PARTY_NOTICES.md "$dest/LICENSES/invar-THIRD_PARTY_NOTICES.md"
    cp -R deps/invar/LICENSES "$dest/LICENSES/invar"
  elif [ "$strict" = 1 ]; then
    echo "app_bundle: deps/invar license files missing" >&2; exit 1
  fi
  if [ -f deps/curl-impersonate/LICENSE ]; then
    mkdir -p "$dest/LICENSES/curl-impersonate"
    cp deps/curl-impersonate/LICENSE* "$dest/LICENSES/curl-impersonate/"
  elif [ "$strict" = 1 ]; then
    echo "app_bundle: deps/curl-impersonate/LICENSE missing" >&2; exit 1
  fi
}

if [ "$(uname -s)" = Darwin ]; then
  case "$(uname -m)" in
    arm64 | aarch64) ARCH=arm64 ;;
    x86_64 | amd64) ARCH=x64 ;;
    *) ARCH=$(uname -m) ;;
  esac

  B=${FV_BIN_DIR:-}
  if [ -z "$B" ]; then
    mkdir -p out
    "$PIONC" build -o out/pion-host pion/cmd/host
    "$PIONC" build -o out/pion-app pion/cmd/app
    B=out
  fi
  A=dist/Farview.app
  rm -rf "$A"
  mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources" "$A/Contents/Frameworks"

  has_icon=0
  if [ -f "$FV_ICON_SRC" ]; then
    swift tools/make_mac_icon.swift "$FV_ICON_SRC" "$A/Contents/Resources/AppIcon.icns"
    [ -s "$A/Contents/Resources/AppIcon.icns" ] || { echo "app_bundle: AppIcon.icns generation failed" >&2; exit 1; }
    has_icon=1
  fi

  plist "$has_icon" > "$A/Contents/Info.plist"
  plutil -lint "$A/Contents/Info.plist" > /dev/null
  launcher > "$A/Contents/MacOS/Farview"
  chmod +x "$A/Contents/MacOS/Farview"
  cp "$B/pion-host" "$B/pion-app" "$A/Contents/MacOS/"
  chmod u+w "$A/Contents/MacOS/pion-host" "$A/Contents/MacOS/pion-app"

  # Recursively bundle every non-system dylib (libnode + its transitive deps) into Contents/Frameworks,
  # rewriting install names to @loader_path and removing build-tree LC_RPATH entries so the bundle is
  # 100% relocatable on any Mac.
  outside() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -v '^/usr/lib/\|^/System/\|^@loader_path/' || true
  }

  rpaths_of() {
    otool -l "$1" | awk '/cmd LC_RPATH/{f=1; next} f && /path /{print $2; f=0}' || true
  }

  resolve_dep() {
    dep="$1"
    owner="$2"
    case "$dep" in
      @rpath/*)
        rel="${dep#@rpath/}"
        for rp in $(rpaths_of "$owner") "${FV_LIB_DIR:-}" "$PWD/deps/node/lib" "$B"; do
          [ -n "$rp" ] || continue
          case "$rp" in
            @loader_path*) rp_dir=$(dirname "$owner")/"${rp#@loader_path}" ;;
            @executable_path*) rp_dir="$A/Contents/MacOS/${rp#@executable_path}" ;;
            /*) rp_dir="$rp" ;;
            *) rp_dir="$PWD/$rp" ;;
          esac
          if [ -e "$rp_dir/$rel" ]; then
            printf '%s\n' "$rp_dir/$rel"
            return 0
          fi
        done
        for d in "${FV_LIB_DIR:-}" "$PWD/deps/node/lib" "$B"; do
          if [ -n "$d" ] && [ -e "$d/$(basename "$rel")" ]; then
            printf '%s\n' "$d/$(basename "$rel")"
            return 0
          fi
        done
        return 1
        ;;
      *)
        if [ -e "$dep" ]; then
          printf '%s\n' "$dep"
          return 0
        fi
        for d in "${FV_LIB_DIR:-}" "$PWD/deps/node/lib" "$B"; do
          if [ -n "$d" ] && [ -e "$d/$(basename "$dep")" ]; then
            printf '%s\n' "$d/$(basename "$dep")"
            return 0
          fi
        done
        return 1
        ;;
    esac
  }

  todo="$A/Contents/MacOS/pion-host
$A/Contents/MacOS/pion-app"
  while [ -n "$todo" ]; do
    f=$(printf '%s\n' "$todo" | head -n 1)
    todo=$(printf '%s\n' "$todo" | tail -n +2)
    [ -n "$f" ] || continue
    for dep in $(outside "$f"); do
      name=$(basename "$dep")
      if [ ! -e "$A/Contents/Frameworks/$name" ]; then
        src=$(resolve_dep "$dep" "$f") || { echo "app_bundle: cannot resolve macOS dependency $dep (from $f); set FV_LIB_DIR" >&2; exit 1; }
        cp -L "$src" "$A/Contents/Frameworks/$name"
        chmod u+w "$A/Contents/Frameworks/$name"
        install_name_tool -id "@loader_path/$name" "$A/Contents/Frameworks/$name" 2>/dev/null
        todo="${todo:+$todo
}$A/Contents/Frameworks/$name"
      fi
      case "$f" in
        */Contents/MacOS/*) install_name_tool -change "$dep" "@loader_path/../Frameworks/$name" "$f" 2>/dev/null ;;
        *) install_name_tool -change "$dep" "@loader_path/$name" "$f" 2>/dev/null ;;
      esac
    done
  done

  for f in "$A/Contents/Frameworks/"* "$A/Contents/MacOS/pion-host" "$A/Contents/MacOS/pion-app"; do
    [ -f "$f" ] || continue
    for rp in $(rpaths_of "$f"); do
      install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
    done
  done

  strip -x "$A/Contents/MacOS/pion-host" "$A/Contents/MacOS/pion-app" 2>/dev/null || true

  left=$(for f in "$A/Contents/Frameworks/"* "$A/Contents/MacOS/pion-host" "$A/Contents/MacOS/pion-app"; do [ -f "$f" ] || continue; outside "$f"; done)
  [ -z "$left" ] || { echo "app_bundle: still loads external or @rpath libs: $left" >&2; exit 1; }
  left_rp=$(for f in "$A/Contents/Frameworks/"* "$A/Contents/MacOS/pion-host" "$A/Contents/MacOS/pion-app"; do [ -f "$f" ] || continue; rpaths_of "$f"; done)
  [ -z "$left_rp" ] || { echo "app_bundle: leftover LC_RPATH entries: $left_rp" >&2; exit 1; }
  ls "$A/Contents/Frameworks/"libnode*.dylib >/dev/null 2>&1 || { echo "app_bundle: libnode*.dylib missing from $A/Contents/Frameworks/" >&2; exit 1; }

  strict_lic=0
  if [ "${FV_REQUIRE_LICENSES:-0}" = 1 ] || [ "${FV_MAKE_DMG:-0}" = 1 ]; then
    strict_lic=1
  fi
  copy_licenses "$A/Contents/Resources" "$strict_lic"

  # Sign inside-out: Frameworks -> helper binaries -> bundle.
  case "$FV_SIGN_IDENTITY" in
    "Developer ID Application:"*)
      for f in "$A/Contents/Frameworks/"*; do
        [ -f "$f" ] || continue
        codesign --force --options runtime --timestamp --sign "$FV_SIGN_IDENTITY" "$f"
      done
      codesign --force --options runtime --timestamp --sign "$FV_SIGN_IDENTITY" "$A/Contents/MacOS/pion-host"
      codesign --force --options runtime --timestamp --sign "$FV_SIGN_IDENTITY" "$A/Contents/MacOS/pion-app"
      codesign --force --options runtime --timestamp --sign "$FV_SIGN_IDENTITY" "$A"
      ;;
    *)
      for f in "$A/Contents/Frameworks/"*; do
        [ -f "$f" ] || continue
        codesign --force --sign "$FV_SIGN_IDENTITY" "$f" 2>/dev/null
      done
      codesign --force --sign "$FV_SIGN_IDENTITY" "$A/Contents/MacOS/pion-host" 2>/dev/null
      codesign --force --sign "$FV_SIGN_IDENTITY" "$A/Contents/MacOS/pion-app" 2>/dev/null
      codesign --force --sign "$FV_SIGN_IDENTITY" "$A" 2>/dev/null
      ;;
  esac
  codesign --verify --deep --strict "$A"

  if [ "${FV_SKIP_LSREGISTER:-0}" != 1 ]; then
    # (LaunchServices learns of its schemes now, not only once it is moved to /Applications)
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$A" 2> /dev/null || true
  fi
  fw_count=$(ls -1 "$A/Contents/Frameworks" | wc -l | tr -d ' ')
  echo "app_bundle: $A (v${FV_VERSION}, ${fw_count} frameworks, icon=${has_icon})"

  if [ "${FV_MAKE_DMG:-0}" = 1 ]; then
    DMG=${FV_DMG_OUT:-dist/Farview-${FV_VERSION}-macos-${ARCH}.dmg}
    STAGE=$(mktemp -d)
    trap 'rm -rf "$STAGE"' EXIT
    ditto "$A" "$STAGE/Farview.app"
    ln -s /Applications "$STAGE/Applications"
    rm -f "$DMG"
    hdiutil create -volname "Farview" -srcfolder "$STAGE" -ov -format UDZO -imagekey zlib-level=9 "$DMG" > /dev/null
    hdiutil verify "$DMG" > /dev/null
    rm -rf "$STAGE"
    trap - EXIT
    echo "app_bundle: $DMG ($(du -h "$DMG" | cut -f1))"
  fi
else
  cat > dist/farview.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Farview
GenericName=Web Browser
Comment=Browse the web
Exec=sh -c 'exec "${FARVIEW_BIN_DIR:-$HOME/.local/lib/farview}/pion-app" "$@"' farview %U
Icon=farview
Terminal=false
Categories=Network;WebBrowser;
MimeType=x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
EOF
  if [ -f "$FV_ICON_SRC" ]; then
    cp "$FV_ICON_SRC" dist/farview.png
  fi
  echo "app_bundle: dist/farview.desktop (x-scheme-handler/http, https)"
fi
