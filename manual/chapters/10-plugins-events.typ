#import "/template.typ": *

An *event* is a function of the table the plugin returns. The server calls it at the place the change happens. Events come in three kinds:

- *Notifications* (`on_tick`, `on_kill`, `on_spawn`...): the return value is ignored.
- *Vetoes* (`on_hit`, `on_build`, `on_chat`...): returning `false` refuses the action.
- *Filters* (`on_damage`, `on_join`, `on_chat`, `on_spawn_point`): a returned value replaces the original.

Every plugin is asked in the order of `load` (a game mode last). Returning `false` from a veto stops the plugins after it from being asked and logs `EVT plugin_refused plugin=<name> event=<event>`. A value returned from a filter replaces the original for the plugins after it and for the server. Returning `nil` (or nothing) leaves things as they are. A Lua error turns the plugin off for the map (chapter 9).

The arguments named `p`, `attacker`, `victim`, `killer` and `bot` are *player tables* (chapter 12). Positions in `x, y, z` arguments are map coordinates: *z grows downwards* and `z = 0` is the top of the world. They arrive as floats (`298.0`); `game.cell(x, y, z)` and the other functions accept them.

#note[
  Keep events short: the server calls them inside its tick. A heavy computation belongs in a timer spread over several ticks, or in `on_bot_team_tick` for work about bots. An event that runs longer than the instruction budget turns the plugin off.
]

== Lifecycle and timers

#entry("on_init", "function plugin.on_init()")[
  The map has started. Called once per map, after the game mode has initialised, in the order of `load`. This is the place to read `game.map_size()`, set limits (`game.set_score_limit`), change class figures for the map (`game.set_class`) and register commands. (Commands and top-level code may also run when the file loads.) *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_init()
      game.set_class("miner", {health = 160, speed = 0.9})
      game.log("map is " .. game.server_info().map)
  end
  return plugin
  ```
]

#entry("on_tick", "function plugin.on_tick()")[
  A plugin tick: 10 times a second. Timers from `game.later`, messages, HTTP answers and queued map changes are handled on the same tick. *Notification.*

  ```lua
  local plugin = {}
  local ticks = 0
  function plugin.on_tick()
      ticks = ticks + 1
      if ticks % 600 == 0 then game.notice("one minute passed") end
  end
  return plugin
  ```
]

#entry("on_player_tick", "function plugin.on_player_tick(p)")[
  Once a second for each player in the game (bots included). Use it for slow per-player checks rather than `on_tick` and a loop. *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_player_tick(p)
      if p.alive and p.hp < 30 then p:message("Low health: find a tent") end
  end
  return plugin
  ```
]

#entry("on_message", "function plugin.on_message(from, topic, data)")[
  Another plugin called `game.send(topic, data)`. `from` is its name (a string), `topic` a string, `data` the value it sent, decoded from JSON (plain data only; `nil` if none). Delivered on a plugin tick, to every plugin but the sender. A plugin does not hear its own messages. *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_message(from, topic, data)
      if topic == "announce" then game.notice(data.text) end
  end
  return plugin
  ```
]

== Players joining, leaving and spawning

#entry("on_join", "function plugin.on_join(p, team) -> team | false | nil")[
  A player joined a team, or a bot picked its team. `team` is 1, 2, or 0 for the spectators. Return a team number to send the player there instead, or `false` to keep it a spectator. Called after the player has chosen its name and team and before it spawns. *Filter and veto.*

  ```lua
  local plugin = {}
  function plugin.on_join(p, team)
      p:message("Welcome, " .. p.name)
      if team == 0 then return nil end
      -- keep the teams even: the smaller team gets the player
      local n = {0, 0}
      for _, q in ipairs(game.players()) do
          if q.team == 1 or q.team == 2 then n[q.team] = n[q.team] + 1 end
      end
      return n[1] <= n[2] and 1 or 2
  end
  return plugin
  ```
]

#entry("on_team", "function plugin.on_team(p, team) -> false | nil")[
  A player asks to change team (`team` 1, 2 or 0). Return `false` to refuse. Only a player's own request goes through here; `p:set_team` and the server's own moves do not. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_team(p, team)
      if p.intel then p:message("Drop the intel first"); return false end
  end
  return plugin
  ```
]

#entry("on_leave", "function plugin.on_leave(p)")[
  A player disconnected (a human; a removed bot goes through the same path). The player table is a snapshot, usable to read but not to call methods on (`that player has left`). *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_leave(p) game.notice(p.name .. " left") end
  return plugin
  ```
]

#entry("on_spawn", "function plugin.on_spawn(p)")[
  A player spawned and is in the world. Typical use: a loadout (`p:set_blocks`, `p:restock`). *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_spawn(p) p:set_blocks(100); p:message("Spawned") end
  return plugin
  ```
]

#entry("on_spawn_point", "function plugin.on_spawn_point(p) -> x, y, z | nil")[
  Where should this player spawn? Return three numbers (map coordinates of the hip; the floor is about `z + 2.4`) to place it there; return nothing for the usual spot. The first plugin that returns three numbers wins. The server logs `EVT plugin action=spawn_point`. *Filter.*

  ```lua
  local plugin = {}
  function plugin.on_spawn_point(p)
      local x, y = game.spawn(p.team == 0 and 1 or p.team)
      local z = game.ground(math.floor(x), math.floor(y)) - 3
      return x + math.random(-5, 5), y, z
  end
  return plugin
  ```
]

