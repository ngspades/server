#!/bin/sh
# Builds the Ace of Spades map generators in this folder (public domain, Sean
# Barrett "nothings": see license.txt). The map size is a compile-time option
# of stb_aos.inl, so one binary is built per generator and per size:
# aos_<name>_<n>.
#
# usage: tools/aosmap/build.sh [outdir] [size...]
#   outdir  where the binaries go (default: tools/aosmap/build, git-ignored)
#   size    the map's side in blocks (default: 512 1024)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/build}
if [ $# -gt 0 ]; then
    shift
fi
sizes=${*:-512 1024}

mkdir -p "$out"
for n in $sizes; do
    for src in "$here"/aos_*.c; do
        name=$(basename "$src" .c)
        ${CC:-cc} -O2 -w -DMAP_X="$n" -DMAP_Y="$n" -o "$out/${name}_$n" "$src" -lm
        echo "built $out/${name}_$n"
    done
done
