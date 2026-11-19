#!/usr/bin/env python3
"""Headless benchmark: compare server builds on bot matches, fast and in parallel.

The server runs alone (no client, nothing drawn) with SP_FAST (the game's clock
counts ticks and the loop never sleeps: 600 s of a 16 bot match take a few real
seconds), SP_BOTS_PLAY (bots play with nobody in) and SP_EXIT_AFTER (it ends
itself). The same seed plays the same match: a difference between two builds
on the same seeds is the change, not luck (CONTRIBUTING.md, "Benchmarks").

    tools/bench.py save before                    # keep the current build as "before"
    ... change the code, make server ...
    tools/bench.py run --bin before --bin current \\
        --match babel:babelstairs --match ctf:2Fort_CTF --seeds 8 --bots 16

    tools/bench.py check --match babel:babelstairs   # same seed twice: identical?
    tools/bench.py check --bin before --bin current --match ctf:2Fort_CTF  # a refactor: the same match?

"current" is server/build's binary; a saved one lives in server/build/bench/.
Metrics are counts of the server's EVT lines and a few figures of its log; add
your own with --metric name=regex (counted per match). Only the graphical
harness (the client's harness.py) draws anything: use it to look.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def server_name():
    """The server binary's name from common/game.cmake (GAME_SERVER_NAME)."""
    with open(os.path.join(ROOT, "common", "game.cmake")) as fh:
        m = re.search(r'set\(GAME_SERVER_NAME "([^"]+)"\)', fh.read())
    return m.group(1)


BUILD = os.path.join(ROOT, "server", "build")
CURRENT = os.path.join(BUILD, server_name())
SAVED = os.path.join(BUILD, "bench")
OUT = os.path.join(BUILD, "bench-runs")

# name: regex counted over the log, or a function of the log text
METRICS = {
    "kills": r" death killer=",
    "falls": r" death killer=\S+ reason=4",
    "pickups": r"intel action=pickup",
    "captures": r"intel action=capture",
    "blocks": r"block action=build \S+ accepted=1",
    "plans_partial": r"bot_nav action=plan \S+ result=partial",
    "stuck": r"bot_path action=stuck",
    "miner_digs": r" bot_miner_dig ",
    "miner_surface": r" bot_miner_dig .* surface=1",
    "drills": r"bot_path action=drill \S+ ok=1",
}


def _tick_ms(log):
    m = re.search(r"Fast run: \d+ ticks, avg ([0-9.]+) ms a tick", log)
    if m:
        return float(m.group(1))
    v = [float(x) for x in re.findall(r"Tick: \d+ ticks, avg ([0-9.]+) ms", log)]
    return statistics.mean(v) if v else None


def _babel_whole(log):
    v = [float(x) for x in re.findall(r"Babel staircase of team \d whole after (\d+) s", log)]
    return statistics.mean(v) if v else None


def _miner_depth(log):
    # median ground over a Miner bot's spade digs (0: open sky; CONTRIBUTING.md)
    v = [int(x) for x in re.findall(r" bot_miner_dig \S+ depth=(\d+)", log)]
    return statistics.median(v) if v else None


def _miner_surface_pct(log):
    v = re.findall(r" bot_miner_dig \S+ depth=\d+ surface=(\d)", log)
    return 100.0 * v.count("1") / len(v) if v else None


FIGURES = {"tick_ms": _tick_ms, "babel_whole_s": _babel_whole, "miner_depth": _miner_depth,
           "miner_surface_pct": _miner_surface_pct}


def binary(label):
    if label == "current":
        return CURRENT
    path = os.path.join(SAVED, label)
    if not os.path.isfile(path):
        sys.exit("no saved build %r (tools/bench.py save %s)" % (label, label))
    return path


def run_one(job):
    label, mode, map_name, seed, bots, seconds, port = job
    out_dir = os.path.join(OUT, label)
    os.makedirs(out_dir, exist_ok=True)
    log_path = os.path.join(out_dir, "%s_%s_%s.log" % (mode, map_name, seed))
    env = dict(os.environ)
    # SP_MASTER=0: a benchmark's servers are not on the public server list
    # (config.toml lists every server on our master: each run was listed
    # with this machine's address)
    env.update({"SP_FAST": "1", "SP_BOTS_PLAY": "1", "SP_EXIT_AFTER": str(seconds), "SP_RAND_SEED": str(seed),
                "SP_EVENTLOG": "1", "SP_MASTER": "0",
                # the repository's maps only: a missing one is an error, never a download
                "SP_MAP_DOWNLOAD": "0"})
    cmd = [binary(label), "--port", str(port), "--gamemode", mode, "--map", map_name]
    if bots:
        cmd += ["--bots", str(bots)]
    with open(log_path, "w") as log:
        try:
            code = subprocess.run(cmd, cwd=BUILD, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                  env=env, timeout=max(600, seconds)).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    text = re.sub(r"\x1b\[[0-9;]*m", "", open(log_path, errors="replace").read())
    values = {name: len(re.findall(rx, text)) for name, rx in METRICS.items()}
    for name, fn in FIGURES.items():
        values[name] = fn(text)
    values["crash"] = int(code != 0)
    return (label, mode, map_name, seed, values, log_path)


