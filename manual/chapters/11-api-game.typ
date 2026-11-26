#import "/template.typ": *

The global table `game` holds every function the server gives a plugin. Conventions used by all entries:

- *Coordinates* are map coordinates. `x` and `y` run along the ground; *`z` grows downwards*, `z = 0` is the top of the world and the water is the bottom layer (`z = 63` on a 64-high map). A player's `x, y, z` is its hip.
- A *team* argument is `1` or `2` (blue, green by default); anything else raises `a team is 1 or 2`. Teams in player tables are `1`, `2`, or `0` for spectators.
- Functions that can fail without being a mistake return `false` (or `nil`) and a reason as a second value. Mistakes in arguments (a missing number, a table where a string belongs) raise a Lua error, which turns the plugin off for the map.
- Entries marked *queued* take effect at the end of the plugin tick. Entries marked *opt-in* need the owner's permission in the config (chapter 9). *Protocol 1.0 only* entries return `nil`/`false` and a reason on the 0.75 profile.

== Players and messages

#entry("game.players", "game.players() -> { player, ... }")[
  Every player in a team or spectating (humans and bots; disconnected ones are left out), as a list of player tables. The list is a snapshot.

  ```lua
  for _, p in ipairs(game.players()) do
      if not p.bot then game.log(p.name .. " is on team " .. p.team) end
  end
  ```
]

#entry("game.player", "game.player(id) -> player | nil")[
  The player with that id (0 to 255), or `nil`. Use it to read a player again later: tables are snapshots.

  ```lua
  game.later(5, function()
      local now = game.player(3)
      if now then now:message("Still here?") end
  end)
  ```
]

#entry("game.find", "game.find(name) -> player | nil")[
  The player with that name (case-insensitive, an exact match wins), or the only player whose name starts with it. `nil` when none matches or the prefix is ambiguous.

  ```lua
  local p = game.find("Bot_Dusty")
  if p then p:message("Found you") end
  ```
]

#entry("game.notice", "game.notice(text [, team])")[
  Sends a line to every connected player, or to one team (1 or 2). Bots do not read it. Also logs `EVT plugin action=notice`.

  ```lua
  game.notice("Match ends in one minute")
  game.notice("Blue team: defend the base", 1)
  ```
]

#entry("game.ban", "game.ban(p, minutes [, reason])")[
  Bans the player's address (an IPv6 player's /64) in the ban list that `/ban` writes (`Bans.json`), and disconnects everyone at that address. `p` is a player table; `minutes` 0 or less bans for good; `reason` is optional text. Raises `that player has left`, or `a bot cannot be banned` (bots share no address).

  ```lua
  local p = game.find("Griefer")
  if p and not p.bot then game.ban(p, 60, "griefing") end
  ```
]

== The match: scores, limits, maps

#entry("game.score", "game.score(team) -> number")[
  The team's score (captures, kills, rounds... what the mode counts).

  ```lua
  game.notice("Score " .. game.score(1) .. " - " .. game.score(2))
  ```
]

#entry("game.set_score", "game.set_score(team, n)")[
  Sets the team's score (0 to 255; the value is stored in one byte). The clients are told on the next state update.

  ```lua
  game.set_score(1, 0)
  game.set_score(2, 0)
  ```
]

#entry("game.score_limit", "game.score_limit() -> number")[
  The score at which the match ends.

  ```lua
  game.log("the limit is " .. game.score_limit())
  ```
]

#entry("game.set_score_limit", "game.set_score_limit(n) -> true | false, reason")[
  Sets the score the match ends at, 1 to 255 (otherwise `false, "the limit is 1..255"`). It also sets the capture limit. The map's toml or the config still decide the next map's.

  ```lua
  game.set_score_limit(5)
  ```
]

#entry("game.set_capture_limit", "game.set_capture_limit(n) -> true | false, reason")[
  The same as `game.set_score_limit`.

  ```lua
  game.set_capture_limit(3)
  ```
]

#entry("game.set_time_limit", "game.set_time_limit(seconds) -> true | false, reason")[
  Sets a time limit from now: when it runs out the match ends by the score (the higher team wins, equal is a draw). `0` takes it off. Seconds 0 to 86400. Works only for the *game mode plugin* (a plugin with `mode = true` being played): any other plugin gets `false, "no script mode is being played"`. `/extend` adds minutes to it.

  ```lua
  local plugin = {mode = true}
  function plugin.on_init() game.set_time_limit(600) end
  return plugin
  ```
]

