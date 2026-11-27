#!/bin/sh
# Pure-Pion self-contained distribution bundler: builds pion/cmd/host and pion/cmd/agent
# with pionc and bundles their non-system shared libraries (libnode, etc.) plus complete
# license and third-party notice files into:
#   dist/farview-pion-<system>-<arch>.tar.gz  (bin/host, bin/agent, lib/, LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/)
# Every library is resolved relative to the binary (@loader_path on macOS, $ORIGIN on Linux),
# with zero absolute or build-tree @rpath references pointing back to the build machine.
#
# Runtime & static dependency licenses:
#   - Farview first-party code: MIT (LICENSE)
#   - Static/embedded components: AFK-surf/invar (MIT + NOTICE + Apache-2.0/third-party notices),
#     AFK-surf/pion runtime (MIT), curl-impersonate v2.3.2 (MIT/BoringSSL/BSD/Zlib + LGPL libidn2/libunistring),
#     Mozilla Public Suffix List (MPL-2.0, file-level copyleft).
#   - Bundled dynamic libraries: Node.js 24 / libnode (MIT, V8 BSD-3-Clause, ICU Unicode-3.0, OpenSSL Apache-2.0),
#     SQLite (Public Domain). See THIRD_PARTY_NOTICES.md and LICENSES/ for full terms.
#
# Environment variables:
#   PIONC          Path to pionc binary (default: pionc in PATH)
#   PION_QBE       Path to qbe binary (forwarded to pionc)
#   PION_RT_LIB    Path to libpionrt.a (forwarded to pionc)
#   PION_CACHE_DIR Optional CAS cache dir (forwarded to pionc)
#   FV_PREBUILT_DIR Optional dir with prebuilt pion-host/pion-agent to reuse instead of rebuilding
#   FV_GZIP_LEVEL  Optional gzip level 1-9 for tarball compression (default: 6)
set -eu

GZIP_LVL="${FV_GZIP_LEVEL:-6}"
case "$GZIP_LVL" in
  [1-9]) ;;
  *) echo "package_pion: invalid FV_GZIP_LEVEL=$GZIP_LVL (expected 1-9)" >&2; exit 1 ;;
esac

PIONC=${PIONC:-pionc}
command -v "$PIONC" > /dev/null 2>&1 || { echo "package_pion: pionc not found (set PIONC=/path/to/pionc)" >&2; exit 1; }
[ -d deps/invar/packages/shape ] || { echo "package_pion: deps/invar missing — run sh tools/deps_pion.sh first" >&2; exit 1; }

copy_licenses() {
  dest="$1"
  cp LICENSE "$dest/LICENSE"
  cp THIRD_PARTY_NOTICES.md "$dest/THIRD_PARTY_NOTICES.md"
  cp -R LICENSES "$dest/LICENSES"

  [ -f deps/node/LICENSE ] || { echo "package_pion: deps/node/LICENSE missing (copy Node.js LICENSE before packaging)" >&2; exit 1; }
  cp deps/node/LICENSE "$dest/LICENSES/Node.js-LICENSE.txt"

  [ -f deps/invar/NOTICE ] || { echo "package_pion: deps/invar/NOTICE missing" >&2; exit 1; }
  [ -f deps/invar/THIRD_PARTY_NOTICES.md ] || { echo "package_pion: deps/invar/THIRD_PARTY_NOTICES.md missing" >&2; exit 1; }
  [ -d deps/invar/LICENSES ] || { echo "package_pion: deps/invar/LICENSES missing" >&2; exit 1; }
  cp deps/invar/NOTICE "$dest/LICENSES/invar-NOTICE.txt"
  cp deps/invar/THIRD_PARTY_NOTICES.md "$dest/LICENSES/invar-THIRD_PARTY_NOTICES.md"
  cp -R deps/invar/LICENSES "$dest/LICENSES/invar"

  [ -f deps/curl-impersonate/LICENSE ] || { echo "package_pion: deps/curl-impersonate/LICENSE missing — re-run sh tools/deps_pion.sh" >&2; exit 1; }
  mkdir -p "$dest/LICENSES/curl-impersonate"
  cp deps/curl-impersonate/LICENSE* "$dest/LICENSES/curl-impersonate/"
}

