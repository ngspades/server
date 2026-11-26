#import "/template.typ": *

== Coverage of this reference

`manual/tools/extract_api.py` scans the server's Lua bindings (`Plugins.c`, `Gamemodes/LuaMode.c` and the class table) and writes `api-surface.txt`: every function, method, field, event, table key, config key, environment variable and log action the sandbox exposes. `make check` fails when a name of that list is not in this reference. The counts:

#include "/gen/coverage.typ"

*How the entries were verified.* `manual/tests/apitest.lua`, `modetest.lua` and `pendtest.lua` are plugins that call the API on a real server (a Busan_Port next gen CTF match with six bots, a Lua mode on `rules = "ctf"`, and a run with the opt-in calls allowed), log `VERIFY <name> ok|fail <result>`, and `manual/tools/run_api_test.py` turns the lines into the list that marks entries "verified by running". The same run checked, among others: the `game.http` POST round trip against a local server (status 200, the echoed body in `on_done`), `game.command` through `game.run_command`, the queued `game.set_mode` (the plugin was loaded again by the new mode), `game.json_*` round trips, the failure texts of the denied opt-in calls, and the player table's field list. Run it with `make api-test` (it needs a built server).

*What was not exercised by running* (read from the C code only): events that need real players (`on_chat`, `on_login`, `on_team`, `on_weapon`...: the test has bots only, so only the events bots cause were seen), `p:kick` and `game.ban` on a human (they raise errors for bots; the error text was verified), `game.next_map` and `game.win` (they reload the map), the 0.75 profile's refusals of 1.0-only calls, the WebRTC, and everything about the 0.75 profile.

== Differences between this reference and `docs/api.md` of the plugins repository

Where the C code and the shorter contract in #link("https://github.com/ngspades/plugins")[ngspades/plugins] `docs/api.md` differ, this manual follows the code. The differences found:

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rgb("#cfc8bd"),
  [*Topic*], [*api.md says / omits; the C code does*],
  [Events], [`on_flashlight(p, on)` exists (a veto, in both profiles) and is not listed.],
  [Player methods], [`p:spawn_heli`, `p:set_outfit` and `p:outfit` exist and are not listed.],
  [game functions], [`game.sky`, `game.set_sky`, `game.set_team_outfit`, `game.set_time_limit`, `game.time_left` exist and are not in the function table (some are in other prose).],
  [`p:set_blocks`], [api.md: 0..255. C: clamps to 0..65535 (a Miner's wallet can hold 1500).],
  [`on_destroy` kinds], [api.md: spade, dig, gun, grenade. C also passes `drill`, `mg`, and explosion causes `rocket`, `landmine`, `dynamite`, `ptb_bomb`.],
  [Kill reasons], [api.md lists 0 to 6. C also uses 7 sentry, 8 explosive, 9 machine gun.],
  [`on_damage` attacker], [For falls and water the attacker is the victim itself; not documented.],
  [Mode `wire`], [api.md: `"ctf"` or `"arena"`. C reads any string (default `"ctf"`).],
  [Class keys], [api.md's table lists 14 keys; the class has 27 (`block_damage`, `spawn_perk`, `dig_delay`, `sprint`, `water_friction`, `headshot`, `melee`, `fall_min`, `fall_max`, `fall_dmg`, `fall_water`, `blocks_max` are missing), and its perk list omits `mg`. The stated default health (130, 85, 70, 125) differs from the shipped config's comment (100, 70, 70, 85).],
  [`p:kick`], [api.md: "(not bots)". C raises `a bot cannot be kicked`; likewise `game.ban`.],
  [`game.later`], [Beyond 64 timers it raises an error (api.md: "at most 64").],
  [Plugin commands], [They run from the chat, the TTY console and `game.run_command`, not from the remote console (`no such command`). `docs/rcon.md` says the remote console runs "the console's commands".],
  [Fields that do nothing], [`arena075.lua` assigns `game.killing`, `game.building` and `game.respawn_time`. Those fields do not exist in the C API; assigning them to the `game` table has no effect on the server.],
  [`set_time_limit`, `objective`, spots], [Return values and errors (`game mode only`) are only in the code.],
)

== Differences between the existing documents and the code

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rgb("#cfc8bd"),
  [*Topic*], [*Finding*],
  [`server_tools.lua`], [`config.toml` loads it, and `tools/package_server.sh`, `tools/install_server.sh` and `server/Dockerfile` copy `server/Resources/plugins/server_tools.lua`, but the file is not in the repository. A source build logs a missing plugin; the Dockerfile's `cp` and the packaging script would fail.],
  [`[server] master`], [The shipped comment says "Enable this if you want your server to show up on the server list". In a 1.0 server the key only logs `Not announcing on the 0.75 master list`; the listing is `[master] enabled`.],
  [Remote console], [`docs/rcon.md` says it runs "any console command"; a command a plugin added with `game.command` answers `no such command`.],
  [Role names], [`/login` takes `mod`; the config key is `moderator`.],
  [Releases], [`docs/hosting.md` says the repository is private; it is public, and no `ngspades-server-v*` release exists yet, so the install script's download step has nothing to fetch.],
  [`SP_EXIT_AFTER`], [Only honoured with `SP_FAST=1`; in real time the server ignores it.],
)

== Building this manual

```sh
cd manual
make site        # HTML for every chapter and the landing page in site/
make pdf         # one PDF a chapter in site/pdf/ and site/ngspades-server-manual.pdf
make check       # coverage of the API, Lua examples, links, placeholders
make api-test    # run the test plugins on a built server (SERVER=path)
make publish     # push site/ to the gh-pages branch
make clean
```

It needs Typst 0.15 (HTML export is experimental), Python 3 and Lua 5.4's `luac`. The HTML pages share one stylesheet that `template.typ` emits and `tools/build_site.py` moves into `site/style.css`.