#entry("allows_respawn", "function plugin.allows_respawn(p) -> false | nil")[
  May this dead player respawn now? Return `false` to keep it waiting. Asked repeatedly while the player waits; it is the only event whose name does not start with `on_`. A mode's own rule is asked first, and both must allow it. *Veto.*

  ```lua
  local plugin = {}
  local round_over = false
  function plugin.allows_respawn(p)
      if round_over then return false end
  end
  return plugin
  ```
]

== Combat

#entry("on_hit", "function plugin.on_hit(attacker, victim, kind) -> false | nil")[
  A bullet or a spade hits a player, after the server's checks and before any damage. `kind`: 0 torso, 1 head, 2 arms, 3 legs, 4 melee. Return `false` for no damage at all. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_hit(attacker, victim, kind)
      if victim.team == attacker.team then return false end
  end
  return plugin
  ```
]

#entry("on_damage", "function plugin.on_damage(attacker, victim, amount, reason) -> amount | false | nil")[
  Any damage: guns, grenades, falls, water, explosions. `amount` is the health points about to be lost (after the mode's own scaling), `reason` the kill reason (0 body, 1 head, 2 melee, 3 grenade, 4 fall...). For damage with no attacker (a fall) `attacker` is the victim itself. Return a number to replace the amount, `false` to cancel the damage. Only damage above zero is asked. *Filter and veto.*

  ```lua
  local plugin = {}
  function plugin.on_damage(attacker, victim, amount, reason)
      if reason == 1 then return amount * 2 end   -- headshots hurt twice
      if reason == 4 then return false end        -- no fall damage
  end
  return plugin
  ```
]

#entry("on_kill", "function plugin.on_kill(killer, victim, reason)")[
  A death. `killer` may be `nil`. `reason`: 0 body, 1 head, 2 melee, 3 grenade, 4 fall, 5 team change (or a plugin's `p:kill`), 6 weapon change, 7 sentry, 8 explosive (landmine, dynamite, bomb), 9 machine gun. Called for every death, including a zombie turn. *Notification.*

  ```lua
  local plugin = {}
  local kills = {}
  function plugin.on_kill(killer, victim, reason)
      if killer == nil or killer.id == victim.id then return end
      kills[killer.id] = (kills[killer.id] or 0) + 1
      if kills[killer.id] % 5 == 0 then game.notice(killer.name .. " is on a roll") end
  end
  return plugin
  ```
]

#entry("on_grenade", "function plugin.on_grenade(p) -> false | nil")[
  A player throws a grenade (only asked when it has one). Return `false` to refuse. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_grenade(p) if p.crouching then return false end end
  return plugin
  ```
]

#entry("on_explode", "function plugin.on_explode(p, x, y, z)")[
  An explosion went off: a grenade, a rocket, a landmine, dynamite, or a bomb. `p` is the thrower or owner; `x, y, z` the point. *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_explode(p, x, y, z) game.log(string.format("boom at %.0f,%.0f", x, y)) end
  return plugin
  ```
]

#entry("on_reload", "function plugin.on_reload(p)")[
  A player started to reload. *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_reload(p) p:message("Reloading") end
  return plugin
  ```
]

#entry("on_tool", "function plugin.on_tool(p, tool) -> false | nil")[
  A player changes tool: 0 spade, 1 block, 2 gun, 3 grenade. The mode's own limits (a zombie's hands only) are applied first. Return `false` to refuse. `p:set_tool` does not call this event. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_tool(p, tool) if tool == 2 and not p.alive then return false end end
  return plugin
  ```
]

#entry("on_weapon", "function plugin.on_weapon(p, weapon) -> false | nil")[
  A player changes weapon: 0 rifle, 1 SMG, 2 shotgun. Return `false` to refuse. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_weapon(p, weapon) if weapon == 2 then return false end end
  return plugin
  ```
]

#entry("on_class", "function plugin.on_class(p, name, by) -> false | nil", profile: "1.0")[
  A player picks a class in a mode that has classes: `name` is `"commando"`, `"rocketeer"`, `"marksman"` or `"miner"`; `by` says who: `"client"` (the player's class screen), `"join"` (the class it joined with) or `"bot"`. Not asked when the class does not change nor when a plugin picks with `p:set_class` (`by` would be `"plugin"`). Return `false` to refuse. Never called on the 0.75 profile (a plugin that defines it is told once: `EVT plugin action=refused ... what=on_class`). *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_class(p, name, by)
      if name == "rocketeer" and by == "client" then return false end
  end
  return plugin
  ```
]

#entry("on_flashlight", "function plugin.on_flashlight(p, on) -> false | nil")[
  A player (or a bot) turns its flashlight on or off (`on` is a boolean). Return `false` to refuse; nobody is told. Asked in both profiles (on 0.75 for ZeroSpades' flashlight extension). A zombie's light is refused by the mode before this event. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_flashlight(p, on)
      if on and p.team == 2 then return false end
  end
  return plugin
  ```
]

