#!/bin/bash
# The V8 the host links (Homebrew's node@24: libnode, its headers and the Homebrew libraries it needs) as one
# directory, for a machine without Homebrew.
#   tools/node-bundle.sh pack OUT.tgz     on a machine with Homebrew's node@24
#   tools/node-bundle.sh unpack IN.tgz    in a checkout: deps/node/{include,lib}, each library's own name and its
#                                         references pointing into deps/node/lib (absolute), signed again
set -eu
case "$1" in
pack)
  T=$(mktemp -d)
  mkdir -p "$T/node/include" "$T/node/lib"
  cp -RL /opt/homebrew/opt/node@24/include/node "$T/node/include/"
  todo=(/opt/homebrew/opt/node@24/lib/libnode.137.dylib)
  while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    b=$(basename "$f")
    [ -e "$T/node/lib/$b" ] && continue
    cp -L "$f" "$T/node/lib/$b"
    d=$(dirname "$(readlink -f "$f")")
    for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
      case "$dep" in
        /opt/homebrew/*) todo+=("$dep") ;;
        @loader_path/*) todo+=("$d/${dep#@loader_path/}") ;;
        @rpath/*) todo+=("$d/${dep#@rpath/}") ;;
      esac
    done
  done
  tar -czf "$2" -C "$T" node
  rm -rf "$T"
  ;;
unpack)
  rm -rf deps/node
  tar -xzf "$2" -C deps
  L=$PWD/deps/node/lib
  for f in "$L"/*.dylib; do
    chmod u+w "$f"
    install_name_tool -id "$L/$(basename "$f")" "$f" 2> /dev/null
    for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
      case "$dep" in
        /opt/homebrew/*|@loader_path/*|@rpath/*) install_name_tool -change "$dep" "$L/$(basename "$dep")" "$f" 2> /dev/null ;;
      esac
    done
    codesign -f -s - "$f" 2> /dev/null
  done
  # (what the link names, -lnode: moon.pkg's, the same on Linux, where it is libnode.so)
  ln -sf libnode.137.dylib "$L/libnode.dylib"
  ;;
esac
