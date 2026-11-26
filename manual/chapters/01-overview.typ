#import "/template.typ": *

== What this is

`ngspades-server` is the game server of *ngspades* (its client is *ngspades-client*), a voxel shooter in the line of Ace of Spades. One program runs everything: the match, the bots, the game modes, the Lua plugins, the list of servers and the browsers' connection. It is also the reference implementation of *protocol 1.0*, the network protocol shared with *ngspades-client*, the game's client.

It started as a fork of SpadesX and grew from there. Its source is at #link("https://github.com/ngspades/server")[github.com/ngspades/server] under the GPL-3.0-or-later license.

This manual has two parts.

- The *user manual* (chapters 1 to 8) teaches an owner to install, configure, run, administer and troubleshoot a server.
- The *Lua plugin reference* (chapters 9 to 14) lists everything a plugin can do: every event, every function, every field of every object, with an example for each.

#note(kind: "tip")[
  Short on time? Read #ch("02-install")[Installing and first start], change the five passwords (see #ch("03-configuration")[Configuration]) and open the game's UDP port (see #ch("07-network")[Network]). That is all a private server needs.
]

== The two profiles

One binary, two profiles. The profile decides which protocol the server speaks and which config file it reads.

#wide-table(
  (auto, 1fr, 1fr),
  table.header([], [*protocol 1.0* (default)], [*0.75* (`--profile 075`)]),
  [Clients], [ngspades-client and its browser version], [the original Ace of Spades 0.75 and 0.76 clients and their forks (OpenSpades, BetterSpades, ZeroSpades) with the community's protocol extensions],
  [Config file], [`Resources/config.toml`], [`Resources/config-075.toml`],
  [Default game port (UDP)], [32887], [32897],
  [Data folder], [the root], [`data-075/` in the root],
  [Players], [up to 128], [up to 32],
  [Maps], [512 x 512 or 1024 x 1024], [512 x 512],
  [Game modes], [capture the flag (two flavours), team deathmatch, king of the hill, territory control, zombies, arena, Plant the Bomb, Babel, creative, and Lua modes], [capture the flag and Babel, and Lua modes made for it],
  [Extras], [classes, perks (jetpack, sentry, machine gun, landmines, dynamite), vehicles, squads, supply crates, the time of day and the weather], [none of the 1.0 inventions; ZeroSpades' extensions instead (damage markers, pings, flashlight, day and night...)],
  [Join], [a 1.0 client only; a vanilla client is refused with "wrong protocol"], [a 0.75 or 0.76 client only],
)

The two profiles run side by side on one machine: they use different ports, config files and data folders. A section of a config that belongs to the other profile is ignored with a warning at start.

#note[
  The profile is chosen once, at start: `--profile 10|075` or `SP_PROFILE`. An unknown value stops the server; it never falls back to the other profile, because a 0.75 server on `config.toml` would take the 1.0 port.
]

== What it does that other servers do not

- *Bots* are real players on the server: ordinary player entries with no network peer, with difficulty levels, roles and objectives (chapter #ch("05-bots")[Bots]).
- *Maps* are kept as a bitset and sent as a stored `ngmap` file: a map already on the player's disk costs 0 bytes to join. Up to 1024 x 1024.
- *Network*: each player gets its own packet, each body as a delta against what that player last saw, at a rate that falls with distance. Enemies past the fog are not sent at all.
- *Plugins* in Lua change anything from a mechanic to a whole game mode, in a sandbox (chapters 9 to 14).
- *Browsers* join over WebRTC on the same server, and a host can give friends a six-letter join code.
- *Administration* is a console, in-game commands by role, and a remote console for tools.

The README of the repository compares the server with SpadesX and piqueserver feature by feature, with measured numbers for the map and the network.

== Concepts used throughout the manual

#fields((
  ("root", "folder", "The folder that holds `Resources/`. The server finds it by itself (the current folder, `..` or `../..`); `--root` points at another. Paths in the config are relative to it."),
  ("data folder", "folder", "Where the server writes: `Bans.json`, `master.key`, `webrtc.pem`, `logs/`, the plugins' stores. The root by default, `data-075/` for the 0.75 profile; `--data` or `SP_DATA_DIR` moves it."),
  ("rotation", "list", "The maps the server plays in turn (`[server] maps`)."),
  ("preset", "table", "A named bundle of mode, maps, limits and plugins (`[presets.<name>]`)."),
  ("role", "word", "A staff level a player takes with `/login`: `trusted`, `guard`, `mod`, `admin`, `manager`."),
  ("EVT", "log line", "A structured line of the gameplay event log, `EVT <ms> <actor> <event> key=value`, on with `SP_EVENTLOG=1`."),
  ("plugin", "Lua file", "`Resources/plugins/<name>.lua`: a script the server runs in a sandbox."),
))

== Where to go next

#table(
  columns: (auto, 1fr),
  stroke: none,
  [Install], [#ch("02-install")[Installing and first start]],
  [Every setting], [#ch("03-configuration")[Configuration]: a table per section, generated from the server's own registry of keys],
  [Pick a mode and maps], [#ch("04-modes-maps")[Game modes and maps]],
  [Run it for a community], [#ch("06-administration")[Administration and commands], #ch("07-network")[Network, the public list, browsers], #ch("08-operations")[Operations]],
  [Write a plugin], [#ch("09-plugins-lifecycle")[Plugins: how they run] and the chapters after it],
)