#entry("game.time_left", "game.time_left() -> seconds | nil")[
  Seconds left of the mode's time limit, or `nil` when there is none (also for any match that is not played by a Lua mode).

  ```lua
  local left = game.time_left()
  if left and left < 60 then game.notice("Last minute!") end
  ```
]

#entry("game.win", "game.win(team)", queued: true)[
  That team wins: the match is over and the next map starts (on 1.0, after the match summary). Queued; at most one reset every 5 seconds is accepted.

  ```lua
  if game.score(1) >= 10 then game.win(1) end
  ```
]

#entry("game.end_match", "game.end_match(team)", queued: true)[
  The same as `game.win`, named for what it does.

  ```lua
  game.end_match(2)
  ```
]

#entry("game.next_map", "game.next_map() -> true | false, reason", queued: true, optin: "allow_map_change")[
  Loads the next map of the rotation (no forced map, same mode). `false, "[plugins] allow_map_change is false"` unless the owner allowed it. On the 0.75 profile it is refused when the running mode is not one the profile has.

  ```lua
  local ok, why = game.next_map()
  if not ok then game.log("no map change: " .. why) end
  ```
]

#entry("game.set_map", "game.set_map(name) -> true | false, reason", queued: true, optin: "allow_map_change")[
  The next load uses this map (a folder name, aliases accepted by the server's resolver; `false, "map not found (or not a safe map name)"` when it is not installed or is a path). When a `game.set_mode` is pending, the map must suit that mode. The change happens at the end of the plugin tick.

  ```lua
  game.set_map("Busan_Port")
  ```
]

#entry("game.set_mode", "game.set_mode(name) -> true | false, reason", queued: true, optin: "allow_map_change")[
  The next load starts this game mode, on the current map unless `game.set_map` follows. `false` with `"unknown game mode"`, or `"that mode does not suit the map"`. On the 0.75 profile a mode other than `ctf` or `babel` is refused with the profile reason.

  ```lua
  game.set_mode("tdm")
  ```
]

#entry("game.run_command", "game.run_command(text) -> true | false, reason", queued: true, optin: "allow_commands")[
  Runs one console command (with all permissions) at the end of the plugin tick. The command's name must be in `[plugins] allow_commands` (the first word, no slash). Always refused: `/shutdown`, `/login`, `/logout`, a line with a newline, `;`, `|`, `&`, a backtick or `$(`, a line over 1000 characters. Only one command can wait at a time (`another plugin command is already waiting`). The command may be a plugin's own (`game.command`).

  ```lua
  -- config.toml: allow_commands = ["say"]
  game.run_command("/say The server is restarting soon")
  ```
]

#entry("game.team_name", "game.team_name(team, name) -> nil | false, reason")[
  Renames a team (10 characters at most: longer gives `false, "the name is too long"`). Clients are told by the next state update.

  ```lua
  game.team_name(1, "Reds")
  game.team_name(2, "Blues")
  ```
]

#entry("game.team_color", "game.team_color(team, r, g, b) -> nil | false, reason")[
  Sets a team's colour, each channel 0 to 255 (otherwise `false, "a colour is 0..255"`).

  ```lua
  game.team_color(1, 220, 40, 40)
  ```
]

#entry("game.set_team_outfit", "game.set_team_outfit(team, n) -> true | false, reason", profile: "1.0")[
  Every player of the team whose outfit is its team's wears outfit `n` (0 to 254; 0 soldier, 7 miner, 64 zombie). `false` with `"team: 1 or 2, outfit: 0..254"` when out of range.

  ```lua
  game.set_team_outfit(2, 64)   -- team 2 looks like zombies
  ```
]

== The world: sky, fog, blocks, ground

#entry("game.fog", "game.fog(r, g, b)")[
  Sets everyone's sky colour (each channel taken modulo 256), also for those who join later.

  ```lua
  game.fog(40, 40, 60)
  ```
]

#entry("game.get_fog", "game.get_fog() -> r, g, b")[
  The current fog colour.

  ```lua
  local r, g, b = game.get_fog()
  game.fog(r // 2, g // 2, b // 2)
  ```
]

