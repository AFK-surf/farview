#!/bin/sh
# Fetches pinned pure-Pion native dependencies into deps/:
#   - AFK-surf/invar (OpenType GSUB/GPOS shaping, Skia/pixels/dmath raster, UI/Metal host)
#   - lexiforest/curl-impersonate v2.3.2 (Chrome TLS fingerprint libcurl for pion/packages/fetch)
set -eu
REV=ca66e7d0c3fbf145b708a8d7c28fef5894aacd10
DIR=deps/invar
if [ -d "$DIR/.git" ] && [ "$(git -C "$DIR" rev-parse HEAD 2>/dev/null)" = "$REV" ]; then
  echo "deps_pion: $DIR already at $REV"
else
  rm -rf "$DIR"
  mkdir -p deps
  git clone -q --depth 1 https://github.com/AFK-surf/invar.git "$DIR"
  if [ "$(git -C "$DIR" rev-parse HEAD)" != "$REV" ]; then
    git -C "$DIR" fetch -q --depth 1 origin "$REV"
    git -C "$DIR" checkout -q "$REV"
  fi
  ACTUAL=$(git -C "$DIR" rev-parse HEAD)
  if [ "$ACTUAL" != "$REV" ]; then
    echo "deps_pion: invar revision mismatch: got $ACTUAL, expected $REV" >&2
    exit 1
  fi
  echo "deps_pion: $DIR ready at $(git -C "$DIR" rev-parse --short HEAD)"
fi

sha256_verify() {
  expected="$1"
  file="$2"
  if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$file" | awk '{print $1}')
  else
    actual=$(shasum -a 256 "$file" | awk '{print $1}')
  fi
  if [ "$actual" != "$expected" ]; then
    echo "deps_pion: SHA-256 mismatch for $file: got $actual, expected $expected" >&2
    exit 1
  fi
}

# curl-impersonate (lexiforest/curl-impersonate): libcurl that shakes hands as Chrome does
CI_VER=v2.3.2
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)
    CI_ARCH=arm64-macos
    CI_SHA256=6c0dc841a8f6b7a8f81a0f55197e8241405fb7dcd97df180e66fc6f796c42bd5
    EXTRA=""
    ;;
  Linux-x86_64)
    CI_ARCH=x86_64-linux-gnu
    CI_SHA256=7716be1ec87f13ecc776c211003bd1122c345287829b1f18a494a5467c90ab1e
    EXTRA=include
    ;;
  Linux-aarch64)
    CI_ARCH=aarch64-linux-gnu
    CI_SHA256=1cb74a3b5103df020c252f9efef5a779c87380ee6244b365a90a0e4f7086864e
    EXTRA=include
    ;;
  *)
    echo "deps_pion: no curl-impersonate for $(uname -s)-$(uname -m)" >&2
    exit 1
    ;;
esac

STAMP="${CI_VER}-${CI_ARCH}-${CI_SHA256}"
if [ -f deps/curl-impersonate/libcurl-impersonate.a ] &&
   [ -f deps/curl-impersonate/LICENSE ] &&
   [ "$(cat deps/curl-impersonate/.version 2>/dev/null || true)" = "$STAMP" ]; then
  echo "deps_pion: deps/curl-impersonate already ready ($CI_VER $CI_ARCH)"
else
  rm -rf deps/curl-impersonate
  mkdir -p deps/curl-impersonate
  TMP_TGZ=$(mktemp)
  trap 'rm -f "$TMP_TGZ"' EXIT
  curl -fsSL "https://github.com/lexiforest/curl-impersonate/releases/download/$CI_VER/libcurl-impersonate-$CI_VER.$CI_ARCH.tar.gz" -o "$TMP_TGZ"
  sha256_verify "$CI_SHA256" "$TMP_TGZ"
  LIC_ENTRIES=$(tar -tzf "$TMP_TGZ" | grep '^LICENSE' | tr '\n' ' ')
  # shellcheck disable=SC2086
  tar -xzf "$TMP_TGZ" -C deps/curl-impersonate libcurl-impersonate.a $EXTRA $LIC_ENTRIES
  rm -f "$TMP_TGZ"
  trap - EXIT
  printf '%s\n' "$STAMP" > deps/curl-impersonate/.version
  echo "deps_pion: deps/curl-impersonate ready ($CI_VER $CI_ARCH, sha256 verified)"
fi