def matches(args):
    out = []
    for m in args.match:
        mode, _, map_name = m.partition(":")
        out.append((mode, map_name))
    return out


def cmd_save(args):
    os.makedirs(SAVED, exist_ok=True)
    shutil.copy2(CURRENT, os.path.join(SAVED, args.name))
    print("saved %s as %s" % (CURRENT, os.path.join(SAVED, args.name)))


def cmd_run(args):
    for m, rx in (x.split("=", 1) for x in args.metric):
        METRICS[m] = rx
    jobs = []
    port = args.port
    for label in args.bin:
        binary(label)
        for mode, map_name in matches(args):
            for seed in range(1, args.seeds + 1):
                jobs.append((label, mode, map_name, seed, args.bots, args.seconds, port))
                # a server holds the game's port and WebRTC's (+1):
                # start each match's pair 2 on, parallel matches took each
                # other's and failed to start ("Failed to create server")
                port += 2
    print("%d matches, %d at a time" % (len(jobs), args.jobs), flush=True)
    began = time.time()
    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for r in pool.map(run_one, jobs):
            results.append(r)
    print("done in %.1f s" % (time.time() - began))
    names = list(METRICS) + list(FIGURES) + ["crash"]
    for mode, map_name in matches(args):
        print("\n== %s on %s, %d bots, %d s, seeds 1-%d" % (mode, map_name, args.bots, args.seconds, args.seeds))
        head = "%-14s" % "metric" + "".join("%18s" % b for b in args.bin)
        if len(args.bin) == 2:
            head += "%12s" % ("%s-%s" % (args.bin[1][:4], args.bin[0][:4]))
        print(head)
        for name in names:
            row, means = "%-14s" % name, []
            for label in args.bin:
                v = [r[4][name] for r in results if r[0] == label and r[1] == mode and r[2] == map_name
                     and r[4][name] is not None]
                if not v:
                    row += "%18s" % "-"
                    means.append(None)
                    continue
                mean = statistics.mean(v)
                sd = statistics.stdev(v) if len(v) > 1 else 0.0
                row += "%18s" % ("%.2f ±%.2f" % (mean, sd))
                means.append(mean)
            if len(means) == 2 and None not in means:
                row += "%12s" % ("%+.2f" % (means[1] - means[0]))
            print(row)
    print("\nlogs: %s" % OUT)


TIMING = re.compile(r" (ms|wait|took|real|avg_ms|ready_in)=-?[0-9.]+")
# events that measure the real machine, not the game (a think over 5 real ms)
MEASURES = (" bot_slow ",)


def _events(path):
    out = []
    for line in open(path, errors="replace"):
        m = re.search(r"EVT \d+ (.*)", re.sub(r"\x1b\[[0-9;]*m", "", line))
        if m and not any(x in line for x in MEASURES):
            out.append(TIMING.sub("", m.group(1)).strip())
    return out


def cmd_check(args):
    ok = True
    for mode, map_name in matches(args):
        logs = []
        # one build twice (determinism), or two builds once each: a change
        # that should play the same match (a refactor) plays it event for
        # event, which a mean over seeds cannot show
        bins = args.bin if len(args.bin) == 2 else [args.bin[0]] * 2
        for k in range(2):
            label, *_, path = run_one((bins[k], mode, map_name, args.seed, args.bots, args.seconds, args.port))
            # one port for both (one after the other): the WebRTC start line
            # names it, and two ports read as two matches
            moved = path.replace(".log", ".check%d.log" % k)
            os.replace(path, moved)
            logs.append(moved)
        a, b = _events(logs[0]), _events(logs[1])
        diff = next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), None)
        if diff is None and len(a) == len(b):
            print("%s on %s: identical, %d events" % (mode, map_name, len(a)))
        else:
            ok = False
            k = diff if diff is not None else min(len(a), len(b))
            print("%s on %s: differ at event %d of %d/%d" % (mode, map_name, k, len(a), len(b)))
            if diff is not None:
                print("  A: %s\n  B: %s" % (a[k][:160], b[k][:160]))
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("save", help="keep the current server build under a name")
    s.add_argument("name")
    for name in ("run", "check"):
        p = sub.add_parser(name)
        p.add_argument("--bin", action="append", default=None,
                       help="a saved build's name or 'current' (twice to compare)")
        p.add_argument("--match", action="append", required=True, help="mode:Map, e.g. babel:babelstairs")
        p.add_argument("--bots", type=int, default=16)
        p.add_argument("--seconds", type=int, default=600, help="game seconds a match (default 600)")
        p.add_argument("--port", type=int, default=35000)
        if name == "check":
            p.add_argument("--seed", type=int, default=1)
        if name == "run":
            p.add_argument("--seeds", type=int, default=4)
            p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
            p.add_argument("--metric", action="append", default=[], help="name=regex, counted per match")
    args = ap.parse_args()
    if args.cmd != "save" and not args.bin:
        args.bin = ["current"]
    {"save": cmd_save, "run": cmd_run, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    main()
