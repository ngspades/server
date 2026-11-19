# Writing a 0.75 server

What a server must do so that Ace of Spades 0.75 clients (the Voxlap client,
OpenSpades, BetterSpades and the Shoot and Destroy client) can join and play.

Where it comes from: the ngspades-server is a SpadesX fork that served
0.75 until it switched to protocol 1.0 only, and its 0.75 layer was removed in
commits `ba72777` and `da14df7`. Everything below is how that layer behaved,
plus what the 0.75 clients expect. The packet layouts are in
[client-0.75.md](client-0.75.md); read it first, since it defines the
coordinates, the enumerations and the text encodings this document relies on.

**Must** marks what clients break without. **Should** marks what they cope
without but players notice.

---

## 1. Transport

| Setting | Value |
| --- | --- |
| Protocol | ENet 1.3 over UDP |
| Port | 32887 by convention |
| Peers | your player limit (32 is the classic one; up to 256 player ids exist) |
| Channels | accept 1; clients ask for one |
| Compression | **must** be the ENet range coder (`enet_host_compress_with_range_coder`) |
| Connect data | `3` = 0.75, `4` = 0.76. Refuse anything else with reason 3 |

- **Must**: send every packet reliable except `WorldUpdate` (2), which goes
  unreliable since the next one replaces it.
- **Must**: one game packet per ENet packet: an id byte, then the payload, all
  little-endian and packed (section 2 of [client-0.75.md](client-0.75.md)).
- **Must**: check the length of every packet a client sends before reading it,
  and check its `player_id` field against the sender's own id. Clients have
  bugs, and cheats exist.

### 1.1 Refusing and dropping clients

Disconnect with a reason in the ENet data:

| Reason | When |
| --- | --- |
| 1 | banned |
| 2 | kicked. Some clients (BetterSpades, piqueserver's habit) read it as "too many connections" |
| 3 | wrong protocol version (connect data not 3/4) |
| 4 | server full |
| 5 | server shutting down |
| 8..11 | banned, full, kicked, wrong version, after the map download. The Voxlap client only knows 0..4 during the download and 8..11 afterwards |
| 20 | invalid name |

Clients cannot tell a reason 0 from a timeout, so do not use 0 on purpose.

Our client also shows, as the reason, a system chat message sent by "player
255" just before the disconnect. That is how a kick message can carry text.

---

## 2. A connection, step by step

```
client                                   server
  ENet connect (data 3) ----------------->   check version, ban, free slot
            <----------------------- MapStart (18): compressed size
            <----------------------- MapChunk (19) × N
            <----------------------- VersionGet (33)                  (optional)
            <----------------------- ExistingPlayer (9) × each player already in
            <----------------------- StateData (15)
  ExistingPlayer (9): team, weapon, name -->
            <----------------------- CreatePlayer (12), to everyone
            <----------------------- WorldUpdate (2), at a fixed rate
```

### 2.1 Accept

On the ENet connect event:

1. Refuse a connect data that is not 3 (or 4, if you serve 0.76) with reason 3.
2. Refuse a banned address with 1.
3. Give the peer the lowest free player id; with none free, refuse with 4.

### 2.2 Send the map (**must**)

1. Write the map as VXL (512 × 512 × 64), compress it with zlib, and split the
   result into chunks. Our server used 8192-byte chunks.
2. Send `MapStart` with the total compressed size, then every chunk as
   `MapChunk`.

Clients accept chunks of any size and some sizes are faster to process, but the
total must match the size in `MapStart`. For 0.76, `MapStart` also carries the
CRC32 and the map name, and the client answers `MapCached` (31): when it says 1,
it already has the map and you may skip the chunks.

Our server built the compressed map once per connection, then sent the chunks
in one go. Throttle a large map if your ENet link is slow.

### 2.3 Introduce the game (**must**)

After the last chunk:

1. Send `ExistingPlayer` for every other player already in the game: id, team,
   weapon, tool, kills, block color, name.
2. Send `StateData`: the new client's own player id, the fog color, both teams'
   names (10 bytes each, NUL-padded) and colors, and the game mode (0 CTF,
   1 TC) with its state (scores, capture limit, intel holders or positions, tent
   positions; or the list of territories).

`StateData` is what ends the download on the client: it loads the map then.

`ExistingPlayer` has no position. **Should**: also send a `CreatePlayer` for
anyone who is alive and does not move on their own (our bots, for example), or
the new client draws them at the corner of the map until the next
`WorldUpdate` moves them.

### 2.4 Fingerprinting (optional)

`VersionGet` (33) asks the client for `VersionSend` (34): a client letter
(`'B'` for BetterSpades and our client, `'o'` for OpenSpades), three version
numbers and an OS string. `HandshakeInit` (31) with 4 bytes asks for
`HandshakeReturn` (32) with the same 4 bytes. The Voxlap client answers
neither, so never require them.

A `VersionGet` with property ids after the packet id (0 application, 1
locale, 2 feature flags) asks for the extended answer: `ExistingPlayer` (9)
whose first byte is `'x'`, then `id, length, payload` per property, one
packet per property, each repeating the ones before. Feature bit 0 says the
client reads UTF-8 chat. **Our server** sends the handshake and the extended
`VersionGet` only after a plain `VersionSend` came (a client that answers 34
speaks these packets), once a connection.

### 2.5 The join (**must**)

The client picks a team and a weapon and sends `ExistingPlayer` (9) with its
id, team, weapon, held tool, block color and name. When you get it:

1. Check the name: a length limit (15 characters is the classic one), no
   control bytes, and unique (append a number when it is taken).
2. Check the team: 0, 1 or spectator (−1, 255 on the wire). Balance the teams if
   you want to; the client takes whatever team `CreatePlayer` says.
3. Spawn the player (section 3.1) and send `CreatePlayer` to **everyone**,
   including the new player.

Older clients may join with `ShortPlayerData` (10: id, team, weapon) instead.
Accept both.

---

## 3. Running the match

### 3.1 Spawning (**must**)

On a spawn, the player gets 100 HP, 50 blocks, 3 grenades, a full magazine and
reserve, the gun in hand, and no input held. Pick a spawn point:

- CTF: in the team's spawn area, on the ground;
- spectators: anywhere, they are not alive.

Send `CreatePlayer` to everyone: id, weapon, team, position, name. The position
is the player's **eye**. The classic client draws the player at `z − 2`, two
blocks above the value sent, and our client uses the value as it is, so test a
spawn with the clients you care about.

### 3.2 The world update (**must**)

Send every client a `WorldUpdate` at a fixed rate: one `{position, orientation}`
pair per player slot, indexed by player id, 24 bytes each, for every slot up to
your player limit. (0.76: one byte of player id in front of each pair, and only
the players present.)

- Our server sent it 60 times a second per client (`ups`, per player).
- Fill a free slot, a dead player or one the receiver should not see with a
  zero position and the orientation `(1, 0, 0)`. Our client keeps the last known
  position of a player whose entry is all zeros.

### 3.3 Movement (**must**)

The client simulates its own movement and reports it:

- `PositionData` (0): about once a second. Accept it when it is close to
  where your simulation puts the player. Otherwise answer with `PositionData`
  to put the client back, which is what fixes speed hacks.
- `OrientationData` (1): many times a second. Normalize it before using it.
- `InputData` (3): the movement keys. Forward them to the other clients (with
  the sender's id), and run the same physics on the server.
- `WeaponInput` (4): fire buttons. Forward them; ignore them while the player
  sprints or reloads.
- `SetTool` (7) and `SetColor` (8): forward to the others.

The physics must match the clients' (pyspades' `world_c.cpp` is the common
reference): run speed, crouch, jumping, falling, climbing one block, and fall
damage (send `SetHP` with type 0).

### 3.4 Combat (**must**)

The client reports its own hits with `Hit` (5): the victim and the body part
(0 torso, 1 head, 2 arms, 3 legs, 4 melee). The server decides whether the hit
counts and how much it hurts. Our server:

- refused hits while the shooter sprints or has an empty magazine;
- checked the fire rate, the distance and the line of sight (`allow_shot`);
- dealt these classic damages:

| | Head | Torso | Arms | Legs |
| --- | --- | --- | --- | --- |
| Rifle | kill | 49 | 33 | 33 |
| SMG | 75 | 29 | 18 | 18 |
| Shotgun (per pellet) | 37 | 27 | 16 | 16 |
| Spade (melee) | 80 | | | |

Send the victim `SetHP` (5) with its new HP, type 1, and the shooter's position
as the source.

`Grenade` (6) from a client is a throw: fuse, position and velocity. Forward it
to the others, simulate it, and when it explodes, damage the players around it
and destroy the 3 × 3 × 3 blocks with a `BlockAction` of type 3.

`WeaponReload` (28) from a client starts a reload. When it finishes, answer the
shooter with the new magazine and reserve, and tell the others (so they hear
it).

### 3.5 Deaths and respawn (**must**)

When HP reaches 0, send `KillAction` (16) to everyone: victim, killer, kind
(0 weapon, 1 headshot, 2 melee, 3 grenade, 4 fall, 5 team change, 6 class
change) and the respawn delay in seconds. Count the kill for the killer unless
it was a suicide. When the delay is over, spawn the player again (section 3.1).

Changing team (`ChangeTeam`, 29) or weapon (`ChangeWeapon`, 30) during the
match is a death with kind 5 or 6, followed by a spawn with the new choice.

### 3.6 Blocks (**must**)

`BlockAction` (13) from a client: check it before you apply it, then send it to
everyone (with the sender's id).

| Type | Allowed when holding | Effect |
| --- | --- | --- |
| 0 | block, and blocks > 0 | build with the player's color |
| 1 | spade or gun | destroy one block |
| 2 | spade | destroy 1 × 1 × 3 (the block and the ones above and below) |

Our server refused an action that did not match the held tool ("BlockExploit"),
any action while sprinting, and building when building was disabled. Also
check the distance from the player (about 4 blocks to build or dig), the map
bounds, and that the bottom layers (z ≥ 62) stay whole.

`BlockLine` (14): a line of blocks between two points (3D DDA). Check the
player holds enough blocks and the ends are in reach, then send it to everyone.

After destroying blocks, run the **block physics**: everything no longer
connected to the ground falls and is removed. Clients run the same check on
their side; you need it for the intel, the tents and the players standing
there. When a tent or an intel loses the ground under it, move it down
(`MoveObject`, 11).

The clients count their own blocks, so restock them with `Restock` (26) when
they reach their tent.

### 3.7 Chat (**must**)

`ChatMessage` (17) from a client: type 0 to everyone, type 1 to the team. Check
the length, drop control bytes you do not want relayed, and send it back with the
sender's id. Server messages use type 2 (system). Many clients also show 3 big,
4 info, 5 warning and 6 error as popups.

Text is CP437 for the original clients. To send other characters, prefix the
string with byte `0xFF` and send UTF-8 (section 2.3 of
[client-0.75.md](client-0.75.md)); clients that do not know the prefix show a
`ÿ` and some garbage, so send it only to a client known to read it (its
extended version answer's feature bit 0, or a `0xFF` line it wrote). A
client's line starting with `0xFF` is UTF-8. Color codes `0x01`..`0x08` are
allowed in server messages.

Types 3 to 6 are the Message Types extension (0xC1): send them only to a
client that agreed, type 2 to the others. Kick Reason (0xC2): a type 2 line
from player 255 right before a kick is the reason the client shows; let it
leave the queue before the disconnect (ENet's `enet_peer_disconnect` drops
queued packets, `enet_peer_disconnect_later` does not).

### 3.7.1 Extensions (optional)

`ExtInfo` (60) lists the extensions you offer, `id, version` pairs; the
client answers with the ones it supports, and each extension's packets go
only to clients that agreed. The registry and the packets are in the
aosprotocol specification (https://github.com/zerospades/aosprotocol).
**Our server** offers Player Properties (0x00), Damage Markers (0x20),
Teamplay (0x30), Flashlight (0x32), Player Limit (0xC0, announced; it holds
32 players), Message Types (0xC1) and Kick Reason (0xC2). Flashlight: relay
a client's Light (sub 0) with the sender's id if you allow it (a refused one
is not relayed), send Light Config 255 and Light State after the State Data,
and turn a light off on Create Player, Kill Action, Player Left and a new
map without sending anything (the clients do the same).

### 3.8 Game modes

**CTF** (mode 0):

- Each team has an intel and a tent (`MoveObject` ids 0, 1 for the intels, 2, 3
  for the tents).
- Touching the enemy intel picks it up: `IntelPickup` (24).
- Carrying it to your own tent captures it: `IntelCapture` (23), 10 points to
  the player and 1 to the team, with `winning` set when the team reached the
  capture limit. Put the intel back with `MoveObject`.
- Dying drops it where the carrier fell: `IntelDrop` (25).
- Standing at your own tent restocks you: `Restock` (26).

**TC** (mode 1):

- Territories have positions and owners (team 0, 1, or neutral 2).
- Standing in one moves its capture: send `ProgressBar` (22) with the
  capturing team, the rate and the progress.
- At 100%, send `TerritoryCapture` (21), with `winning` when one team owns them
  all.

When a round ends, reset the scores and send a new `StateData`, or reconnect
everyone with a new map (`MapStart` again).

### 3.9 Leaving

When a peer disconnects, send `PlayerLeft` (20) with its id to everyone,
return what it carried (the intel), and free the id.

---

## 4. The master server list

To appear in the public lists, the server announces itself to a master. The
classic master (`master.buildandshoot.com`) takes an ENet connection on port
32886 (2 channels, connect data 31). The first packet describes the server:
`u8` max players, `u16` port, then the name, the mode and the map as
NUL-terminated strings. After that, send one `u8` packet with the current
player count whenever it changes.

Our own master (https://master.shootanddestroy.obex.pw, its code is not
public) is HTTP, and our server uses it for protocol 1.0. Its list is in the same JSON format the
0.75 clients read (section 1.4 of [client-0.75.md](client-0.75.md)).

---

## 5. Checklist

**Must**:

1. An ENet host with the range coder, accepting connect data 3.
2. Disconnect reasons; ban, full and version checks.
3. Map: VXL, zlib, `MapStart`, `MapChunk`.
4. `ExistingPlayer` for each player and `StateData`, in that order, after the
   map.
5. The `ExistingPlayer` (or `ShortPlayerData`) join, then `CreatePlayer` to
   everyone.
6. `WorldUpdate` at a steady rate, unreliable.
7. Relay and validate `InputData`, `WeaponInput`, `SetTool`, `SetColor`,
   `OrientationData`, `PositionData`.
8. `Hit` → `SetHP` → `KillAction` → respawn.
9. `BlockAction` and `BlockLine` with validation, block physics, `Restock`.
10. Chat, `PlayerLeft`.
11. At least one game mode (CTF).

**Should**:

- Server-side movement physics, with corrections.
- `CreatePlayer` for still players when someone joins.
- Grenades simulated on the server.
- `VersionGet` and `HandshakeInit`, as optional checks.
- `0xFF` + UTF-8 for text that does not fit CP437, to clients that read it.
- `ExtInfo` and the extensions (section 3.7.1).
