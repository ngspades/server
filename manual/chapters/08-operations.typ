#import "/template.typ": *

== Logs

The server writes its log to the terminal (the standard output): lines of the form `26/11 16:13:19 INFO: ...`, `WARNING:`, `ERROR:` and `STATUS:`. Under systemd read it with `journalctl -u ngspades-server -f`; under Docker with `docker compose logs -f`. At start it prints the profile, config path, data folder, port (and `no browsers` or the WebRTC port), the master state and the mode and map. A tick over 16 ms is a stall the players feel; the server prints `Tick:` every 30 seconds (average and maximum, split into bots, physics, network and the rest) and `Slow tick:` with the parts for any tick over 20 ms.

Files in the data folder's `logs/`:

#fields((
  ("admin.log", "file", "The admin audit: every command, host command and remote console line (`[audit]`)."),
  ("crash.log", "file", "A stack trace written by the crash handler on `SIGSEGV`, `SIGABRT` or `SIGBUS` (POSIX builds with `execinfo`)."),
))

On macOS a crash also leaves a report in `~/Library/Logs/DiagnosticReports/ngspades-server-*.ips`: read the stack before guessing.

== The event log (EVT)

The event log is a structured record of what happens in the game, one line each:

```text
EVT <ms> <actor> <event> key=value key=value ...
```

`<actor>` is the player (`p4:Deuce`), a bot (`bot3:Bot_SwiftFox`), or `console`; events about a position include the cell. It is *off by default* (a public server with 128 players does not need the writing): `SP_EVENTLOG=1` turns it on; `SP_EVENTLOG=all` adds a line per player movement; `0` or `off` turns it off. Lines go into the normal log, after the timestamp and `INFO:`.

What it covers: spawns, damage and deaths, every block placement with the reason when refused, the bots' phases, goals, routes and shots, the game modes' rounds, the map load (`map action=shape|preview`), the plugin system (`EVT plugin action=...`), the master and WebRTC (`EVT master`, `EVT webrtc`), the admin audit, and `EVT net` for connections. Refusals say why (`hit_refused why=spawn_protect`, `block action=dig_refused why=protected`).

#note(kind: "tip")[
  To find out why something was refused, turn the event log on and `grep` for the actor or the word `refused`. Most bugs of the server were found in minutes once the right line existed.
]

A few lines every owner meets:

#fields((
  ("EVT plugin action=load|error|refused|missing", "plugins", "A plugin loaded, failed (and is off), was refused for its profile, or is named but has no file."),
  ("EVT plugin_refused plugin= event=", "plugins", "A plugin's event handler returned `false`."),
  ("EVT net action=closed kind=timeout|quit rtt= loss= pending=", "network", "A connection ended and how its link looked."),
  ("EVT proto10 map_sent bytes= ticks= ms=", "map", "A map transfer ended."),
  ("EVT map_download action=start|done|failed|alias|unpack", "maps", "A map download."),
  ("EVT abuse action=warn|kick why=packet|chat|command", "flood", "A player hit a flood limit."),
  ("EVT master action=listed|error|left family=4|6", "master", "The public list."),
  ("EVT gamemode mode= perks= assists=", "modes", "At every map start."),
  ("EVT wire profile=075 ...", "0.75", "The 0.75 profile's wire events."),
))

== Updating

*Installed by the script*: rerun it with `--reset`. It downloads the newest `ngspades-server-v*` release and keeps your `config.toml` (answer "keep the current settings"), then `systemctl restart ngspades-server`.

*From an archive*: unpack the new one over the folder, but keep `Resources/config.toml`, `config-075.toml`, `Bans.json`, `master.key` and `webrtc.pem`. Compare your config with the new shipped one: a new key is only a warning when you lack it (every key has a default), and `--check-config` tells you what the server no longer reads.

*Docker*, from the checkout's `server/` folder:

```sh
git pull
docker compose build
docker compose up -d
```

Plugins are in `Resources/plugins/`, which an update does not touch (`server_tools.lua` is overwritten with the release's). Their stores in `Resources/plugins/data/<name>.json` stay.

A wire change in protocol 1.0 updates ngspades-server and ngspades-client together: while the game is not public the minor version is informational and every 1.0 client gets the whole protocol, so update the server when a new client is released.

== Backups

Keep a copy of: `Resources/config*.toml` (your settings), `Bans.json`, `master.key` (your identity on the list: lose it and the server is a new one), `webrtc.pem`, `Resources/plugins/*.lua` and `data/*.json` (plugin stores), and `logs/admin.log` if you want the audit. Maps need no backup: they download again.

== Security notes

#note(kind: "warning")[
  *Change all five passwords.* The server warns at every start while one is still `changeme...`; a default password lets anyone log in as the manager and shut the server down. The install script refuses passwords that start with `changeme` and generates the others at random.
]