#entry("game.sky", "game.sky() -> { time, rate, weather, intensity, moonlight } | nil, reason", profile: "1.0")[
  The time of day and the weather. See the table below for the fields. On the 0.75 profile: `nil` and the profile reason.

  ```lua
  local s = game.sky()
  if s and s.weather == "rain" then game.notice("Mind the rain") end
  ```
]

#entry("game.set_sky", "game.set_sky{ time, rate, weather, intensity, moonlight } -> true | false, reason", profile: "1.0")[
  Changes any of the sky's settings (keys you leave out keep their value). `time` is minutes since midnight or a string: `"hh:mm"`, `"day"`, `"noon"`, `"night"`, `"midnight"`, `"dawn"`, `"dusk"`; `weather` is `clear`, `rain`, `snow`, `fog` or `storm`; `intensity` and `moonlight` 0 to 100; `rate` 0 to 1440 minutes of the day per second. On a bad value: `false` and the reason (`"weather: clear, rain, snow, fog or storm"`...), nothing is changed.

  ```lua
  game.set_sky{time = "dusk", weather = "rain", intensity = 40}
  ```
]

#entry("game.cell", "game.cell(x, y, z) -> boolean")[
  `true` when that cell is a solid block; `false` for air and for cells outside the map. The bottom layer (the water layer) counts as solid in the map data.

  ```lua
  if game.cell(100, 100, 40) then game.log("solid") end
  ```
]

#entry("game.color", "game.color(x, y, z) -> r, g, b | nothing")[
  The block's colour, or nothing when the cell is air or off the map.

  ```lua
  local r, g, b = game.color(100, 100, 45)
  if r then game.log(("%d,%d,%d"):format(r, g, b)) end
  ```
]

#entry("game.set_block", "game.set_block(x, y, z, r, g, b) -> true | false, why")[
  Puts a block of that colour into the map for everyone, like a player's build without spending blocks. `false` and why: `"outside"` (off the map or at `z` 62 or more: the water layers), `"protected"` (a cell the mode or the map keeps: arena walls, a protected base) or `"rate"` (the plugin changed 4096 cells this tick: try the rest on the next `on_tick` or with `game.later`). Setting a cell that is solid already is allowed.

  ```lua
  local x, y = game.spawn(1)
  local z = game.ground(math.floor(x), math.floor(y)) - 1
  game.set_block(math.floor(x), math.floor(y), z, 255, 0, 0)
  ```
]

#entry("game.clear_block", "game.clear_block(x, y, z) -> true | false, why")[
  Removes a block like a spade; what then hangs on nothing falls. Same failure reasons as `game.set_block`; a cleared cell counts 4 against the 4096 per-tick budget (the check for what floats is the cost). Clearing air is allowed and free.

  ```lua
  for z = 30, 40 do game.clear_block(256, 256, z) end
  ```
]

#entry("game.map_size", "game.map_size() -> x, y, z")[
  The map's size: 512, 512, 64 (or 1024, 1024, 64).

  ```lua
  local sx, sy, sz = game.map_size()
  game.log("map " .. sx .. "x" .. sy .. "x" .. sz)
  ```
]

#entry("game.ground", "game.ground(x, y) -> z")[
  The column's top block, its *smallest* solid `z` (the ground's height; the water layer's `size_z - 1` for open water). Raises `bad argument ... off the map` outside the map.

  ```lua
  local z = game.ground(256, 256)
  game.log("ground at z=" .. z)
  ```
]

#entry("game.spawn", "game.spawn(team) -> x, y")[
  The middle of the team's spawn area.

  ```lua
  local x, y = game.spawn(1)
  game.log(("team 1 spawns near %.0f,%.0f"):format(x, y))
  ```
]

#entry("game.drop_crate", "game.drop_crate(x, y, kind) -> true | false, reason", profile: "1.0")[
  A supply crate (`"ammo"` or `"health"`) falls from the sky onto the nearest dry ground to the column `x, y`. The first player at it who needs it takes it (ammo is the tent's refill, health goes to 100); it goes after 2 minutes. `false` with `"too many crates out, or no ground there"` (16 at most). A `kind` other than the two raises an error.

  ```lua
  local x, y = game.spawn(1)
  game.drop_crate(x, y, "ammo")
  ```
]

== Time and logging

#entry("game.time", "game.time() -> seconds")[
  The game's clock in seconds (a float). Only differences mean something: use it to time things. A test may run it faster than real time.

  ```lua
  local start = game.time()
  game.later(5, function() game.log("elapsed " .. (game.time() - start)) end)
  ```
]