== Building and destroying

#entry("on_build", "function plugin.on_build(p, x, y, z) -> false | nil")[
  A player places a block (once per cell of a block line). Return `false` to refuse. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_build(p, x, y, z)
      if z < 2 then p:message("Too high to build"); return false end
  end
  return plugin
  ```
]

#entry("on_destroy", "function plugin.on_destroy(p, x, y, z, kind) -> false | nil")[
  A block is about to be destroyed. `kind` is a string: `"spade"` (one block), `"dig"` (three blocks), `"gun"` (a hit that breaks it), `"grenade"`, `"drill"`, `"mg"`, or the cause of an explosion: `"rocket"`, `"landmine"`, `"dynamite"`, `"ptb_bomb"`. For an explosion the event is asked once for the centre cell and `false` keeps the whole crater. `p` is the player responsible. Return `false` to refuse. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_destroy(p, x, y, z, kind)
      if kind == "grenade" then return false end
  end
  return plugin
  ```
]

== Chat, login and the objective

#entry("on_chat", "function plugin.on_chat(p, text, team_only) -> string | false | nil")[
  A chat line, not a `/command` (those go to the commands, see `game.command`). `team_only` is true for team chat. Return a string to replace the text, or `false` to drop the line. A muted player's lines never reach it. *Filter and veto.*

  ```lua
  local plugin = {}
  function plugin.on_chat(p, text, team_only)
      if text:find("badword", 1, true) then return false end
      return text:gsub("noob", "friend")
  end
  return plugin
  ```
]

#entry("on_login", "function plugin.on_login(p, role) -> false | nil")[
  A player gave `/login` the right password for `role` (`"trusted"`, `"guard"`, `"mod"`, `"admin"` or `"manager"`). The player already has the role while this runs. Return `false` to undo the login (a second factor): tell the player why with `p:message`. *Veto.*

  ```lua
  local plugin = {}
  function plugin.on_login(p, role)
      if role == "manager" and not game.store.allowed then
          p:message("The manager login is off")
          return false
      end
  end
  return plugin
  ```
]

#entry("on_intel", "function plugin.on_intel(p, action)")[
  The intel changed hands in a capture the flag mode (classic, next gen, Babel, and Lua modes on `rules = "ctf"`). `action` is `"pickup"`, `"capture"` or `"drop"`. `p` may be `nil` when no player caused it. *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_intel(p, action)
      if p and action == "capture" then game.notice(p.name .. " captured the intel!") end
  end
  return plugin
  ```
]

== Bots

The bot events are the plugin's way to steer the bots (chapter 13 has the goal table and the intents).

#entry("on_bot_think", "function plugin.on_bot_think(bot) -> goal | nil")[
  Asked at every think of every bot (five a second), before the game mode. Return a *goal table* to give the bot that goal, or `nil` to leave it to the mode. The first plugin that returns a table wins. A malformed table (an unknown `role` or `intent`) is an error and turns the plugin off. A call costs 3 to 7 microseconds with 120 bots; keep it short and work out what the whole team needs once a second in `on_bot_team_tick`. *Filter.*

  ```lua
  local plugin = {}
  function plugin.on_bot_think(bot)
      if bot.team ~= 1 then return nil end
      local x, y = game.spawn(2)
      return {name = "raid", x = x, y = y, intent = "push"}
  end
  return plugin
  ```
]

#entry("on_bot_arrived", "function plugin.on_bot_arrived(bot, name)")[
  A bot reached the goal this plugin gave it (once per goal): within `hold` blocks of it (1.5 at least). `name` is the goal's name. A goal that follows someone counts as new only when its destination jumps far (twice the reach, 8 blocks at least). *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_bot_arrived(bot, name) game.log(bot.name .. " reached " .. name) end
  return plugin
  ```
]

#entry("on_bot_stuck", "function plugin.on_bot_stuck(bot, why)")[
  A bot made no progress toward its goal for a while. `why` is `"no_progress"`. *Notification.*

  ```lua
  local plugin = {}
  function plugin.on_bot_stuck(bot, why) game.log(bot.name .. " is stuck: " .. why) end
  return plugin
  ```
]

#entry("on_bot_team_tick", "function plugin.on_bot_team_tick(team, bots)")[
  Once a second for each team (1 and 2): `bots` is a list of that team's bot player tables, alive or not. The place to hand out jobs ("the two nearest defend") that each `on_bot_think` then reads from a table of the plugin's own. *Notification.*

  ```lua
  local plugin = {}
  local defenders = {}
  function plugin.on_bot_team_tick(team, bots)
      defenders[team] = {}
      for i = 1, math.min(2, #bots) do defenders[team][bots[i].id] = true end
  end
  return plugin
  ```
]

== Not events

A table field of the returned table that is not a function named above is ignored by the server, with these exceptions that a *game mode* plugin reads (chapter 13): `mode`, `rules`, `wire`, `time_limit`, `squads` and `squad_spawn`. Names such as `on_init` that are misspelt are silently never called: check them in the log with `game.log`.
