#import "/template.typ": *

== How the configuration works

The server reads one TOML file at start: `Resources/config.toml` (protocol 1.0) or `Resources/config-075.toml` (the 0.75 profile). `--config <path>` names another file. The file is read again at every map start for the parts that are map-scoped (the plugins, the classes), but most keys are read once: restart the server to apply a change.

The order of precedence, from the weakest:

+ The server's built-in default for a key.
+ The config file.
+ A preset (`--preset <name>`, `[server] preset` or `SP_PRESET`) over `gamemode`, `maps`, `capture_limit`, `aim_assist`, `fog_distance`, bot difficulty and plugins.
+ An environment variable (`SP_*`), where the table below shows one.
+ The command line (`--port`, `--gamemode`, `--map`, `--bots`...).

Every key the server reads is registered in `ConfigKeys.c` together with the profile that reads it. An unknown key, or a section of the other profile, is a warning at every start and in `--check-config`; the tables below are generated from that registry, so none is missing. In the tables, *Profile* says who reads the key (`both`, `1.0`, `0.75`) and *Env* names the environment variable that overrides it.

#note(kind: "tip")[
  Run `ngspades-server --check-config` after every edit. It parses the file, lists unknown keys and tells you which maps will be downloaded.
]

== Command line options

#fields((
  ("--profile <10|075>", "10, 075", "Protocol 1.0 (default) or the classic 0.75 game. Also `SP_PROFILE`."),
  ("--config <path>", "path", "The config file. Default `Resources/config.toml`, or `Resources/config-075.toml` with `--profile 075`."),
  ("--root <dir>", "path", "The folder holding `Resources/`. Default: the current folder, `..` or `../..`."),
  ("--data <dir>", "path", "Where the server writes: bans, keys, logs, plugin stores. Also `SP_DATA_DIR`. Default: the root, or `data-075/` in it with `--profile 075`."),
  ("--check-config", "flag", "Validate the profile's config file and exit."),
  ("--port <n>", "int", "The game's UDP port (overrides `[server] port`)."),
  ("--preset <name>", "string", "A `[presets.<name>]` table. Also `SP_PRESET`."),
  ("--gamemode <name>", "string", "`ctf`, `ngctf`, `babel`, `arena`, `zombie`, `creative` or the name of a Lua mode. On the 0.75 profile `ctf` or `babel`. Wins over `[server] gamemode` and a preset."),
  ("--map <Name>", "string", "Play this map instead of the rotation (the folder name; aliases resolve)."),
  ("--maps-dir <dir>", "path", "A folder of maps searched before `Resources/maps/`."),
  ("--classes <0|1>", "0, 1", "The zombie mode's classes off or on."),
  ("--first-infected <n>", "int or auto", "The zombie mode's patient zeros a round (0 or `auto`: one per 6 players)."),
  ("--bots <n>", "int", "Start with n balanced bots."),
  ("--team-bots <a> <b>", "ints", "Start with a bots on team 1 and b on team 2."),
  ("--bots-test", "flag", "Start with exactly one strong enemy bot (a combat test)."),
  ("--listen, --server, --public", "flags", "Used by ngspades-client when it starts a server for a player's own match (private; listed with `--public`). Not for dedicated servers."),
  ("--join-code <code>", "string", "A join code for a listen server's match."),
  ("--help, -h", "flag", "The usage text."),
))

== A walk through config.toml

The shipped `config.toml` is commented key by key. In short:

+ `[server]`: the name, the port, the mode, the maps. Change `name`, `maps`, `max_players` if you add it.
+ `[teams]`: names and colours.
+ `[passwords]`: change all five.
+ `[maps]`: `download = true` fetches missing maps from the maps repository.
+ `[master]`, `[webrtc]`, `[rcon]`: the public list, browsers, the remote console.
+ `[plugins]`: the plugin list and the opt-in dangerous calls.
+ `[presets.*]`: ready bundles per mode.
+ Everything else tunes a mode or a rule and has working defaults.

A minimal public 1.0 server:

```toml
[server]
name = "My ngspades server"
port = 32887
gamemode = 6                      # ngctf
maps = ["Busan_Port", "Raindance"]

[passwords]
manager   = "use-a-long-random-one-1"
admin     = "use-a-long-random-one-2"
moderator = "use-a-long-random-one-3"
guard     = "use-a-long-random-one-4"
trusted   = "use-a-long-random-one-5"

[master]
enabled = true                    # list it on the public list
url = "https://master.shootanddestroy.obex.pw"
country = "BR"
```

#note[
  `[server] gamemode` is a number in the shipped file: 0 `ctf` (classic), 1 `tc`, 2 `babel`, 3 `arena`, 4 `zombie`, 6 `ngctf`, 7 `creative`, 8 `tdm`, 9 `koth`. `--gamemode` takes names.
]

== Every section and key

The tables below list every key, its type, its default (what the shipped file shows), its meaning and its profile.

#include "/gen/config-tables.typ"

== Environment variables

Every `SP_*` variable the server reads. The ones tied to a config key are also shown in that key's row above. Variables for tests and benchmarks are marked as such; none is needed on a normal server.

#include "/gen/env.typ"

== The 0.75 profile's config

`config-075.toml` is a copy of `config.toml` with these differences: its own port (32897) and WebRTC port (32898), the classic CTF rotation (`2Fort_CTF`, `Raindance`, `Busan_Port`), tolerant rules in `[profile.075]` (grief kicks at 20/40 points, AFK kick at 300 s, no spawn protection, a packet outside the standard is logged, not a kick), the public 0.75 master (`[master075]`), the ZeroSpades extensions (`[ext075]`), and none of the 1.0 inventions (no `[jetpack]`, `[perks]`, `[classes]`, `[sentry]`, `[squads]`, `[zombie]`; a section of that kind in this file is warned about as a key the profile does not read). Its presets are `ctf` and `babel`. Its rcon port is 32900.

Details of what the profile speaks and leaves out are in `docs/plan-075-server.md`.