#entry("game.unix_time", "game.unix_time() -> seconds")[
  The machine's clock: whole seconds since 1970. A test may speed the game clock up, never this one.

  ```lua
  game.store.last_start = game.unix_time()
  ```
]

#entry("game.later", "game.later(seconds, fn)")[
  Calls `fn()` that many seconds of the game from now, on a plugin tick (the resolution is a tenth of a second). A negative delay means now. A plugin may have 64 timers waiting; the next raises `too many timers (64)`. A timer that was set before the map changed is lost (the plugin is loaded again). An error in `fn` turns the plugin off.

  ```lua
  game.later(10, function() game.notice("Ten seconds passed") end)
  ```
]

#entry("game.log", "game.log(text)")[
  Writes `text` to the server's log (`Plugin <name>: <text>`) and to the event log (`EVT plugin action=log`). The way a plugin says what it did.

  ```lua
  game.log("loaded, " .. #game.players() .. " players")
  ```
]

== Data: JSON, URLs, messages, storage

#entry("game.json_encode", "game.json_encode(value) -> string")[
  A string of JSON. A table whose keys are exactly `1..n` is an array, any other table an object (keys become strings); `nil` encodes as `null`. Integers stay integers. A function, a thread or a userdata raises `json: a function cannot be encoded`; nesting past 32 levels raises `json: nested too deep`. Note that an empty table encodes as an object, `{}`.

  ```lua
  local text = game.json_encode({name = "x", scores = {1, 2, 3}})
  game.log(text)   -- {"name":"x","scores":[1,2,3]}
  ```
]

#entry("game.json_decode", "game.json_decode(text) -> value | nil, error")[
  The value, with JSON arrays as Lua lists and objects as tables; or `nil` and the parser's error text for malformed input. Depth is limited to 32.

  ```lua
  local v, err = game.json_decode('{"a":[1,2,{"b":true}]}')
  if v then game.log(tostring(v.a[3].b)) else game.log(err) end
  ```
]

#entry("game.url_encode", "game.url_encode(text) -> string")[
  Percent-encodes everything but letters, digits and `-_.~`, for a query string. Bytes of UTF-8 are encoded one by one.

  ```lua
  local url = "https://api.example.com/q?name=" .. game.url_encode("a b&c")
  ```
]

#entry("game.send", "game.send(topic, data)")[
  Queues a message for every *other* plugin's `on_message(from, topic, data)` on the next plugin tick. `data` is plain data: it crosses as JSON, so tables, strings, numbers and booleans, not functions or players. The queue holds 256 messages; more raises `too many messages waiting`. A message sent from `on_message` is delivered on the following tick, so a reply cannot recurse.

  ```lua
  game.send("announce", {text = "A flag was captured"})
  ```
]

#entry("game.store", "game.store  (a table)")[
  A table of the plugin's own, kept across maps and server restarts. It is read when the plugin loads, before its file runs, and written when the map ends, when the server stops, and on the next tick after `game.store_save()`. On disk it is `Resources/plugins/data/<name>.json` (`plugins-data/<name>.json` in the data folder when `--data` is used), one file a plugin; the owner may read or edit it while the server is off. A file that is not a JSON object is moved to `<name>.json.bad` and the store starts empty.

  It holds what JSON holds. A function or a player in it stops the save (`EVT plugin action=store_refused why=encode`); keep names or ids, not player tables. At most 1 MB (`why=size`). Assigning `game.store = {}` replaces it all.

  ```lua
  game.store.visits = (game.store.visits or 0) + 1
  game.log("visit number " .. game.store.visits)
  ```
]

#entry("game.store_save", "game.store_save()", queued: true)[
  Asks for `game.store` to be written on the next plugin tick. The map's end and the server's shutdown write it anyway; call this after a change that must survive a crash.

  ```lua
  game.store.bans = (game.store.bans or 0) + 1
  game.store_save()
  ```
]

#entry("game.server_info", "game.server_info() -> table")[
  A table about the server; its fields are listed below.

  ```lua
  local i = game.server_info()
  game.log(i.name .. " plays " .. i.mode .. " on " .. i.map)
  ```
]

== Commands

