#!/usr/bin/env python3
"""Start a server on a free port per scenario, run p10probe against it, stop it.

Usage:
    python3 tools/p10probe/run_probe.py [--server PATH] [--probe PATH]
                                        [--out DIR] [scenario]

The server runs with stdin from /dev/null: without a TTY it skips its console
instead of reading EOF and shutting down. Its log is kept in --out (a temp
directory by default) and its proto10 lines are printed after the run, so a
failing scenario comes with the server's own reason.
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def _game_value(key):
    """A value from common/game.cmake, the one place the game's names are set."""
    import re
    text = open(os.path.join(ROOT, "common", "game.cmake")).read()
    return re.search(r'set\(%s "([^"]*)"\)' % key, text).group(1)

DEFAULT_SERVER = os.path.join(ROOT, "server", "build", _game_value("GAME_SERVER_NAME"))
DEFAULT_PROBE = os.path.join(HERE, "build", "p10probe")


def free_udp_port(start=33000):
    # 32887 and 32900+ are used by other developers' servers
    for port in range(start, start + 100):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            try:
                s.bind(("", port))
                return port
            except OSError:
                continue
    sys.exit("no free UDP port for the test server")


def start_server(binary, log_path, extra_args=(), extra_env=None):
    """Returns (proc, port) once the server reports 'Server started'."""
    if not os.path.isfile(binary):
        sys.exit("server binary not found: %s (run `make server`)" % binary)
    port = free_udp_port()
    log = open(log_path, "w")
    # the probe opens more than [net] max_connections_per_ip (4) connections
    # from 127.0.0.1 in a window: the guard answers the rest with data 0 and
    # every scenario after the fourth read as a wrong disconnect reason.
    # SP_MASTER=0: a test server never announces itself on the public list.
    # SP_MAP_DOWNLOAD=0: tests never reach the network.
    env = dict(os.environ, SP_MAX_PER_IP="0", SP_MASTER="0", SP_MAP_DOWNLOAD="0", **(extra_env or {}))
    proc = subprocess.Popen([binary, "--port", str(port), "--listen", *extra_args],
                            cwd=os.path.dirname(binary), stdin=subprocess.DEVNULL,
                            stdout=log, stderr=subprocess.STDOUT, env=env)
    deadline = time.time() + 15
    while time.time() < deadline:
        if proc.poll() is not None:
            sys.exit("server exited early (code %s), see %s" % (proc.returncode, log_path))
        with open(log_path, errors="replace") as fh:
            if "Server started" in fh.read():
                break
        time.sleep(0.2)
    else:
        stop(proc)
        sys.exit("server did not report 'Server started' within 15 s, see %s" % log_path)
    # it announces itself right before entering the network loop
    time.sleep(0.5)
    return proc, port


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# the handshake scenario wants a VXL, which the server sends only for a map
# with no stored ngmap whose encoding failed (Creative_Flat ships as a VXL);
# everything else plays on the default match (Busan_Port has its ngmap)
SCENARIOS = ["handshake", "ngmap", "mismatch", "idle", "garbage", "early", "vanilla", "unknown"]
VXL_SERVER = (["--gamemode", "creative", "--map", "Creative_Flat"], {"SP_NGMAP_FAIL": "1"})


def run_scenario(args, out, name, extra_args, extra_env):
    log_path = os.path.join(out, "server-%s.log" % name)
    # a data folder of its own: the server keeps every ngmap it encoded in
    # <data>/ngmap and serves it on the next run, which would hide the VXL path
    data = os.path.join(out, "data-%s" % name)
    os.makedirs(data, exist_ok=True)
    server, port = start_server(os.path.abspath(args.server), log_path,
                                ["--data", data, *extra_args], extra_env)
    print("server on 127.0.0.1:%d, log %s" % (port, log_path), flush=True)
    try:
        # idle alone needs ~5 s; one scenario stays well under a minute
        code = subprocess.call([args.probe, "127.0.0.1", str(port), name], timeout=120)
    except subprocess.TimeoutExpired:
        print("FAIL probe did not finish within 120 s")
        code = 1
    finally:
        stop(server)
    with open(log_path, errors="replace") as fh:
        # the server colours its log; the escapes are noise in a terminal report
        lines = [re.sub(r"\x1b\[[0-9;]*m", "", l).rstrip() for l in fh if "proto10" in l]
    if lines:
        print("server proto10 log (%s):" % name)
        for line in lines:
            print("  " + line)
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", nargs="?", default="all")
    ap.add_argument("--server", default=DEFAULT_SERVER, help="server binary")
    ap.add_argument("--probe", default=DEFAULT_PROBE, help="p10probe binary")
    ap.add_argument("--out", help="directory for the server logs (default: a temp dir)")
    args = ap.parse_args()

    if not os.path.isfile(args.probe):
        sys.exit("probe binary not found: %s (run `make probe-proto10`)" % args.probe)
    if args.scenario != "all" and args.scenario not in SCENARIOS:
        sys.exit("unknown scenario %s (%s, all)" % (args.scenario, ", ".join(SCENARIOS)))
    out = args.out or tempfile.mkdtemp(prefix="p10probe-")
    os.makedirs(out, exist_ok=True)

    names = SCENARIOS if args.scenario == "all" else [args.scenario]
    code = 0
    for name in names:
        extra = VXL_SERVER if name == "handshake" else ([], {})
        code |= run_scenario(args, out, name, *extra)
    sys.exit(1 if code else 0)


if __name__ == "__main__":
    main()
