#import "/template.typ": *

This part of the manual is the contract between a Lua plugin and the server. It is written from the server's C code (`server/Source/Server/Plugins.c`, `Gamemodes/LuaMode.c` and the places that ask the plugins) and checked by running a real server with a test plugin: each entry says whether it was *verified by running* or taken *from the C code*. A coverage check (`make check` in the `manual/` folder) fails when a name the sandbox exposes has no entry here.

#note[
  Plugins need a server built with Lua 5.4 (`ENABLE_LUA`). Community plugins live in #link("https://github.com/ngspades/plugins")[github.com/ngspades/plugins], which also keeps a shorter `docs/api.md`. Where the two differ, this reference follows the C code; the appendix lists the differences found.
]

== A plugin is a file that returns a table

A plugin is one file, `Resources/plugins/<name>.lua`. The name may use only `a-z`, `0-9`, `_` and `-`. The file runs once each time a map starts and must return a table; the functions of that table named `on_*` are its *events*.

```lua
-- Resources/plugins/hello.lua
local plugin = {}

function plugin.on_join(p, team)
    p:message("Hello, " .. p.name)
end

game.log("hello is loaded")
return plugin
```

Three globals exist when the file runs:

#fields((
  ("game", "table", "Every function the server offers (chapter 11)."),
  ("config", "table", "The plugin's own `[plugins.<name>]` table from the config file, or an empty table. TOML strings, integers, floats, booleans, arrays and sub-tables become Lua strings, integers, numbers, booleans and tables."),
  ("the standard libraries", "tables", "`base`, `table`, `string` and `math`, nothing more (see the sandbox below)."),
))

== When a plugin loads

+ The server reads the plugins' configuration at every *map start*: `[plugins] load = [...]` (or the preset's `plugins` list, or `SP_PLUGINS=a,b`, which replaces both), then, for each name in order, loads `Resources/plugins/<name>.lua` (or `$SP_PLUGINS_DIR/<name>.lua`).
+ Every plugin has *its own Lua state*. Plugins never share variables; they talk through `game.send` and `on_message`.
+ A plugin's state is thrown away and made again at every map: *its variables start over at every map*. What must last goes in `game.store`.
+ After the game mode has initialised, each plugin's `on_init()` runs (the mode's own plugin runs its `on_init` as part of the mode's init, before the others).
+ Every event is asked of the plugins in the order of `load`; a plugin that is a game mode is asked last.

A name that is listed but has no file is logged (`Plugin X: no Resources/plugins/X.lua`, `EVT plugin action=missing`) and skipped. More than 32 plugins are not loaded. The server's own `server_tools` plugin is listed in the shipped `config.toml` (see the appendix about the file).

#fields((
  ("plugins.load", "array", "`load = [\"a\", \"b\"]` in `[plugins]`: the plugin files to run, in order. A `[presets.<name>] plugins` list replaces it for that preset."),
  ("plugins.allow_map_change", "bool", "`false` by default. `true` lets `game.set_map`, `game.set_mode` and `game.next_map` work."),
  ("plugins.allow_commands", "array", "Console command names (no slash, at most 16) that `game.run_command` may run."),
  ("plugin.allow_hosts", "array", "`allow_hosts = [\"api.example.com\", \"*.example.org\"]` in `[plugins.<name>]` (at most 16 entries): the hosts that plugin's `game.http` may call. `\"*\"` is any host, `\"*.x\"` any subdomain. None listed: no network."),
  ("plugin.profiles", "array", "`profiles = [\"10\"]` in `[plugins.<name>]`: keep the plugin out of the other profile. The profile's config file is the one that counts (`config.toml` for 1.0, `config-075.toml` for 0.75)."),
  ("SP_PLUGINS", "env", "Comma-separated plugin names, replacing `load` (tests)."),
  ("SP_PLUGINS_DIR", "env", "A folder to load the scripts from instead of `Resources/plugins`."),
  ("SP_PLUGIN_ALLOW_MAP", "env", "`1` or `0` for `allow_map_change`."),
  ("SP_PLUGIN_COMMANDS", "env", "Comma- or space-separated names for `allow_commands` (appended to the file's)."),
))

Everything under `[plugins.<name>]` other than `allow_hosts` and `profiles` is the plugin's own: the server never looks into it. Keep a plugin's secrets (tokens, webhook URLs) there, not in the Lua file.

