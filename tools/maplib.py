"""Read a map file in either of our two formats.

The maps repository holds a map as `<Name>.vxl` (the Ace of Spades format)
or, once it is converted, as `<Name>.ngmap` (the codec protocol 1.0 sends
maps in, common/ngmap). The game and the server still read VXL in this
step, so a tool that wants a map's bytes or its path asks here instead of
naming the extension: `map_file()` finds whichever file exists (ngmap
first) and `load_vxl()` hands back VXL bytes, decoding an ngmap through the
same ngmap_cli the client and the server link.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "common", "ngmap")
BUILD = os.path.join(SOURCE, "build")
CLI = os.path.join(BUILD, "ngmap_cli")
MAP_EXTS = (".ngmap", ".vxl")


def ensure_cli():
    """the ngmap command line tool, built on first use

    The codec is C++ and the tools are Python, so every tool that meets an
    ngmap runs the same binary the game does rather than growing a second
    decoder. A tag_maps.py worker pool asks from several processes at once:
    the lock file makes one build and the others wait for it.
    """
    if os.path.exists(CLI):
        return CLI
    os.makedirs(BUILD, exist_ok=True)
    lock = CLI + ".lock"
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        for _ in range(1200):  # up to 10 minutes; the build itself is seconds
            if os.path.exists(CLI):
                return CLI
            time.sleep(0.5)
        raise RuntimeError("ngmap_cli is still being built by another process: %s" % lock)
    try:
        subprocess.run(["cmake", "-S", SOURCE, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["cmake", "--build", BUILD, "-j", str(os.cpu_count() or 2),
                        "--target", "ngmap_cli"], check=True)
    finally:
        os.close(fd)
        try:
            os.unlink(lock)
        except FileNotFoundError:
            pass
    if not os.path.exists(CLI):
        raise RuntimeError("cmake did not produce %s" % CLI)
    return CLI


def load_vxl(path):
    """the VXL bytes of a `.vxl` or `.ngmap` map

    An ngmap is decoded into a temporary file (the tool's own contract) and
    read back; nothing is kept, so the caller may ask for the same map as
    often as it likes.
    """
    if path.endswith(".vxl"):
        with open(path, "rb") as fh:
            return fh.read()
    if not path.endswith(".ngmap"):
        raise ValueError("%s is neither .vxl nor .ngmap" % path)
    cli = ensure_cli()
    with tempfile.TemporaryDirectory(prefix="ngmap-decode-") as tmp:
        out = os.path.join(tmp, "map.vxl")
        r = subprocess.run([cli, "decode", path, out], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("%s: %s" % (path, (r.stderr or r.stdout).strip()))
        with open(out, "rb") as fh:
            return fh.read()


def map_file(folder, name):
    """the map's path in `folder`: the ngmap when both exist, else the VXL

    ngmap first: once a folder has been converted, the VXL is stale (or was
    never there); reading it would compare a map with an older copy of
    itself.
    """
    for ext in MAP_EXTS:
        path = os.path.join(folder, name + ext)
        if os.path.exists(path):
            return path
    return None


def norm(name):
    """the name every alias is filed under: lower case, letters and digits
    only

    One map is written many ways ("2Fort CTF", "2fort", "2Fort_CTF"), so a
    name is compared after this: the server does the same in C
    (MapDownload.c), and the two must agree or an alias is never found.
    """
    return "".join(c for c in name.lower() if "a" <= c <= "z" or "0" <= c <= "9")


def resolve(repo, name):
    """the folder of the maps repository that `name` means, or None

    The folder's own name first, then one that differs in case only, then
    aliases.json (tools/map_aliases.py). None when the repository has no
    such map: the caller reports it, never downloads a guess.
    """
    root = os.path.join(repo, "maps")
    folders = sorted(os.listdir(root))
    if name in folders:
        return name
    low = name.lower()
    for folder in folders:
        if folder.lower() == low:
            return folder
    key = norm(name)
    if not key:
        return None
    try:
        with open(os.path.join(repo, "aliases.json"), encoding="utf-8") as fh:
            aliases = json.load(fh)
    except (OSError, ValueError):
        return None
    folder = aliases.get(key) if isinstance(aliases, dict) else None
    if isinstance(folder, str) and os.path.isdir(os.path.join(root, folder)):
        return folder
    return None


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="print a map's VXL bytes to stdout")
    ap.add_argument("map", help="a .vxl or .ngmap file")
    args = ap.parse_args()
    sys.stdout.buffer.write(load_vxl(args.map))


def solid_masks(vxl, width=512, height=512, depth=64):
    """each column's solid cells as an integer (bit z set: solid), x
    fastest as the file stores the columns; None for bytes that are not a
    VXL of that size. Two files of one map compare equal here however
    their writers spanned it: the duplicate test and the round trip check
    both lean on that

    A span (N, S, E, A) is air down to S, then solid from S to the next
    span's air start (its A byte), or to the bottom on the last span (N 0)
    """
    out = []
    p, n = 0, len(vxl)
    full = (1 << depth) - 1
    try:
        for _ in range(width * height):
            mask = 0
            while True:
                count, s, e = vxl[p], vxl[p + 1], vxl[p + 2]
                # S == depth with E == S - 1 is a column with no cell at all
                # (no water layer under it): valid, and some maps have them
                if s > depth or e >= depth or e + 1 < s:
                    return None
                if count == 0:
                    mask |= full & ~((1 << s) - 1)
                    p += 4 * (e - s + 2)
                    break
                p += 4 * count
                a = vxl[p + 3]
                # an air start past the bottom is no VXL of this depth (and the
                # mask would not fit the 8 bytes shape_hash writes)
                if a > depth:
                    return None
                mask |= ((1 << a) - 1) & ~((1 << s) - 1)
            out.append(mask)
    except IndexError:
        return None
    return out if p == n else None


SIDES = (512, 1024)  # the square map sizes the game plays


def vxl_side(vxl, depth=64):
    """the side of a square VXL map (512 or 1024), None when the bytes are
    neither: a VXL carries no size, so the walk that ends exactly at the
    file's end is the one that names it. The cheap guess first (a 1024 map is
    four times the columns, so its file is rarely under 4 MB)
    """
    order = SIDES if len(vxl) < (4 << 20) else tuple(reversed(SIDES))
    for side in order:
        if solid_masks(vxl, side, side, depth) is not None:
            return side
    return None


def shape_hash(vxl):
    """the map's shape as 64 lowercase hex digits, None for bytes that are
    not a VXL of a size the game plays (512 or 1024 a side, `vxl_side`)

    SHA-256 of every column's solid cells as 8 little-endian bytes, in the
    file's order (`solid_masks`). Two files of one map give the same digest
    however they were spanned, and the server sends it in HELLOINFO as
    "map_shape" (common/maphash/maphash.h): that walk and this one must stay
    identical, so the maps repository's shapes.json and the server list's
    lookups agree
    """
    side = vxl_side(vxl)
    if side is None:
        return None
    masks = solid_masks(vxl, side, side)
    h = hashlib.sha256()
    for m in masks:
        h.update(m.to_bytes(8, "little"))
    return h.hexdigest()
