# Contributing

Thank you for looking. This file is how the code is organised, the rules it
keeps and how to test a change. Read it before the code: most of the rules
below were learnt from a bug.

Everything in the repository (code, comments, commits, docs) is written in
English. Report a vulnerability privately, never in an issue: see
[SECURITY.md](SECURITY.md).

## Building and testing

```bash
make server          # Release build, server/build/ngspades-server
make server-asan     # AddressSanitizer + UndefinedBehaviorSanitizer, server/build-asan
make test            # protocol 1.0's codec (fuzzed), the 0.75 extensions, both configs
make smoke           # every game mode played by 16 bots under the sanitizers
```

The server builds with `-Werror`: no warnings. A pull request runs the same
checks in CI (`.github/workflows/ci.yml`).

## Protocols

- **Each profile speaks one protocol.** The default profile speaks protocol
  1.0 only (a 0.75 client is refused with "wrong protocol"); `--profile 075`
  speaks 0.75/0.76 only. New mechanics (classes, perks, vehicles) are 1.0
  features with no 0.75 fallback.
- **The 0.75 wire lives in `server/Source/Server/Legacy/` only.** Its senders
  run only under `profile_is_075()`. Never send a 0.75 packet outside that
  gate.
- **One game, one code path.** A packet of either protocol is parsed by its
  front end and applied through the internal game API
  (`server/Source/Server/GameApi.h`, `game_player_*`), the same functions the
  bots call. Never write a second code path, never build a fake packet:
  change the `game_player_*` both wires share.
- **Every map change calls `proto10_world_cell(server, x, y, z, cause)`**
  right after it. That is how 1.0 clients (and the bots' planner, and the
  supply crates) learn about it.
- **`common/` is the protocol's reference.** The client builds the same
  files: a change to the wire changes `common/proto10/`, its tests and
  `docs/protocol-1.0.md` together.

## Bots are players

Bots are ordinary `player_t` entries with no connection (`player->conn` is
NULL). If you find yourself adding a special case for bots in the join flow,
gameplay or the packet paths, look at how a human does it and copy that.
Every send helper returns early for a NULL connection.

## Game modes are one interface

`server/Source/Server/Gamemodes/Gamemode.h` defines `gamemode_ops_t`. Nothing
outside `Gamemodes/` reads the current mode: a behaviour that depends on the
mode is a hook or a field of the ops, never an `if (mode == ...)`. A new mode
is one file with its ops and one registry line (and its entry in
`common/modes/modes.h` for the client's picker).

## Coordinates (the source of most bugs)

- **Map z grows downwards.** `z = 0` is the top of the world; the floor is
  the smallest solid z found scanning from 0.
- Network coordinates are mirrored: `internal_y = 63 - packet_z`.
- **Water is the bottom layer** (`z = size_z - 1`), solid in the voxels but
  empty to the physics. A column whose first solid block is the last layer is
  open water (`is_water_column()`).
- Every map access goes through `server/Source/Server/MapStore.h`, which takes
  any coordinates: off the map is air. Never read the map through a library
  directly.

## Logging

`server/Source/Server/EventLog.h` writes structured gameplay events, off by
default (`SP_EVENTLOG=1`):

```
EVT <ms> <actor> <event> key=value ...
```

When you add a behaviour, add its event, and **make every refusal say why**
(`why=`). Prefer measuring to guessing: most bugs here were found in minutes
once the right line existed.

## Measuring a change

`tools/bench.py` plays matches with the server alone (no client), fast and
deterministic: `SP_FAST=1` runs the game's clock by ticks, `SP_RAND_SEED`
makes a seed play the same match event for event.

```bash
python3 tools/bench.py save before                      # keep the current build
python3 tools/bench.py run --bin before --bin current --match ctf:2Fort_CTF --seeds 8
python3 tools/bench.py check --bin before --bin current --match ngctf:Raindance
```

A refactor must play event for event the same (`check`); a behaviour change
shows its numbers (`run`) in the pull request.

## Bugs already found (do not reintroduce them)

- A send to a bot (no connection) crashed the server: every sender returns
  early for a NULL connection.
- A bot marked ready directly was never announced; bots walk the join states
  like a player (`STARTING_MAP -> JOINING -> SPAWNING -> READY`), skipping
  only the map download.
- A packet buffer one byte short wrote out of bounds: check `sizeof` against
  what you write.
- The tick spun on `sleep(0)` and starved every other thread; it paces at 60
  Hz. ENet is driven from the tick's thread only: work from other threads is
  queued.
- A death applied only when a client received it left bots alive at 0 hp:
  the game state never depends on who is listening.
- The original 0.75 join buffered the download's block actions on the wrong
  player, so joiners missed edits made during their download.
- Lengths kept in `uint8_t` overflowed the heap when a line was longer than
  255 bytes, and `++` past a parsed number read past its buffer: lengths are
  `size_t`, and a parser never steps past the string's end.
- Reading `Bans.json` without checking it crashed the first ban of a new data
  folder: `bans_open()` always returns the array.
- A plugin call that changes the map is queued to the end of the plugins'
  tick: a map reset closes every Lua state, including the one running.

## Style

- Follow the surrounding file: 4 spaces, `LOG_*` macros.
- Comments explain **why**, not what. A rule that looks odd names the bug it
  prevents.
- A new configuration key is registered in `ConfigKeys.c` in the same commit
  (the CI fails on an unknown key in the shipped configs).

## Pull requests

One change a pull request, with what it fixes or adds, how it was tested
(the commands and their results) and, for gameplay, the bench numbers.