case "$(uname -m)" in
  arm64 | aarch64) ARCH=arm64 ;;
  x86_64 | amd64) ARCH=x64 ;;
  *) ARCH=$(uname -m) ;;
esac

if [ "$(uname -s)" = Linux ]; then
  command -v patchelf > /dev/null || { echo "package_pion: patchelf is needed on Linux" >&2; exit 1; }
  N=farview-pion-linux-$ARCH
  D=dist/$N
  rm -rf "$D" && mkdir -p "$D/bin" "$D/lib"
  if [ -n "${FV_PREBUILT_DIR:-}" ] && [ -x "$FV_PREBUILT_DIR/pion-host" ] && [ -x "$FV_PREBUILT_DIR/pion-agent" ]; then
    cp "$FV_PREBUILT_DIR/pion-host" "$D/bin/host"
    cp "$FV_PREBUILT_DIR/pion-agent" "$D/bin/agent"
  else
    "$PIONC" build -o "$D/bin/host" pion/cmd/host
    "$PIONC" build -o "$D/bin/agent" pion/cmd/agent
  fi
  chmod u+w "$D/bin/"*
  strip --strip-unneeded "$D/bin/"*

  # Never bundle host/conda C/C++ runtime libraries (glibc, libstdc++, libgcc_s)
  system() {
    case "$(basename "$1")" in
      ld-linux* | libc.so* | libm.so* | libpthread.so* | libdl.so* | librt.so* | linux-vdso* | libstdc++.so* | libgcc_s.so*) return 0 ;;
    esac
    return 1
  }

  todo="$D/bin/host
$D/bin/agent"
  while [ -n "$todo" ]; do
    f=$(printf '%s\n' "$todo" | head -n 1)
    todo=$(printf '%s\n' "$todo" | tail -n +2)
    [ -n "$f" ] || continue
    for dep in $(LD_LIBRARY_PATH="$PWD/deps/node/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$f" | awk '/=> \//{print $3}'); do
      system "$dep" && continue
      name=$(basename "$dep")
      if [ ! -e "$D/lib/$name" ]; then
        cp "$dep" "$D/lib/$name"
        chmod u+w "$D/lib/$name"
        todo="${todo:+$todo
}$D/lib/$name"
      fi
    done
  done

  for f in "$D/bin/"*; do patchelf --set-rpath '$ORIGIN/../lib' "$f"; done
  if [ -n "$(ls -A "$D/lib" 2>/dev/null)" ]; then
    for f in "$D/lib/"*; do patchelf --set-rpath '$ORIGIN' "$f"; done
  fi
  left=$(for f in "$D/bin/"* "$D/lib/"*; do [ -f "$f" ] || continue; LD_LIBRARY_PATH= ldd "$f" | awk '/not found/{print $1}'; done)
  [ -z "$left" ] || { echo "package_pion: not found next to it: $left" >&2; exit 1; }
  copy_licenses "$D"
  COMPRESSOR=gzip
  if command -v pigz >/dev/null 2>&1; then COMPRESSOR=pigz; fi
  rm -f "dist/$N.tar" "dist/$N.tar.gz"
  tar -C dist -cf "dist/$N.tar" "$N"
  "$COMPRESSOR" -n -"$GZIP_LVL" "dist/$N.tar"
  echo "package_pion: dist/$N.tar.gz ($(du -h "dist/$N.tar.gz" | cut -f1)), $(ls "$D/lib" | wc -l | tr -d ' ') libraries"
  exit 0
fi

N=farview-pion-macos-$ARCH
D=dist/$N
rm -rf "$D" && mkdir -p "$D/bin" "$D/lib"
if [ -n "${FV_PREBUILT_DIR:-}" ] && [ -x "$FV_PREBUILT_DIR/pion-host" ] && [ -x "$FV_PREBUILT_DIR/pion-agent" ]; then
  cp "$FV_PREBUILT_DIR/pion-host" "$D/bin/host"
  cp "$FV_PREBUILT_DIR/pion-agent" "$D/bin/agent"
else
  "$PIONC" build -o "$D/bin/host" pion/cmd/host
  "$PIONC" build -o "$D/bin/agent" pion/cmd/agent
fi
chmod u+w "$D/bin/"*

