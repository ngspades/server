#!/usr/bin/env python3
# Maps from nothings' Ace of Spades generators.
#
# tools/aosmap/ holds Sean Barrett's "nothings" map generators for Ace of
# Spades, in the public domain (tools/aosmap/license.txt). The map size is a
# compile-time option there (-DMAP_X/-DMAP_Y on stb_aos.inl), so this tool
# builds the generator for the size asked for (tools/aosmap/build.sh) and
# runs it, then writes the map in the server's own layout:
#
#     <out>/<Name>/<Name>.vxl    the map the generator wrote
#     <out>/<Name>/<Name>.toml   its size, and the two spawn rectangles
#
# usage: tools/gen_maps.py --kind caves --size 1024 --seed 7 \
#            --name Caves_1024_7 --out /tmp/maps
#        tools/gen_maps.py --kind all --size 1024 --seed 1 --out /tmp/maps
#
# The VXL is checked with the repository's own reader (tools/maplib.py)
# before it is kept: the server trusts a map file's size from its .toml, so
# a stream that does not walk to its exact end must never land in a maps
# folder.
import argparse
import os
import random
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import maplib  # noqa: E402  (the repository's VXL reader)

AOSMAP = os.path.join(TOOLS, "aosmap")
BUILD = os.path.join(AOSMAP, "build")
# kind (the aos_<kind>.c generator) -> the name the map is filed under
KINDS = {
    "caves": "Caves",
    "forest_river": "Forest_River",
    "incomplete_fortress": "Incomplete_Fortress",
    "rainbow_towers": "Rainbow_Towers",
    "sinc": "Sinc",
    "skybridge": "Skybridge",
    "slope": "Slope",
}
DEPTH = 64


def generator_binary(kind, size, outdir=BUILD):
    """the generator built for this size, built first when it is missing

    stb_aos.inl and the seven aos_*.c are the sources; a binary older than
    either is built again, so an edited generator never generates the map it
    used to.
    """
    binary = os.path.join(outdir, "aos_%s_%d" % (kind, size))
    src = os.path.join(AOSMAP, "aos_%s.c" % kind)
    sources = [src, os.path.join(AOSMAP, "stb_aos.inl"), os.path.join(AOSMAP, "stb.h")]
    stale = not os.path.exists(binary)
    if not stale:
        when = os.path.getmtime(binary)
        stale = any(os.path.getmtime(s) > when for s in sources if os.path.exists(s))
    if stale:
        subprocess.run(["bash", os.path.join(AOSMAP, "build.sh"), outdir, str(size)], check=True)
    if not os.path.exists(binary):
        raise SystemExit("%s: build.sh did not produce %s" % (kind, binary))
    return binary