```toml
[plugins]
load = ["server_tools", "spawn_protect", "my_plugin"]
allow_map_change = false
allow_commands = ["say"]

[plugins.my_plugin]
greeting = "Welcome"
allow_hosts = ["api.example.com"]
profiles = ["10"]
```

== The sandbox

The Lua state has the *base* library (without `dofile`, `loadfile`, `load`, `require` and `collectgarbage`, which are removed), `table`, `string` and `math`. There is no `io`, `os`, `package` or `debug`, and no way to load other code. A plugin cannot read a file, run a program or reach the network except through the functions of `game`, which the server's owner permits one by one. `print` exists (base library) and writes to the server's standard output; prefer `game.log`, which also writes an event-log line.

*Instruction budget.* Every call into a plugin (an event, a timer, a command, a callback) may run up to 20 million Lua instructions (a count hook every 100 000, stopped after 200). A call that runs longer is stopped with the error `the call ran too long (an endless loop?)`.

*Errors.* An error in any call of a plugin (including its top level and a bad bot goal table) turns *that plugin off for the rest of the map*: the server logs

```text
EVT <ms> console plugin action=error plugin=<name> in=<where> msg="<Lua error with file:line>"
```

and the game and every other plugin go on. The plugin is loaded again at the next map. `in=` is `load` for the top level, the event's name for an event, `a timer`, `on_done`, `on_message`, or the command's name.

*Tables are snapshots.* A player table handed to an event is a copy taken at that moment. Read the player again with `game.player(id)`. A method called on a player that has left raises `that player has left`.

== Profiles: plugins on the 1.0 and the 0.75 server

The server runs as profile `"10"` (protocol 1.0) or `"075"` (classic). A plugin states which it is written for as the *first line* of its file:

#entry("game.supported_profiles", "game.supported_profiles(profile, ...)", profile: "both")[
  Declares the profiles the plugin is for, by name: `"10"` (or `"1.0"`) and `"075"` (or `"0.75"`). Call it at the top level, before anything else; it is an error from an event (`game.supported_profiles is only taken at the plugin's top level`), with no argument (`needs at least one profile`) or with an unknown name.

  When the running profile is *not* listed, the call raises an error that stops the file right there: nothing after it runs, and the server does not load the plugin. This is a refusal, not an error: `EVT plugin action=refused name=<name> why=profile profile=<p> from=script`. Wrapping the call in `pcall` does not help: the plugin is refused all the same when the file returns. Code above the call has already run, so put it first. A plugin that says nothing loads in every profile.

  ```lua
  game.supported_profiles("10")        -- only the 1.0 server
  game.supported_profiles("10", "075") -- both
  ```
]

The operator can also refuse a plugin per profile with `profiles = [...]` in its `[plugins.<name>]` table (`from=config` in the refusal line). Both rules apply.

*1.0-only calls on the 0.75 server.* A call that needs a 1.0 feature does not silently do nothing: a query returns `nil` and the reason, an action returns `false` and the reason `"profile: protocol 1.0 only, the server runs the 0.75 profile"`, and one line a map is logged: `EVT plugin action=refused name=<name> why=profile what=<call>`. The 1.0-only calls are: `game.classes`, `game.class`, `game.set_class`, `p:set_class`, `p:set_squad`, `p:set_jetpack`, `p:drop_crate`, `game.drop_crate`, `game.sky`, `game.set_sky`, `p:set_outfit`, `p:outfit`, `game.set_team_outfit`, `game.set_mode` to a mode the 0.75 profile lacks (it has `ctf` and `babel`), `game.next_map` when the running mode is another, the `class` field of a player and the `on_class` event (never asked there). Entries below mark them *protocol 1.0 only*.

== Timing: when plugin code runs

#wide-table(
  (auto, 1fr),
  table.header([*Thing*], [*When*]),
  [`on_tick`, timers (`game.later`), `game.send` messages, `game.http` callbacks, queued map or mode changes, store saves], [On a *plugin tick*, which is every sixth server tick: 10 times a second.],
  [`on_player_tick`], [Once a second per player in the game.],
  [Events from the game (`on_hit`, `on_chat`, `on_build`...)], [At the very place the server applies the change, synchronously.],
  [`on_bot_think`], [At every think of every bot, 5 a second.],
  [`on_bot_team_tick`], [Once a second for each team.],
)