#entry("game.command", "game.command(name [, options], fn) -> true | false")[
  Adds a chat command typed with `/`, like the server's own. Call it when the file runs or in `on_init`; it is registered again at every map.

  - `name`: lowercase letters, digits, `_` and `-`, 1 to 30 characters, with or without the `/`. (Players may type it in any case.)
  - `options.role`: who may use it: that role or a higher one (`"trusted"`, `"guard"`, `"mod"`, `"admin"`, `"manager"`); none means everyone. The server checks it: `fn` is never called for anyone else (they read "You do not have permissions to use this command"). The console may use any command.
  - `options.help`: its line in `/help`, shown only to those who may use it.
  - `fn(p, args, line)`: `p` is the player, or `nil` from the console; `args` the words after the name (`/hello a b` gives `{"a", "b"}`); `line` the rest as typed (`"a b"`). A string it returns is sent back to whoever typed it.

  The server's commands come first: a name the server or another plugin already has gives `false` (`EVT plugin action=command_refused why=taken`); `true` when added. At most 16 a plugin; more raises an error. Plugin commands run from the game's chat, the server's TTY console and `game.run_command` (when allowed), *not* from the remote console, which answers `no such command` for them.

  ```lua
  game.command("hello", {help = "Says hello"}, function(p, args, line)
      return "hello " .. (p and p.name or "console")
  end)

  game.command("wipe", {role = "admin", help = "Clears the scores"}, function()
      game.set_score(1, 0)
      game.set_score(2, 0)
  end)
  ```
]

== The network

#entry("game.http", "game.http{ url, method, headers, body, json, timeout, on_done } -> true | nil, reason")[
  Starts an HTTP request on the server's own network thread; the game never waits for it. `on_done(status, body, err)` is called on a plugin tick after the answer arrives (`status` 0 and `err` set when it failed; `body` a string or `nil`). An answer that arrives after the map changed is dropped.

  The call returns `true`, or `nil` and why it was refused: `"the url must be http:// or https://"`, `"host not in this plugin's allow_hosts (config.toml)"` or `"too many requests waiting"`. Rules: only hosts the owner lists in `[plugins.<name>] allow_hosts`; http and https only; *no redirects* (one could lead to a host not listed); answers up to 1 MB; `timeout` 1 to 30 seconds (default 10); 16 requests waiting per plugin, 64 in all. `json` sends the value as the body with `Content-Type: application/json`; `body` a string. A header name with a colon or a line break, or a value with a line break, is dropped.

  ```lua
  -- config.toml: [plugins.my_plugin] allow_hosts = ["api.example.com"]
  local ok, err = game.http{
      url = "https://api.example.com/scores",
      method = "POST",
      headers = {["Authorization"] = "Bearer " .. (config.token or "")},
      json = {player = "x", kills = 3},
      timeout = 10,
      on_done = function(status, body, err)
          game.log("answered " .. status)
      end,
  }
  if not ok then game.log("refused: " .. err) end
  ```
]

== Classes

The next gen modes' classes. *Protocol 1.0 only* (all three return `nil`/`false` and the reason on 0.75).

#entry("game.classes", "game.classes() -> { class, ... }", profile: "1.0")[
  Every class's figures as a list, in order: Commando, Rocketeer, Marksman, Miner. The fields of a class table are listed below.

  ```lua
  for _, c in ipairs(game.classes()) do game.log(c.name .. " health " .. c.health) end
  ```
]

#entry("game.class", "game.class(name_or_n) -> class | nil", profile: "1.0")[
  One class's figures, by name (`"miner"`) or number (0 to 3), or `nil`.

  ```lua
  local miner = game.class("miner")
  if miner then game.log("miner digs " .. miner.dig) end
  ```
]

#entry("game.set_class", "game.set_class(name_or_n, figures) -> true | false, key", profile: "1.0")[
  Changes the class's figures *for this map* (every map starts from `config.toml` again) and tells every client. `figures` is a table of the keys in the class table below. Returns `true`, or `false` and the first key refused (unknown, out of its range, or the attempt to turn the last enabled class off); the keys before it were applied. An unknown class gives `false, "class"`.

  ```lua
  function plugin.on_init()
      game.set_class("miner", {health = 160, speed = 0.9})
      game.set_class("rocketeer", {enabled = false})
  end
  ```
]

== The tables these functions return

