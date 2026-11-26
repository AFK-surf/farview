#!/bin/sh
# Writes deps/icon_pack, the icon pack pion/packages/shell_ui draws its icons from (deps/ is gitignored).
#
#   sh tools/deps_icons.sh                  # default: the empty pack, so invar's own open icons are drawn
#   FV_ICONS=central sh tools/deps_icons.sh # Farview's own builds: Central Icons by Iconists
#
# Central Icons are licensed to AFK for its products (https://iconists.co/license): they may be drawn in Farview's own
# builds, but the icons themselves must not be handed on. So they are never committed here: this script fetches the
# pinned npm packages and generates only the names in tools/icon_map.txt (at most 300 per style, as the license says).
# Builds without FV_ICONS=central draw the same names from invar's MIT icons.
set -eu
cd "$(dirname "$0")/.."
OUT=deps/icon_pack
VER=1.2.31
mode=${FV_ICONS:-invar}
case "$mode" in
  invar)
    rm -rf "$OUT"
    python3 tools/gen_icon_pack.py "$OUT"
    ;;
  central)
    CACHE=deps/icon_pack_src/$VER
    if [ ! -d "$CACHE/outlined/package" ] || [ ! -d "$CACHE/filled/package" ]; then
      rm -rf "$CACHE" && mkdir -p "$CACHE/outlined" "$CACHE/filled"
      (cd "$CACHE" && npm pack --silent \
        "@central-icons-react/round-outlined-radius-2-stroke-2@$VER" \
        "@central-icons-react/round-filled-radius-2-stroke-2@$VER" >/dev/null)
      tar xzf "$CACHE"/central-icons-react-round-outlined-radius-2-stroke-2-$VER.tgz -C "$CACHE/outlined"
      tar xzf "$CACHE"/central-icons-react-round-filled-radius-2-stroke-2-$VER.tgz -C "$CACHE/filled"
      rm -f "$CACHE"/*.tgz
    fi
    rm -rf "$OUT"
    python3 tools/gen_icon_pack.py "$OUT" "$CACHE/outlined/package" "$CACHE/filled/package"
    ;;
  *)
    echo "deps_icons: FV_ICONS must be invar or central, got $mode" >&2
    exit 1
    ;;
esac
echo "deps_icons: $OUT ($mode)"
