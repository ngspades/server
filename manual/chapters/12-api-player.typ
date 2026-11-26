#import "/template.typ": *

== The player table

Every event and `game.players()` hands out *player tables*. A player table is a snapshot taken when it was handed out: its fields do not change afterwards. To read a player again later, call `game.player(id)`. A table has a metatable that gives it the methods below, which act on the live player through its `id`; calling one on a player who has left raises `that player has left`.

Positions use map coordinates: `x`, `y` along the ground, `z` growing downwards, and the player's `z` is its hip (the floor under a standing player is about `z + 2.4`).

=== Fields

#fields((
  ("p.id", "integer", "The player's id, 0 to 255. Unique while connected; the key to use in tables."),
  ("p.name", "string", "The name. Bots are `Bot_` plus two words."),
  ("p.team", "integer", "1, 2, or 0 for a spectator."),
  ("p.alive", "boolean", "In the world, not waiting to respawn."),
  ("p.hp", "integer", "Health, 0 to 100 (a class with more health is scaled to this)."),
  ("p.kills", "integer", "Kills this match."),
  ("p.deaths", "integer", "Deaths this match."),
  ("p.assists", "integer", "Kill assists (modes that count them)."),
  ("p.score", "integer", "The personal score column (what `p:set_score` writes)."),
  ("p.captures", "integer", "Intel captures."),
  ("p.shots", "integer", "Shots fired."),
  ("p.hits", "integer", "Shots that hit a player."),
  ("p.blocks_placed", "integer", "Blocks built."),
  ("p.squad", "integer", "Its squad, 1 to 16, 0 for none."),
  ("p.reloading", "boolean", "Reloading now."),
  ("p.crouching", "boolean", "Crouched."),
  ("p.sprinting", "boolean", "Sprinting."),
  ("p.airborne", "boolean", "Not on the ground."),
  ("p.wade", "boolean", "In water."),
  ("p.jetpack", "boolean", "Wearing a jetpack this life."),
  ("p.bot", "boolean", "A server-side bot."),
  ("p.goal", "string", "Bots only: the name of the goal it follows."),
  ("p.target", "integer", "Bots only, and only while it has one: the id of the enemy it fights."),
  ("p.tool", "integer", "The tool in hand: 0 spade, 1 block, 2 gun, 3 grenade."),
  ("p.weapon", "integer", "0 rifle, 1 SMG, 2 shotgun."),
  ("p.blocks", "integer", "Blocks carried."),
  ("p.grenades", "integer", "Grenades carried."),
  ("p.intel", "boolean", "Carrying the enemy intel."),
  ("p.class", "string", "1.0 only, and only in a mode with classes: `commando`, `rocketeer`, `marksman` or `miner`. Absent otherwise."),
  ("p.ip", "string", "Humans only: its address as text (an IPv6 player's full address). Absent for bots."),
  ("p.x", "number", "East-west position of the hip."),
  ("p.y", "number", "North-south position of the hip."),
  ("p.z", "number", "Height of the hip, growing downwards."),
  ("p.ox", "number", "Where the player looks: x of a unit vector."),
  ("p.oy", "number", "y of the look vector."),
  ("p.oz", "number", "z of the look vector (positive looks down)."),
))

```lua
local plugin = {}
function plugin.on_player_tick(p)
    if p.alive and not p.bot and p.z < 5 then
        p:message(("You are very high: z=%.1f"):format(p.z))
    end
end
return plugin
```

== Methods

Methods are called with a colon: `p:message("hi")`. Unless said otherwise they return nothing.

#entry("p:message", "p:message(text)")[
  A line only this player reads. Nothing happens for a bot.

  ```lua
  local p = game.player(0)
  if p then p:message("Hello") end
  ```
]

#entry("p:set_hp", "p:set_hp(n)")[
  Sets the health (capped at 100). Zero or less kills the player. Nothing happens to a dead player.

  ```lua
  local p = game.player(0)
  if p then p:set_hp(50) end
  ```
]

#entry("p:damage", "p:damage(n, by)")[
  Hurts the player through the normal path: hit marks, kill credit and the kill feed go to `by`, a player table (default: the player itself). `n` must be above zero, an integer of health points.

  ```lua
  local victim, attacker = game.player(1), game.player(2)
  if victim and attacker then victim:damage(25, attacker) end
  ```
]

