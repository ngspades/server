# Plugins

Plugins are Lua scripts that change how the whole server plays: game modes,
mechanics (airstrikes, rampages, spawn protection), admin and building
tools. Most are the pyspades / PySnip scripts ported to Lua. They do not
ship with the server, with one exception: `server_tools.lua` is the
server's own and is loaded by the shipped `config.toml`. It gives the
admin role (and the host of a listen server, who enters as admin):

- `/spawn` - a helicopter on dry ground in front of the player;
- `/spawn crate` - a random ammo or health supply crate falling from the sky
  in front of the player;
- `/goto <x> <y>` - teleport to the middle of the 16 block chunk x, y (0
  based: 32 x 32 on a 512 map, 64 x 64 on a 1024 map).

The commands need protocol 1.0 (the helicopter and the crates are 1.0
entities). To use a community plugin:

1. Get it from https://github.com/ngspades/plugins (`plugins/<name>.lua`).
   `make plugins` (tools/install_plugins.py) downloads the whole catalogue here.
2. It is already here as `Resources/plugins/<name>.lua` after that command.
3. List it in `Resources/config.toml`:

   ```toml
   [plugins]
   load = ["server_tools", "spawn_protect", "rampage"]

   [plugins.spawn_protect]   # the plugin's own settings, if it has any
   spawn_protect_time = 3.0
   ```

A plugin that is a game mode (`tdm10`, `koth10`, `push10`, `arena075`, ...) is played with
`--gamemode <name>` instead.

## Catalogue

Game modes (run with `--gamemode <name>`):
`arena075` (0.75 only), `koth10`, `push10`, `tdm10` (1.0 only).

Mechanics: `spawn_protect`, `daycycle`, `rampage`.

Admin/building tools: `server_tools` (ships with the server), `welcome`.

Tests (not for production): `apitest`, `cmdtest`, `modetest`, `pendingtest`,
`wintest`.

## 0.75-only plugins

Some plugins only work on the 0.75 profile (`--profile 075`) and declare it
with `game.supported_profiles("075")` at the top of the file. They use only
the APIs that both profiles answer (blocks, chat, teleports) and never the
1.0-only ones (crates, classes, perks, outfits, fog as a GenericCommand).

- `arena075.lua` -- round-based elimination (last team standing wins the round),
  a port of the pyspades/piqueserver arena script. It builds and destroys
  the gates as ordinary blocks and moves players with teleports, the way the
  classic servers did. It is NOT the native 1.0 arena mode (`--gamemode arena`,
  `Arena.c`): no arena HUD, no countdown keys, no refill path. Use
  `--gamemode arena` on a 1.0 server for that. On 0.75, `arena075.lua` is the
  arena a 0.75 client and a 1.0 client (which falls back to 0.75 there) can
  play together.

`game.set_mode`, `game.set_map`, `game.next_map` and `game.run_command` are
off by default. Enable map changes with `[plugins] allow_map_change = true`
and list each console command a plugin may run in `allow_commands`. They are
queued to the end of the plugin tick; a map reset closes every Lua state and
must never run inside a Lua call.