- *The remote console* gives admin rights to whoever knows its password. Keep `[rcon] bind = "127.0.0.1"` and never forward its port; reach it through SSH.
- *`master.key`* is the server's identity on the list: anyone with it can impersonate the server there. Keep it readable by the server's user only.
- *Plugins* are code. The sandbox gives them no files, no OS and no network unless `allow_hosts` lists a host, and the dangerous calls are opt-in (`allow_map_change`, `allow_commands`). Still read a plugin before you load it; keep secrets (tokens) in its `[plugins.<name>]` section, never in the file.
- *Maps and plugin data downloaded are never executed.* The `.txt` map metadata is parsed by the server and never run as Python.
- *DDoS*: a host with network filtering is the only defence against a volume attack (see #ch("07-network")[Network]).
- Report a vulnerability as `SECURITY.md` of the repository says.

== Troubleshooting

#wide-table(
  (1fr, 2fr),
  table.header([*Symptom*], [*Cause and fix*]),
  [`Config file 'Resources/config.toml' not found`], [Started from the wrong folder. `cd` into the server's folder, or give `--root <dir>`. The 0.75 profile reads its own file: `cp Resources/config.toml Resources/config-075.toml` and change its port.],
  [Players cannot join], [Both firewalls (the machine's and the provider's) must allow UDP on the game's port; a browser needs `[webrtc] enabled` and the port + 1; a join from the internet needs the master to be reachable (connect tokens) or `connect_tokens = false`. Check `EVT net` and `EVT abuse`.],
  [`Address already in use`, or the 0.75 server fights the 1.0 server], [Another process holds the UDP port (a leftover server). The two profiles need different ports: 32887 and 32897. `lsof -iUDP:32887`.],
  [Offers from browsers fail with `Failed to gather local ICE candidates`], [A leftover server on the WebRTC port (`lsof -iUDP:32888`), or no route for UDP.],
  [`Map 'X' could not be downloaded`], [The machine needs https to `raw.githubusercontent.com`; or copy the map folder into `Resources/maps/`, or use `--maps-dir`. `[maps] download = false` plays only what is there. A name may be an alias: check `maps/aliases.json`.],
  [`Map 'X' does not suit the ngctf mode` and it leaves the rotation], [ngCTF plays only maps whose spawns are 200 blocks apart; pick another mode or map. An empty rotation takes the mode's catalog.],
  [`Plugin X: no Resources/plugins/X.lua`], [The plugin is named in `load` but the file is missing (`EVT plugin action=missing`). Copy it from github.com/ngspades/plugins. For `server_tools` see the appendix.],
  [`EVT plugin action=refused name=X why=profile`], [The plugin declared `game.supported_profiles(...)` without the running profile, or the config's `[plugins.X] profiles` excludes it. Not an error; run the right profile or edit the list. `from=config` or `from=script` says which.],
  [`EVT plugin action=error plugin=X in=...`], [The plugin raised a Lua error or ran too long; it is off until the next map, and the game and other plugins go on. The `msg=` has the Lua error with file and line.],
  [`plugin ... game.set_map` returns `false, "[plugins] allow_map_change is false"`], [The dangerous calls are opt-in: set `allow_map_change = true` or list the command in `allow_commands`.],
  [Plugins do nothing at all], [The server was built without Lua (`ENABLE_LUA`): install `liblua5.4-dev` and rebuild.],
  [The rcon does not start], [A password under 16 characters turns it off with a warning, and the Windows build has no rcon.],
  [A warning `Config: [x] unknown key 'y'`], [A typo, or a key of the other profile. `--check-config` lists every one.],
  [A map's rotation vote never opens], [It needs the 1.0 profile and two or more other maps in the rotation.],
  [Bots stand still], [No human is in the match (`Bots paused`). They play as soon as one joins; `SP_BOTS_PLAY=1` is for tests.],
  [A 0.75 client is refused with `wrong protocol`], [It joined the 1.0 server. Use `--profile 075` (or `start-classic`) for classic clients; 1.0 clients cannot join a 0.75 server.],
  [A `Slow tick` often], [The machine is overloaded, or too many bots on a huge map; lower the bots or check CPU steal on a VPS. The bots' planner runs off the tick; a tick over 20 ms logs where its time went.],
)

When something is wrong and none of the above helps: turn on `SP_EVENTLOG=1`, reproduce it, and read the lines around the actor and the word `refused` or `error`.
