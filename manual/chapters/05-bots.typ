#import "/template.typ": *

== What a bot is

Bots are players that live on the server. They join, spawn, shoot, dig and build through the same paths as a client, and the server validates every action the same way: a wall costs real blocks, digs respect the spade's delays, a hit needs a real ray. A bot has no network connection and appears on the scoreboard like anyone, with the name `Bot_` plus two words (`Bot_SwiftFox`).

#note[
  Bots only play while at least one human is in the match (waiting to respawn, respawning or ready). With nobody in, they exist and stand still, and the log says `Bots paused`. `SP_BOTS_PLAY=1` lets them play alone, for tests and benchmarks.
]

This chapter is what an owner needs. How the bots decide in detail (senses, fighting, routes, roles) is in #link("https://github.com/ngspades/server/blob/main/docs/bots.md")[docs/bots.md]; how a plugin or a Lua game mode steers them is in #ch("13-api-modes-bots")[Game modes and bots in Lua].

== Adding bots

#fields((
  ("--bots <n>", "option", "Start with n balanced bots."),
  ("--team-bots <a> <b>", "option", "Start with a bots on team 1 and b on team 2. The shipped maps have defaults of 10 a team where the bases are close and 20 on the large ones."),
  ("--bots-test", "option", "One strong enemy bot on the human's own spawn: the fastest way to test combat."),
  ("/addbot [n] [blue|green]", "command", "Adds bots to a team (or balanced). Role: everyone, unless the server restricts it."),
  ("/autofill <n>", "command", "Keeps n players in the match: as humans join, bots leave, and the reverse. A count alone does not switch it on in the host's menu until its box is ticked."),
  ("/botfill <n>", "command", "Tops both teams up to n players each."),
  ("/bots", "command", "Shows bot, human and autofill counts."),
  ("/kickbot <id>, /kickbots", "command", "Removes one bot by id, or all of them."),
  ("host menu", "client", "A player hosting a game from ngspades-client changes team counts, difficulty and autofill from the pause menu (\"Bots\"). The contract is in `docs/host-commands.md`."),
))

Autofill (and "keep the match full") trims bots when humans arrive; it frees them only at a human's spawn, never inside the loops that walk the player list.

== How well they play

```toml
[bots]
difficulty = "normal"      # easy, normal, hard
```

#wide-table(
  (auto, auto, 1fr),
  table.header([*Level*], [*Skill range*], [*Feel*]),
  [`easy`], [0.05 - 0.35], [Rookies. Aim error x1.5: barely hit beyond 10 blocks.],
  [`normal`], [0.15 - 0.90], [Poor shots: about 55% of hits at 10 blocks, 25% at 20, 10% at 30.],
  [`hard`], [0.60 - 0.95], [Good players, still no aimbots. Aim error x0.6.],
)

Each bot rolls its own skill in the range when it spawns; skill drives aim error, reaction time (0.45 s for the weakest, 0.15 s for the best), dodge rhythm and how many shots are wasted. `SP_BOT_DIFFICULTY` and a preset's `bot_difficulty` override the config; the host's menu changes it during a match. The `--bots-test` bot keeps a fixed skill of 0.9.

== What they do

- *See and hear like a player.* A new enemy must be inside a 150 degree cone or within 6 blocks, and in sight up to the fog's end (`[server] fog_distance`); night and weather shorten it as they do for a player; bots turn their flashlights on in the dark. They hear spades (30 blocks) and steps (14 walking, 24 running).
- *Fight.* They stop, aim and shoot at an enemy in their gun's reach, seek cover before building a wall, throw grenades when the numbers promise damage, use the spade up close, aim down the sights past 12 blocks, and never shoot at a wall between them and the target.
- *Move.* They plan routes on their own thread against a copy of the map: walking, swimming, jumping, digging, bridging, building pillars. They avoid water that kills and edges that hurt.
- *Build together.* Two bots share bridges; up to four raise fortifications (a ring, a bunker, towers, a barricade) where the mode asks for them. `[bots] cooperative_bridges` and `cooperative_forts` switch these off; `build_chance`, `help_chance` and `build_cooldown` tune how eagerly they build.
- *Fly.* Rocketeer bots with a jetpack fly onto roofs and to goals up to 20 blocks away; on 1024 maps, bots board helicopters (`heli_enabled`, `heli_chance`).
- *Play the objective.* In CTF they take roles by their place in the team: a defender at the intel, an infiltrator who digs a hidden way, its cover, a camper, the rest attack. In KotH they raise a tower on the hill and hold posts round it; in territory control they take the nearest territory their team does not hold; in the zombie mode the survivors hold high ground and forts and the zombies hunt in hordes. Every mode's bots are described in `docs/bots.md`.

== Bots and performance

The planner runs on its own thread and never on the tick. Measured on an Apple M4 with 127 bots on a large map: 60 ticks a second, 0.6-0.7 ms a tick on average, 5-8% of one core, about 76 MB of memory. A think over 5 ms logs `bot_slow` with what it was doing, and a tick over 20 ms logs `Slow tick:` with the parts. `SP_BOT_*` variables (see the environment table) switch single behaviours off for a measurement.

== Bots in the logs

With `SP_EVENTLOG=1`, bots log as `bot3:Bot_SwiftFox` actors: `bot_goal`, `bot_nav action=plan|lost|blocked`, `bot_shot result=miss|head|torso|legs`, `bot_cover`, `bot_wall`, `bot_fort`, `bot_perch`, `bot_jet`... A bot that wanders off the map's edge stops at it; a bot in the water swims to a shore it can climb. A server that reports `bot_path action=stuck` often has a map the bots cannot read; report it with the map's name.
