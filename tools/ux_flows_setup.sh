#!/bin/bash
# Sets up deps/ (invar, curl-impersonate, libnode as CI gets it) and builds out/pion-{host,agent,app} on macOS
# arm64, for tools/ux_flows.sh on a machine of its own (a deps symlink from elsewhere is replaced by a real dir).
set -u
export PATH=$HOME/.pion/bin:$PATH
[ -L deps ] && rm -f deps
mkdir -p deps
sh tools/deps_pion.sh 2>&1 | tail -3
if [ ! -f deps/node/lib/libnode.dylib ]; then
  MM=deps/.mm/micromamba
  mkdir -p deps/.mm
  [ -x $MM ] || { curl -fsSL https://github.com/mamba-org/micromamba-releases/releases/download/2.10.0-0/micromamba-osx-arm64 -o $MM; echo "c547639f5caf742236641c362746abb4a3505774753ecd08fe341643670c4c01  $MM" | shasum -a 256 -c - && chmod +x $MM; }
  $MM create -y -q -r $PWD/deps/.mm/root -p $PWD/deps/.mm/n24 -c conda-forge 'nodejs=24.*' 2>&1 | tail -2
  mkdir -p deps/node/include deps/node/lib
  cp -R deps/.mm/n24/include/node deps/node/include/
  cp -P deps/.mm/n24/lib/*.dylib deps/node/lib/
  ln -sf libnode.137.dylib deps/node/lib/libnode.dylib
fi
ls deps/node/lib | head -3
mkdir -p out
for c in host agent app; do
  t0=$(date +%s)
  pionc build -o out/pion-$c pion/cmd/$c > build-$c.log 2>&1; echo "build $c rc=$? $(( $(date +%s)-t0 ))s"
  grep -E '"ok":false|error:' build-$c.log | grep -v warning | head -5
done
ls -la out/