Calls that reload the map or the plugins (`game.win`, `game.set_map`, `game.set_mode`, `game.next_map`, `game.run_command`) are *queued* and applied at the end of the plugin tick, after every plugin has returned: a map reset closes every Lua state, and doing that under a running Lua call would free the state that is running. A reset is refused when another happened in the last 5 seconds (`EVT plugin action=reset_refused why=rate`).

== Limits at a glance

#wide-table(
  (auto, auto, 1fr),
  table.header([*Limit*], [*Value*], [*What happens past it*]),
  [plugins loaded], [32], [the rest are not loaded, an error is logged],
  [pending `game.later` timers per plugin], [64], [`game.later` raises `too many timers (64)`],
  [`game.command` names per plugin], [16], [raises `more than 16 commands`],
  [queued `game.send` messages (all plugins)], [256], [`game.send` raises `too many messages waiting`],
  [`game.set_block` per plugin tick], [4096 cells (a `clear_block` counts 4)], [returns `false, "rate"`],
  [`game.http` waiting], [16 per plugin, 64 in all], [returns `nil, "too many requests waiting"`],
  [`game.http` answer], [1 MB; timeout 1 to 30 s], [the call fails with an error text],
  [`game.store` on disk], [1 MB], [the save is refused (`EVT plugin action=store_refused why=size`)],
  [JSON nesting], [32 levels], [`json: nested too deep`],
  [instructions per call], [20 million], [the plugin is turned off for the map],
)

== The event log lines of the plugin system

With `SP_EVENTLOG=1` the plugin system writes these `EVT plugin` lines (the first word after `plugin` is the `action=`):

#fields((
  ("action=load", "line", "`plugin=<name> mode=<0|1>`: a plugin loaded."),
  ("action=error", "line", "`plugin=<name> in=<where> msg=\"...\"`: the plugin is off for the map."),
  ("action=refused", "line", "`name=<name> why=profile` (with `from=config|script`, or `what=<call>` for a 1.0-only call)."),
  ("action=missing", "line", "`plugin=<name>`: a listed plugin has no file."),
  ("action=log", "line", "`plugin=<name> text=\"...\"`: a `game.log` call."),
  ("action=notice", "line", "A `game.notice`."),
  ("action=kick", "line", "A `p:kick`."),
  ("action=ban", "line", "A `game.ban`."),
  ("action=teleport", "line", "A `p:teleport`."),
  ("action=spawn_heli", "line", "A `p:spawn_heli`."),
  ("action=spawn_point", "line", "An `on_spawn_point` that placed a player."),
  ("action=win", "line", "A `game.win` or `game.end_match`."),
  ("action=set_map", "line", "A queued `game.set_map`."),
  ("action=set_mode", "line", "A queued `game.set_mode`."),
  ("action=next_map", "line", "A queued `game.next_map`."),
  ("action=reset_refused", "line", "A reset was refused for the rate limit."),
  ("action=command", "line", "`game.command` registered a command, or `game.run_command` queued one (`plugin=<name> command=<name>`)."),
  ("action=command_run", "line", "A plugin command ran."),
  ("action=command_denied", "line", "Someone without the role tried a plugin command."),
  ("action=command_refused", "line", "`why=taken`: the name exists."),
  ("action=score_limit", "line", "A `game.set_score_limit`."),
  ("action=time_limit", "line", "A `game.set_time_limit`."),
  ("action=objective", "line", "A mode's `game.objective`."),
  ("action=cells", "line", "`plugin=<name> cells=<n>`: cells changed in a plugin tick."),
  ("action=store_save", "line", "`bytes= why=map|shutdown|asked ok=`: the store was written."),
  ("action=store_refused", "line", "`why=encode|size`: the store was not written."),
  ("action=store_bad", "line", "The store file was not a JSON object and was moved to `.bad`."),
  ("action=http", "line", "A `game.http` request started."),
  ("action=http_done", "line", "It ended (`status=`, `bytes=`, `error=`)."),
  ("action=http_refused", "line", "It was refused (`why=`)."),
  ("action=bot_arrived", "line", "A bot reached a plugin's goal."),
))
