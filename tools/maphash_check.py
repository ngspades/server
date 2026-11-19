#!/usr/bin/env python3
"""Check that the C and Python shape hashes of a map agree.

common/maphash/maphash.h is what the server hashes a map with; maplib.shape_hash
is what the tools deduplicate the maps repository by, and the server list will
look a map up by the digest the server sends in HELLOINFO. A walk
that differed by one cell would make every lookup miss, so this compiles a
driver with the same header and compares it with Python on every map the
server ships, and on three maps round-tripped through a quality 100 ngmap.

Usage:
    python3 tools/maphash_check.py [--maps server/Resources/maps]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import maplib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <maphash/maphash.h>

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <vxl> <w> <h> <d>\n", argv[0]);
        return 2;
    }
    FILE* fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        perror(argv[1]);
        return 2;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0) {
        fclose(fp);
        return 2;
    }
    unsigned char* data = malloc((size_t) len == 0 ? 1 : (size_t) len);
    if (data == NULL || fread(data, 1, (size_t) len, fp) != (size_t) len) {
        fclose(fp);
        free(data);
        return 2;
    }
    fclose(fp);
    char hex[65];
    int  rc = maphash_shape(data, (size_t) len, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), hex);
    free(data);
    if (rc != 0) {
        printf("-\n");
        return 1;
    }
    printf("%s\n", hex);
    return 0;
}
"""


def build_driver(tmp):
    src = os.path.join(tmp, "maphash_driver.c")
    exe = os.path.join(tmp, "maphash_driver")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(DRIVER)
    cc = os.environ.get("CC", "cc")
    r = subprocess.run([cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I",
                        os.path.join(ROOT, "common"), "-o", exe, src], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("cannot build the driver:\n%s" % (r.stderr or r.stdout))
    return exe


def dims_of(folder, name):
    """the map's map_size from its toml; the default the server uses"""
    try:
        with open(os.path.join(folder, name + ".toml"), encoding="utf-8") as fh:
            m = re.search(r"map_size\s*=\s*\[\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\]", fh.read())
    except OSError:
        m = None
    return tuple(int(x) for x in m.groups()) if m else (512, 512, 64)


def c_hash(driver, path, dims):
    r = subprocess.run([driver, path, *(str(d) for d in dims)], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def _as_vxl(path, tmp):
    """a VXL file of the map: an ngmap-only folder (the server unpacks it at
    start-up) is decoded into tmp, as the C driver and the encoder read VXL"""
    if not path.endswith(".ngmap"):
        return path
    out = os.path.join(tmp, "decoded-" + os.path.basename(path)[:-6] + ".vxl")
    with open(out, "wb") as fh:
        fh.write(maplib.load_vxl(path))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--maps", default=os.path.join(ROOT, "server", "Resources", "maps"),
                    help="the map folders to check (default: the server's own)")
    args = ap.parse_args()

    folders = sorted(d for d in os.listdir(args.maps) if maplib.map_file(os.path.join(args.maps, d), d))
    if not folders:
        print("no maps under %s" % args.maps)
        return 1

    failures = 0
    with tempfile.TemporaryDirectory(prefix="maphash-check-") as tmp:
        driver = build_driver(tmp)
        for name in folders:
            path = _as_vxl(maplib.map_file(os.path.join(args.maps, name), name), tmp)
            dims = dims_of(os.path.join(args.maps, name), name)
            with open(path, "rb") as fh:
                data = fh.read()
            want = maplib.shape_hash(data)
            got = c_hash(driver, path, dims)
            ok = want is not None and want == got
            failures += 0 if ok else 1
            print("%s %-22s %s%s" % ("PASS" if ok else "FAIL", name, got,
                                     "" if ok else " (python %s)" % want))

        # a quality 100 ngmap keeps every cell, so it must keep the digest
        print("quality 100 ngmap round trips:")
        for name in folders[:3]:
            path = _as_vxl(maplib.map_file(os.path.join(args.maps, name), name), tmp)
            dims = dims_of(os.path.join(args.maps, name), name)
            with open(path, "rb") as fh:
                vxl = fh.read()
            ngmap = os.path.join(tmp, name + ".ngmap")
            r = subprocess.run([maplib.ensure_cli(), "encode", path, ngmap, "100"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                failures += 1
                print("FAIL %-22s encode: %s" % (name, (r.stderr or r.stdout).strip()))
                continue
            decoded = maplib.load_vxl(ngmap)
            decoded_path = os.path.join(tmp, name + ".vxl")
            with open(decoded_path, "wb") as fh:
                fh.write(decoded)
            want = maplib.shape_hash(vxl)
            py = maplib.shape_hash(decoded)
            got = c_hash(driver, decoded_path, dims)
            ok = want is not None and want == py == got
            failures += 0 if ok else 1
            print("%s %-22s %s%s" % ("PASS" if ok else "FAIL", name, got,
                                     "" if ok else " (vxl %s, decoded %s)" % (want, py)))

    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
