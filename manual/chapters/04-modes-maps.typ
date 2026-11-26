#import "/template.typ": *

== The game modes

A *game mode* is the rule set of a match: how it is won, what a spawn is, what bots go for. The mode is chosen at start; the rotation changes the map, not the mode (a plugin may change either, see `game.set_mode`).

Pick one with `[server] gamemode = <number>`, with `--gamemode <name>` (wins over the config and a preset), or through a preset.

#wide-table(
  (auto, auto, 1fr, auto),
  table.header([*Name*], [*Number*], [*What it is*], [*Profiles*]),
  [`ctf`], [0], [Classic capture the flag, the 0.75 rules as they were: two intels, two tents, no perks or assists. The squads' bots keep their lanes, but a respawn lands at the team's spawn.], [both],
  [`ngctf`], [6], [Next gen CTF: the same game with perks (jetpack, sentry, machine gun), kill assists, 3D spotting, respawn next to a squadmate and a rally point, classes (Commando, Rocketeer, Marksman, Miner). Plays only large maps: the two spawn areas at least 200 blocks apart.], [1.0],
  [`tc`], [1], [Territory control (pyspades' rules): 3 to 7 territories in a line between the spawns; standing in one pushes it to your team; holding all wins, or the most at the time limit (1200 s).], [1.0],
  [`babel`], [2], [Capture the flag on a platform in the sky (Babel maps), built up with staircases and pillars by the players and the bots.], [both],
  [`arena`], [3], [Elimination rounds with no respawn on Counter-Strike style maps; the map's gates open and close.], [1.0],
  [`zombie`], [4], [Rounds where one survivor turns zombie; every survivor killed joins the zombies. Played at night with flashlights and night vision; optional classes.], [1.0],
  [`creative`], [7], [Building only: spade and blocks, free flight, no guns, no bots, on the `Creative_Flat` map.], [1.0],
  [`tdm`], [8], [Team deathmatch; its limit is a number of kills (default 100 in the shipped preset).], [1.0],
  [`koth`], [9], [King of the hill: a team alone on the moving hill scores a point a second; first to 60 wins the round; three rounds win the map.], [1.0],
  [`ptb`], [10], [Plant the Bomb, Counter-Strike's defusal on the arena's maps: terrorists plant, counter-terrorists defuse; the sides swap at halftime.], [1.0],
  [any Lua mode], [-], [A plugin with `mode = true`, started with `--gamemode <plugin name>`. See #ch("13-api-modes-bots")[Game modes and bots in Lua].], [both],
)

The 0.75 profile registers only `ctf` and `babel`; any other name falls back to `ctf` (`EVT gamemode action=fallback`) or is refused (`why=profile`). Plugin modes written for 1.0 declare `game.supported_profiles("10")` and are refused there.

#note[
  `ngctf`, `koth` and `tc` can run with the *classes* when the host turns them on (`--classes 1` or `/classes on`, `[zombie] classes = true` for the zombie mode).
]

=== Mode settings

Each mode with settings has a section of its own in the config, with tables in #ch("03-configuration")[Configuration]: `[zombie]`, `[arena]`, `[ptb]`, `[squads]`, `[perks]`, `[classes.<name>]`, `[sentry]`, `[jetpack]`. A map may narrow a few of them with tables of its own in its `.toml` (`[arena]`, `[ptb]`).

=== Presets

A preset bundles a mode, a rotation, a limit and plugins under one name:

```toml
[presets.koth]
gamemode = 9
maps = ["Normandie", "hiesville", "20thCenturyTown"]
capture_limit = 90
bot_difficulty = "normal"
plugins = []
```

Start with `ngspades-server --preset koth`, or set `[server] preset = "koth"`. A preset's `plugins` list replaces `[plugins] load` (the `[plugins.<name>]` settings stay global). `--gamemode` and `--map` still win over a preset. The 1.0 file ships `ngctf`, `zombie`, `tdm`, `arena`, `babel` and `koth`; the 0.75 file `ctf` and `babel`.

== Maps

A map is a folder under `Resources/maps/` named for the map, holding files of the same name:

#fields((
  ("<Name>.vxl", "file", "The map in the classic VXL format (512 x 512 x 64; 1024 x 1024 on 1.0). Or `<Name>.ngmap`, the compressed format protocol 1.0 sends; a folder with only an ngmap is unpacked beside itself at start."),
  ("<Name>.toml", "file", "Metadata: `[map] modes = [...]`, `author`, `fog_color`, `map_size`, `capture_limit`, and `[spawnpoints]` with `team1` and `team2` rectangles."),
  ("<Name>.txt", "file", "Optional pyspades metadata (extensions: water damage, boundary damage, arena gates and spawns, protected sectors, intel and tent positions). The server reads it itself and never runs Python."),
))

