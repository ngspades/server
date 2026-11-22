# Writing a protocol 1.0 client

Protocol 1.0 is the protocol of the ngspades-server. It starts from
the wire format of OpenSpades' unfinished `ngspades` branch ("WorldOfSpades
0.1"), with its bugs fixed and the packets a whole game needs added. Our server
speaks **only** 1.0; our client speaks 1.0 and falls back to 0.75/0.76 for
public servers.

This document is the roadmap: what a client must do, in the order it has to do
it, with what our client (the client's `proto10*.c`) does about each step. The
byte layout of every packet is in the dossier,
[protocol-1.0.md](../protocol-1.0.md), and the section numbers below (§)
point into it. The C codec in `common/proto10/` (encoders, decoders and tests
for every packet) is plain C99 with no dependencies, so a new client can use it
as it is.

See also [server-1.0.md](server-1.0.md), and [client-0.75.md](client-0.75.md)
for the coordinate system and game rules both protocols share.

---

## 1. Transport

| Setting | Value |
| --- | --- |
| Transport | ENet 1.3 over UDP (the server uses enet6, wire-compatible; encryption off), IPv4 and IPv6; WebRTC DataChannels in a browser (§3.4) |
| Port | 32887, the same as 0.75 |
| Connect data | `0x08101919` |
| Channels | 2. Channel 0 reliable for everything; channel 1 unreliable sequenced, for movement only (our server sends everything on channel 0 for now; read both) |
| Compression | the ENet range coder, as in 0.75 |
| Framing | one ENet packet = `u8 id` + payload |

Ids below 128; an id you do not know must be **ignored** (not a reason to
disconnect), so the protocol can grow (§3.2, §12.1).

Disconnect reasons have their own table (§3.3): 0 unknown, 1 internal error,
2 full, 3 kicked (a `Kick` packet with the text came first), 4 stopped,
5 timeout, 6 malformed packet, 7 protocol mismatch.

A vanilla server answers the 1.0 connect data with reason 3 ("wrong
protocol") and no `Kick`: take that as "not a 1.0 server" and reconnect with
`3` (0.75).

**Our client** (`network.c`) tries 1.0 first, then 0.75 and 0.76. It waits up
to 5 s for the `Greeting`, and a server that accepts the magic but then speaks
0.75 is dropped and reconnected as 0.75. Addresses from our master look like
`aos10://203.0.113.5:32887` or `aos10://[2001:db8::5]:32887`.

---

## 2. Encoding

Read §4 before anything else; every packet uses these primitives:

- fixed-width integers and floats, **little-endian**;
- `varint` (LEB128, at most 10 bytes) and `svarint` (zigzag);
- `bytes` and `string` with a varint length (up to 1 MiB). Strings are UTF-8,
  with no NUL inside;
- `map`: key/value strings ending with an empty key;
- colors in **B, G, R** order.

A decoder must reject a truncated packet, an over-long varint and a string with
a NUL in it. `common/proto10/buf.c` does all of this.

**Text is UTF-8 everywhere.** There is no CP437 in 1.0: the server converts
names and chat when it bridges to 0.75 data.

---

## 3. Handshake

```
ENet connect (0x08101919)
          <-- Greeting (1): "Hello", server nonce
InitiateConnection (2) -->
          <-- ServerCertificate (3): isValid = 0
ClientCertificate (4): isValid = 0 -->
          <-- GameStateHeader (6)
          <-- MapData (7) × N
          <-- MapDataFinal (11)
MapDataAcknowledge (10) -->          (after decoding the map)
          <-- GameStateFinal (8)
GenericCommand "join <team> <weapon>" -->
          <-- GenericCommand "local-player <id>"
          <-- EntityUpdate / PlayerUpdate / TerrainUpdate ...
```

### 3.1 InitiateConnection (§6.2, §12.1)

| Field | What to send |
| --- | --- |
| protocolName | exactly `"WorldOfSpades 1.0"`; anything else gets reason 7 |
| major, minor | the protocol revision, currently 1 and 3; informational (below) |
| revision, package, environment | your build, free text |
| locale | e.g. `pt_BR` |
| playerName | UTF-8, up to 63 bytes |
| nonce | 256 random bytes |
| mapQuality | 1..100 for an `ngmap` map at that quality, 0 to ask for plain VXL |

The version is informational: the server logs it and negotiates nothing on
it. Every 1.0 client gets the whole protocol, including the jetpack bits in
entity flags and input (§12.4), the compact player trajectory (§12.7) and the
three weapon slots of a player entity (§8.4), so a client must decode all of
them. The game has not been released, so 1.0 keeps no backward compatibility
with itself yet: a wire change updates the server and the client together
(§12.1).

**Our client** (`proto10.c`) announces 1.3 and sends `mapQuality` 100: our
server sends the map's stored ngmap q100 whatever it asks.

### 3.2 Certificates (§6.3)

Send `ClientCertificate` with `isValid = 0`. Nothing is verified yet; the
packets are kept for accounts later.

### 3.3 The map (§7)

1. `GameStateHeader` is a `map` of properties: `map-name`, `map-size`
   (`512,512,64`), `map-bytes`, and **`map-format`**: `vxl` or `ngmap`. Trust
   that key, not what you asked for, because the server falls back to VXL when
   it cannot encode ngmap.
2. Append every `MapData` fragment.
3. On `MapDataFinal`, decode the whole stream:
   - `vxl`: zlib-compressed VXL, as in 0.75;
   - `ngmap`: zlib-compressed column geometry plus JPEG colors (§7.5).
     `common/ngmap/` decodes it to VXL bytes.
4. Apply, in order, every `TerrainUpdate` that arrived during the transfer,
   **then** send `MapDataAcknowledge`.
5. `GameStateFinal` brings the world parameters (§7.6: gravity, jump
   velocity, jetpack numbers when the server has one) and every entity and
   player already in the match, each with the Create bit.
   Our client takes `fog-distance` (32 to 256 blocks) as its fog: where
   the fog ends and how far players and entities are shown. The map is
   drawn to the player's view distance setting; the fog closes in only
   when that is shorter.

**Our client** (`proto10_world.c`, `network_load_map_ngmap`) loads both formats
through the same VXL loader and logs the format, size, download and decode
times. Only 512 × 512 × 64 maps are accepted.

### 3.4 Joining (§10)

The game starts with you watching. Send a `GenericCommand`:

| Command | Meaning |
| --- | --- |
| `join <team> <weapon> [<class>]` | team 0 or 1 (255 = spectator), weapon 0 rifle, 1 SMG, 2 shotgun; a class in class modes (§12.6) |
| `team <n>`, `weapon <n>` | change later, from the in-game menus |
| `leave` | back to watching |

The server answers `local-player <id>`: from then on, player `<id>` and entity
`<id>` are you.

---

## 4. The world

### 4.1 Players and entities (§8)

1.0 separates a **player** (name, team, score, flags: `PlayerUpdate` 50,
`PlayerRemove` 51) from its **body**, an entity (`EntityUpdate` 30,
`EntityRemove` 33). A player body has the same id as its player. Grenades,
rockets, flags and command posts are entities too.

An entity item (§8.4) is a varint id, a mask, and only the parts the mask
announces: create (with the entity type; for a player, its three weapon slots
and their skins, §8.4 revision 1.3), flags, trajectory, input, tool, block
color, health. Updates are **deltas**: keep the last value of each part and
change only what the mask says.

A player body's Tool field names what it holds: `Weapon1` the class's primary
gun, `Weapon2` its second gun (the revolver; a body in a class without one
never shows it, and that class's second `WeaponParameters` is all zero with
magazine 0), `Weapon3` the grenades or the Miner's drill. Another player's gun
is read from that field, not from a GenericCommand (revision 1.3 removed `gun`
and `gun-swap`).

### 4.2 Trajectories (§8.5, §12.7)

Positions arrive as a trajectory to extrapolate, not as samples:

| Type | Moves |
| --- | --- |
| constant | stays at `origin` |
| linear | `origin + velocity × t` |
| gravity | linear plus gravity (`(0, 0, +32)` blocks/s², z down) |
| player | origin, velocity and euler angles of a player; run the player physics from there |
| rigid body | origin, velocity, rotation and angular velocity |
| player compact | the player trajectory in 17 bytes; the server sends every body in it |

Coordinates are the 0.75 ones: `z` grows downwards (section 2.1 of
[client-0.75.md](client-0.75.md)).

**Our client** keeps its `players[]` array as the model and converts every
trajectory into it (`proto10.c`, `proto10_entities.c`); grenades and rockets go
into a small entity table and are extrapolated every frame.

### 4.3 Your own body (§8.7, §8.13)

Report your body with `ClientSideEntityUpdate` (35): your trajectory (position,
velocity, euler angles) and your input (fire buttons, sprint, stance,
`xmove`/`ymove`). Jumping and reloading are actions: `PlayerAction` (60) with
`Jump` or `ReloadWeapon`.

**Our client** sends it 30 times a second, unreliable on channel 1. Right before
a hit or a block edit (both reliable on channel 0) it sends one reliably, so the
server sees the input that produced them first.

The same item carries the tool slot you hold, and for a gun that slot is the
choice: send `Weapon2` to take the class's second gun and `Weapon1` to take the
primary back (the server swaps the clip and reserve, and drops the change when
it refuses: no second gun, dead, or within 0.25 s); `Weapon3` is the grenades
or the Miner's drill. A `HitEntity` says which slot fired (`Weapon1` or
`Weapon2`) and the server refuses it if that is not the gun in hand
(`hit_refused why=slot`). The rocket launcher and the drill have no hit to
report: the trigger fires them. Send the input update at once, reliable, when
you fire, with `toolPrimary` set, and with either in hand set that bit only for
a press you fired on (until the button is released): the server fires on the
bit's press, from its own copy of your eye and look.

### 4.4 Shooting and damage (§9)

| Packet | Direction | Use |
| --- | --- | --- |
| `HitEntity` (90) | C→S | your shot or melee hit an entity: where you fired from, where it hit, the hit type |
| `HitTerrain` (91) | C→S | your shot or spade hit a block (digging is `HitTerrain` with damage type `Dig`) |
| `Damage` (92) | S→C | you lost health: amount, damage type, where it came from |
| `EntityDie` (32) | S→C | an entity died, with the damage that killed it (the killer is its `fromEntity`). Our server puts the 0.75 kill kind in the low byte of `param` and the respawn seconds in the next byte (§8.9) |

Like in 0.75 the client reports its hits and the server decides the damage.
**Our server** keeps a block's health for the guns: send a `HitTerrain` for
every pellet that meets a block (not only the one your count says breaks
it) and draw the crack from that cell's health in the tick's `TerrainUpdate`
(§9.3, §10); the spade's hits still count on the client, which sends the one
that breaks the block.

### 4.5 Building, grenades, chat (§12.2)

| Packet | Use |
| --- | --- |
| `BuildBlock` (93) | place one block with your color |
| `BuildLine` (94) | place a line of blocks |
| `ThrowGrenade` (95) | throw a grenade (fuse, position, velocity) |
| `TerrainUpdate` (40) | the server's block changes, created and destroyed cells in one packet |
| `ChatMessage` (70) | your chat line (all or team) |
| `ChatBroadcast` (71) | a chat line or a server notice to show |

`TerrainUpdate` already groups what changed together, so the batching that 0.75
needs (section 3.6 of [client-0.75.md](client-0.75.md)) comes for free: apply a
packet whole, then run the block physics.

### 4.6 Game modes (§12.2.3)

`GameModeState` (80) is a string map, like `GameStateHeader`: team names and
colors, scores, the capture limit, intel holders and positions, territories and
their progress. A packet whose first key is `mode` (`ctf`, `ngctf`, `tc`...) is
a snapshot: forget everything and apply it. Any other packet is a delta: each
key replaces its old value, and an empty value removes the key. Ignore keys you
do not know; for a mode you do not know, show the scores. It replaces 0.75's
`StateData` mode part, `IntelPickup`/`Capture`/`Drop`, `TerritoryCapture` and
`ProgressBar`. The respawn delay comes with the death, in `EntityDie`'s
`param` (section 4.4).

### 4.7 Our additions (§12.4–12.6, §10)

Our server can also send:

- `GenericCommand`s for the hit and kill markers (`hit`, `kill`), spotting
  (`spot`), perks (`perk-*`) and the bot menu (`host-status`). Ignore the ones
  you do not use.
- Classes, perks, the jetpack and the sentry gun, announced through world
  parameters, entity flags and `GameModeState`.

A minimal client can ignore all of them and still play.

---

## 5. Minimum for a working client

1. ENet with 2 channels and the 1.0 connect data; ignore unknown packet ids.
2. The primitive codec, with the strict decoder rules (§4).
3. Handshake: `InitiateConnection` with `"WorldOfSpades 1.0"` and
   `mapQuality` 0 so you only need the VXL loader (the reference server;
   ours sends ngmap whatever is asked, so a client for it needs the ngmap
   decoder).
4. Map transfer with the `TerrainUpdate`s kept until the map is loaded.
5. `GameStateFinal`, `EntityUpdate`, `PlayerUpdate`, `PlayerRemove`,
   `EntityRemove` with the delta rule, the player trajectory and its compact
   form, and the jetpack bits (at least decoded and ignored).
6. `join`, `local-player`, then `ClientSideEntityUpdate` at a steady rate.
7. `HitEntity`, `HitTerrain`, `Damage`, `EntityDie`.
8. `BuildBlock`, `BuildLine`, `ThrowGrenade`, `ChatMessage`, `ChatBroadcast`.
9. `GameModeState` for CTF.

Then ngmap (§7.5) and the rest of the additions in §12.

---

## 6. Testing against our server

- Run it locally: `make server && make run-server` (it listens on 32887).
- Every handshake step and every refused packet is logged as an `EVT` line
  with the reason, which is the quickest way to see what the server did not like.
- `common/proto10/tests/` has golden vectors for every packet; encode and
  decode them with your codec first.