#entry("p:kill", "p:kill()")[
  Kills the player (reason 5 on `on_kill`). Nothing happens to a dead player.

  ```lua
  local p = game.find("Bot_Dusty")
  if p then p:kill() end
  ```
]

#entry("p:teleport", "p:teleport(x, y, z)")[
  Moves the player to the map coordinates of the hip, its own client included; its velocity is zeroed. Off the map raises `bad argument ... off the map`. Logged as `EVT plugin action=teleport`.

  ```lua
  local p = game.player(0)
  if p then
      local x, y = game.spawn(1)
      p:teleport(x, y, game.ground(math.floor(x), math.floor(y)) - 3)
  end
  ```
]

#entry("p:spawn_heli", "p:spawn_heli() -> entity id | nil, why", profile: "1.0")[
  A helicopter on dry ground in front of the player, of the player's team (team 1 for a spectator). It is not a base slot, so nothing replaces a wreck: ask again. Returns the entity's id, or `nil` and `"profile"`, `"no_ground"` or `"too_many_objects"`.

  ```lua
  local p = game.player(0)
  if p then
      local id, why = p:spawn_heli()
      if not id then p:message("No helicopter: " .. why) end
  end
  ```
]

#entry("p:restock", "p:restock()")[
  Full health, blocks, grenades and ammunition, as at a tent. Nothing happens to a dead player.

  ```lua
  function plugin_on_spawn_example(p) p:restock() end
  ```
]

#entry("p:fog", "p:fog(r, g, b)")[
  This player's sky colour only (each 0 to 255); `game.fog` sets everyone's.

  ```lua
  local p = game.player(0)
  if p then p:fog(80, 0, 0) end
  ```
]

#entry("p:drop_crate", "p:drop_crate(kind) -> true | false, reason", profile: "1.0")[
  A supply crate, `kind` `"ammo"` or `"health"`, falls from the sky onto the player, as a reward. The first player at it who needs it takes it; it goes after 2 minutes on the ground. `false` with `"too many crates out, or no ground there"` (16 at most). Another `kind` raises an error.

  ```lua
  local p = game.player(0)
  if p then p:drop_crate("health") end
  ```
]

#entry("p:set_outfit", "p:set_outfit(n [, colour]) -> true | false, reason", profile: "1.0")[
  How the player looks (the aosprotocol draft's numbers: 0 soldier, 7 miner, 64 zombie, 255 its team's). `colour` is 0 or 1 for a team's colour, or 255 (default) for its own. `false` with `"outfit: 0..255"`. Cosmetic only: the rules never read it.

  ```lua
  local p = game.player(0)
  if p then p:set_outfit(64) end
  ```
]

#entry("p:outfit", "p:outfit() -> number | nil, reason", profile: "1.0")[
  What the player is dressed in (255 means its team's outfit).

  ```lua
  local p = game.player(0)
  if p then game.log("outfit " .. tostring(p:outfit())) end
  ```
]

#entry("p:has_role", "p:has_role(role) -> boolean")[
  `true` when the player is logged in as that role or a higher one: `"trusted"`, `"guard"`, `"mod"`, `"admin"`, `"manager"`.

  ```lua
  for _, p in ipairs(game.players()) do
      if p:has_role("admin") then p:message("You are an admin") end
  end
  ```
]

#entry("p:sees", "p:sees(q) -> boolean")[
  `true` when there is a clear line from this player's eye to `q`'s chest (the bots' own sight test, for any player). `q` is a player table. No range limit other than the map.

  ```lua
  local a, b = game.player(0), game.player(1)
  if a and b and a:sees(b) then game.log("a sees b") end
  ```
]

#entry("p:kick", "p:kick([reason])")[
  Disconnects the player and tells everyone `Player X (#id) has been kicked: reason`. A bot cannot be kicked (raises `a bot cannot be kicked`): remove bots with `/kickbot`. Logged as `EVT plugin action=kick`.

  ```lua
  local p = game.find("Spammer")
  if p and not p.bot then p:kick("spamming") end
  ```
]

