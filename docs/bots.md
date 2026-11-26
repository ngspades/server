# Bots

How the bots of ngspades play. This is what a player meets in a
match and what a server owner can count on. CONTRIBUTING.md holds the code-level
notes: the files, the log lines and the bugs already fixed.

Bots are server-side players. They join, spawn, shoot, dig and build through
the same paths as a client, and the server validates every action the same
way: walls cost real blocks, digs respect the spade's delays, a hit needs a
real ray. They only play while a human is in the match.

Every bot is named `Bot_` plus a generated name (`Bot_SwiftFox`). The words
are the ones the client uses to name players (`common/names/names.h`).

## Seeing and hearing

- **The view cone.** A bot notices a new enemy only when it is inside a 150
  degree cone in front of it, or within 6 blocks, and in sight, up to 128
  blocks (the fog's end, as for a player). In sight means what a player
  sees: a line from its eye to the enemy's head, chest or legs, every block
  of the way checked (it used to see through walls past about 20 blocks). It
  does not see behind itself.
- **Night and weather.** The sky the server sets (`/sky`, `[sky]` in
  config.toml) limits sight as it does a player's: in full darkness an
  unlit enemy is seen within 24 blocks, plus a little more for the
  moonlight (39 at the default 25%), the twilight in between; rain, snow
  and fog cut that further. An enemy with its flashlight on, or firing, is
  seen as far as by day. Bots switch their own flashlights on in the
  dark (and off by day), so a survivor bot shows itself; a zombie has
  none and sees in the dark as by day (night vision). Measured, 16 bots, 400 s, 3 seeds, night against
  day: 2Fort kills 130 to 119 and intel pickups 1.7 to 5.3, Raindance kills
  97 to 91 and pickups 4 to 5.3 (the attackers get there unseen).
- **Getting shot.** The bot that was hit reacts, and so does every bot of its
  team within 3 blocks of it. After a reaction time (0.45 s for the weakest
  bot, 0.15 s for the best), the shooter becomes its target if it can be
  seen. If it cannot be seen, the bot ducks behind something that hides it
  from the shot, then stands up aiming where the shot came from. The intel
  carrier ignores this and keeps running.
- **Hearing a spade.** Every block an enemy opens with the spade is heard by
  the bots within 30 blocks, for 10 s. A digger they heard is looked for
  outside the view cone too.
- **Hearing steps.** Every bot hears an enemy walking within 14 blocks or
  running within 24, on its feet (crouched or sneaking is silent), for 3 s.
  If it can see that enemy, it takes it on even behind it; if not, it turns
  its look to the sound while it walks on, so the enemy is in front of it
  when it shows.
- **Turning.** A bot turns like a player with a mouse, not in an instant:
  540 degrees a second on its way, and onto an enemy from 260 (the weakest)
  to 720 (the best). Its feet keep going where it was walking while it
  turns.

## Fighting

- **Skill.** Each bot rolls a skill when it spawns. The server's difficulty
  sets the range: easy, normal or hard. Skill drives aim error, reaction,
  dodge rhythm and how many shots are wasted. Bots miss honestly: a hit is
  only reported when the aimed ray crosses the target's body.
- **An enemy in sight is shot at.** Whatever its role or its way, a bot
  that sees an enemy within its gun's reach **stops, aims and shoots**:
  after its reaction time (0.45 s for the weakest, 0.15 s for the best) it
  turns onto the enemy and fires only once its look is really on it, and it
  stays on that enemy until it has been out of sight for a second; then it
  goes on its way. It only stops for an enemy within its gun's range and
  for at most 4 s in a row (then it walks on for 3 s); a farther one is
  shot at on the move. The intel carrier (within 30 blocks) and the infiltrator
  (within 35) only shoot back on the move. The aim error drifts every
  quarter second, like a hand, rather than jumping.
- **No shots into walls.** A bot fires only when the way to the enemy is
  clear, both along its aim and along the way its body looks (what the other
  players see the tracer follow). With part of the enemy hidden it aims at
  what shows (the head over a wall, the legs under a beam) and corrects an
  aim that lands on a window frame. A shot still held after a second makes
  it step a block or two aside to where the line is clear; with no such
  place it leaves that enemy alone for 2 s.
- **Gun range.** Each gun has a range it fights at: shotgun 25 blocks, SMG
  35, rifle 80. An enemy farther than that is walked towards, so an SMG bot
  closes in instead of standing far away, but a bot still takes the hard
  shot up to 1.6 times that range (rifle 128, about the fog), less often,
  and the spread makes most of those miss.
- **Grenades.** At an enemy that just ducked behind cover, or at two
  enemies standing close together, 8 to 30 blocks away, at most one every
  20 s. The throw is worked out on the game's own grenade physics,
  bounces included: the angle and the fuse that do the most damage where
  the target can see the blast, and never more than a little to the
  thrower. Weaker bots throw less precisely. The carrier does not throw.
- **Shotguns wait.** A bot with a shotgun (the defender, others) that sees
  an enemy farther than 22 blocks crouches behind what there is and keeps
  its gun on it without firing; once the enemy is closer it stands, fires
  every time it can and side-steps between the shots.
- **Reloading.** With nobody about and its clip half empty, a bot reloads,
  as a player does after a fight.
- **Blocks are for cover and short crossings.** The way to an objective
  never builds a stair or pillar block (the spade still digs one), but a gap
  or water in front can become a shared bridge (below). A wall goes up for
  cover in 15% of the fights with nothing to hide behind, and the posts
  build theirs.
- **Up close.** An enemy within arm's reach always gets the spade.
  Within 8 blocks in the open the bot stops, crouches and shoots (it stands
  only when a low wall would hide the enemy from a crouched eye); no
  zig-zag, no wall. An enemy that ducked
  behind a few blocks within 6 blocks gets that cover dug away with the
  spade, block by block; farther than that, a grenade.
- **Perks (CTF).** A bot that earned the sentry places it at its post, in a
  fight or near the enemy intel, a few blocks away on the enemy's side. With
  the jetpack it flies straight up to an objective three or more levels
  above it and close by, instead of building a pillar or walking round.
- **The gun in hand.** 1.5 s after its last block or dig a bot puts the gun
  back in hand. It runs on its way (shift) on flat ground; the infiltrator
  and its cover sneak. Along an edge or up a tower it walks slowly
  (sneaking, half speed) in short steps.
- **Cover.** A bot entering a fight looks within 4 blocks for something that
  is already there (a wall, a corner, a crate) and hides its crouched body
  from the enemy. It prefers a spot it can look over standing up. From there
  it dodges side to side, ducks, and stands up to shoot. Every 8 s it checks
  that the cover still hides it; if the enemy got around, it moves to another
  spot. Only when there is nothing to hide behind does it sometimes (15% of
  the time) raise its own two block wall: the block goes in the cell the
  enemy's line of sight crosses, right in front of it, standing on ground
  level with the bot (a step down in front would put the blocks under its
  chest, so it fights in the open instead). The bot holds the middle of its
  own cell behind it without stepping aside (a side step would put it round
  a one cell wall), ducking and standing to fire. Once the enemy is more than
  45 degrees off that wall it stops treating it as cover at once and fights
  in the open.
- **Close fights.** An enemy within 10 blocks is attacked in the open, with
  no cover sought: straight at it, shooting, side-stepping. This ends when
  the enemy is farther than 14 blocks.

## Getting there

- **Planning.** A bot plans its whole way to the objective over the cells a
  player can stand in. It weighs what each step costs a player: walking,
  swimming, jumping a step, dropping, a bridge block, a stair block, a pillar
  block, a spade dig. So it walks around when that is cheap, builds when it
  must, and digs through what is in the way.
- **Water.** A bot that falls in plans its way out toward its objective. It
  climbs out at a shore it can jump onto: from the water nothing can be built
  or dug.
- **Protected cells.** The plan never puts a block or digs where the map or
  the mode forbids it (Raindance's bases, Babel's platform, closed arena
  gates).
- **Along walls.** Every bot prefers a way along walls and under roofs to
  one across open ground (a covered way up to ~1.8 times as long wins), and
  runs across the open when there is no other way. The intel carrier takes
  the short way home.
- **Walking straight.** On its way a bot aims at the furthest point of its
  way it can walk to in a line, not at each next cell, and turns its head
  at most 200 degrees a second while walking; a sound it hears while walking
  does not turn its head (an enemy it heard is still fought the moment it
  shows). Bots used to swing left and right as they ran.
- **No turning round at a step.** Close to the next point of its way (a
  step up, a cell it ran past, the start of a new plan), a bot looks the
  way its path goes on and steps forward, back or sideways onto the cell,
  like a player lining up; in the air it keeps its heading. It used to face
  the cell itself, which made it spin right round past a cell and round
  again once on it. A step up in front and a drop into a hole are still
  faced and walked into straight, as the body needs.
- **Protected places.** Where the map forbids digging (Raindance's bases,
  its intel towers) a bot never spades: it takes the stairs, and when the
  way is closed it goes round. It used to stand at the foot of the intel
  tower spading its wall over and over.
- **Its team's blocks.** A bot never digs a block its own team just put,
  nor one whose builder is still by it (on the stair it is building, up
  its pillar): a mate's fresh wall, stair or pillar is an obstacle to walk
  round like a protected cell (an old wall on the way is dug as before), and
  a bot low on blocks does not harvest one either. Coming down its own
  pillar on purpose and Babel's stair breakers cutting the enemy's steps
  are the exceptions. Bots used to spade the stairs and pillars their
  mates were climbing, and the mate fell or started over.
- **Reloading.** A bot reloads an empty clip, and also between fights when
  its clip is under half; a shotgun waiting for a close fight reloads while
  it waits. Everyone hears and sees it: the reload sound plays on the bot.
- **Building up near the goal only.** A pillar or a stair block is only
  planned within 16 columns of where the bot is going, like a player builds
  up to what it wants to reach; farther away it uses the map's own stairs
  (Raindance's intel is up a spiral staircase inside its tower).
- **No fall in a fight.** Fighting (charging, zig-zag, side steps), a bot
  never steps off a drop of three blocks or more.
- **Babel.** The intel is on the slab at the top of the world. A bot walks
  to a column beside the slab on its own side (one column per bot), builds
  a pillar there up to the slab's level and steps onto it; with the intel
  it goes down a whole staircase when there is one, or else to a pillar
  (its own side first, one whose top is level with the slab), crouches
  onto it and digs it down one block at a time, then runs home. Bots on
  the slab with nothing to do there go down the same way.
- **Babel's staircases.** Each team builds a staircase two blocks wide from
  the ground up to the edge of the slab on its side (each step a block on a
  support block, ~40 to 50 steps; the lowest step rests on the ground, one
  block up, so it can be climbed). Until it is whole the team's crew builds
  it together (the guard stands at its foot, one of the wallers): each takes
  another of the cells that can be put now (missing, touching a block
  already there), lowest first, standing on the step below; out of blocks
  they restock at the tent. Nobody raises a pillar of its own while the
  staircase is coming. Built together it is whole 58 to 66 s after the bots
  start on Nine-Eye and 79 to 152 s on babelstairs (the wallers join once
  their wall is whole). Builders keep the terrain
  (they never spade, not even out of a jam: they had spaded the steps they
  had just put) and ignore enemies past 30 blocks while working. Once it is
  whole, one crew bot (from 4) is a breaker: it goes up our staircase
  onto the slab and from its edge spades the top steps of the enemy's; the
  rest climb ours to the intel. A climber on our staircase when the enemy
  cuts it waits on its step while the crew puts the top back. The whole
  team takes on first whoever is at the top of our staircase.
- **Babel's jobs.** A team does not put every bot on the staircase: by rank
  a quarter of its bots build the wall, 15% hunt and the rest crew the steps
  (a team of under 4 bots crews it all; the crew keeps at least two). A
  hunter goes out to kill: the nearest enemy on the ground it can walk to
  (never one under the slab or up the staircase past the sixth step, and it
  never digs), shooting on the move; with nobody there it patrols the ground
  in front of the enemy's staircase's foot, and it keeps at it once the
  staircase is whole. The wallers' last rank is the guard, at the foot of
  our staircase while it is not whole. A waller raises its team's wall, then
  joins the crew, and hunts with them once the staircase is whole; it comes
  back to put a missing cell back, and stands behind a window only while an
  enemy comes over the ground at the wall.
- **Babel's wall.** One per team, 20 columns between its staircase's foot
  and the enemy: 5 columns from the foot toward the enemy's tent, across the
  ground the enemy comes over, covering the foot's lanes and the tent's side.
  It used to stand 5 columns outward of the foot, behind the staircase, where
  it shielded nothing the enemy crossed. Three blocks high from the ground it
  stands on, with every third column's top block left out as a window at
  shooting height, facing the enemy. The wallers share the missing cells,
  stand behind the wall beside a step's column (or beside that, where a
  staircase step hangs over it), restock at the tent when out of blocks, and
  put back a cell that was shot or dug out. Built it is whole 26 to 70 s after
  the bots start.
- **Stuck.** When the plan fails or a step will not go through, the bot plans
  again and avoids that cell for a while. As a last resort it walks straight
  at the goal and clears the way.
- **Keeping out of each other.** Players walk through one another in Ace of
  Spades, so nothing else keeps two bots out of the same cell (a whole arena
  team used to stand on its one spawn cell). A bot with another player within
  a block and a bit turns its step away from it, stops rather than walk into
  one under a block, and a bot standing still takes a short step off - only
  onto ground at its own level, never in the air, in the water, on the way up
  a pillar or holding its post. Two bots still pass each other a block apart,
  as players do; what they no longer do is aim from inside each other.

## Shared bridge projects

A bot that meets a gap or water in front while clearing its way can start a
short shared bridge (`Server/BotProjects.c`, from BattleSpades' bounded
project layer). It reserves up to six cells from the near bank toward the far
one and looks for a partner within 16 blocks. The owner builds the near
cells, the partner the far ones, alternating from the bank outward, so two
bots never spend blocks on the same hole. Every placement still goes through
the ordinary block action (reach, support, rate and the player's wallet).

- The partner crosses to the landing once the line is solid; then both
  return to their mode goal. The ordinary planner sees the new blocks.
- A partner that dies or stays away for 8 s is dropped and the owner builds
  the rest; a dead owner hands the project to a living partner. A project
  expires after 30 s.
- `[bots] cooperative_bridges = true` (default; `SP_BOT_PROJECTS=0` for
  tests). `EVT bot_project action=create|cell|built|expire|takeover`.
- Bounded on purpose: short bridges only. The cooperative cover is the
  fortifications below; the full BattleSpades layer also has breach tunnels
  and single-bot sniper outposts, which are still open.
- **Licence:** BattleSpades is AGPL-3.0, which cannot be merged into this
  GPL-3.0 tree. This is an independent, much smaller implementation of the
  same behaviour (read from its design notes, not copied code); do not paste
  its source here.

## Cooperative fortifications

Where an objective is held by a group, the bots raise a structure on it
together (`Server/BotForts.c`). The shapes come from the same place as the
bridges: BattleSpades' schematic library and its prefabs (`bunker`,
`watchtower`, `objective_ring`, `fort_wall`), read as ideas and reimplemented
with this server's own cells and its ordinary block action. Every cell is
validated by the server: support, reach, the block rate, protected cells and
the water level are the same rules a player meets.

- **Ring** (22 cells on flat ground, more where a column has to be skipped):
  a one-cell-high circle of radius four with a **three-cell gate on each
  side** of the threat line, and a battlement post on each diagonal. It is
  the first thing a team raises at a territory or on a hill: cover a bot can
  shoot over and walk into through a gate.
- **Bunker** (19 cells, 3 x 3): a one-high firing wall at the front, two-high
  sides, a roof over the back two rows and a two-high door at the back. Bots
  inside fire over the front and are covered from above; the door keeps the
  walkable exit the server wants.
- **Tower** (17 cells, 2 x 2 and three high): a solid lookout with a parapet
  on the threat side and a one-block stair up one side. The stair needs three
  level columns; the builder tries the back first, then the sides, so a
  tower is only raised where its top can be reached. A two-by-two footprint
  three high was chosen after a three-by-three one was lost half built on a
  contested hill.
- The shapes follow a **plinth**: a bunker or a tower on a slope of up to two
  cells gets foundation cells under its lower columns; a ring instead puts
  each of its cells on its own column's ground. A slope steeper than that is
  no site for a bunker or a tower, and the mode is told no.
- Cells are built **bottom up and from outside**: a cell is claimed only
  when it has a solid neighbour now and a bot can stand where it reaches it
  without being inside the structure. A stand inside a ring or a bunker is
  the last choice (it has to be walked into through a gate); the parapet and
  the roof become claimable only once the wall below them exists. A builder
  places as soon as the server's four-block reach is met, not only on the
  exact stand: a bot that stopped at the stand's edge never placed and read
  as stuck.
- **Who builds:** the mode asks for a structure and the nearest up to four
  living bots of that team with blocks are assigned, within 26 blocks of the
  site. A dead builder's slot is refilled by a mate respawning near it. A
  builder keeps working while no enemy is within 12 blocks and it is not
  being shot at; closer than that it fights, then goes back to the work. It
  still uses its gun, so a fort goes up under fire.
- A structure is **complete** when every cell is solid; it is then kept (the
  mode can put the rank 0 bot on a tower's top, `bot_fort_top`) and repaired
  if it is shot open while its builders are near. An unfinished one expires
  after 90 s (renewed by every cell placed). At most 16 structures live; the
  oldest complete one gives its place up when a new one is asked for. The
  sites are shared: one structure of each kind a site, whatever team raised
  it, and a territory that changes hands drops the old owner's structures
  (`bot_fort_discard`), whose cells stay standing.
- The structures are the **team's colour**, so players can find their
  team's fort; the quick walls a post raises stay the ground's colour, as
  camouflage.
- `[bots] cooperative_forts = true` (default; `SP_BOT_FORTS=0` for tests).
  `EVT bot_fort action=create|assign|place|complete|damaged|expire|discard|
  status|settle|drop_cell|overlook|overlook_none|refused why=shape|water|
  protected|overlap|unreachable|no_builders|declined` (an expiry lists the
  first cells left in the structure's frame, `missing=u/v/w`).
- **Where:** king of the hill raises a tower at the hill's middle from three
  bots of the team on it together, then, with the hill **held**, a ring from
  four. The ring 0 bot moves onto the tower's top once it is complete
  (`EVT bot_goal goal=hill-tower-top`); without a tower it keeps its old
  four-block pillar. Territory control raises a ring at a territory from
  three bots of the team there, and the **holder** then roofs the middle
  with a bunker. The ring changes the hold places: on king of the hill and
  territory control the bots keep inside the ring or outside it, never on
  its wall (a spot on the wall read as stuck at its foot).
- Measured, 12 bots, 600 s, three seeds, Raindance and hiesville: king of
  the hill 4.7 structures a match, 3.3 complete, the rank 0 on a tower 92
  goal checks, 4.67 rounds and 74.3 kills (73.7 before, within the spread);
  territory control 2.7 structures a match, 2.3 complete, 14.3 captures
  (11.7 before) and no stuck bots. The benchmark runs took the same 0.05 to
  0.09 ms a tick (a match's structures are a few hundred cells).
- **Watchtower** (34 cells on flat ground, more on a slope): a two by two
  core six high and a parapet on the threat side, so a bot on its top
  stands six blocks over the ground, its eye about eight. A stair of single
  steps winds round two sides of the core from the front corner to the back
  (the corner step stands on a support column, the others lean on the
  core), so it needs no level run of seven columns. Six and not seven: the
  server takes a block within five of the builder, so every core cell goes
  up from the ground (a builder's own reach check was 3.8 from the cell's
  corner and stopped one at a good stand for the fifth level); a seventh
  level needed a builder on the half built stair, and seven of seventeen
  towers were given up with only that level missing.
- **Barricade** (40 cells, seven wide, two floors): a two-high ground wall
  with a firing deck one level up behind it, merlons on the deck (the gaps
  between them its loopholes), a step up to the deck at each end and cheeks
  covering the deck's sides. The ground floor's bots shoot over the deck and
  the wall, the deck's over everything: four to six bots hold one front.
- **The site sees the objective.** A watchtower and a barricade the mode
  asks for at an objective (`bot_fort_overlook`) go on the highest ground,
  nearest the middle of a range round it, from which the eye of a bot on
  the finished structure sees a body standing on the objective
  (`physics_line_clear`, every cell). Only ground walkable from the
  objective counts (dry columns joined by one-block steps): the highest
  ground alone was a roof seven over the hill that no plan reached, and its
  builders walked partial plans until it expired. An objective with little
  walkable ground round it (a territory on a building) gets none
  (`overlook_none why=island`). The hilltop search (`bot_fort_hilltop`)
  keeps to the same walkable ground. A site whose plan is refused is not
  offered again for 60 s.
- **The watch.** A finished watchtower or barricade asked with its
  objective (`bot_fort_request_watch`) is manned: its top (one post) or its
  deck (two, either side of the middle merlon) goes to the nearest willing
  bot of the team within 40 for 45 s, which looks at the objective and
  fights from there like a bot perched on a roof (it does not leave the
  height to take cover), then rests. `EVT bot_fort action=watch|on_post|
  unwatch|post_fight`.
- **Bots do not build all the time.** Asked to start a structure, a bot
  builds only on a `[bots] build_chance` roll (35%); one that does not, and
  any free bot within 40 of an unfinished structure of its team, joins on a
  `help_chance` roll (70%), the same roll a watch takes. A roll holds 20 s
  (the slots are filled every tick: a roll a tick was a yes within a
  second). A builder that put its share (24 cells, while a mate is on it),
  or whose structure was finished or given up, and a watch that ended, rest:
  they fight `build_cooldown` (60) seconds before they are asked again. A
  carrier is never asked. `EVT bot_fort action=rules|decide why=start|help|
  watch|rest why=complete|share|watch|expire|settle|discard`, `refused
  why=declined asked=`. `SP_BOT_BUILD_CHANCE`, `SP_BOT_HELP_CHANCE`,
  `SP_BOT_BUILD_COOLDOWN`.
- **Kept and finished.** A team's bots never spade a cell of one of its
  structures (`bot_fort_cell_kept`, in `bots_team_built`): mates walking
  through dug the stair's first step once its 30 s had passed, and the tower
  above it could not be climbed. A cell whose claim ran out three times with
  its builder beside it leaves the plan (`drop_cell`; never a watchtower's
  top), and a structure nine tenths up when it expires is kept as it stands
  (`settle`) instead of being forgotten.
- **Where, after this change:** the CTFs raise at their intel a bunker, a
  watchtower 10 to 36 blocks off that sees it, a barricade 6 to 22 off, then
  the ring; a squad's lane waypoint gets a barricade on the nearest walkable
  hilltop (it got a bunker). King of the hill adds a watchtower 6 to 26 from
  the hill (inside the ring it counts as on the hill) and, held by four, a
  barricade 6 to 16 off facing the enemy. Territory control adds a
  watchtower 6 to 28 from a territory a team is in, and the holder's
  barricade 5 to 14 off. Each searches once every 15 s a team or territory.
- Measured, 16 bots, 500 s, three seeds, the build before against after
  (Busan_Port: koth, tc, ngctf; Normandie: ctf, koth): watchtowers 19
  created, 7 complete and 4 kept at nine tenths, the rest given up at the
  start (one builder pulled into fights); barricades 21 created, 20 finished;
  watches 76, 31 on their post. Structures started: koth 4.7 to 7.7, tc 6.3
  to 9.7, ctf 17 to 8, ngctf 17.7 to 5 (the chance), cells put about the
  same or more where the forts are new. Partial plans halved (koth Busan_Port
  1017 to 486, ngctf 471 to 243: no more builders walking at roofs), stuck
  bots 9.7 to 0 on ngctf. Kills koth 89 to 112 and 88 to 142, ngctf 70 to
  110 with pickups 8.3 to 14 and captures 1.7 to 4.3, ctf and tc within the
  spread. The tick the same or lower; no crash, the 0.75 profile too.
- **Licence:** the same as the bridges. BattleSpades is AGPL-3.0; this is
  our own implementation of the idea, no cell list and no code taken from it.

## Forward posts on large maps

On a large map (bases 200 blocks apart or more: Raindance, Busan_Port)
some squads hold posts in the middle instead of everyone running base to
base. One squad in two holds them: two of its bots go to a high, flat spot
on the squad's side of the way, put up a wall toward the enemy and stay
there, shooting from it and mending it. The squad's other two hold a second
post further on. The posts are spread over the team's own half of the
map, 20 to 50% of the way to the enemy. The holders stay in their squad, so
the squad respawns next to them, but they never lead the squad's lane; they
leave the post only for an enemy carrying an intel within 30 blocks, or to
carry the intel themselves. They do not walk home with our carrier: our
carrier runs home from post to post (the next of ours on its way, if the
detour is short), and each post watches round it as it passes. A squad
that comes within 50 blocks of a manned enemy post goes to clear it
instead of walking past. With
posts the players are spread most evenly along the map (every tenth of the
way holds 8-13% of them, 5-19% without), for some fighting and captures.

## Zombies

In the zombie mode a zombie bot has no gun: it knows where every human is
at any range and goes for the nearest, a human alone counting as nearer
than one in a group (each mate within 8 blocks makes it count half as far
again, up to three times), and runs at it along its planned way (digging through a
fort when the way is shut). Going at a group, out of its sight and 15 to
40 blocks off, it waits for two more zombies to come up (8 s at most) and
they go together. A zombie's normal jump clears two blocks; it jumps at
a step or a wall that high at once and leaps high at a human standing
higher or at a higher wall, climbs houses with that leap (onto ledges 2 or 3
up, or into notches it cuts with the spade there, waiting for the leap
to come back), spades a wall between it and a human close by,
and scratches it within reach. The survivors spawn anywhere on the map.
Each human bot walks up the highest place it can reach within 48 blocks
of it (stairs, slopes: steps of one block), without digging and shooting on
the move, and holds its own place there facing out; the bots that spawned
in the same four chunks share that high ground, and two searches that end
on the same roof are one group. Where nothing is higher on foot (a town's
houses with no stairs), it takes the best roof 3 to 10 blocks over a
street it can walk to and climbs it with a pillar against the wall, each
bot its own column. Up there they patrol: every 4 s each walks a few
places on across the site (never onto its edge) and its look sweeps from
side to side. A bot whose place is flat ground goes to the nearest
high ground or roof another bot found within 64 blocks; with none, a bot
alone joins the nearest group, and the group walls itself in: a ring
three blocks high round it, built and mended from inside. A zombie close to one of them turns the whole group to it. A zombie comes back 35 to 50 blocks
from a survivor drawn at random.

## Arena

An arena round is fought until one team is left, on a map nothing may be
dug or built on, so a bot's job is only to find the other team. Each bot
walks a lane of its own across the map: the team spreads over 90 blocks
sideways between the two spawns (up to nine bots a row, closer together in
a smaller team), each walking its lane toward the enemy's spawn 30 blocks at
a time, and only to ground at its own level (a lane with no ground near
its next point, off the map's land, is given up for the nearest enemy).
While the gates are closed it waits at its spawn facing the enemy's. An
enemy within 80
blocks is the fight instead, and a bot that has walked its whole lane goes
after the nearest enemy like it always did. Without the lanes both teams ran
at whoever was nearest and every fight of a round happened in one cell of the
middle of the map; now the damage of a round is spread over 4 to 12 cells of
24 blocks, and a round takes a little longer to settle because the bots meet
in more places.

## King of the hill

Every bot of a team has a job on the hill, the same one every life (by its
place among the team's bots):

- The first is the tower's keeper: with three bots of the team on the hill
  together the team raises a cooperative watchtower in its middle (the
  section above), and once it is complete he stands on its top, which still
  counts as on the hill. Until then (or on ground that refuses a tower) he
  builds the old four-block pillar and fights from it.
- With the classes on, the team's first two Marksmen watch the hill from
  high ground round it instead of standing on it (a rifle on the hill is
  wasted at ten blocks): the highest solid ground that sees the hill, a
  roof only where the ground has none.
- The next bots hold posts round the edge of the hill in pairs, facing
  out, two holders a block and a half either side of a spoke, at most two
  thirds of the team. Each pair builds one bunker: a front five wide with
  a loophole in the middle, high at its ends, and a low wing at each side.
  The team that holds the hill walls it in. With the hill held and four
  bots on it, the team also rings it with the battlemented ring.
- The others take a place on the hill and fight there; with a ring they
  hold inside it or outside, never on its wall.
- A Marksman whose way to its spot is cut, or who walks to it too long,
  takes another spot, but only once it is within a hundred blocks of it:
  from its spawn, three hundred blocks off, it gave up spot after spot
  before it ever arrived.

Off the hill they all run onto it shooting as they go, never stopping for
an enemy on the way: on a small map both teams see each other from their
spawns, and bots that stopped to fight never reached the hill. When a
round is won the hill moves and they all head for the new one.

A Rocketeer does not stand on the ground to hold the hill: any height over
the ring counts, so it flies onto a roof or a tree top within twelve blocks
of its place — a roof three blocks up is enough, and it prefers one over
the ground — and stays up there for a minute, shooting down at the enemy's
way onto the hill from above. It is given the hill, never a post: a
Rocketeer holding a bunker kept its jetpack on the ground, so the post
rotation skips it and the rest of the team builds them.

## Capture the flag roles

In CTF every team splits its bots into roles. Humans are not counted. The
roles with a seat are drawn at random among the team's bots when the match
starts and kept all match, so a bot keeps its role from one life to the
next. A seat changes hands only when its bot leaves the team; another bot
draws it then. The gun comes with the role and is given while the bot is
dead or about to spawn, like a player picking one on the respawn screen.

| Role | When | Gun | What it does |
| --- | --- | --- | --- |
| Defender | 2+ bots | shotgun | guards our intel from cover |
| Infiltrator | 3+ bots | SMG | goes for the enemy intel unseen |
| Overwatch | 5+ bots | rifle | the defender's rifle cover: watches our intel from high ground |
| Cover | 6+ bots | shotgun | the infiltrator's bodyguard |
| Tactical camper | 7+ bots | rifle | snipes from a spot on its side |
| Rusher | every second bot without a seat | rifle or SMG | takes the intel whatever it costs |
| Attacker | the rest | rifle or SMG | the objective, fighting on the way |

With 10 bots a team has one of each seat, 2 rushers and 3 attackers.
Rushers and attackers draw a rifle or an SMG once and keep it. (The
overwatch came from 4 bots in the first version: a team of 4 then kept two
bots home, sent its only attacker the hidden way and fought half as much,
3 intel pickups in six 240 s matches instead of 12.)

### Choosing in the chat

When the bots start playing (a player is in), each team talks its roles
over in its team chat, the way players do in the first seconds of a round:

- the defender says it guards the intel ("i'll guard the intel"), then
  names its overwatch ("SlyRhino cover me with your rifle"), who answers
  ("ok");
- the infiltrator says it goes in with the SMG and names its cover ("Badger
  come with me, bring a shotgun"), who answers;
- the tactical camper says it camps ("tactical camper here");
- the others say nothing and attack.

The defender and the infiltrator type at the same time, each line takes
the time a quick typist needs for it (~16 characters a second, plus a
moment to think), and the wording is drawn from a few ways of saying it.
Meanwhile the whole team stands where it spawned, looking at whoever spoke
last (the one speaking looks at the one it names): ~10 s. An enemy in
sight still gets shot. A seat drawn later in the match is said alone ("i'll
guard the intel now"), without the wait. A new match draws the seats and
talks again. Lines go through the server's chat handler like a player's,
so the server logs them (`Player Bot_.. (Team) said: ..`).

### Defender

- It takes a post 2 to 5 blocks off our intel, on the side away from the
  enemy, on a cell it can stand on with ground all round it: watching the
  intel, it watches them come. (A fixed point
  3 blocks off was the air beside Raindance's intel tower: the defender
  tried to bridge into a protected cell and fell off the tower every life.)
- It builds an L-shaped wall at the post, two blocks high: one arm at its
  back (the side the enemy comes from) and one on its side. Each block has
  the colour of the ground under it, so the wall blends in. A broken block
  is rebuilt.
- It stays crouched behind it, always, eyes on our intel, its look slowly
  sweeping a little to each side.
- In a fight it uses that wall as its cover, never builds another, and does
  not wander off its post.
- It **mends the base**. When the match starts the server keeps the blocks
  round each team's intel (10 blocks each way, 8 above it and 4 below) with
  their colours. With our intel at home and no enemy close in sight, the
  defender looks for one of those blocks that is gone (a tunnel's mouth, a
  wall shot through, a grenade's crater), nearest the intel first and
  one with something solid beside it (a tunnel is filled from its far end),
  walks beside it and puts it back in the colour it had. Out of blocks it
  goes to the tent first. About its base it plans without digging: its
  plan to its post went through the base's walls, and it dug away the
  block it had just put back, 60 times in a match. A block filled three
  times anyway is left alone for a minute.
- From its post, when it hears an enemy dig within 20 blocks of it, it goes
  after it, digging into the tunnel if it must. It gives up after 12 s or
  25 blocks from its post, and goes back when it gets there and finds
  nobody. (Hunting every dig within 45 blocks kept it away from its post all
  match: with 20 bots someone is always digging.)
- The moment our intel is taken it leaves its post and hunts the carrier,
  with the overwatch and the 4 bots closest to the carrier.

### Infiltrator

- It plans its way to the enemy intel sneaking:
  - a cell under open sky costs much more than one under a roof;
  - digging is cheap, and it may dig a step down to go underground;
  - blocks it would put up in the open cost more.
  
  So it uses the tunnels that are already there (2Fort's water tunnels) and
  digs its own when there are none, from up to ~120 blocks off the intel
  (70 before: on Raindance it barely sneaked at all).
- It only fights enemies within 12 blocks, or one that shoots it.
- With the intel it plans the fast way home.
- Its digging can be heard. That is the enemy defender's cue.

### Cover

- It follows the infiltrator, also sneaking (so it tends to take the tunnel
  the infiltrator opened), and stops within 3 blocks of it, looking where it
  looks.
- It never places a block.
- It only takes on enemies within 20 blocks of itself. Fighting, it keeps
  following the infiltrator while it is more than 4 blocks away, shooting
  on the move; next to it, it holds and fights in the open without
  charging or chasing off.
- Left more than 25 blocks behind, it ignores the fight and only catches
  up. The first version stopped at every enemy in sight and lost the
  infiltrator.
- When the infiltrator has the intel it escorts it home. While the
  infiltrator is dead, it attacks.

### Camper

- A quarter of the rifle attackers and rushers (the 2nd, 6th... rifle bot
  of a team) camp instead.
- Each life it picks a spot on its own side: high ground between its spawn
  and the enemy, scored by height and by how much of the way toward the
  enemy it can see.
- There it puts up a castle front the colour of the ground, facing the enemy
  (seven wide and a block high through the firing lane, a three-high tower
  at each end and a two-high wing behind each: 15 blocks, from the idea of
  BattleSpades' fort wall), and stands behind it shooting over the middle.
  It only ducks to reload. Every post's front leaves the way it shoots open:
  a wall two high a block away covered a standing bot's eye for any shot
  down a hill, and a crouched one's entirely. The towers are at the ends,
  where they cover the flanks instead of the lane.
- It does not leave the spot to fight: it shoots whoever shows within the
  rifle's range. Away from the spot it fights like an attacker, and with the
  intel it runs home.
- The defender's rifle cover is the **overwatch**: its spot is the
  **highest** ground 10 to 45 blocks from our intel with a clear view of it
  while crouched and flat ground round it. Every column of that ring is
  looked at, from the top down. Among spots as high (or one block lower) it
  prefers one behind the intel as the enemy comes, with the ground they
  cross in sight: looking at the intel, it looks at them too. (On Raindance
  its first spot had the enemy arriving at its back. Height used to count
  little, and 200 random draws missed the tops: it sat low under a tower
  that saw the whole base.) A spot it cannot reach is given up for good,
  with the rest of that roof (the same height within 12 blocks): when its
  plan finds no way there, or after 25 s of walking toward it over its
  lives (fighting does not count). 2Fort's tower roofs are ten one-block
  steps up, and it stuck on one of them life after life. It puts up a wall
  three wide at its back, its middle a block high to shoot through (away
  from the intel, so it is not
  shot from behind; nothing in front, where the intel is), stays crouched,
  and sweeps its aim slowly across the intel, one side then the other,
  resting at each end, as a watch does. It works with the defender: the defender holds the intel up
  close, the overwatch covers it from afar. The moment our intel is taken it
  leaves its spot (its wall and any cover it was using) and goes after the
  carrier with the defender. It knows who took it, as every player is told,
  so it looks for the carrier wherever it is and shoots it before anyone
  else. With the carrier in sight and in rifle range it stops in the open
  and shoots; anyone else it shoots on the move, without stopping or seeking
  cover. When the intel is dropped somewhere else (its carrier died there)
  the overwatch takes a new spot around it, preferably beside something low
  to crouch behind that still lets it see the intel, and watches it from
  there. When the intel goes home it moves back. (It used to fight from its
  spot while the carrier ran, then go back to the old spot and watch an
  empty tent.)
- Every camper at its post **hears steps**: an enemy walking within 14
  blocks, or running within 24, on its feet (crouched or sneaking is
  silent). It leaves its watch, stands up and turns to where it heard them
  for 3 s, so the enemy is in its view when it shows.
- On its way to its spot a camper shoots on the move and does not stop to
  build or hide (it used to get stuck fighting a few blocks short of it).
- A camper keeps its distance: it never walks up to an enemy it cannot
  reach with its rifle, and sent after our intel's carrier it stops as soon
  as it sees it, like the overwatch.
- It takes on the enemy's campers first: the rifle duel at range is its job
  (only the intel's carrier and the enemies at our intel come before).

### Rusher

- An attacker that never stops. With an enemy in view it keeps walking its
  way to the intel and shoots on the move.
- It builds no wall and does not hide.

### Attacker

- It goes for the objective and fights what it meets on the way, using cover
  as described above, but only enemies within 30 blocks: farther ones it
  shoots at on the move and keeps going. (Attackers used to stop 70 blocks
  from Raindance's intel tower and trade rifle shots with its guard behind
  its wall until the match ended.)
- A third of the attackers are **sneaky** (the team's 1st, 4th... attacker):
  they walk the lane with their squad like the others and, past it, take
  the hidden way like the infiltrator (tunnels already there, or their
  own) and, while on it, only fight enemies within 12 blocks or one that
  shoots them. Only the bots with a seat of their own (guard, infiltrator,
  cover, overwatch, camper) are in no squad; the rest fill
  the squads, a new one only when the others are full.

### The dropped intel

- When the enemy intel lies where its carrier died, an SMG bot goes for it
  from up to 80 blocks (up close is its fight) and any other bot passing
  within 25 blocks, even one walking its squad's lane; campers with a rifle
  leave it to them. Bots used to walk past it.
- Near the intel (within 8 blocks) a bot walks in, never runs or hops, and
  sneaks by an edge: one at the top of Raindance's tower ran past the intel
  and off the tower. With the intel and no way home planned yet, the carrier
  never steps off a drop of three blocks or more.

### Squads and lanes

Attackers and rushers play as squads (the squads of CTF, groups of 4).
Bots with a role of their own (defenders, infiltrators, their cover,
campers) are in no squad at all; they get one again if their role goes.

- Each squad has a **leader**, fixed: the first one is its lowest id bot,
  and it only changes when the leader leaves the squad (a team change, a
  role, leaving the server), not when it dies.
- The leader picks the squad's **lane** to the enemy intel: squad 1 left,
  squad 2 right, squad 3 the middle, then again. A lane is two waypoints, a
  third and two thirds of the way from our base to the enemy intel, pushed
  to its side (a third of the distance, 20 to 100 blocks) onto dry land.
  The leader goes through them, then for the intel.
- The others **follow the leader**: they stay within 4 blocks of it, looking
  where it looks.
- While the leader is dead the squad waits at its next waypoint for it
  (with the lane done, it goes for the intel itself).
- On its lane a squad only takes on enemies within 40 blocks, and shoots on
  the move, as rushers do, instead of stopping behind cover: with 64 a side
  everyone fought across the middle and nobody reached the intel.

Before, every squad went down the middle in a line, and the sides of a big
map stayed empty.

### Everyone

- **When a teammate has the enemy intel**, every other bot of that team
  (except the defender and the bots with a post) goes to the carrier, stays
  within 8 blocks of it, and fights in the open around it, shooting on the
  move, with no wall and no ducking, even if it dies for it. Bots that
  respawn meanwhile join it too. (They used to go back to our base: on a big
  map the carrier took minutes, and the base filled with bots walking in
  circles.)
- **When the enemy has our intel**, the defender and the 4 bots of the team
  closest to the carrier hunt it. The others keep going for the enemy intel:
  a whole team running back after one carrier left the enemy intel alone.
- **The carrier** runs home and does not stop to fight or build.
- **The capture.** Pickup and capture are the normal checks: the bot stands
  by the intel's cell or in the tent. They run every tick on a box of 3 blocks
  round the intel (from above only), so a bot running over it takes it; the bot
  still aims for the exact cell.

## Classes (next gen CTF)

In ngCTF a bot picks a class the way a player does (the same request the
class screen sends), while it is dead or about to spawn, and the class
gives it its gun. There are **no roles** there: no defender,
infiltrator, cover, overwatch, campers, rushers or sneaky attackers, no
briefing and no forward posts. Those are classic CTF's; in ngCTF the class
is how a bot plays, and every bot goes for the enemy intel its class's way
(the squads' lanes, the escort of our carrier and the hunt of theirs stay).
The classes are dealt round each team's bots by id (team B two classes on),
so every class plays from 4 bots a team; a class switched off goes to the
next one on. They used to follow the seats (the guard always a Rocketeer,
the campers Marksmen) and held half a team at posts: 16 bots, 600 s, six
seeds, Raindance pickups 1.3 to 5.2 and captures 0.2 to 1.5, Busan_Port
pickups 7 to 10.7, kills up on both; classic CTF event for event the same.

What stays of the old ways is the classes' own:

- The team's first Marksman bot by id camps: a high spot on its side with
  a view of the enemy's way, its castle wall, the rifle (the camper's
  `_bot_camp_spot`, `_ctf_camping_marksman`), after it put its landmine
  at our intel. The other Marksmen attack: every one camping kept a
  quarter of the team on the hills (Raindance pickups 5.1 to 1.2).
- Every attacking Miner tunnels the last 90 blocks to its goal and does
  not follow its squad's leader on the surface (`miner_way`,
  `_ctf_tunnels`). A tunnel begun is kept to the end: out to 160 blocks
  from the goal, through a fight (the way dropped after it is planned
  under the ground again) and through a partial plan (the next stretch
  goes on from its end). While a mate carries the enemy intel the Miner
  does not escort: it digs on to where that intel lay (goal `tunnel-on`)
  and waits there, under the enemy's base, for it to come back; and a
  Miner on a tunnel is never one of the hunters of our intel's carrier.
- A squadmate under the ground near the squad's Miner, also under the
  ground (a squad spawn by a Miner lands in its tunnel), goes on with it
  (goal `tunnel-follow`, on foot, no digs: it walks the tunnel the Miner
  digs) for at most 20 s a stretch (`TUNNEL_WAIT`), then makes its own
  way (`bot_tunnel action=follow depth=`). Before, it dug its own way up
  to the leader on the surface at once and left the Miner. "Under the
  ground" is solid ground three cells thick right over the head
  (`_ctf_underground`): the column's top alone counted every house
  (hiesville, 68 follows a match). 16 bots, 600 s, 10 seeds, against no
  roles at all: pickups Raindance 5.1 / 5.4, Busan_Port 10.1 / 8.3,
  hiesville 5.7 / 5.1, captures 1.5 / 1.2, 3.5 / 3.7, 1.3 / 1.7.
  `SP_BOT_TUNNEL_FOLLOW=0` turns it off for a measurement.

What each class's bot does with its own things:

- Commando: fires rockets at an enemy's feet from 25 blocks out, with a
  wider miss than a bullet's; closer it swaps to the revolver (a slow
  rocket is sidestepped that close, and within 7 its blast comes back),
  and against an enemy touching it it swings the spade; brings fallen
  mates back with the revive perk.
- Rocketeer: wears the jetpack from every spawn and flies to a goal 3
  levels or more above it within 20 blocks; an attacking one flies onto a
  roof or a tree top on its way (3 to 16 up, within 10, on 70% of a tank)
  and fights from up there — an enemy in its gun's reach is shot standing,
  one farther off is not chased down off the roof — for 12 s, and 60 s over
  the hill's ring (see King of the hill). Any bot with the jetpack brakes a
  fall that would hurt; puts its sentry like before.
- Marksman: carries its landmine to our intel (goal `plant-mine`) and puts
  it there, where the enemy must step; at most two down at once.
- Miner: where it would spade its way through a wall it drills instead
  (a bit that bores 12 blocks of ground, 5 charges a life), but only into
  the ground: in a tunnel it is in (solid ground over its head, no air in
  it) or at the face of a hill ahead taller than itself, and never with a
  mate within 8 blocks (the bit bores their ground too). Any roof over its
  head used to be enough, and Miners holding a town's hill drilled the
  ground their team stood on; the spade (3x3x3) digs everywhere else. It is
  a shotgun: an enemy farther than 14 blocks is no reason to stop and aim
  in the open, it closes in the hidden
  way (a sneaking plan: a tunnel or along walls, `bot_shotgun
  action=close_in`, planned to the enemy's own floor) and fights within 14,
  walking in to point blank. An enemy that ducks out of sight while it
  closes in is not given up: it keeps after it underground for 10 s
  (`action=stalk`) and comes up beside it. Only enemies within 30 blocks
  (let go at 40): a far one sends it under the ground on its way to its
  goal for 20 s instead (`bot_miner action=threat`; from up to 130 blocks
  out), and the way to its goal is kept meanwhile and taken up again after
  the chase (`bot_nav action=resume_kept`). Every attacking Miner, not only
  a sneaky one, takes the hidden way for the last 90 blocks to its goal.
  In its own tunnel (three cells high, no room to jump) it digs its way up
  a step: the cube its spade clears takes the roof over its head too.
  Each tunnel to its goal is measured from its first dig
  (`bot_miner action=tunnel_end why=arrived|death|goal|surface|stalled`,
  with its digs and how near it got) and each plan counts its steps
  through tunnels already there (`bot_nav action=plan ... tunnel=`). Digging is cheap
  for it, so its plans tunnel (a dig costs it what walking under a roof
  costs) and it plans sneaking from 2.5 x as far (`BOT_MINER_SNEAK`).
  Its tunnels go under the ground, not through its skin: a dig with less
  than 3 blocks of ground over its opening costs it 2.5 more for each one
  missing (`MINER_ROOF`, `MINER_SHALLOW` in `BotPlanner.c`), so it goes
  down a ramp first, on along 3 or more blocks under the surface (under a
  hill or a mountain as they come), and up only near its goal; where no dig
  is needed it walks on the surface like anyone. Its cheap digs used to
  carve trenches into the surface, which players read as griefing the map.
  Each of its spade digs logs `bot_miner_dig depth=` (blocks of ground over
  the opening, 0 is open sky) and `surface=` (1 for 0 or 1 block); 16 bots,
  400 s, six seeds: digs with 0-1 block over them 61% to 15% on Raindance
  and 69% to 7% on Busan_Port, 3 to 5 blocks deep 9% to 41% and 4% to 42%,
  kills, pickups and captures the same or better.
  Puts its dynamite at the enemy's intel or before an enemy 6 to 15 blocks
  off, then runs: the farthest dry column 12 off, until it is 10 from the
  charge or the fuse is out (`bot_perk action=flee|fled`). It stood by the
  charge and died with it before.

Measured after the balance pass (16 bots and an idle human Commando,
`SP_TIME_SCALE=20`, ~800 game s a match; rocket 0.5 of a grenade, reserve
4, the bots' rocket spread widened; Miner 125 health and 0.95 speed;
Rocketeer 85 health), kills/deaths per class:

| Map, seed | Commando | Rocketeer | Marksman | Miner | Rocket kills | Drills | Perks earned |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2Fort 11 | 62/59 | 51/33 | 15/29 | 21/32 | 54 | 23 | revive 2, sentry 3 |
| 2Fort 12 | 19/33 | 58/29 | 47/56 | 3/13 | 10 | 2 | landmine 5, revive 1, sentry 3 |
| Raindance 11 | 14/21 | 37/22 | 8/18 | 8/13 | 13 | 0 | dynamite 1, landmine 1, sentry 1 |
| Raindance 12 | 12/19 | 38/36 | 35/43 | 1/1 | 10 | 0 | landmine 2 |

With the Miner infiltrator and sneaky attackers and the Rocketeer guard
(2Fort infiltrator plans mostly `open=0`, 60-73 cells dug of ~100 steps):

| Map, seed | Commando | Rocketeer | Marksman | Miner | Drills | Miner pickups | Infiltrator under a roof |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2Fort 11 | 38/31 | 6/5 | 12/11 | 10/26 | 152 | 2 of 3 | 63% |
| 2Fort 12 | 42/34 | 4/7 | 40/33 | 25/42 | 278 | 2 of 4 | 83% |
| Raindance 11 | 18/14 | 2/1 | 6/8 | 7/14 | 17 | 0 of 0 | 15% (32% sneaking) |
| Raindance 12 | 13/18 | 0/0 | 46/49 | 6/7 | 5 | 0 of 0 | 0 (no sample) |

Before the pass (rocket 0.6 of a grenade, reserve 5, a tight bot aim)
rockets made 74 of 156 kills on 2Fort seed 11 and 42 of 142 on seed 12.
Rocket jumps: 6 to 24 a match. Miners closing in the hidden way (2Fort,
800 s, seeds 11 and 12): Miner 57/69 and 61/72 (0.84 against 0.54), 13 of
18 pickups and 6 of 9 captures by Miners.

## Asking bots for something

Everything above is the bots' core, and it does not know which mode it
plays. A mode, and later a plugin, drives bots through one
struct, `gm_bot_goal_t` in `server/Source/Server/BotGoal.h`. The mode's
`bot_goal` hook fills it for a bot at every think (five a second), and the
core plays it: plans the way, walks, fights, digs and builds. A zeroed goal,
or no hook, means "nothing in particular": the bot walks toward the mode's
patrol anchor and fights what it meets.

**What for.** The core reads only the intent, never the name.

| field | what the core does with it |
| --- | --- |
| `name` | logged (`bot_goal goal=`) when it changes; set with `gm_goal_name(out, name, intent)` |
| `intent` | `GM_INTENT_*`, below |
| `role` | `GM_BOT_ATTACK`, `DEFEND`, `INFILTRATE`, `COVER`, `RUSH`: the gun (without classes), the fighting style, which enemies come first |
| `seated`, `camp` | the mode picks the campers itself: 1 a camper (holds a spot behind a castle wall), 2 the overwatch (a high spot that sees home) |

| intent | what the bot does |
| --- | --- |
| `TAKE` | going to take the objective: close to it, it stops for no fight; puts a sentry or dynamite by it |
| `PUSH` | an attacker on its way: an enemy farther than 30 blocks does not stop it |
| `SQUAD` | on its squad's lane: picks enemies within 60 only, sneaks only off the lane |
| `WITH` | keeping up with someone (a leader, a carrier): shoots on the move |
| `POST` | holds a post: fights from it like a camper and never steps aside |
| `HOME` | the defender's work at home: its plans dig nothing there |
| `GUARD` | the defender's post by home: crouched behind its wall |
| `CHASE` | after the enemy carrying our objective: the camper leaves its wall at once |
| `BRIEF` | listening to the team's plan: holds still, looking at whoever talks |
| `PLANT` | taking an explosive (a landmine) to where it goes |
| `COVER` | covering a mate (the infiltrator) |

**Where.**

| field | |
| --- | --- |
| `x`, `y`, `z` | the destination and its floor cell (z grows downwards; 0 unknown) |
| `exact` | the very cell, not just near it (a pickup) |
| `hold` | stop this close and watch |
| `watch_x`, `watch_y` | where to look while holding |
| `perch` | > 0: a roof or a tree top within this of the destination counts as being there, so a bot with the jetpack flies onto one, prefers it and stays (the hill's ring) |

**How to get there.**

| field | |
| --- | --- |
| `rush` | run, ignoring fights (a carrier) |
| `assault` | fight in the open: no wall, no ducking |
| `walk_only` | on foot: no digging, no sneaking |
| `keep_terrain` | not even the spade out of a jam |

**What to do there.**

| field | |
| --- | --- |
| `climb` | in the goal's column: 1 builds a pillar up to floor `z`, 2 digs down to it |
| `work`, `work_x/y/z`, `work_color` | within reach: 1 puts a block there (of that colour), 2 spades it |

**What the team defends.**

| field | |
| --- | --- |
| `has_home`, `home_x/y/z` | home: what the team protects. The guard and the overwatch post by it, and enemies near it come first |
| `has_guard`, `guard_x/y` | something else to defend: enemies there come first |

Outside the goal, the mode's ops say the rest:

- `player_rules` (`GM_RULE_NO_GUN`: the melee brain, `BotMelee.c`; `HIGH_JUMP`: the leap; `NO_DESTROY`: nobody sneaks or digs);
- `player_carries` (a carrier throws nothing, places nothing, stops for no fight, and is the enemy defenders' first target);
- `patrol_anchor` (where a bot with no goal goes).

### From a plugin

A Lua plugin (the plugins repository's `docs/api.md`) gives bots their
goals with the same fields, before the mode does:

```lua
function plugin.on_bot_think(bot)          -- every think of every bot
    if bot.team ~= 1 then return nil end    -- nil: the mode's own goal
    local v = game.player(vip)
    return {name = "escort", x = v.x, y = v.y, hold = 4,
            intent = {"with", "push"}, watch = {x = v.x + v.ox * 10, y = v.y + v.oy * 10}}
end
function plugin.on_bot_arrived(bot, name) end    -- once a goal is reached
function plugin.on_bot_stuck(bot, why) end       -- the core made no progress for a while
function plugin.on_bot_team_tick(team, bots) end -- once a second: hand out roles
```

- **The table's keys** are the struct's fields (`x`, `y`, `z`, `exact`, `hold`, `rush`, `assault`, `walk_only`, `keep_terrain`, `climb`, `camp`) with these forms:
  - `role` a word (`"attack"`, `"defend"`, `"infiltrate"`, `"cover"`, `"rush"`);
  - `intent` a word or a list of them (`"take"`, `"push"`, `"squad"`, `"with"`, `"post"`, `"home"`, `"guard"`, `"chase"`, `"brief"`, `"plant"`, `"cover"`);
  - `watch`, `home` and `guard` points `{x =, y =, z =}` (a player table is one);
  - `work = {x =, y =, z =, dig = bool, color =}`.

  An unknown role or intent is the plugin's error (it is turned off).
- **A bot's player table** has `goal` (its goal's name) and `target` (the enemy it fights, nil none); `p:sees(q)` is the bots' sight test.
- **What stays in C:** the plugin decides, the core does the work: the way (the planner's thread never calls Lua), sight, aim, digging.

`server/tools/bottest` is a test mode on this API (a VIP escort) with its
measurements: 2.8 to 7.3 µs a call, 0.06 to 0.14 ms a tick with 120 bots.

Examples from the modes (`server/Source/Server/Gamemodes/`):

- **CTF:** `take-intel` (`TAKE | PUSH`, `exact`); the squad leader's `lead` (`PUSH | SQUAD | WITH`, the next waypoint); `follow` (the leader, `hold` 4); the guard's `defend` (`GUARD | HOME`, role `DEFEND`, `home` our intel); `carry-home` (`rush`).
- **Babel:** `climb` (`climb` 1 in the ladder's column); `stair-build` (`work` 1 at the next step, `keep_terrain`, `walk_only`); `babel-hunt` (`CHASE`, role `RUSH`: the nearest enemy on the ground); `babel-wall` (`work` 1 at a missing wall cell) and `babel-window` (`POST`, a window to hold behind).
- **KotH:** the tower builder (`climb` in the hill's middle, or `hill-tower-top` on a complete cooperative tower); the posts round the ring (`POST`, a castle front facing out); the rest `hill` (role `RUSH`, shooting on the move, until on it; then `hold` and `watch` outward).
- **Zombie:** the survivors' `high-ground` (`walk_only`, `hold`, `watch` outward, role `RUSH` on the way); `roof-climb` (`climb` 1 against the roof's wall). The zombies have no goal at all: `GM_RULE_NO_GUN` gives them the melee brain.
- **TC:** `tc-take` / `tc-defend` / `tc-hold` (a territory, `hold`; a ring changes the places to inside or outside it, and a completed bunker gives every third bot the middle).
  Bots and players respawn at the team's territory nearest the front that has
  no enemy within 96 blocks, behind it, facing the next territory to take.
- **Arena:** `gate` (hold at the spawn while the gates are shut); `push` (the bot's lane point).
- **A Lua mode:** `objective` (`game.objective(x, y, radius)`: each bot its own spot inside, `hold`).

## What bots do not do yet

- Use the jetpack anywhere but straight up to a goal close by (no flying
  across water or gaps on the way).
- Plan around enemies: the way is planned over the map only, fights happen
  where they meet.

Bots never ping: pings are for people.