#fields((
  ("sky.time", "number", "Minutes since midnight (0 to 1440; 720 is noon)."),
  ("sky.rate", "number", "Minutes of the day per second of the game's clock (0: the time stands still)."),
  ("sky.weather", "string", "`clear`, `rain`, `snow`, `fog` or `storm`."),
  ("sky.intensity", "integer", "How much of the weather, 0 to 100."),
  ("sky.moonlight", "integer", "How much light a night shows, 0 (only flashlights) to 100."),
  ("set_sky.time", "number or string", "Minutes since midnight, `\"hh:mm\"`, or one of `day noon night midnight dawn dusk`."),
  ("set_sky.rate", "number", "0 to 1440."),
  ("set_sky.weather", "string", "`clear`, `rain`, `snow`, `fog`, `storm`."),
  ("set_sky.intensity", "integer", "0 to 100."),
  ("set_sky.moonlight", "integer", "0 to 100."),
  ("info.name", "string", "The server's name."),
  ("info.map", "string", "The current map's folder name."),
  ("info.mode", "string", "The game mode's name (`ctf`, `ngctf`, a Lua mode's name...)."),
  ("info.players", "integer", "Players in the game (bots included)."),
  ("info.max_players", "integer", "The cap."),
  ("info.port", "integer", "The game's UDP port."),
))

The fields of a *class table* (from `game.classes` and `game.class`; the keys that `game.set_class` accepts), with the ranges the server enforces. A class table also has `name` and `n`.

#fields((
  ("class.name", "string", "`commando`, `rocketeer`, `marksman` or `miner`. Read only."),
  ("class.n", "integer", "Its number, 0 to 3. Read only."),
  ("class.health", "number", "1 to 1000. What it effectively has: hp stays 0..100 and every hit is scaled by 100 / health."),
  ("class.speed", "number", "0.3 to 3, times the usual ground speed."),
  ("class.jump", "number", "0.1 to 1.5, the jump's upward speed (0.36 is the usual)."),
  ("class.weapon", "string", "`rifle`, `smg`, `shotgun`, `rocket` or `revolver`."),
  ("class.blocks", "number", "0 to 65535 a life starts with."),
  ("class.gear", "string", "`grenades` or `drill`."),
  ("class.gear_count", "number", "0 to 255."),
  ("class.jetpack", "boolean", "Worn from every spawn."),
  ("class.dig", "number", "1 (the spade's usual dig) or 3 (3x3x3)."),
  ("class.perk", "string", "`none`, `sentry`, `landmine`, `dynamite` or `mg`."),
  ("class.swing", "number", "0.05 to 2 seconds between spade swings."),
  ("class.title", "string", "Its name on the class screen (31 bytes)."),
  ("class.blurb", "string", "Its line on the class screen (95 bytes)."),
  ("class.enabled", "boolean", "`false`: nobody may pick it (the last one on stays on)."),
  ("class.secondary", "string", "`none` or a weapon swapped with the first."),
  ("class.block_damage", "number", "0 to 100: damage of a pellet to a block of 100 (0: the gun's own)."),
  ("class.spawn_perk", "boolean", "Every life starts with one charge of its perk."),
  ("class.dig_delay", "number", "0.2 to 3 seconds between right-button digs."),
  ("class.sprint", "number", "0 to 3 (0 keeps the walk speed)."),
  ("class.water_friction", "number", "1 to 30."),
  ("class.headshot", "number", "0.1 to 4: what a head shot does."),
  ("class.melee", "number", "0 to 500: the swing's damage."),
  ("class.fall_min", "number", "0 to 63 blocks: the fall that starts to hurt."),
  ("class.fall_max", "number", "0 to 63 blocks: where `fall_dmg` is reached."),
  ("class.fall_dmg", "number", "1 to 200."),
  ("class.fall_water", "number", "0 to 1: the share a water landing takes."),
  ("class.blocks_max", "number", "0 to 65535: what the wallet holds."),
))

The keys of the `game.http` argument table:

#fields((
  ("http.url", "string", "Required. `http://` or `https://`, its host in `allow_hosts`."),
  ("http.method", "string", "`GET` by default."),
  ("http.headers", "table", "Name to value, both strings."),
  ("http.body", "string", "The request body."),
  ("http.json", "any", "A value sent as JSON; wins over `body`."),
  ("http.timeout", "integer", "1 to 30 seconds, 10 by default."),
  ("http.on_done", "function", "`on_done(status, body, err)`; optional."),
))