#entry("p:set_class", "p:set_class(name_or_n) -> boolean", profile: "1.0")[
  Picks the class for the player, as its class screen does (a living player respawns with it). `false` when refused (a class turned off, or the mode has none). `on_class` is not asked for this call.

  ```lua
  local p = game.player(0)
  if p and not p:set_class("miner") then p:message("No miner for you") end
  ```
]

#entry("p:set_team", "p:set_team(team) -> boolean")[
  Moves the player to 0 (spectators), 1 or 2, through the same path as ngspades-client's request; the mode's rules still apply (a zombie does not cross). `true` when it is on that team afterwards, `false` otherwise. `on_team` is not asked.

  ```lua
  local p = game.player(0)
  if p then p:set_team(0) end
  ```
]

#entry("p:set_tool", "p:set_tool(tool) -> boolean")[
  Puts a tool in hand: 0 spade, 1 block, 2 gun, 3 grenade. `false` when out of range or the mode forbids it (no gun, no grenade, no blocks). `on_tool` is not asked.

  ```lua
  local p = game.player(0)
  if p then p:set_tool(0) end
  ```
]

#entry("p:set_weapon", "p:set_weapon(weapon) -> boolean")[
  Gives the player a weapon: 0 rifle, 1 SMG, 2 shotgun (refilling to its usual clip); `false` out of range. ngspades-client shows the new gun at its next update. In a mode with classes the class decides the gun.

  ```lua
  local p = game.player(0)
  if p then p:set_weapon(2) end
  ```
]

#entry("p:set_ammo", "p:set_ammo(clip, reserve)")[
  Sets the current clip and the reserve (each clamped to 0 to 255).

  ```lua
  local p = game.player(0)
  if p then p:set_ammo(30, 90) end
  ```
]

#entry("p:set_blocks", "p:set_blocks(n)")[
  Sets the blocks carried (0 to 65535); the owner's client is told.

  ```lua
  local p = game.player(0)
  if p then p:set_blocks(200) end
  ```
]

#entry("p:set_grenades", "p:set_grenades(n)")[
  Sets the grenades carried (0 to 255).

  ```lua
  local p = game.player(0)
  if p then p:set_grenades(3) end
  ```
]

#entry("p:set_score", "p:set_score(n)")[
  Sets the personal score column of the scoreboard (not the team's: see `game.set_score`). Negative becomes 0.

  ```lua
  local p = game.player(0)
  if p then p:set_score(10) end
  ```
]

#entry("p:set_kills", "p:set_kills(n)")[
  Sets the kills column.

  ```lua
  local p = game.player(0)
  if p then p:set_kills(0) end
  ```
]

#entry("p:set_deaths", "p:set_deaths(n)")[
  Sets the deaths column.

  ```lua
  local p = game.player(0)
  if p then p:set_deaths(0) end
  ```
]

#entry("p:set_assists", "p:set_assists(n)")[
  Sets the assists column.

  ```lua
  local p = game.player(0)
  if p then p:set_assists(0) end
  ```
]

#entry("p:set_squad", "p:set_squad(n) -> boolean", profile: "1.0")[
  Puts the player in squad `n` (0 alone, 1 to 16), by the same rules as ngspades-client's squad command; `true` when it was taken.

  ```lua
  local p = game.player(0)
  if p then p:set_squad(1) end
  ```
]

#entry("p:set_jetpack", "p:set_jetpack(on) -> boolean", profile: "1.0")[
  Grants or removes the jetpack for this life. Returns whether the player has one afterwards (it can stay `false` when the mode forbids jetpacks: the zombie mode, classic CTF).

  ```lua
  local p = game.player(0)
  if p and not p:set_jetpack(true) then p:message("No jetpack here") end
  ```
]

== Putting it together

A plugin that welcomes players, gives a loadout, and remembers how many times each name has joined:

```lua
local plugin = {}

function plugin.on_join(p, team)
    game.store.joins = game.store.joins or {}
    local n = (game.store.joins[p.name] or 0) + 1
    game.store.joins[p.name] = n
    game.store_save()
    p:message(("Welcome back, %s (visit %d)"):format(p.name, n))
end

function plugin.on_spawn(p)
    if not p.bot then
        p:set_blocks(100)
        p:set_grenades(2)
    end
end

return plugin
```
