# ngspades-server

The game server of **ngspades**, a voxel shooter in the line of Ace of
Spades, and the reference implementation of its network protocol, **protocol
1.0**. The code in `common/` (the protocol's codec, the map format, the rules
both sides must agree on) is the source of truth: the game client imports it
from this repository.

One binary, two profiles:

- **protocol 1.0** (the default): the game as ngspades plays it,
  with classes, perks, vehicles, squads, the time of day and the weather,
  1024 x 1024 maps and up to 128 players.
- **0.75** (`--profile 075`): the classic Ace of Spades 0.75 game, for the
  original clients and their forks (OpenSpades, BetterSpades, ZeroSpades...),
  with the community's protocol extensions.

It started as a fork of [SpadesX](https://github.com/SpadesX/SpadesX) and grew from there:
server-side bots, game modes (capture the flag in two flavours, king of the
hill, territory control, zombies, arena, Babel, creative...), Lua plugins and game modes, browsers over
WebRTC, a master server list, and a remote console for
administering a server from a tool such as sadctl.

## Compared with the other servers

Two community servers keep Ace of Spades alive and this one builds on their
work: [SpadesX](https://github.com/SpadesX/SpadesX) (the code ours started
from) and [piqueserver](https://github.com/piqueserver/piqueserver) (the
widest-used 0.75 server). This is what differs, read from their repositories
(SpadesX at its January 2026 commit, piqueserver at its September 2026
one); it compares features, not people, and the other projects are alive, so
their lists change.

| | ngspades-server | SpadesX | piqueserver |
| --- | --- | --- | --- |
| **Language** | C | C | Python, with Cython for the hot parts |
| **Licence** | GPL-3.0-or-later | GPL-3.0 | GPL-3.0 |
| **Protocols** | 1.0 (the default) and the classic 0.75 with the community's extensions (`--profile 075`) | 0.75 | 0.75 |
| **Game modes** | capture the flag (two flavours), team deathmatch, king of the hill, territory control, zombies, arena, Babel, creative | capture the flag, Babel | capture the flag (and one flag), territory control, team deathmatch, free for all, arena, Babel, push, tug of war, infiltration |
| **Your own modes and scripts** | Lua plugins and Lua game modes | none: change the C | Python scripts (about 50 come with it) and map scripts |
| **Bot players** | server-side bots with difficulty levels, objectives and roles ([docs/bots.md](docs/bots.md)) | no | no |
| **Browsers** | WebRTC, next to the UDP players | no | no |
| **Remote administration** | a remote console for tools ([docs/rcon.md](docs/rcon.md)), off until a long password is set | the server console and in-game commands | an SSH console, an IRC bot, a JSON status server |
| **Maps** | up to 1024 x 1024; the maps of the rotation it lacks are downloaded from [ngspades/maps](https://github.com/ngspades/maps) | 512 x 512 | 512 x 512, with map metadata and extensions |
| **Players** | up to 128 (protocol 1.0), 32 on 0.75 | 32 | 32 |
| **Master list** | yes | yes | yes |
| **Packaging** | one static binary per platform, Docker image from scratch | Docker | Python package, Docker |

What the others have that this one does not: piqueserver's years of
community scripts (airstrikes, rollback, vote kicks and many more) and its
web status page; a big map or a script written for it will not run here as
it is.

## Maps and networking, compared with SpadesX

Most of what we changed from SpadesX is invisible in a feature table: how the
map is kept and sent, and how the game state reaches each player. The figures
below are ours, measured on an Apple M4 with `tools/bench.py` and a map
benchmark ([docs/map-memory.md](docs/map-memory.md) has the method and every
number). The "SpadesX" column is what the server did before we changed it:
its map library (`libmapvxl`, which this server no longer builds) and the way
it wrote and sent the map, measured on that inherited code and not on a
SpadesX binary. We did not benchmark SpadesX's network code: where the network table
names it, it is what the 0.75 protocol itself requires of a 0.75 server.

### The map

| | SpadesX (0.75) | ngspades-server (protocol 1.0) |
| --- | --- | --- |
| **In memory** | `libmapvxl`: a dense array, 4 bytes for every cell, air and buried ground included. 64 MiB at 512 x 512, 256 MiB at 1024 x 1024 | `libvxl`: the solid cells as a bitset, and colours only for the cells that touch the air. 6.5-8.8 MiB at 512, 29-34 MiB at 1024. The whole server process: 90 to 31 MiB on Raindance, 297 to 60 MiB on a 1024 map, with the tick unchanged |
| **Sent to a joining player** | The map written as VXL from the live map for every joiner, then compressed: 0.2-0.7 s of CPU at 512 and 1.1 s at 1024, and a buffer of hundreds of MiB for a 1024 map | The map's stored **ngmap**, sent as it is: nothing is encoded, nothing is allocated, 0 ms on the tick. A map with only a VXL is encoded once, on a thread, and the result is kept in the data folder |
| **A player who has it already** | The 0.75 protocol has no way to skip it (0.76 added a CRC32 a client can answer) | The join announces the CRC32 and size of the exact stream; a client that has it answers `map-cached 1` and **receives 0 bytes**. A map a player has seen is instant |
| **Changes since the map loaded** | A joiner gets the live map | A joiner gets the stored stream plus the cells changed since the load as one `TerrainUpdate`. Every map change goes through one function, `proto10_world_cell`, so nothing can change the map without the clients hearing of it |
| **On disk and on the wire** | VXL: about 2273 MiB for the maps in [ngspades/maps](https://github.com/ngspades/maps) | ngmap at quality 100 (geometry exact, colours within a step): 324 MiB for the same maps, about a seventh. A 512 map is 385-648 KB on the wire, a 1024 one 2.5-2.7 MB |

A joiner on a slow link is not buried: the fragments pace themselves by what
the connection already holds, up to 64 a tick while it carries less than 64
KiB, so a fast link gets the map at once. On the 0.75 profile the server still
sends VXL, because the wire demands it, but compresses a slice of columns at a
time straight into the deflate stream with no whole-map buffer, and keeps the
compressed copy while the map is unchanged, so the joins of one map start cost
one compression.

### The network

| | SpadesX (0.75 protocol) | ngspades-server (protocol 1.0) |
| --- | --- | --- |
| **Who sees whom** | The world update carries the position of every player, so a client knows where everyone is, fog or not | A player gets only the bodies it can use. Past the fog an enemy is sent once, standing still, and then nothing until it is back in range: bandwidth saved, and a wallhack that cannot exist |
| **How often** | One world update with all the players | Each client gets its **own** packet, each body as a delta against what *that client* last received, at a rate by distance: 60 Hz within 32 blocks, 30 within 64, 15 within 128, 5 beyond |
| **How big** | Six floats (24 bytes) a player | A body's trajectory (position and velocity) in fixed point: 17 bytes instead of 37, with a fallback to floats when a value does not fit |
| **Measured** | not measured by us | 127 bots on Busan_Port (bases about 390 blocks apart): about 161 KB/s to a client with one broadcast of every body, **about 26 KB/s** with the per-client deltas. The server ticks at 60 Hz in 0.6-0.7 ms on average, at 5-8% of one core and about 76 MiB resident |
| **Channels** | A client sends everything on one reliable channel | Two: the player's own body (position, orientation, input) goes **unreliable** on channel 1, because a lost one is replaced by the next; everything else is reliable on channel 0. A packet that must be seen before an action (a hit, a block, a tool change) goes reliable |
| **ENet** | SpadesX's ENet fork | [enet6](https://github.com/SirLynix/enet6), with the packet throttle's deceleration set to 0 (the throttle shrank the reliable window to one MTU a round trip when a line jittered, and the clients' views fell seconds behind) and a 15 s minimum, 40 s maximum timeout, so a phone that stalls briefly is not dropped. One IPv6 socket that also answers IPv4 |
| **Browsers** | no | WebRTC DataChannels on one UDP port: the same two channels, the same packets |
| **Abuse** | not compared | Token buckets per player for packets (400 a second), chat and commands, before anything parses them. UDP queries are rate limited and an answer is never larger than its query, so the server cannot be used to amplify an attack |

Two choices make this possible. The whole game runs on one codec,
`common/proto10/`, and the 0.75 wire lives only in `Server/Legacy/`: both
protocols call the same game functions, so a rule exists once. And what cannot
be done in a 16 ms tick is not done on it: bot path planning runs on its own
thread against its own copy of the map, and a map is encoded on a background
thread.

## Building

You need CMake 3.16+, a C11 compiler and these libraries:

| | Debian / Ubuntu | macOS (Homebrew) |
| --- | --- | --- |
| json-c, OpenSSL, libcurl, zlib, readline | `libjson-c-dev libssl-dev libcurl4-openssl-dev zlib1g-dev libreadline-dev` | `json-c openssl@3` (curl, zlib and libedit come with the system) |
| Lua 5.4 (plugins, optional) | `liblua5.4-dev` | `lua` |

```bash
git clone --recursive https://github.com/ngspades/server.git
cd server
make server          # server/build/ngspades-server
make test            # the protocol's tests, the 0.75 extensions' tests, the configs
```

The first configure downloads and builds libdatachannel (WebRTC); later
builds reuse it.

`make server-static` builds the release binary for Linux (one static musl
executable, in Docker), the way the releases are made.

## Running

```bash
cd server && build/ngspades-server                     # protocol 1.0, Resources/config.toml
cd server && build/ngspades-server --profile 075       # the classic 0.75 server, Resources/config-075.toml
build/ngspades-server --help                            # every option
```

The server finds its `Resources/` folder by itself (`--root` to point at
another). The maps of the rotation that are not in `Resources/maps/` are
downloaded at start-up from the public map repository,
[ngspades/maps](https://github.com/ngspades/maps) (`[maps] download` in
the config). Change every password in `config.toml` before opening the
server to the internet: the server warns at every start while one is still
`changeme`.

Hosting a public server (ports, the firewall, DDoS protection, systemd,
Docker): [docs/hosting.md](docs/hosting.md).

```bash
cd server && docker compose up -d      # the server in a container (~16 MB, from scratch)
```

## Documentation

| | |
| --- | --- |
| [The manual](https://ngspades.github.io/server/) | the user manual (install, configure, run, administer) and the complete Lua plugin reference, as HTML and PDF; the Typst sources are in [manual/](manual/) (`cd manual && make site`) |
| [docs/protocol-1.0.md](docs/protocol-1.0.md) | protocol 1.0: the wire, every packet, the join, the entities |
| [docs/implementers/](docs/implementers/) | writing a compatible client or server, for 0.75 and for 1.0 |
| [docs/plan-075-server.md](docs/plan-075-server.md) | the 0.75 profile: what it speaks and what it leaves out |
| [docs/bots.md](docs/bots.md) | how the bots play, and how a plugin or a game mode drives them |
| [docs/host-commands.md](docs/host-commands.md) | the host's commands (bots, difficulty) |
| [docs/rcon.md](docs/rcon.md) | the remote console: managing a server from a tool |
| [docs/map-memory.md](docs/map-memory.md) | the map in memory, measured |
| [docs/hosting.md](docs/hosting.md) | hosting a public server |
| [CONTRIBUTING.md](CONTRIBUTING.md) | how the code is organised, its rules and how to test a change |
| [SECURITY.md](SECURITY.md) | reporting a vulnerability |

Plugins (Lua scripts that change the whole server, pyspades' scripts ported)
live in [ngspades/plugins](https://github.com/ngspades/plugins), with their API.

## Layout

```
server/Source/        the server (C11)
server/Resources/     config.toml, config-075.toml (maps: five example maps, the rest download, or tools/gen_maps.py)
server/tools/         test programs (the 0.75 extensions, the bots' plugin API)
common/proto10/       protocol 1.0's codec and its tests (fuzzed)
common/ngmap/         ngmap, the compressed map format protocol 1.0 sends
common/...            rules shared with the client: classes, modes, physics, sky...
tools/                build, packaging, install, benchmarks, protocol probes
docs/                 the documents above
```

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The server is a fork of SpadesX
(GPL-3.0); the files keep their authors' copyright lines, and [NOTICE.md](NOTICE.md)
lists SpadesX's authors and the files that descend from theirs.

## Credits

[SpadesX](https://github.com/SpadesX/SpadesX) (the server this one grew from),
[pyspades](https://github.com/infogulch/pyspades) and
[piqueserver](https://github.com/piqueserver/piqueserver) (game rules, the
arena and squad scripts), the OpenSpades `ngspades` branch (protocol 1.0's
first design and the ngmap codec), [ZeroSpades](https://github.com/zerospades/zerospades)
and the [aosprotocol](https://github.com/zerospades/aosprotocol) specification
(the 0.75 extensions), [enet6](https://github.com/SirLynix/enet6) (SirLynix's fork of
[ENet](https://github.com/lsalzman/enet), wire-compatible with ENet 1.3), [libvxl](https://github.com/xtreme8000/libvxl),
[tomlc99](https://github.com/cktan/tomlc99), json-c and libdatachannel.
The maps belong to their authors, credited in each map's files.
