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
(the 0.75 extensions), [ENet](https://github.com/lsalzman/enet) with
SirLynix's IPv6 layer, [libvxl](https://github.com/xtreme8000/libvxl),
[tomlc99](https://github.com/cktan/tomlc99), json-c and libdatachannel.
The maps belong to their authors, credited in each map's files.