# Return every non-system, non-@loader_path dependency (including @rpath/*)
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
      for rp in $(rpaths_of "$owner") "$PWD/deps/node/lib"; do
        case "$rp" in
          @loader_path*) rp_dir=$(dirname "$owner")/"${rp#@loader_path}" ;;
          @executable_path*) rp_dir="$D/bin/${rp#@executable_path}" ;;
          /*) rp_dir="$rp" ;;
          *) rp_dir="$PWD/$rp" ;;
        esac
        if [ -e "$rp_dir/$rel" ]; then
          printf '%s\n' "$rp_dir/$rel"
          return 0
        fi
      done
      if [ -e "$PWD/deps/node/lib/$(basename "$rel")" ]; then
        printf '%s\n' "$PWD/deps/node/lib/$(basename "$rel")"
        return 0
      fi
      return 1
      ;;
    *)
      if [ -e "$dep" ]; then
        printf '%s\n' "$dep"
        return 0
      fi
      if [ -e "$PWD/deps/node/lib/$(basename "$dep")" ]; then
        printf '%s\n' "$PWD/deps/node/lib/$(basename "$dep")"
        return 0
      fi
      return 1
      ;;
  esac
}

todo="$D/bin/host
$D/bin/agent"
while [ -n "$todo" ]; do
  f=$(printf '%s\n' "$todo" | head -n 1)
  todo=$(printf '%s\n' "$todo" | tail -n +2)
  [ -n "$f" ] || continue
  for dep in $(outside "$f"); do
    name=$(basename "$dep")
    if [ ! -e "$D/lib/$name" ]; then
      src=$(resolve_dep "$dep" "$f") || { echo "package_pion: cannot resolve macOS dependency $dep (from $f)" >&2; exit 1; }
      cp -L "$src" "$D/lib/$name"
      chmod u+w "$D/lib/$name"
      install_name_tool -id "@loader_path/$name" "$D/lib/$name" 2>/dev/null
      todo="${todo:+$todo
}$D/lib/$name"
    fi
    case "$f" in
      */bin/*) install_name_tool -change "$dep" "@loader_path/../lib/$name" "$f" 2>/dev/null ;;
      *) install_name_tool -change "$dep" "@loader_path/$name" "$f" 2>/dev/null ;;
    esac
  done
done

# Strip build-tree LC_RPATH entries, strip binaries, and ad-hoc sign everything in parallel
SIGN_PIDS=""
SIGN_FAIL=0
for f in "$D/lib/"* "$D/bin/"*; do
  [ -f "$f" ] || continue
  for rp in $(rpaths_of "$f"); do
    install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
  done
  case "$f" in */bin/*) strip -x "$f" 2>/dev/null || true ;; esac
  codesign -f -s - "$f" 2>/dev/null &
  SIGN_PIDS="$SIGN_PIDS $!"
done
for pid in $SIGN_PIDS; do
  wait "$pid" || SIGN_FAIL=1
done
[ "$SIGN_FAIL" -eq 0 ] || { echo "package_pion: codesign failed" >&2; exit 1; }

left=$(for f in "$D/lib/"* "$D/bin/"*; do [ -f "$f" ] || continue; outside "$f"; done)
[ -z "$left" ] || { echo "package_pion: still loads external or @rpath libs: $left" >&2; exit 1; }
left_rp=$(for f in "$D/lib/"* "$D/bin/"*; do [ -f "$f" ] || continue; rpaths_of "$f"; done)
[ -z "$left_rp" ] || { echo "package_pion: leftover LC_RPATH entries: $left_rp" >&2; exit 1; }
ls "$D/lib/"libnode*.dylib >/dev/null 2>&1 || { echo "package_pion: libnode*.dylib missing from $D/lib/" >&2; exit 1; }

copy_licenses "$D"
COMPRESSOR=gzip
if command -v pigz >/dev/null 2>&1; then COMPRESSOR=pigz; fi
rm -f "dist/$N.tar" "dist/$N.tar.gz"
tar -C dist -cf "dist/$N.tar" "$N"
"$COMPRESSOR" -n -"$GZIP_LVL" "dist/$N.tar"
echo "package_pion: dist/$N.tar.gz ($(du -h "dist/$N.tar.gz" | cut -f1)), $(ls "$D/lib" | wc -l | tr -d ' ') libraries"
