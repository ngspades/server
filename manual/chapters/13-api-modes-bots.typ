#import "/template.typ": *

== A game mode written in Lua

A plugin whose returned table has `mode = true` is also a *game mode*. Start it with `--gamemode <plugin name>` (or `[server] gamemode`/a preset by name). The mode plugin is loaded when the mode starts, is asked last by every event, and its `on_init` runs as part of the mode's own initialisation. If it fails to load the server logs `Lua mode X: not loaded, the server plays CTF` and plays classic capture the flag.

```lua
-- Resources/plugins/vip.lua: start with  ngspades-server --gamemode vip
game.supported_profiles("10")
local plugin = {name = "vip", mode = true, wire = "ctf", time_limit = 900}

function plugin.on_init()
    game.set_score_limit(10)
    local x, y = game.spawn(1)
    game.objective(x, y, 12)       -- every bot goes there and holds
end

function plugin.on_kill(killer, victim, reason)
    if killer and killer.team ~= victim.team then
        game.set_score(killer.team, game.score(killer.team) + 1)
        if game.score(killer.team) >= game.score_limit() then game.win(killer.team) end
    end
end

return plugin
```

The fields the server reads from the table a mode returns:

#fields((
  ("mode.mode", "boolean", "`mode = true` declares a game mode. Without it the plugin cannot be started as one (`Plugin X is not a game mode (no mode = true)`)."),
  ("mode.rules", "string", "`\"ctf\"` plays the mode over capture the flag's rules: the intels, tents, pickup and capture, the CTF hooks (spawns by squad, intel drops when a carrier leaves) and the bots' CTF roles. It also unlocks `game.set_intel_spot` and `game.set_tent_spot`. Anything else (or none): the mode has no objective of its own and bots hunt the nearest enemy, unless `game.objective` is set."),
  ("mode.wire", "string", "How 1.0 clients draw the mode, the name sent as the mode (`\"ctf\"` by default: two team scores and CTF's HUD; `\"arena\"`... any wire name ngspades-client knows)."),
  ("mode.time_limit", "number", "Seconds, 1 to 86400: the match ends by the score when it runs out (the higher team wins, equal is a draw: `EVT lua action=time_over`). `/extend` adds to it; `game.set_time_limit` changes it."),
  ("mode.squads", "boolean", "`squads = true`: the generic squads (not for a mode on `rules = \"ctf\"`, which has the CTF's own)."),
  ("mode.squad_spawn", "boolean", "With `squads`: also the respawn next to a squadmate and the rally point."),
))

The mode plugin's other fields and functions are the same as any plugin's. Things a C mode can set that Lua modes cannot: a start-of-map sky (`sky_start`, use `game.set_sky` in `on_init`), and the mode hooks for protected cells and the HUD state keys. A mode with no `bot_goal` hunts the nearest enemy (`hunt-enemy`).

#entry("game.objective", "game.objective(x, y [, radius])", optin: "game mode only")[
  Every bot goes to a spot of its own inside `radius` (default 4) round `x, y` and holds it, as at a hill; with no argument, they hunt again. For the *game mode plugin* only: any other plugin gets the error `game.objective is for the game mode`. Cleared at every map start. Logs `EVT plugin action=objective`.

  ```lua
  local plugin = {mode = true}
  function plugin.on_init() game.objective(256, 256, 10) end
  return plugin
  ```
]

#entry("game.set_intel_spot", "game.set_intel_spot(team, x, y)", optin: "mode with rules = ctf")[
  Where a team's intel stands: the dry ground nearest `x, y`, now and after every capture. Only for a mode with `rules = "ctf"`; the error `intels and tents need a game mode with rules = "ctf"` otherwise.

  ```lua
  local plugin = {mode = true, rules = "ctf"}
  function plugin.on_init() game.set_intel_spot(1, 300, 300) end
  return plugin
  ```
]

#entry("game.set_tent_spot", "game.set_tent_spot(team, x, y)", optin: "mode with rules = ctf")[
  Where a team's tent (base) stands; the same rules as `game.set_intel_spot`.

  ```lua
  local plugin = {mode = true, rules = "ctf"}
  function plugin.on_init() game.set_tent_spot(1, 310, 300) end
  return plugin
  ```
]

== Steering the bots

Bots are server-side players. A plugin decides *what* they should do; the server does the heavy work (finding the way, seeing, aiming, fighting, digging and building). A plugin never walks a path or casts rays in Lua.

`on_bot_think(bot)` (chapter 10) returns a *goal table*, or `nil` to let the mode decide. Every field is optional:

