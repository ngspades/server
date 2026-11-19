#!/usr/bin/env python3
"""Builds and runs the 0.75 extensions' unit test (ext075test.c with the
server's Legacy/Ext075.c and tomlc99), with the server's warnings as errors.

    python3 server/tools/ext075test/run.py
"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
server = os.path.normpath(os.path.join(here, "..", ".."))
root = os.path.dirname(server)
out_dir = os.path.join(server, "build", "ext075test")
os.makedirs(out_dir, exist_ok=True)
binary = os.path.join(out_dir, "ext075test")

cc = os.environ.get("CC", "cc")
cmd = [
    cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g",
    "-I", os.path.join(server, "Source"),
    "-I", os.path.join(server, "Extern"),
    "-I", os.path.join(server, "Extern", "tomlc99"),
    "-I", os.path.join(root, "common", "enet", "include"),
    "-I", os.path.join(root, "common", "enet-compat"),
    "-I", os.path.join(root, "common"),
    "-I", os.path.join(root, "netlib", "include"),
    os.path.join(here, "ext075test.c"),
    os.path.join(server, "Source", "Server", "Legacy", "Ext075.c"),
    os.path.join(root, "common", "proto10", "text.c"),
    os.path.join(server, "Extern", "tomlc99", "tomlc99", "toml.c"),
    "-lm", "-o", binary,
]
build = subprocess.run(cmd)
if build.returncode != 0:
    sys.exit("ext075test: the build failed")
sys.exit(subprocess.run([binary]).returncode)