The folder name, the `.vxl` and the `.toml` must match exactly. The repository keeps five example maps (Busan_Port, Normandie, Titanic_Arena, babelstairs, Creative_Flat); between them every mode can be played.

=== Choosing the maps

```toml
[server]
maps = ["Busan_Port", "Raindance"]
map_rotation_mode = "toml"     # "toml" (in order), "alphabetic" or "random"
```

`--map <Name>` plays one map instead of the rotation. At the end of a match the next map loads, or, on 1.0 with two or more other maps in the rotation, a vote opens: after the match summary the players vote for the next map among up to five, and the most voted loads (a tie or no vote draws).

A mode may refuse a map that does not suit it: `Map 'X' does not suit the ngctf mode` is logged and the map leaves the rotation (an empty rotation takes the mode's catalog). ngCTF wants spawn areas at least 200 blocks apart; the zombie mode wants maps tagged `zombie`; the arena and Plant the Bomb want maps with gates or sites; Babel wants Babel maps.

=== Downloading maps

Every map a rotation names that is not on disk is downloaded at start-up, synchronously, one log line per map, from #link("https://github.com/ngspades/maps")[github.com/ngspades/maps] (`raw.githubusercontent.com`, https only, no redirects). The repository keeps ngmap files at quality 100; the server decodes them into a `.vxl`. Each file goes to a `.part` file first; the `.toml` must parse, the VXL must walk to its exact end for the declared size, and only then is it renamed into place. A failure leaves nothing behind.

#fields((
  ("[maps] download", "bool", "`true` by default; `false` plays only the maps on disk. `SP_MAP_DOWNLOAD=0|1` wins."),
  ("where", "folder", "`Resources/maps/<Name>/`, or the data folder's `maps/` when the first cannot be written (a read-only app bundle)."),
  ("limits", "bytes", "64 MB for the `.vxl`, 1 MB for the others; names are letters, digits, `_`, `-`, `.`."),
  ("/map <Name>", "command", "Loads a map now; one that is not installed starts a download in the background, and you run `/map` again when it is there."),
  ("--maps-dir <dir>", "option", "A folder of your own maps, searched before `Resources/maps/`. Downloads land there too when it is writable."),
  ("SP_MAPS_URL", "env", "Points downloads at a copy of the maps repository (https, or http on 127.0.0.1) for a test."),
))

=== One map, many names

A map goes by many names: the folder is `2Fort_CTF`, a site titles it "2Fort CTF", an old server's file is `2fort.vxl`. The maps repository keeps an `aliases.json` (`{"<alias>": "<folder>"}`) and the server resolves a name it does not have before downloading it. Names are compared with case and punctuation dropped (`2Fort CTF` is `2fortctf`). The resolved folder replaces the name in the rotation, in `--map` and in `/map`, logged as `Map '2fortctf' is 2Fort_CTF (alias)`. A name two folders answer to is left out of the list. `SP_MAP_DOWNLOAD=0` limits aliasing to the folders on disk.

=== Your own maps

Copy a map folder from the maps repository, or make one:

```sh
tools/gen_maps.py --kind caves --size 512 --seed 1 --out server/Resources/maps
```

then name it in `maps`. Map files from outside are data, not code: nothing downloaded or copied is ever executed.

=== Map metadata the server applies

From the map's `.txt`: the intel and tent positions (`get_entity_location`), spawn points (`get_spawn_location`), `fog`, `protected` sectors that nobody can build on or dig, `water_damage`, `boundary_damage`, arena gates and spawns, rules written as `on_block_destroy` and `on_block_build_attempt`. A precedence rule: the `.txt` wins over the `.toml` for the same thing; the `.toml` keeps the server's extras (capture limit, arena timings). Anything it could not read is listed once in a warning: `map script rules not applied: ...`.