#fields((
  ("goal.name", "string", "The goal's name, for the logs and `on_bot_arrived` (default `\"plugin\"`; 31 characters)."),
  ("goal.x", "number", "Where to go, map coordinates (default: where the bot stands)."),
  ("goal.y", "number", "As `x`."),
  ("goal.z", "number", "The floor cell there (its top block, `game.ground(x, y)`); 0 or none: unknown."),
  ("goal.exact", "boolean", "That very cell, not just near it."),
  ("goal.hold", "number", "Stop this close (blocks) and watch."),
  ("goal.watch", "table", "Where to look while holding: a point `{x =, y =}` or a player table."),
  ("goal.role", "string", "How it fights and, when the mode has no classes, its gun: one of the roles below (default `attack`)."),
  ("goal.intent", "string or list", "What the goal is for: a word or a list of words from the intents below. A bad word is the plugin's error."),
  ("goal.rush", "boolean", "Run there, ignoring fights."),
  ("goal.assault", "boolean", "Fight in the open: no walls, no ducking."),
  ("goal.walk_only", "boolean", "On foot: no digging and no sneaking."),
  ("goal.keep_terrain", "boolean", "Not even the spade to get out of a jam."),
  ("goal.climb", "integer", "`1`: build a pillar up to floor `z` in the goal's column; `2`: dig down to it."),
  ("goal.work", "table", "`{x =, y =, z =, dig = bool, color = 0xRRGGBB}`: within reach, put a block there (or spade it with `dig`)."),
  ("goal.camp", "integer", "`1`: a camper (holds a spot behind a wall); `2`: the overwatch (a high spot that sees `home`)."),
  ("goal.home", "table", "`{x =, y =, z =}`: what the team protects; defenders post by it and go for enemies near it first."),
  ("goal.guard", "table", "`{x =, y =}`: something else to defend; enemies there come first."),
  ("goal.color", "integer", "Inside `work`: the colour `0xRRGGBB` of the block to place."),
  ("goal.dig", "boolean", "Inside `work`: spade the cell instead of building on it."),
))

#wide-table(
  (auto, 1fr),
  table.header([*Role*], [*The bot...*]),
  [#raw("role.attack")], [attacks (the default); a rifle or an SMG when the mode has no classes.],
  [#raw("role.defend")], [defends: a shotgun, holds near its post.],
  [#raw("role.infiltrate")], [sneaks to the objective and fights only close.],
  [#raw("role.cover")], [escorts and covers a mate, fighting from beside it.],
  [#raw("role.rush")], [runs and guns without stopping for fights.],
)

#wide-table(
  (auto, 1fr),
  table.header([*Intent*], [*The bot...*]),
  [#raw("intent.take")], [is going to take the objective: close to it, it stops for no fight.],
  [#raw("intent.push")], [is an attacker on its way: an enemy farther than 30 blocks does not stop it.],
  [#raw("intent.squad")], [is on its squad's lane: it only takes on enemies within 60.],
  [#raw("intent.with")], [keeps up with someone (a leader, a carrier): it shoots on the move.],
  [#raw("intent.post")], [holds a post: it fights from it like a camper and never steps aside.],
  [#raw("intent.home")], [works at its own base: its way there digs nothing.],
  [#raw("intent.guard")], [is the defender at its post by `home`.],
  [#raw("intent.chase")], [is after the enemy carrying its team's objective.],
  [#raw("intent.brief")], [listens to the team's plan: it holds still.],
  [#raw("intent.plant")], [takes an explosive to where it goes.],
  [#raw("intent.cover")], [covers a mate.],
)

The names in the first column are written `goal.x`, `role.attack`, `intent.take` in the reference's checklist; in a goal table you write the bare word (`role = "defend"`, `intent = {"post", "home"}`).

```lua
-- every bot of team 1 follows the first admin it finds
local plugin = {}
function plugin.on_bot_think(bot)
    if bot.team ~= 1 then return nil end
    for _, p in ipairs(game.players()) do
        if not p.bot and p.alive and p:has_role("admin") then
            return {name = "follow-admin", x = p.x, y = p.y, hold = 4, intent = "with",
                    watch = {x = p.x + p.ox * 10, y = p.y + p.oy * 10}}
        end
    end
    return nil
end
return plugin
```

A better pattern for jobs: hand them out once a second in `on_bot_team_tick`, and read them in `on_bot_think`:

```lua
local plugin = {}
local job = {}   -- bot id -> "defend" | "attack"

function plugin.on_bot_team_tick(team, bots)
    for i, b in ipairs(bots) do job[b.id] = (i <= 2) and "defend" or "attack" end
end

function plugin.on_bot_think(bot)
    local x, y = game.spawn(bot.team)
    if job[bot.id] == "defend" then
        return {name = "guard-spawn", x = x, y = y, hold = 6, role = "defend", intent = {"guard", "home"},
                home = {x = x, y = y}}
    end
    return nil
end

function plugin.on_bot_arrived(bot, name) game.log(bot.name .. " arrived at " .. name) end
return plugin
```

How the choices play out (what bots do with a goal, their senses and their costs) is in #link("https://github.com/ngspades/server/blob/main/docs/bots.md")[docs/bots.md], section "Asking bots for something". Measured with 120 bots: 3 to 7 microseconds a call, under 0.15 ms of a 16.7 ms tick.

== Bots at a glance for plugin authors

- A bot's player table has `bot = true`, `goal` (the name of its current goal) and, while it fights, `target` (an enemy's id).
- `p:kick` and `game.ban` raise errors for bots; remove bots with the commands `/kickbot` and `/kickbots` (through `game.run_command`, if allowed).
- A bot's events are the same as a player's (`on_spawn`, `on_kill`...), and its join goes through `on_join`.
- `p:sees(q)` is the bots' own sight test; it is cheap enough for a few calls a tick, not for a loop over every pair of players.