def generate_vxl(kind, size, seed, workdir, flags=()):
    """runs the generator; its VXL bytes, checked against the size

    flags are the generator's own switches ("-n": forest_river without its
    trees, the bare valley)
    """
    binary = generator_binary(kind, size)
    path = os.path.join(workdir, "map.vxl")
    # stb_getopt_param turns "-s<seed>" and "-o<path>" into the seed and the
    # path (an option's parameter may be joined to it)
    r = subprocess.run([binary, "-s%d" % seed, "-o" + path] + list(flags), cwd=workdir,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("%s: generator %s failed (%d): %s" %
                         (kind, binary, r.returncode, (r.stderr or r.stdout).strip()))
    if not os.path.exists(path):
        raise SystemExit("%s: generator %s wrote no map: %s" %
                         (kind, binary, (r.stdout or r.stderr).strip()))
    with open(path, "rb") as fh:
        data = fh.read()
    # the reader walks exactly width*height columns and refuses a stream that
    # ends anywhere else (maplib.shape_hash takes no size: it assumes 512)
    if maplib.solid_masks(data, size, size, DEPTH) is None:
        raise SystemExit("%s: the generator's %d bytes are not a %dx%dx%d VXL" %
                         (kind, len(data), size, size, DEPTH))
    return data


def toml_of(name, kind, size):
    """the map's .toml: the format server/Resources/maps uses

    The spawn rectangles are the generator's own start regions (build_debug
    draws them: blue west, green east), scaled with the map: x size/32
    .. size/4 and size*3/4 .. size*31/32, y size/4 .. size*3/4. z is 0 in
    every point: the server searches the ground for a standable cell.
    """
    x0, x1 = size // 32, size // 4
    y0, y1 = size // 4, size * 3 // 4
    return '''[map]
name = "%(name)s"
author = "nothings' aosmap generator (%(kind)s), ngspades"
fog_color = [128, 200, 255]
map_size = [%(size)d, %(size)d, %(depth)d]
capture_limit = 10

# the generator's own blue (west) and green (east) start regions
[spawnpoints]
team1 = { start = [%(x0)d, %(y0)d, 0], end = [%(x1)d, %(y1)d, 0] }
team2 = { start = [%(x2)d, %(y0)d, 0], end = [%(x3)d, %(y1)d, 0] }
''' % {"name": name, "kind": kind, "size": size, "depth": DEPTH,
       "x0": x0, "x1": x1, "x2": size * 3 // 4, "x3": size * 31 // 32,
       "y0": y0, "y1": y1}


def make_map(out, kind, size, seed, name, flags=()):
    with tempfile.TemporaryDirectory(prefix="aosmap-") as work:
        data = generate_vxl(kind, size, seed, work, flags)
        folder = os.path.join(out, name)
        os.makedirs(folder, exist_ok=True)
        vxl = os.path.join(folder, name + ".vxl")
        # the map first: its .toml is what makes the server look for it
        with open(vxl, "wb") as fh:
            fh.write(data)
        with open(os.path.join(folder, name + ".toml"), "w") as fh:
            fh.write(toml_of(name, kind, size))
    print("wrote %s (%d bytes, %dx%dx%d)" % (vxl, len(data), size, size, DEPTH))
    return vxl


def main():
    ap = argparse.ArgumentParser(
        description="build and run nothings' Ace of Spades map generators "
                    "and write the maps in the server's layout")
    ap.add_argument("--kind", required=True,
                    help=", ".join(sorted(KINDS)) + ", or all")
    ap.add_argument("--size", type=int, default=512,
                    help="the map's side in blocks, 512 or 1024 (default 512)")
    ap.add_argument("--seed", type=int, default=None,
                    help="the generator's seed (default: a random one, printed)")
    ap.add_argument("--name", default=None,
                    help="the map's name (one --kind only; default "
                         "<Kind>_<size>_<seed>)")
    ap.add_argument("--out", required=True, help="the maps folder to write into")
    ap.add_argument("--no-trees", action="store_true",
                    help="forest_river only: leave the trees out (the mountains "
                         "and the river alone)")
    args = ap.parse_args()

    if args.kind == "all":
        kinds = list(KINDS)
    elif args.kind in KINDS:
        kinds = [args.kind]
    else:
        ap.error("unknown --kind '%s': %s or all" % (args.kind, ", ".join(sorted(KINDS))))
    if args.name is not None and len(kinds) > 1:
        ap.error("--name names one map: use it with one --kind, not all")
    if args.no_trees and kinds != ["forest_river"]:
        ap.error("--no-trees is forest_river's switch: use it with --kind forest_river")
    if args.size < 128 or args.size > 4096 or args.size & (args.size - 1):
        ap.error("--size must be a power of two from 128 to 4096 (512 or 1024 today)")

    seed = args.seed
    if seed is None:
        seed = random.randrange(1, 1 << 31)
        print("seed %d" % seed)

    os.makedirs(args.out, exist_ok=True)
    for kind in kinds:
        name = args.name if args.name is not None else "%s_%d_%d" % (KINDS[kind], args.size, seed)
        make_map(args.out, kind, args.size, seed, name, ["-n"] if args.no_trees else [])
    return 0


if __name__ == "__main__":
    sys.exit(main())
