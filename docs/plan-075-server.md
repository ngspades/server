# Plan: the 0.75 server profile (decisions taken, to be reviewed)

> **Implemented (2026-10-23)**, all sections, see CONTRIBUTING.md "The 0.75
> profile" for what is in the code and what is still open (0.76's map
> cache, the browser and TURN tried for real, the CI smoke join).

> **Status: the seven design decisions are taken (see section 0).** What is
> left is the work, and a short list of consequences that still need an answer
> (section 15). Nothing here changes the current server: the 1.0 server keeps
> its behavior whatever happens to the 0.75 profile.

## 0. The decisions

1. **One binary.** The 1.0 server and the 0.75 server share the sources (the
   bots, the plugins, the map rules, the transport layer): they are two
   profiles of one executable, not two code bases and not two repositories.
2. **Each profile has its own TOML.** `config.toml` for the 1.0 profile,
   `config-075.toml` for the 0.75 one. A Lua plugin says which version it
   supports; the server does not load it in a profile it does not support.
3. **The configuration stays as it is.** The config file is mandatory (there
   is no "config by environment variables only" mode).
4. **The browser reaches the 0.75 server, and so do the old clients.** The
   web client speaks 0.75 over the WebRTC transport the server already has
   (the browser cannot speak 1.0 to a 0.75 server, and it cannot speak UDP at
   all); the unmodified 0.75 clients join over ENet/UDP.
5. **The server supports the ZeroSpades extensions** (Player Properties,
   Damage Markers, Teamplay), so an unmodified ZeroSpades gets more than a
   bare 0.75 client.
6. **The 0.75 server announces to the public 0.75 masters** (the ones the
   vanilla clients already know) **and to our master** (the browser's list and
   the WebRTC signalling). A server that is in both lists is listed **once**,
   as ours: the client's merge already does that (`hud_serverlist_add`: the
   "ours" row replaces the 0.75 one, the same identifier).
7. **The 0.75 server is the 0.75 game.** Everything the 0.75 protocol can
   express, with all of our server-side work (the bots, the planner, the
   plugins, the map metadata, the anti-abuse), and **nothing that is a 1.0
   invention**: no perks, no classes, no squads, no kill assists, no 3D
   spotting, no revive, and only the classic modes (section 6).

## 1. What this is

Add a **profile to the server binary** where the server speaks **only the
0.75 protocol**: original 0.75 clients (BetterSpades, pyspades, ZeroSpades)
join it, the browser joins it over WebRTC, and the game keeps everything the
1.0 server has — the bots, the planner, the Lua plugins, the map metadata, the
anti-abuse rules — as long as it stays playable by a client that was never
touched.

The server's **default behavior does not change**: without a flag, the server
is exactly what it is today (protocol 1.0 only, refusing every other connect
data).

Two rules shape everything:

1. **One binary, one easy switch.** `ngspades-server` starts the 1.0
   server (today's behavior). `ngspades-server --profile 075` starts
   the 0.75 server. Nothing else changes between them.
2. **The two profiles never share a configuration.** Each profile reads its
   own config file, writes its own data folder, listens on its own port and
   has its own master identity. Running both at the same time on one machine
   is a supported, documented case.

## 2. Ground rules for the implementation

1. **One source tree.** The gameplay code is shared at the source level (the
   same files compiled once), never copied between profiles.
2. **The wire is the only thing that differs.** Protocol 1.0 lives in
   `server/Source/Server/Proto10/`; protocol 0.75 goes into a new
   `server/Source/Server/Legacy/`. The shared gameplay never learns which one
   it talks to: it calls a small wire interface, and each profile supplies its
   own implementation.
3. **No protocol-specific logic in the shared files.** A shared file that must
   tell a client something calls the wire interface; it never includes
   `Proto10/*.h`.
4. **Only what needs the 1.0 wire is gated.** Everything else (the bots, the
   plugins, the map rules) is shared and works in both profiles. The 1.0
   inventions (the perks, the classes, the squads, the assists, the spotting,
   the revive, the 1.0 modes) are **absent** in the 0.75 profile, not
   degraded (section 6).
5. **The client's protocol lock is a debug tool.** It lives in the harness and
   in an environment variable, never in `config.ini` and never in the Settings
   screen (section 11).
6. **English everywhere**: comments, logs, commit messages, docs.
7. **Measure, do not guess.** Each phase ends with a log line or a harness run
   that proves it.

## 3. Architecture

### 3.1 The profile

A runtime value on the server, not a compile-time one (the whole tree is one
binary):

```
enum server_profile_t {
    SERVER_PROFILE_10,   /* protocol 1.0 only: today's behavior, the default */
    SERVER_PROFILE_075,  /* protocol 0.75 only: the classic game */
};
```

The profile decides:

| | `SERVER_PROFILE_10` | `SERVER_PROFILE_075` |
|---|---|---|
| Accepted connect data | only `P10_CONNECT_MAGIC` | only `3` (0.75) and `4` (0.76) |
| The wire | `Proto10/` | `Legacy/` |
| The features | all | the 0.75 game: no perks, no classes, no squads, no assists, no spotting, no revive (section 6) |
| The game modes | the full catalog | the classic CTF and Babel |
| The config file | `Resources/config.toml` | `Resources/config-075.toml` |
| The data folder | the default one | the default one plus `-075` |
| The masters | our master (`Net/Master.c`) | the public 0.75 masters **and** ours (the browser's list and the signalling) |
| The player cap | 64 | 32 (the 0.75 limit) |

It lives in the server struct (`server/Source/Server/Structs/ServerStruct.h`)
and is read through `server_profile()`. It is set at start-up from the command
line (section 4), before the config is read.

### 3.2 The wire interface (the shim)

The shared gameplay calls protocol 1.0 functions directly today. These are the
**exact** symbols referenced outside `server/Source/Server/Proto10/` (measured;
re-measure after the first edits):

```
proto10_world_cell                  Block.c Grenade.c Nodes.c Packets/BlockLine.c Plugins.c Gamemodes/Arena.c
proto10_world_chat                  Packets/Message.c
proto10_world_tick                  Server.c
proto10_world_player_update         Player.c
proto10_bridge_damage               Packets/SetHP.c
proto10_bridge_kill                 Packets/KillAction.c
proto10_bridge_player_left          Packets/PlayerLeft.c
proto10_bridge_reload               Packets/WeaponReload.c
proto10_bridge_teleport             Plugins.c
proto10_bridge_generic              Assists.c Classes.c EscapeWatch.c Player.c Plugins.c Revive.c Gamemodes/Zombie.c
proto10_gamemode_intel_event        Gamemodes/Ctf.c
proto10_gamemode_round_start        Gamemodes/Gamemode.c
proto10_entities_grenade_thrown     Packets/Grenade.c
proto10_entities_grenade_removed    Player.c
proto10_entities_grenade_exploded   Grenade.c
proto10_objects_grenade             Grenade.c
proto10_objects_at                  Perks.c
proto10_objects_destroy             Perks.c
proto10_begin                       Player.c
proto10_update                      Player.c
proto10_on_packet                   Server.c
proto10_session_tick                Server.c
proto10_session_map_loaded          Map.c
proto10_bridge_tick                 Server.c
proto10_netstats_add                Net/Conn.c
proto10_netstats_log                Server.c
```

**Do not edit the shared files to make this work.** `Legacy/` provides the same
symbols with 0.75 semantics:

| Symbol | 0.75 semantics in the shim |
|---|---|
| `proto10_world_cell` | the 0.75 block action / world update to the affected clients (a 0.75 client computes collapses and craters itself) |
| `proto10_world_chat` | the 0.75 chat packet |
| `proto10_world_tick` | the per-tick 0.75 broadcast (`send_world_update`) |
| `proto10_world_player_update` | no-op (0.75 has no map-reset handling) |
| `proto10_bridge_damage` | the 0.75 `SetHP` output |
| `proto10_bridge_kill` | the 0.75 `KillAction` output |
| `proto10_bridge_player_left` | the 0.75 `PlayerLeft` output |
| `proto10_bridge_reload` | the 0.75 `WeaponReload` output |
| `proto10_bridge_teleport` | the 0.75 position output (`send_position_packet`) |
| `proto10_bridge_generic` | a 0.75 chat message **or** the matching extension packet (section 7) |
| `proto10_gamemode_intel_event` | the 0.75 `send_intel_capture/pickup/drop` |
| `proto10_gamemode_round_start` | a 0.75 notice (0.75 has no round state) |
| `proto10_entities_grenade_*` | the 0.75 grenade packet (a 0.75 grenade is a packet, not an entity) |
| `proto10_objects_*` | no-op (the 1.0 object registry does not exist in 0.75) |
| `proto10_begin`, `proto10_update` | the 0.75 join machine (section 8.2) |
| `proto10_on_packet` | the 0.75 dispatcher (section 8.1) |
| `proto10_session_tick`, `proto10_session_map_loaded` | no-op (the 1.0 handshake does not exist in 0.75) |
| `proto10_bridge_tick` | the 0.75 per-tick broadcast |
| `proto10_netstats_add`, `proto10_netstats_log` | count the 0.75 bytes (the same counters, the 0.75 packet types) |

The shim is the **only** place where the two protocols meet.

## 4. Starting the server: one binary, two profiles

### 4.1 The command line

```
ngspades-server                  # protocol 1.0 (today's behavior, the default)
ngspades-server --profile 075    # protocol 0.75 only
ngspades-server --profile 10     # the same as no flag
ngspades-server --config <path>  # a config file of your own (wins over the profile's)
```

- `--profile <10|075>`: the profile. Default `10`.
- `SP_PROFILE=075`: the environment override (the project uses `SP_*` for
  tests; keep it consistent).
- `--config <path>`: an explicit config file, for both profiles. Without it,
  the profile picks its own (section 4.3).
- An unknown profile value: a clear error listing the two, and exit. Never
  fall back silently to 1.0.
- `--help`: list `--profile` next to the other flags.

### 4.2 The order of parsing

The profile decides the config path, so it must be known first:

1. Parse `--root`, `--data`, `--profile`, `--config` (the flags that do not
   need the config).
2. Resolve the profile and the config path.
3. Read the config, then apply the rest of the flags (`--port`, `--gamemode`,
   `--map`, `--preset`, `--bots`, ...) as today.

`Main.c` currently resolves the root (`resolve_data_root`) and the data
folder (`resolve_data_dir`) before reading `Resources/config.toml`
(`TOMLH_READ_FROM_FILE`) and before `--check-config`. The profile resolution
goes next to those, and the config path becomes a variable used in both
places.

### 4.3 The configurations never interfere

Each profile has its own config file, its own data folder, its own port and
its own identity:

| | profile `10` | profile `075` |
|---|---|---|
| Config file | `<root>/Resources/config.toml` (unchanged) | `<root>/Resources/config-075.toml` |
| `--config` | overrides it | overrides it |
| Data folder (what the server writes) | the default data folder (unchanged) | the default data folder **plus `-075`** |
| `--data` / `SP_DATA_DIR` | wins | wins |
| Game port | `config.toml`'s `[server] port` | `config-075.toml`'s `[server] port` (a different one) |
| WebRTC port | `[webrtc] port` (default: game port + 1) | its own `[webrtc] port` |
| Identity (`master.key`) | the data folder's | the 0.75 data folder's (its own key) |
| Bans (`Bans.json`), logs, plugin stores | the data folder's | the 0.75 data folder's |
| Map rotation, fog, teams, bots, presets | `config.toml` | `config-075.toml` |

Details:

- **`config-075.toml` is a committed file**, a copy of `config.toml` with the
  profile-specific lines changed: its own port, the classic-CTF map rotation,
  the tolerant conduct/anticheat values (section 10.1), the public masters on,
  and the 1.0-only sections off. It is the 0.75 server's defaults, so a fresh
  install runs with one flag and no editing.
- **A missing config file is an error, never a fallback.** If
  `config-075.toml` is absent, print what to do
  (`cp Resources/config.toml Resources/config-075.toml`) and exit. Falling
  back to `config.toml` would start the 0.75 server on the 1.0 port and
  fight the other server for it.
- **The data folder suffix**: the 0.75 profile's default data folder is the
  1.0 one with `-075` appended, so `Bans.json`, `master.key`, `webrtc.pem`,
  `logs/` and the plugins' data never mix. An explicit `--data` always wins.
- **Ports must not collide.** The two servers can run at the same time on one
  machine: the game ports differ, and each profile's WebRTC port follows its
  own game port (default `port + 1`).
- **`--check-config` follows the profile**: it validates the file the profile
  would actually read.

### 4.4 The feedback (so opening it is easy and obvious)

- **The startup banner** prints the profile and everything it depends on, one
  line per fact, before the first map loads:

  ```
  Profile: 0.75 only (classic CTF)
  Config:  server/build/Resources/config-075.toml
  Data:    server/build/data-075
  Port:    32890 (WebRTC 32891)
  Master:  public 0.75 masters on, our master off
  ```

  and, for the 1.0 profile, the same lines with today's values. The 1.0
  banner must not change the 1.0 server's current output in a way that breaks
  the harness (`CEVT`/`EVT` lines are the contract; the banner is a `LOG_`
  line).
- **A console command `/profile`** prints the same facts at runtime (the
  profile, the config path, the data folder, the port, the master state).
  Cheap to add, and it answers "which server am I talking to?" in the game.
- **Makefile targets**: `make run-server` (1.0, as today) and
  `make run-server-075` (the 0.75 profile). `make server` still builds the
  one binary.
- **The client's listen server** (Singleplayer, "Play with bots") keeps
  starting the default profile; it needs no change.

## 5. The plugins say which version they support

A plugin is written against the game's API, and the API differs between the
profiles (the classes, the perks, the modes). A plugin that only makes sense
in the 1.0 profile must not be loaded in the 0.75 one.

- **The declaration**, in the plugin's own file, at the top:

  ```lua
  game.supported_profiles("10")        -- only the 1.0 profile
  game.supported_profiles("10", "075") -- both
  game.supported_profiles("075")       -- only the 0.75 profile
  ```

  A plugin that says nothing is loaded in every profile (the plugins that
  exist today keep working), and the 1.0-only API it calls is refused with a
  reason (section 6.2).
- **The server** refuses to load a plugin that does not support the running
  profile, and says so: `EVT plugin action=refused name= why=profile`. It
  does not fail the map and it does not stop the other plugins.
- **The API's documentation** (the plugins repository, `docs/api.md`) gains
  `game.supported_profiles`, and the profile-dependent calls are listed as
  "1.0 only" (the classes, the perks, `game.set_mode` to a 1.0-only mode).
- **The config** may also restrict a plugin, for the operator who does not
  want a plugin at all in one profile: `[plugins.<name>] profiles = ["10"]`
  in that profile's TOML. The two restrictions are combined (the plugin must
  pass both).

## 6. The features in the 0.75 profile: the 0.75 game, with our server-side work

The 0.75 server supports **only what the 0.75 protocol has**. The 1.0
inventions are not there at all — not "hidden from a vanilla client", not
"server-side only": **absent**. What stays is the whole classic game plus
every server-side improvement we wrote, because none of them needs the 1.0
wire.

### 6.1 In the 0.75 profile

| What | Where | Note |
|---|---|---|
| The classic CTF and Babel (the intel, the tents, the captures) | `Gamemodes/Ctf.c` | the 0.75 rules as they were |
| The bots (dig, swim, cover, the roles, the lanes, the honest misses) | `Server/Bot*.c` | as they are: they act through the 0.75 handlers |
| The bot planner (A* on its own thread, the map copy) | `Server/BotPlanner.c` | as is |
| The bot projects (the cooperative bridges) | `Server/BotProjects.c` | as is |
| The map metadata (the pyspades rules: the spawns, the intel, the protected sectors, the fog, the water and boundary damage) | `Server/MapMeta/MapMeta.c` | as is: this is what makes the server feel like a pyspades |
| The map catalog, the tags, the rotation, the presets, the per-map bot counts | `common/modes/modes.h`, `tools/tag_maps.py` | as is |
| The Lua plugins (the chat commands, the blocks, the fog, the bans, the store, the mode and map changes) | `Server/Plugins.c` | as is, minus the 1.0-only API (section 6.3) |
| The anti-abuse rules (the conduct, the anticheat, the spawn protect, the escape watch), the lag compensation, the bans, the audit, the stats, the event log | several | as is, with the tolerant preset (section 10.1) |
| The ZeroSpades extensions (the damage numbers, the teammates through the walls, the pings, the compass) | `Legacy/Ext075.c` | the 0.75 ecosystem's own features (section 7) |
| The IPv6, the `--data` folder, the WebRTC transport, the masters | `Server/Net/` | as is |

The bots keep their classic roles in the 0.75 profile: without the classes,
`_bot_role_weapon` falls back to the 0.75 gun set (rifle, SMG, shotgun) and
the seats (defender, infiltrator, overwatch, camper) as they were before the
classes.

### 6.2 Not in the 0.75 profile (the 1.0 inventions)

These are off in the 0.75 profile. The code is compiled (one binary) but the
profile makes it inert: no grant, no announcement, no wire packet.

| Feature | Why | Where the gate goes |
|---|---|---|
| The perks (the sentry, the jetpack, the landmine, the dynamite) | entities and input bits the 0.75 protocol has not | `Server/Perks.c`: the grant path (`_perk_grant`, the wear-on-grant, the inventory) does not run in the 0.75 profile. The grant is today protocol-agnostic — that is the bug this fixes. |
| The classes | a 1.0 screen and a 1.0 announcement; the 0.75 team screen has a weapon, not a class | `Server/Classes.c`: no class is chosen in the 0.75 profile; every player is the game's default (the Commando's figures are the game's own). |
| The squads (the names, the rally point) | a 1.0 wire | `Server/Gamemodes/Squads.c`: the wire is already gated on `p->protocol != PROTOCOL_10`; the bots' own lanes stay (they are server-side). |
| The kill assists | a 1.0 wire | `Server/Assists.c`: the tracking may stay, the 1.0 announcement must not happen. |
| The 3D spotting | a 1.0 wire (a 0.75 client has the Teamplay ESP of its own, which is ZeroSpades' feature, not ours) | the `spot` command: refuse in the 0.75 profile (`why=profile`). |
| The revive | a 1.0 entity (the marker) | `Server/Revive.c`: refuse the perk. |
| The zombie mode, ngCTF, the arena, the creative mode, the Lua modes | the GameModeState and the 1.0-only mechanics behind them | not registered in the 0.75 profile (section 6.4). |
| The streak bar, the perk HUD | client-side, and only on a 1.0 connection | nothing to do on the server. |

### 6.3 What a refused 1.0-only call looks like

The plugin API's 1.0-only functions (`game.classes()`, `game.class()`,
`game.set_class()`, `p:set_class()`, the class events, the perk API) are
refused in the 0.75 profile **with a reason**, the same way a mode refuses a
class (`why=mode`):

```
EVT plugin action=refused name=myplugin why=profile what=game.set_class
```

and `game.set_mode` to a mode the 0.75 profile does not register (the ngCTF,
the zombie, the arena, the creative) is refused the same way.

### 6.4 The modes

The 0.75 profile registers the **classic CTF** and **Babel** (the same CTF
code with Babel's rules). The other modes (ngCTF, zombie, arena, creative,
the Lua modes) are 1.0-shaped: the GameModeState has no 0.75 equivalent, so
the client cannot be told about the mode, the round clock, the perks or the
classes. They stay in the 1.0 profile.

`common/modes/modes.h` (the catalog the client reads) stays as it is: the
client filters the picker by what the server supports.

## 7. The ZeroSpades extensions on the 0.75 server

Our client already speaks ZeroSpades' protocol extensions on a 0.75 server
(the client's `ext075.c`, the client's `ext075.h`; the credit for the wire format
is there, and the client's `probe.c` is a 0.75 server that only
sends these, as the reference). The **server** side does not exist yet: this
section writes it, in `server/Source/Server/Legacy/Ext075.c`.

### 7.1 The wire, exactly as the client reads it

The extension ids and their packet ids (`PACKET_EXT_BASE = 0x40`; an
extension below 192 has its packet at `0x40 + id`):

| Extension | id | packet | origin |
|---|---|---|---|
| Player Properties | 0x00 | 64 | BetterSpades |
| Damage Markers | 0x20 (32) | 96 | ZeroSpades |
| Teamplay | 0x30 (48) | 112 | ZeroSpades |
| 256 players | 0xC0 | — | BetterSpades: **not offered** |
| Message types | 0xC1 | — | BetterSpades: **not offered** |
| Kick reason | 0xC2 | — | BetterSpades: **not offered** |

**Never offer 0xC0, 0xC1 or 0xC2.** Our client lists them as "known" (so it
would agree to them) but implements no handler for their packets, and 256
players would break the one-byte player ids. Offer only what the server
really sends.

**ExtInfo (packet 60), server to client**: `count` (u8), then `count` pairs
of (id u8, version u8). The client answers with the same shape, listing only
the extensions both sides know. The server records the answer per player and
sends the extension packets only to the players that agreed.

**Player Properties (packet 64)**, packed:

```
subID u8 (=0), player_id u8, health u8, blocks u8, grenades u8,
ammo_clip u8, ammo_reserved u8, score u32
```

**Damage Markers (packet 96)**: `victim` u8, then the amount, whose width the
packet length picks: 2 bytes = u8, 3 = i16, 5 = i32. A negative amount is
healing. The client ignores it for our own player and for a victim it does not
know.

**Teamplay (packet 112)**: the first byte is the sub-packet.

| Sub | Payload |
|---|---|
| 0 Config | `features` u8 (bit 1 = teammates through walls, bit 2 = pings allowed) + two f32 (the north vector) |
| 1 Ping | `player` u8, `x` f32, `y` f32, `z` f32, `life` f32, `surfaces` u8, colour (blue, green, red) u8 x3, `message id` u8, the label (free UTF-8) |
| 2 Mark | `player` u8, `life` f32, `surfaces` u8, `show name` u8, colour (blue, green, red) u8 x3, `message id` u8, the label |

The map coordinates on the wire: the wire carries (x, y, z) where the internal
is (x, `63 - z`, y) — the same conversion the client does when it reads a
ping. The client's own ping sends the same layout, with the player id 255
(the server's own).

### 7.2 What the server sends

- **ExtInfo** at the join (with or right after the state data), listing the
  extensions the server really has, with the versions the client knows
  (`1` for each of the three).
- **Player Properties** whenever a player's state changes: the health, the
  blocks, the grenades, the ammo, the score. The 0.75 protocol has no packet
  for those, so a vanilla client shows nothing; this is how it learns them.
  Send it to the players who can see that player (the same visibility rule as
  the entities), and to the player itself.
- **Damage Markers** when we damage someone: the victim and the amount (the
  server already knows both in the `send_set_hp` path). Send it to the
  attacker's team and to the victim's team. The amount is positive for a hit
  and negative for a heal.
- **Teamplay Config** after the join, with the permissions from the config.
- **Teamplay Ping**: the relay of a teammate's ping (section 7.3), with the
  server's own colour and a lifetime; the server's own ping uses the player
  id 255.
- **Teamplay Mark**: the ESP mark — a player shown through the walls. This is
  the 0.75 way of marking an enemy, the counterpart of the 1.0 spotting.

### 7.3 What the server receives

- **Packet 60** (the client's ExtInfo answer): record the agreed set per
  player; log it (`EVT ext075 action=agree player= agreed=`).
- **Packet 112, sub 1** (the client's own ping): the server relays it to the
  pinger's team with the server's colour, a lifetime from the config and the
  label, converting the coordinates to the map ones. A ping during a map load
  is dropped (the client's rule, and it is right: the map is being replaced).

### 7.4 The configuration

```
[ext075]
# The ZeroSpades protocol extensions the 0.75 server offers.
damage_markers = true
teamplay = true
player_properties = true
team_esp = true       # teammates through walls (the Config bit 1)
pings = true          # pings allowed (the Config bit 2)
compass = true        # the compass bar (the Config bit 4)
ping_lifetime = 12.0
```

Read it in the 0.75 profile only; the 1.0 profile ignores it (1.0 has its own
ways).

### 7.5 The credit and the files

- `server/Source/Server/Legacy/Ext075.c` (the writer) carries the same credit
  as the client's `ext075.h`: the wire format and the rules (the lengths, the
  colours in blue-green-red order, a duration of 0 removing and +inf lasting,
  the player 255 for the server's own ping, what is dropped during a map load)
  are ZeroSpades', read from its `Sources/Client/NetProtocol.h`,
  `NetClient.cpp`, `Client.cpp` and `Teamplay.h`
  (https://github.com/zerospades/zerospades, GPL-3; Teamplay by Fran6nd and
  the ZeroSpades developers).
- The client's `ext075.c` reads and this file writes: **do not merge them**
  into a shared module — they are two directions of the same wire, and the
  client one is client-only code (it renders).

### 7.6 The rest of the aosprotocol specification (done)

The aosprotocol specification (https://github.com/zerospades/aosprotocol,
`protocol075.md` and its `extensions/` folder) has more than ZeroSpades'
two, and both sides now speak all of it but the Powerthirst packets
(script begin/chunk/end/call, which need an AngelScript client):

- **Offered** in ExtInfo, each `[ext075]` key on by default: Flashlight
  (0x32), Player Limit (0xC0), Message Types (0xC1), Kick Reason (0xC2),
  next to the three above. Each goes only to a client that agreed.
- **Flashlight** (`Ext075.c`, packet 114): a client's Light (sub 0) is
  relayed to every client that agreed, the asker too, when it is alive in
  a team and the plugins' `on_flashlight(p, on)` does not refuse it
  (`plugins_on_flashlight`; `EVT ext075 action=light|light_refused why=`).
  Light Config 255 (the `flashlight_reach/cone/color/flicker` keys) and
  Light State (one bit per lit player) go to a client after it agrees and
  after every map, both after the State Data. A light goes off without a
  word on Create Player, Kill Action, Player Left and a map change
  (`ext075_light_off`, `ext075_map_start` from `server_reset`), as every
  client does; a Kill Action the victim did not get (made invisible) is
  followed by an explicit Light off.
- **Message Types**: `send_server_notice_kind` / `broadcast_server_notice_kind`
  (`Util/Notice.h`, `NOTICE_BIG|INFO|WARNING|ERROR`) send the type to a
  client that agreed and type 2 to the others (`ext075_chat_packet`). Used
  by the chat warnings, the AFK and team damage warnings, the spam mute
  (error), "Loading <map>" (info), the match's end and the zombie outbreak
  (big). The 1.0 profile has one kind of notice.
- **Kick Reason**: `notice_kick(player, reason, how, text, ...)` sends the
  text as a system line from player 255 to a client that agreed, then
  disconnects (`CONN_CLOSE` becomes `CONN_CLOSE_LATER`: ENet's polite
  goodbye drops what is queued). Every kick and ban goes through it.
- **UTF-8 chat**: a client's chat whose text starts with `0xFF` is UTF-8;
  `Dispatch075.c` hands the CP437 the server keeps to the chat handler and
  `ext075_chat_utf8_begin` keeps the original, so the relay of that line
  gives UTF-8 readers the sender's own characters (Cyrillic) and the others
  CP437 (`?` for what it lacks). A reader is a client whose extended version
  answer set feature bit 0, one that wrote a `0xFF` line, or OpenSpades
  (`'o'`). Plain ASCII never gets the prefix.
- **Extra Packets**: after a client's plain version answer (34) it is sent
  Handshake Init (31, a challenge) and an extended Version Get (33 with 0,
  1, 2), once a connection; Handshake Response (32) and the extended answer
  (packet 9 marked `'x'`) are logged (`EVT ext075 action=handshake ok=`,
  `version_ext app= locale= features= utf8=`). 31 from a client (0.76's
  MapCached) is ignored: the 0.75 Map Start carries no CRC.
- **Player Limit** is announced, but the profile keeps 32 players: the 0.75
  World Update has 32 slots, and nothing in the spec asks a server for more.
- Not done: the 0.76 wire (Map Start with CRC and name, MapCached, the
  per-player World Update) for a real 0.76 client; this profile sends 0.75
  packets to a client that connected with 4.
- Tests: `server/tools/ext075test/run.py` (every new packet, byte for
  byte), the client's `aosprotocol_075.py` (the server and two of our
  clients locked to 0.75: agreement, a light seen by the other client,
  Cyrillic both ways, a warning line, a kick reason).

## 8. Phase: restore the 0.75 wire

Everything needed exists in this repository's git history. It was removed on
2026-10-03 by two commits:

- `ba72777` — "remove: server 0.75 send paths (dead since protocol 1.0 switch) and PacketChecks utility"
- `da14df7` — "Finish removing the server's dead 0.75 send code"

**Use the history as the source of truth**: `git show ba72777^:<path>` gives
the original file. Copy the original bytes (packet ids, field order); adapt
only what changed.

### 8.1 The dispatcher (the receive side)

`ba72777^:server/Source/Server/Packets/PacketManager.c` has `init_packets()`
with the 18-entry table and `on_packet_received()`:

```
0 position, 1 orientation, 3 input, 4 weapon input, 5 hit, 6 grenade,
7 set tool, 8 set color, 9 existing player, 10 short player, 13 block action,
14 block line, 17 chat message, 28 weapon reload, 29 change team,
30 change weapon, 34 version response, 63 host command
```

All 18 receive handlers **still exist** in `server/Source/Server/Packets/`,
as the 0.75 wire's parsers: each reads its packet and calls the internal game
API (`server/Source/Server/GameApi.h`, the `game_player_*` functions), which
the 1.0 bridge (`Proto10/`) and the bots call directly. Put the table in
`Legacy/Dispatch075.c`; the gameplay stays shared.

The 0.75 profile adds two more ids to its table (section 7): **60** (the
client's ExtInfo answer) and **112** (the client's Teamplay ping).

In `server_conn_receive` (`Server.c`), route by profile:

- profile `10` → `proto10_on_packet` (unchanged),
- profile `075` → `on_packet_received_075`.

### 8.2 The join state machine

`ba72777^:server/Source/Server/Player.c` has the old `on_player_update` switch
with `STATE_STARTING_MAP → STATE_LOADING_CHUNKS → STATE_JOINING →
STATE_SPAWNING → STATE_READY` (and the bot shortcut that skips the map
transfer). Restore it in `Legacy/Join075.c`:

- `STATE_LOADING_CHUNKS`: send the map in VXL chunks (`send_map_start` /
  `send_map_chunks`).
- `STATE_JOINING`: `send_joining_data` (the 0.75 state data). The client
  answers with its team and weapon (`receive_short_player`), and that is what
  moves it to spawning.
- The bot path must keep working: a bot has no client to download the map, so
  it skips to `STATE_JOINING` and picks a team (that code is in the current
  `Player.c` — keep it).

The 1.0 path (`STATE_PROTO10`, `Proto10/Session.c`) stays untouched.

### 8.3 The senders

The full list of the 0.75 senders the history has
(`git show ba72777^:server/Source/Server/Packets/Packets.h`):

```
send_restock            send_move_object        send_intel_capture
send_intel_pickup       send_intel_drop         send_grenade
send_player_left        send_weapon_reload      send_weapon_input
send_set_color          send_set_color_to_player send_set_tool
send_block_line         send_block_line_to_player send_block_action
send_block_action_to_player  send_state_data   send_input_data
send_kill_action_packet send_set_hp             send_existing_player
send_map_start          send_map_chunks         send_create_player
send_respawn            send_world_update       send_position_packet
send_version_request
```

Plus `Util/Notice.c` (`broadcastServerNotice`), deleted by `ba72777`.

Files that were deleted and come back whole: `MapStart.c`, `MapChunk.c`,
`StateData.c`, `CreatePlayer.c`, `WorldUpdate.c`, `MoveObject.c`, `Restock.c`,
`IntelCapture.c`, `IntelPickup.c`, `IntelDrop.c`, `Util/Notice.c`.

Files that kept their names but lost their 0.75 output: `SetHP.c`,
`KillAction.c`, `PlayerLeft.c`, `ExistingPlayer.c`, `BlockAction.c`,
`BlockLine.c`, `Message.c`, `SetTool.c`, `SetColor.c`, `WeaponInput.c`,
`WeaponReload.c`, `Grenade.c`, `HostCommand.c`, `PositionData.c`,
`VersionRequestResponse.c`, `ShortPlayerData.c`, `InputData.c`,
`ChangeTeam.c`, `ChangeWeapon.c`, `Hit.c`.

- **Do not change the 1.0 build's copies of these files.** Put the 0.75
  output in `Legacy/` and let the profile choose the implementation. If a
  file must hold both, guard it with the profile — but prefer new files in
  `Legacy/`.

### 8.4 The transport migration (mandatory)

The 2026-10-03 code sent with `enet_peer_send(player->peer, ...)`; today's
transport is `conn_send` (`server/Source/Server/Net/Conn.h`):

```
conn_send(conn, channel, data, len, reliable)
```

Every restored 0.75 sender must use `conn_send(player->conn, 0, data, len, 1)`
— **channel 0, reliable** — because the same bytes must travel over the
WebRTC transport (the browser), and the 0.75 protocol is single-channel and
reliable. Use `conn_send_reliable` (`Net/Conn.c`) so the 0.75 bytes also show
up in the network statistics.

Two mandatory guards in every restored sender:

1. `player->conn == NULL` means a bot: return early (this is the historic
   crash: `enet_peer_send` with a NULL peer).
2. `PROTOCOL_075 == 0` is **also the value every bot has** (`player_t` is
   calloc'd), so a protocol check must always test `player->conn != NULL`
   first. Copy the existing shape:
   `p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME`.

### 8.5 The per-tick broadcast

0.75 expects a full world snapshot per client per tick: `send_world_update`
(`WorldUpdate.c` in the history). Wire it into the tick (`Server.c`) in the
0.75 profile, next to the 1.0 bridge tick (`proto10_bridge_tick`).

### 8.6 The masters: the public 0.75 ones and ours

`ba72777^:server/Source/Server/Master.c` announces to the classic master over
ENet at the hardcoded address `67.205.183.163:32886`, and
`Structs/MasterStruct.h` holds the state. That master is alive:
`http://services.buildandshoot.com/serverlist.json` currently lists live 0.75
servers.

- Restore the announce in `Legacy/Master075.c`, but **make the address
  configurable** (`[master075] address = "67.205.183.163:32886"`,
  `enabled = true`), not hardcoded.
- The announce reports the classic limits: `max_players` 32, the map name, the
  mode name, the server name.
- **The 0.75 profile announces to our master too** (`Net/Master.c`), because
  the browser's list is our master's (the web build asks no other list) and
  because the WebRTC signalling goes through it. The announce carries the
  profile's protocol (`"0.75"`), so the master's row says `game_version:
  "0.75"` and its identifier is the classic `aos://` form; the row must carry
  the 0.75 fields too (the name, the map, the mode, the players, the
  country), so the client's merge can replace the public master's row with
  ours.
- **A server in both lists is listed once, as ours.** The client's merge
  already does that (`hud_serverlist_add`: the "ours" row replaces the row
  with the same address and port, `hud_serverlist_same` splits both
  identifier forms). The test is that the 0.75 server appears **once** in the
  list, with our master's row, and that the browser finds it there.

### 8.7 Acceptance criteria

- The 1.0 server's behavior is unchanged: the same harness suite passes, the
  same connect data is accepted, the same log lines.
- A 0.75 client joins the 0.75 profile and plays:
  - our own client, **locked to 0.75** by the harness
    (`harness.py --proto-only 075 --join <port>`): the client speaks **only**
    0.75 here, so a successful join proves the 0.75 lane,
  - the harness `--vanilla` mode (against the unmodified SpadesX server), which
    keeps the client's 0.75 path tested against a foreign server,
  - the **negative test**: the same client with `--proto-only 075` against the
    1.0 server must fail with "wrong protocol" (the lock keeps it out of the
    1.0 lane, so the test cannot pass over it),
  - the server log shows the join, the map transfer, the team choice and the
    spawn.
- New log lines:
  - `EVT wire profile=075 action=connect id= address=`,
  - `EVT wire profile=075 action=map_sent bytes= chunks=`,
  - `EVT wire profile=075 action=state_data id=`,
  - `EVT wire profile=075 action=world_update bytes=`.

## 9. Phase: the web client speaks 0.75

The web build currently refuses anything but protocol 1.0, in
the client's `transport_webrtc.c`:

```
if(connect_data != P10_CONNECT_MAGIC)
    return -1;
```

That single check is what stops the browser from reaching a 0.75 server. The
client already contains the **complete 0.75 implementation** (it joins public
0.75 servers today); it is compiled into the web build and simply never
reached.

Changes:

1. the client's `transport_webrtc.c`: accept the 0.75 connect data as well. The
   address check (`master_find_id`) stays: the browser may only join servers
   the master knows (section 15).
2. The join order on the web: `network_connect` already tries 1.0 first and
   falls back to 0.75/0.76 (the client's `network.c`). Against a 0.75-only
   server the 1.0 attempt fails and the 0.75 one must succeed — verify the
   path, and make sure the web build does not wait for a 1.0 greeting that
   will never come (the 1.0 wait is `P10_GREETING_WAIT`, 5 s).
3. The 0.75 path uses **one** reliable channel (`transport_connect(...,
   version == P10_CONNECT_MAGIC ? NETWORK_CHANNELS : 1, version)`). The
   WebRTC transport opens both DataChannels regardless; the second stays
   idle. Confirm the 0.75 client code sends everything on channel 0.
4. The WebRTC transport carries **0.75 bytes** in this profile.
   the client's `net.js` documents "a binary one is one protocol 1.0 packet" —
   update the comment; the code is payload-agnostic.
5. The browser's control channel ("connect <n>") carries the connect data:
   the browser must send `3` for the 0.75 lane.

**This client change is additive and must not affect the 1.0 server or the 1.0
lane**: the native client already prefers 1.0 and only falls back to 0.75.

Acceptance:

- `make client-web` builds; the page loads; the browser joins the 0.75 server
  and plays (the map arrives as VXL chunks, the team screen works, the HUD
  works).
- The same browser still joins a 1.0 server (the 1.0 lane is unchanged).
- `CEVT net action=connect protocol=0.75 channels=1` in the browser's console
  log.
- The browser honours the protocol lock (section 11): with it on, the page
  joins only the 0.75 lane, and the negative test against a 1.0 server fails.

## 10. Phase: the public server

### 10.1 A tolerant preset for original 0.75 clients

Our server is stricter than the pyspades servers a 0.75 player is used to. A
vanilla BetterSpades client can get kicked for things the old servers allowed.
Add a profile section in `server/Resources/config-075.toml`:

```
[profile.075]
# The rules a 0.75 client (BetterSpades, pyspades) expects from a public server.
anticheat_protocol_violation = false   # a packet outside the standard is logged, not a kick
grief_warn_points = 20
grief_kick_points = 40
afk_kick_seconds = 300
spawn_protection_seconds = 0
```

`Conduct.c`, `Anticheat.c` and `SpawnProtect.c` already read their own config
sections; make them profile-aware (the 0.75 profile's defaults are the values
above). Measure it: a 0.75 client must be able to play a full match without
being kicked or warned.

### 10.2 The browser, our master and the duplicate servers

The browser only joins servers **our master knows** (`transport_webrtc.c`
refuses an address the master does not list), and the signalling (the offer
and the answer) goes through our master. So the 0.75 profile announces to our
master as well as to the public 0.75 masters (section 8.6).

Our master is **our own code** (a separate, private
repository, a Go program). Reading it settles what has to change there:

- **The list's rows are already the 0.75 master's shape**
  (`registry.go`: "The first fields are the 0.75 master's, so the desktop
  client's parser reads this list unchanged"), so a 0.75 server's row is
  readable by a vanilla client as it is.
- **The announce accepts any key** unless the deployment sets an allowlist
  (`newRegistry(perIP, allowed)`, `registry.go`), so nothing blocks a 0.75
  server from announcing.
- **The signalling is protocol-agnostic** (`POST /v1/poll`, `/v1/answer`,
  `/v1/servers/{id}/offer`): the server polls for the browsers' offers and
  answers them, whatever the game protocol is.
- **Two things are hardcoded for 1.0** and must follow the announce:
  - `registry.go`: the row's `GameVersion` is the literal `"1.0"` → the
    announce gains a `protocol` field ("0.75" or "1.0") and the row publishes
    it;
  - `registry.go`: the identifier is always `aos10://ip:port` → the prefix
    follows the protocol (`aos://` for 0.75, the classic form, so a
    third-party 0.75 client reads the row; our client reads both). For a 0.75
    row it should be the **classic numeric form** (`aos://<the address as
    the 32-bit word>:port`), the one the public 0.75 list carries: the old
    master's address was ENet's `ENetAddress.host`, a word, and the old
    clients read it with a number (`network_identifier_split` in the client
    still does: `strtoul` and the four shifts).

On our side, two changes:

- **`Server/Net/Master.c`**: the announce carries the profile's protocol.
- **the client's `hud.c`**: the row's `ours` flag (which decides the HELLOINFO
  request, `hud.c`: "HELLOINFO is ours alone, a 0.75/0.76 server is not sent
  it") must come from the row's `game_version`, not from the list the row
  came from. Today `e.ours = ours` means "it came from our master's list",
  which would be wrong for a 0.75 server in that list: it would be sent a
  packet it does not know. The LAN path already does it right
  (`ping.c`: `e.ours = version && !strcmp(version, "1.0")`).

**The duplicates need no work**: the client already lists a server that is in
both lists once, as ours (`hud_serverlist_same` compares the address and the
port, whatever the identifier's form). The test is that the 0.75 server shows
up **once** in the list, and that the browser joins it over WebRTC.

### 10.3 STUN and TURN

`[webrtc] stun` currently takes a single STUN URL and `Net/RtcPeer.cpp` puts
one entry in libdatachannel's `iceServers`. For a public server, players
behind symmetric NAT (many mobile operators) will not connect with STUN alone.

- Make `[webrtc] stun` accept a comma-separated list, and add `[webrtc] turn`
  / `[webrtc] turns` (with the credentials) so libdatachannel's `iceServers`
  can carry TURN entries.
- The **browser** needs the same list: it currently uses its own `SP_STUN` /
  `WEB_STUN_DEFAULT` (the client's `transport_webrtc.c`). Publish the server's
  ICE server list in the master's row and have the browser use it (falling back
  to its own default).
- Test from a phone on a mobile network, not only from a desktop.

### 10.4 The page (out of this repository, but required)

`docs/web.md` phase 5 is still open: the wasm, the data package and the page
have to be served over **HTTPS** with the **COOP/COEP** headers (the chunk
workers need `SharedArrayBuffer`). That work lives in the website repository.
Requirements to hand over: HTTPS, `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`, the data package cached, and a
"click to play" overlay.

## 11. Phase: the client's protocol lock (harness only)

To test the 0.75 lane, the client must be **locked to 0.75**: it may not fall
back to 1.0, or a broken 0.75 lane would hide behind a successful 1.0 join.
The lock is a **debug tool, reachable only through the harness** — never a
setting in `config.ini`, never a line in the Settings screen, never something
a player can find. The client's default behavior stays what it is today: try
1.0, then 0.75, then 0.76 (the client's `network.c`, `network_connect`).

- **The lock**: a new environment variable `SP_PROTO_ONLY`, read next to
  `SP_PROTO10` (the place the protocol is chosen, `network_identifier_split`):
  - `SP_PROTO_ONLY=075` → try **only** protocol 0.75 (`VERSION_075`),
  - `SP_PROTO_ONLY=076` → only 0.76,
  - `SP_PROTO_ONLY=10` → only the 1.0 magic.

  With the lock set, `network_connect` attempts that one protocol and **fails
  when it does not connect**: no fallback, no second attempt. Without it,
  nothing changes (the phased fallback).
- **The harness**: a new flag `--proto-only {075,076,10}` that sets the
  variable in the client's environment (the harness already sets the `SP_*`
  variables for the client). It is orthogonal to `--vanilla` (which starts the
  unmodified SpadesX server) and to `--proto10` (which forces the 1.0 attempt
  through the `aos10://` prefix); `--proto-only` is the strict version of
  both.
- **The web build**: the same lock applies to the WebRTC transport
  (the client's `transport_webrtc.c`), so a browser can be tested against the
  0.75 lane without the 1.0 attempt.
- **The log**: the client says it is locked —
  `CEVT net action=proto_lock protocol=075` — so a test's log proves the
  client could not have used the 1.0 lane.
- **The documentation**: the harness README only. The client's own docs do
  not mention it: it is not a feature, it is a test instrument.

## 12. Phase: the config says what it does not read

A setting the server does not read is ignored in silence: a typo, or a key
that belongs to the other profile (a `[perks]` section in the 0.75 config, a
`[profile.075]` section in the 1.0 one). The server must **say so in the log,
at every start**, not only when `--check-config` is asked for.

- **The registry**: one table of the keys the server reads, per section. Put
  it in `server/Source/Server/ConfigKeys.c`, or in `ConfigCheck.c` next to the
  validator that already knows most of the keys. For each section: the keys
  it holds, the keys inside its sub-tables, and the names of its sub-tables
  when they are known (the perk names, the class names, the team names) or
  free (a plugin, a preset).
- **The check**: walk the parsed config and warn about every key that is not
  in the registry:
  - a key in a known section →
    `Config: [server] unknown key 'prot' (a typo, or a setting of another profile?)`
  - a sub-table whose name is not known →
    `Config: [perks] unknown table 'unknown_perk' (a typo, or a setting of another profile?)`
  - a top-level table that is not known → the "unknown top-level key"
    warning that exists today, but it must also reach the log.
  - **never** inside a plugin's own `[plugins.<name>.config]`: those keys are
    the plugin's business.
- **Where it warns**: at every start-up (the log) **and** in `--check-config`
  (the same lines, so the two agree).
- **The profile decides the registry**: the 0.75 profile warns about the
  1.0-only keys and the 1.0 profile about the 0.75-only ones. That is what
  makes "the configurations do not interfere" (section 4.3) true and visible:
  a `[perks]` section left in `config-075.toml` says so at every start.
- **The rule**: a new config key goes into the registry in the same commit
  that reads it. The shipped `config.toml` must pass `--check-config`
  **without a single warning** (the CI checks it), so a key that is read but
  not registered is caught there instead of warning on every start.

**Acceptance:**

- `--check-config` on the shipped `config.toml`: no warnings.
- A config with `[server] prot = 32887` logs the warning at start-up and in
  `--check-config`.
- The same config run with `--profile 075` warns that the profile does not
  read the 1.0-only sections it holds.
- A plugin's own `[plugins.<name>.config]` keys never warn.

## 13. Phase: packaging, CI and docs

- The binary stays one (`ngspades-server`); `tools/package_release.sh`
  packages it as before. Add the 0.75 profile to the package's notes (the
  flag and the config file).
- `.github/workflows/build.yml`: the job builds the server as it does; add a
  step that starts the server with `--profile 075` and a smoke test (a join,
  a map transfer, a spawn).
- `common/game.cmake`: nothing to change (one binary, one name).
- Docs, in this order:
  - `CONTRIBUTING.md`: rules 1 and 5 today state that the server speaks 1.0 only
    and has no 0.75 send path. Rewrite them: the server has two profiles, the
    default is 1.0 only (unchanged), `--profile 075` is the 0.75-only server,
    the gameplay is shared, and the 0.75 wire lives in `Server/Legacy/`. Also
    document the config/data separation (4.3), the shared-features rule
    (section 6), the plugin's version declaration (section 5) and the
    `/profile` command.
  - `docs/architecture.md`: the two profiles and the shared-source rule.
  - `docs/protocol-1.0.md`: the 0.75 wire is back in a second profile; the
    1.0 server is untouched.
  - `docs/web.md`: phase 5 (publishing) and the browser's 0.75 mode.
  - the client's `README.md`: the harness's `--proto-only` flag (the
    only place the protocol lock is documented).
  - The issues: close the ones this delivers.

## 14. The order of work

| Step | What | Estimate |
|---|---|---|
| 1 | The profile plumbing: the enum, `--profile`, the config/data separation, the banner, `/profile` | 1 day |
| 2 | The client's protocol lock (`SP_PROTO_ONLY`) and the harness's `--proto-only` (section 11) | 0.5 day |
| 3 | The 0.75 dispatcher and the join state machine (the map transfer included) | 1-2 days |
| 4 | The senders, migrated to `conn_send`, plus the shim (`Legacy/Wire075.c`) | 2-3 days |
| 5 | The per-tick world update and the public 0.75 masters announce | 1 day |
| 6 | The profile's feature gates (no perks, no classes, no squads, no assists, no spotting, no revive) and the classic-mode registry (section 6) | 1-2 days |
| 7 | The config says what it does not read: the registry, the walk, the log (section 12) | 1 day |
| 8 | The plugins' version declaration (section 5) | 0.5-1 day |
| 9 | The ZeroSpades extensions on the server (ExtInfo, Player Properties, Damage Markers, Teamplay) | 2-3 days |
| 10 | The web client speaking 0.75 | 1-2 days |
| 11 | The tolerant preset, TURN, the master's WebRTC row | 1-2 days |
| 12 | The master: the announce's protocol, the row's `game_version` and the identifier's prefix (a separate, private repository), and the client's `ours` flag (the HELLOINFO decision) | 0.5-1 day |
| 13 | The packaging, the CI, the docs | 1 day |

Total: roughly **three weeks** of focused work, plus the operational work for
the public server (the page hosting, the master, the address).

## 15. The traps (all of them were real bugs once)

1. **`PROTOCOL_075 == 0` is also every bot's value.** `player_t` is calloc'd,
   so a bot reads as protocol 0.75. Every protocol branch must test
   `player->conn != NULL` first.
2. **A NULL conn is a bot**, never a client: every sender returns early.
3. **The 0.75 client computes collapses and craters itself.** Do not send
   terrain deltas to it; send block actions and let it work it out (that is
   why `proto10_world_cell` maps to the 0.75 block/world output, not to a 1.0
   `TerrainUpdate`).
4. **The 0.75 map transfer is the VXL**, in chunks, with the map start packet
   first. A 0.75 client cannot read the ngmap format.
5. **CP437, not UTF-8**, on the 0.75 wire.
6. **0.75 has one reliable channel.** Everything goes on channel 0.
7. **The version handshake is 0.75's own** (`PACKET_VERSION_REQUEST` /
   `VERSION_RESPONSE`): copy the old code, do not invent one.
8. **The block rate limit and the anchor rule** are the 0.75 ones
   (`BLOCK_DELAY`, a solid neighbour required): they are unchanged, keep them.
9. **The server paces its tick at 60 Hz** and must not spin: keep the current
   pacing in both profiles.
10. **The two profiles must never share a port, a config file or a data
    folder**: that is what makes running both at once safe (section 4.3).
11. **The extension packets go only to the clients that agreed** (the ExtInfo
    answer), and only in the 0.75 profile. Offering an extension the server
    does not implement is a broken promise: our client agrees to the three
    BetterSpades ones it knows but cannot read, so never offer 0xC0, 0xC1 or
    0xC2.
12. **The colour order on the extension wire is blue, green, red** (the client
    reads `{d[20], d[19], d[18]}` as {b, g, r}), not the red, green, blue the
    rest of the 0.75 protocol uses in some places. Copy the client's reader.
13. **A plugin that says nothing about its profile is loaded everywhere**: the
    existing plugins keep working, and the 1.0-only calls they make are
    refused with a reason, not silently ignored.
14. **A server in both lists is listed once.** The client's merge replaces the
    0.75 master's row with ours when the address and the port match; the row
    we publish must carry the 0.75 fields too (the name, the map, the mode,
    the players, the country, the `game_version`), or the list shows an empty
    row.
15. **HELLOINFO goes only to the 1.0 servers.** A 0.75 server in our
    master's list does not know the packet: the client's `ours` flag (which
    decides it) must come from the row's `game_version`, not from the list
    the row came from.

## 16. Still open

1. **The master's two hardcoded 1.0 bits** (`registry.go`: the row's
   `game_version` and the identifier's prefix) are ours to change — the master
   is our own repository, a separate private one — and section 10.2 says
   exactly what to change. It is a separate repository, so it is a separate
   change (and a separate commit), not a file in this tree.
2. **The gates' exact shape.** The features the 0.75 profile makes inert
   (section 6.2) are few and each has one place: the perk grant, the class
   choice, the squads' wire, the assists' announcement, the `spot` command,
   the revive, the mode registry. Which of them are a runtime check and which
   are a compile-time one (the file is only in the 1.0 build) is the
   implementer's choice — one binary compiles both, so a runtime check is the
   straightforward answer.

## 17. Out of scope

- The 1.0 server's behavior (it does not change; the default profile is
  exactly today's server).
- The 1.0 inventions in the 0.75 profile: the perks, the classes, the
  squads, the kill assists, the 3D spotting, the revive.
- The ngCTF, zombie, arena and creative modes in the 0.75 profile.
- A mixed match (0.75 and 1.0 clients in the same match): the profile is
  exclusive, one server is one protocol.
- Porting the bot AI: the bots are already shared source, so nothing to do.
- The 1.0 client's features that only work on a 1.0 server (the class screen,
  the perk HUD): the 0.75 client path already handles their absence.
- The ZeroSpades extensions the client does not implement (256 players,
  message types, the kick reason).
- A server configured only by environment variables (the config file stays
  mandatory).
