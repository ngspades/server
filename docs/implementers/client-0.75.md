# Writing a 0.75 client

What a client needs to join and play on an Ace of Spades 0.75 server, as
implemented by ngspades-client (a BetterSpades fork).
It is written for people building their own client. Where our client does
something on purpose, the text says so under **Our client**, with the file it
lives in. Our client is under conformance review: it is not a reference for
edge cases, so section 8 lists what to check rather than what we do.

The companion documents:

- [server-0.75.md](server-0.75.md): what a 0.75 server must do for clients
  like this one.
- [client-1.0.md](client-1.0.md) and [server-1.0.md](server-1.0.md): our
  own protocol, which our client tries first.

Version 0.76 differs only where noted (a different connect value, a bigger
`MapStart` and a `WorldUpdate` with player ids).

---

## 1. Transport

### 1.1 ENet

0.75 runs over ENet 1.3 on UDP/IPv4. Set the connection up like this:

| Setting | Value |
| --- | --- |
| Channels | 1 (channel 0) |
| Compression | the range coder (`enet_host_compress_with_range_coder`). A server may compress, so decompression is mandatory. |
| Connect data | `3` for 0.75, `4` for 0.76 |
| Reliability | everything reliable, except `WorldUpdate`, which servers send unreliable |
| Default port | 32887 |

```c
ENetHost* host = enet_host_create(NULL, 1, 1, 0, 0);
enet_host_compress_with_range_coder(host);
ENetPeer* peer = enet_host_connect(host, &address, 1, 3); /* 3 = 0.75 */
```

Each ENet packet holds exactly one game packet: one id byte, then the payload.

**Our client** (`network.c`, `transport_enet.c`) connects with the 1.0 magic
`0x08101919` first. A vanilla server refuses it at once with reason 3
("wrong protocol"), and the client tries `3`, then `4`. `SP_PROTO10=0` skips
the 1.0 attempt. It waits up to 2.5 s for the ENet connect, then listens for
1 s more for a refusal before it counts the connection as up.

### 1.2 Disconnect reasons

The server passes a `uint32` with the ENet disconnect event. ENet also uses 0
for a timeout, so 0 can mean either.

| Value | Meaning |
| --- | --- |
| 0 | timed out (or an unspecified server reason) |
| 1 | banned |
| 2 | kicked. Some servers (piqueserver) use it for "too many connections from your address" |
| 3 | wrong protocol version |
| 4 | server full |
| 5 | server shutting down |
| 8, 9, 10, 11 | banned, full, kicked, wrong version, sent while in game instead of during the map download |
| 20 | invalid name |
| 61 | server shutting down (alternative value) |

Any other value must be shown as an unknown reason, never trusted as an
index into a table.

**Our client** (`network_reason_disconnect`) knows 1, 2 (as "Connection
limit"), 3, 4, 5, 10 and 20, and shows everything else as "Unknown". When it
gets 3 while trying 0.75 it moves on to 0.76 without leaving the menu. A
server can override the text: a system chat message from player 255 just
before the disconnect becomes the reason shown.

### 1.3 Server addresses (`aos://`)

```
aos://<address>[:<port>][<version suffix>]
```

- `<address>` is the IPv4 address as one decimal number in **little-endian**
  byte order: `aos://16777343` is `127.0.0.1` (`0x0100007F`). Byte 0 of the
  address is the low byte of the number.
- `<port>` defaults to 32887.
- Anything after the port (`:0.75`, `/0.76`) names a version. Treat an
  unknown suffix as 0.75.

```c
uint32_t n = strtoul(text, NULL, 10);
snprintf(ip, sizeof ip, "%u.%u.%u.%u", n & 255, (n >> 8) & 255, (n >> 16) & 255, n >> 24);
```

**Our client** (`network_identifier_split`) also accepts a dotted address
(`aos://1.2.3.4:32887`), a bare `host:port`, and `aos10://` (protocol 1.0,
with IPv6 in brackets).

### 1.4 The server list

The public lists are JSON arrays served at `/serverlist.json`, for example
`http://services.buildandshoot.com/serverlist.json`. Each entry has at least:

| Key | Example | Notes |
| --- | --- | --- |
| `name` | `"Deuces Wild"` | |
| `identifier` | `"aos://16777343:32887"` | the address to join |
| `map` | `"Hallway"` | |
| `game_mode` | `"ctf"` | |
| `country` | `"US"` | ISO 3166-1 alpha-2 |
| `latency` | `120` | ms, between the list and the server |
| `players_current`, `players_max` | `16`, `32` | |
| `last_updated` | `1301931731` | Unix seconds |
| `game_version` | `"0.75"` | |

Ignore keys you do not know, and expect duplicate entries.

**Our client** (`hud.c`, `master.c`) merges this list with our own master's
(`https://master.shootanddestroy.obex.pw/v1/servers`, whose rows carry the same
keys plus `id`, `identifier6` and `info`) and lists a server found in both
once. It fetches over HTTPS through the system's HTTP client (`http.c`).

---

## 2. Data on the wire

- Every integer is **little-endian**, signed ones are two's complement.
- `float` is IEEE-754 binary32.
- Structures are **packed**: no padding between fields.
- A color is 3 bytes in the order **B, G, R**.

```c
#pragma pack(push, 1)
typedef struct { float x, y, z; } vec3f;
typedef struct { int32_t x, y, z; } vec3i;
typedef struct { uint8_t b, g, r; } color3;
#pragma pack(pop)
```

### 2.1 Coordinates

The map is 512 × 512 × 64 voxels.

- `x` goes east (0..511), `y` goes south (0..511), and `z` goes **down**: 0 is
  the sky, 63 is the bottom layer.
- A float position `(0, 0, 0)` is the corner of voxel `(0, 0, 0)`;
  `(0.5, 0.5, 0.5)` is its centre. Floor each axis to get the voxel.
- A player's position is its **eye**, not its feet.
- The two bottom layers (z = 62 and 63) cannot be dug by spades or grenades.
- The map repeats: terrain outside 0..511 on x and y is drawn as a copy of
  the map (toroidal wrap). Players, block edits and block physics do **not**
  wrap; grenades do.

**Our client** keeps OpenGL coordinates internally (`y` up): it converts
`(x, y, z)` from the wire to `(x, 63 − z, y)` and back on every packet.

### 2.2 Sectors

The minimap grid is 8 × 8 sectors of 64 × 64. A sector is named by a letter
`A`..`H` from `floor(x / 64)` and a digit `1`..`8` from `floor(y / 64) + 1`.
Territory captures are announced by sector (`network.c`, `TerritoryCapture`).

### 2.3 Text

Strings are NUL-terminated or sit in fixed-size fields. Two encodings are in
use:

- **CP437**, the original one. Bytes `0x80`..`0xFF` map to the IBM PC code
  page.
- **UTF-8**, sent by newer servers. A string that starts with byte `0xFF` and is
  valid UTF-8 after it is UTF-8 (drop the `0xFF`); anything else is CP437.
  To send, use CP437 when every character fits, otherwise `0xFF` + UTF-8.

Inside chat, a few control bytes change the color of the text that follows:

| Byte | Color |
| --- | --- |
| `0x01` | team 1's color |
| `0x02` | team 2's color |
| `0x03` | spectators' color |
| `0x04` | red |
| `0x05` | green |
| `0x06` | normal (white) |
| `0x07` | gray |
| `0x08` | yellow |

`\n` and `\r` break the line. Other bytes below `0x20` are shown as a
replacement character.

**Our client** (`lang.c`, `network.c`):

- A chat text that starts with `0xFF` is UTF-8 (the UTF-8 Chat extension of
  the aosprotocol specification): the prefix is dropped. Without it, a string
  whose bytes form valid multi-byte UTF-8 sequences is still read as UTF-8 (a
  guess for servers that send UTF-8 without the prefix), anything else as
  CP437.
- When sending, it sends `0xFF` + UTF-8 once it knows the server reads it:
  the server asked for the feature flags in an extended `VersionGet`, or sent
  a `0xFF` line itself. Otherwise, and for plain ASCII, it converts to CP437
  and replaces what does not fit with `?`.
- The chat renders colors `0x01`..`0x07`; `0x08` is not drawn as yellow yet.

---

## 3. Game rules the client must know

### 3.1 Players

Player ids go from 0 to 255; most servers use 32 slots. Keep state for all
256. Per player you need at least: joined, alive, team, weapon, held tool,
score, name, block color, position, orientation, movement and tool inputs.
Only for yourself do you also track HP, blocks, grenades, magazine and
reserve.

**Our client**: `PLAYERS_MAX` is 256 (`player.h`). Size the name buffer for
UTF-8 (accents take 2 bytes each), or a long name loses its last characters.

### 3.2 Enumerations

| Teams | |
| --- | --- |
| −1 (255 on the wire) | spectator |
| 0 | team 1 (left, usually blue) |
| 1 | team 2 (right, usually green) |
| 2, 3 | neutral, only for the owner of a territory |

| Weapons | | Tools | |
| --- | --- | --- | --- |
| 0 | rifle | 0 | spade |
| 1 | SMG | 1 | block |
| 2 | shotgun | 2 | gun |
| | | 3 | grenade |

When a value is out of range, fall back to a safe one (spectator, rifle,
spade) instead of indexing a table with it.

**Check in yours**: validate player ids, ignore a `SetTool` above 3, and map an
out-of-range team or weapon to a safe default (spectator, rifle) instead of
using it as an index.

### 3.3 Block colors and the palette

Placing a block uses the color the player holds. The classic palette is 8 × 8:
row 0 is grays, rows 1..7 are red, orange, yellow, green, cyan, blue and
magenta, from dark to light. The arrow keys move a cursor over it; up and down
wrap within the column. Picking a color sends `SetColor`.

`E` copies the color of the block under the crosshair, up to 128 blocks away,
or the fog color when there is none.

**Our client** (`texture_block_color`, `hud.c`) generates exactly the classic
64 colors. Check how your palette steps at the row edges: the classic
behaviour moves to the previous or next row. `E` picks the color under the
crosshair while holding a block.

### 3.4 Fog

Fog limits sight to a cylinder of radius 128 around the camera. Nothing
beyond it is drawn, shots stop there, and the sky is the fog color.
`StateData` and `FogColor` set it.

**Our client**: `settings.render_distance`, 128 by default.

### 3.5 Block physics

When a voxel stops being solid, check its six face neighbors. Each solid one
starts a flood fill through solid voxels; if the fill reaches the ground layer
(z ≥ 62), nothing happens. Otherwise everything it reached falls and is
removed. Voxels that were already empty inside a spade or grenade area must not
start a check.

**Our client** (`map_update_physics`, `map.c`) runs the flood fill on a worker
thread and animates the falling pieces. It checks the neighbors of every voxel
in a grenade or spade area, even the ones that were already empty.

### 3.6 Applying block edits

Edits that arrive together should land together. Collect the voxels every
`BlockAction` and `BlockLine` touches in a table keyed by position (create or
destroy, the last one wins). After draining the ENet events of a frame, apply
the creates, then the destroys. Servers rely on this to repaint a block (a
destroy followed by a build at the same position), and a structure resting on
that block must not collapse in between.

**Check in yours**: whether you apply edits per packet or per frame. Applying
each packet at once starts the physics check early, so a repaint can make a
structure fall.

---

## 4. Connecting, step by step

```
client                                   server
  ENet connect (data 3) ----------------->
            <----------------------- MapStart (18)
            <----------------------- MapChunk (19) × N
            <----------------------- VersionGet (33), HandshakeInit (31)   (optional)
  VersionSend (34), HandshakeReturn (32) -->
            <----------------------- ExistingPlayer (9) × players already in
            <----------------------- StateData (15)        (download done)
  ExistingPlayer (9): team, weapon, name -->                 (team/weapon menu)
            <----------------------- CreatePlayer (12)      (you spawn)
            <----------------------- WorldUpdate (2), ... (the match)
```

1. **Map.** `MapStart` gives the compressed size. Append every `MapChunk`
   payload; the chunks may be any size. The concatenation is a zlib stream of a
   VXL map.
2. **State.** `StateData` ends the download: decompress and load the map, and
   take your player id, the fog, the team names and colors and the game mode
   from it.
3. **Join.** Show the team and weapon menus, then send `ExistingPlayer` with
   your id, team, weapon and name. Choosing spectator joins at once.
4. **Spawn.** The server answers with `CreatePlayer` for you (and for everyone
   else, whenever anyone spawns).

**Our client** (`network.c`, `hud.c`): `hud_join` sends the `ExistingPlayer`
join; `read_PacketStateData` loads the map and opens the team menu.

---

## 5. Packets from the server

Sizes include the id byte. Positions are in wire coordinates (section 2.1).

| Id | Name | Size |
| --- | --- | --- |
| 0 | PositionData | 13 |
| 1 | OrientationData | 13 |
| 2 | WorldUpdate | 1 + 24 × n (0.76: 1 + 25 × n) |
| 3 | InputData | 3 |
| 4 | WeaponInput | 3 |
| 5 | SetHP | 15 |
| 6 | Grenade | 30 |
| 7 | SetTool | 3 |
| 8 | SetColor | 5 |
| 9 | ExistingPlayer | 12 + name |
| 11 | MoveObject | 15 |
| 12 | CreatePlayer | 16 + name |
| 13 | BlockAction | 15 |
| 14 | BlockLine | 26 |
| 15 | StateData | 84 (CTF), 33 + 13 × n (TC) |
| 16 | KillAction | 5 |
| 17 | ChatMessage | 3 + text |
| 18 | MapStart | 5 (0.76: 73) |
| 19 | MapChunk | 1 + data |
| 20 | PlayerLeft | 2 |
| 21 | TerritoryCapture | 4 |
| 22 | ProgressBar | 8 |
| 23 | IntelCapture | 3 |
| 24 | IntelPickup | 2 |
| 25 | IntelDrop | 14 |
| 26 | Restock | 2 |
| 27 | FogColor | 5 |
| 28 | WeaponReload | 4 |
| 31 | HandshakeInit | 5 |
| 33 | VersionGet | 1 |

Check every packet's length before reading it, and ignore ids you do not
handle.

**Check in yours**: verify a packet's length against its id before decoding it,
so a short or malformed packet is never read past its end.

### 0 PositionData

`vec3f pos`: moves you there at once (a teleport or a correction).

### 1 OrientationData

`vec3f ori`: turns you to that direction.

**Check in yours**: store it as the orientation, never as a position.

### 2 WorldUpdate

One `{vec3f pos; vec3f ori;}` per player slot, indexed by player id, up to 256
(0.76 adds a `uint8 player_id` in front of each). Skip your own entry.

**Our client** also skips an all-zero position: servers send zeros for
players you should not see, and teleporting them to the corner looks wrong.

### 3 InputData

`uint8 player_id, uint8 keys`: bits 1 forward, 2 back, 4 left, 8 right,
16 jump, 32 crouch, 64 sneak, 128 sprint. For yourself only the jump bit
counts.

### 4 WeaponInput

`uint8 player_id, uint8 buttons`: bit 1 primary, bit 2 secondary. Ignore it for
yourself. It is not the raw mouse: the server sends 0 while a player sprints,
switches tools or reloads.

### 5 SetHP

`uint8 hp, uint8 type, vec3f source`: your new HP. When it drops, show the hurt
effect. When `source` is not zero, also point the damage indicator at it.
`type` is 0 for fall damage and 1 for anything else.

**Check in yours**: which `type` values show the hurt effect and play the sound.

### 6 Grenade

`uint8 player_id, float fuse, vec3f pos, vec3f vel`: spawns a grenade that
explodes `fuse` seconds after the packet arrives.

### 7 SetTool

`uint8 player_id, uint8 tool`: that player switches to `tool`.

### 8 SetColor

`uint8 player_id, color3 color`: that player's block color. Keep it even for
a player who is not alive or not joined yet, since later block packets use it.

### 9 ExistingPlayer

```c
struct { uint8_t id; int8_t team; uint8_t weapon; uint8_t tool;
         uint32_t kills; color3 color; char name[]; /* NUL-terminated */ };
```

A player who was already in the game when you joined. It carries no position:
keep the player out of sight until a `WorldUpdate` or `CreatePlayer` places
them.

### 11 MoveObject

`uint8 object, int8 team, vec3f pos`. In CTF, object 0 and 1 are the two
intels and 2 and 3 the two tents. Moving an intel also drops it from whoever
carried it, silently. In TC, `object` is a territory index and `team` its new
owner.

### 12 CreatePlayer

```c
struct { uint8_t id; uint8_t weapon; int8_t team; vec3f pos; char name[]; };
```

That player spawns at `pos` (or joins, the first time). The player is alive
unless the team is spectator. It faces east for team 1 and west for team 2.
Reset its block color to `(111, 111, 111)` and restock it. A packet of exactly
16 bytes has no name: keep the one you had.

The classic client places the player at `pos.z − 2` (two blocks higher).

**Our client**:

- Check the −2 on `z` that this packet carries.
- A 16-byte packet has no name: keep the one you have.
- It prints "X joined the Y team" the first time a player appears.

### 13 BlockAction

`uint8 player_id, uint8 type, vec3i pos`:

| Type | Effect |
| --- | --- |
| 0 | build one block with the player's color (a repaint when the voxel is already solid, silently) |
| 1 | destroy one block |
| 2 | spade: destroy the block and the ones above and below it (1 × 1 × 3) |
| 3 | grenade: destroy 3 × 3 × 3, silently |
| 4 | destroy one block (a gun) |
| 5 | destroy one block (a spade) |

Types 2 and 3 leave the bottom layers (z ≥ 62) alone. Voxels outside the map
are ignored. For yourself, a type 0 costs one block, and types 1 (while holding
the spade) and 5 give one back, up to 50.

**Our client** (`read_PacketBlockAction`):

- Check that every block action type, 4 and 5 included, is handled.
- It counts its own blocks when it sends an action, not when the server
  confirms it.

### 14 BlockLine

`uint8 player_id, vec3i start, vec3i end`: builds a line of blocks between the
two points (a 3D DDA), skipping voxels that are already solid. It costs
`|dx| + |dy| + |dz| + 1` blocks.

### 15 StateData

```c
struct {
    uint8_t player_id;            /* you */
    color3 fog, team1_color, team2_color;
    char team1_name[10], team2_name[10];  /* NUL not guaranteed */
    uint8_t mode;                 /* 0 CTF, 1 TC */
    /* CTF: scores, limit, intel holders/positions, tent positions (52 bytes)
       TC:  uint8 count + count × {vec3f pos; uint8 team} */
};
```

Ends the map download. Read the team names as 10 bytes that may lack a NUL.
Treat an unknown mode as TC.

**Our client** cuts the names at 10 bytes and converts them from CP437. For an
unknown mode it logs an error and keeps going.

### 16 KillAction

`uint8 victim, uint8 killer, uint8 kind, uint8 respawn_seconds`. `kind`: 0
weapon, 1 headshot, 2 melee, 3 grenade, 4 fall, 5 team change, 6 class change.
The killer gets a point unless it is a suicide.

**Our client** draws the kill feed with weapon icons and starts the respawn
countdown from `respawn_seconds`.

### 17 ChatMessage

`uint8 player_id, uint8 type, char text[]`. Types: 0 all, 1 team, 2 system;
newer servers also send 3 big, 4 info, 5 warning, 6 error.

**Our client** shows types 3..6 as popups. A system message from player 255
is kept as the reason for a disconnect that follows.

### 18 MapStart, 19 MapChunk

See section 4. 0.76's `MapStart` adds a CRC32 and the map name, and the client
answers whether it has the map cached (`MapCached`, id 31, C→S).

### 20 PlayerLeft

`uint8 player_id`: the player is gone; clear their slot.

### 21 TerritoryCapture, 22 ProgressBar

`uint8 index, uint8 winning, int8 team` and
`uint8 index, int8 team, int8 rate, float progress`: TC captures and the
capture bar.

**Check in yours**: a capture may name a team other than 0 or 1 (neutral): do
not use the value as an index.

### 23 IntelCapture, 24 IntelPickup, 25 IntelDrop

`uint8 player_id, uint8 winning`, `uint8 player_id`, `uint8 player_id, vec3f pos`.
A capture is worth 10 points to the player and 1 to the team; `winning` ends
the round.

### 26 Restock

`uint8 player_id`: you get full HP, 50 blocks, 3 grenades and a full reserve.
When you are dead, only play the sound.

**Check in yours**: restock only a living player, and take the counts from the
rules of the server's mode.

### 27 FogColor

`uint8 alpha, color3 color`: the new fog color.

### 28 WeaponReload

`uint8 player_id, uint8 magazine, uint8 reserve`. For yourself it sets the
ammo; for anyone else it only plays the reload sound.

### 31 HandshakeInit, 33 VersionGet

Some servers fingerprint clients. Answer `HandshakeInit` with
`HandshakeReturn` (32) carrying the same 4 bytes, and `VersionGet` with
`VersionSend` (34).

A `VersionGet` that lists property ids (the aosprotocol specification's
extended form) is answered with `ExistingPlayer` (9) marked by `'x'` as its
first byte, then `id, length, payload` per property in ascending order, one
packet per property, each repeating the ones before. Our client answers 0
(application: `0 1 5` and `BetterSpades`, what it calls itself on a 0.75
server), 1 (the interface language, `en_us`, `pt_br`, ...) and 2 (feature
flags, bit 0: it reads UTF-8 chat); an unknown id gets an empty payload.

---

## 6. Packets to the server

| Id | Name | When our client sends it |
| --- | --- | --- |
| 0 | PositionData | every second while alive, if you moved more than 0.01 |
| 1 | OrientationData | up to 120 times a second when the aim turned more than 0.5°, and right before each shot |
| 3 | InputData | whenever the movement keys change |
| 4 | WeaponInput | whenever the buttons change, except while sprinting |
| 5 | Hit | `uint8 victim, uint8 part` (0 torso, 1 head, 2 arms, 3 legs, 4 melee) for each hit your shot or spade made |
| 6 | Grenade | when you throw: fuse left, position, velocity |
| 7 | SetTool | whenever the held tool changes |
| 8 | SetColor | when you pick a color |
| 9 | ExistingPlayer | to join: team, weapon, name (section 4) |
| 13 | BlockAction | build, dig, spade (1 × 1 × 3), or a gun hit on a block |
| 14 | BlockLine | dragging to build a line |
| 17 | ChatMessage | chat, in CP437, or `0xFF` + UTF-8 to a server known to read it |
| 28 | WeaponReload | when you start reloading |
| 29 | ChangeTeam | `uint8 player_id, int8 team` from the in-game team menu |
| 30 | ChangeWeapon | `uint8 player_id, uint8 weapon` from the in-game weapon menu |
| 32 | HandshakeReturn | the answer to 31 |
| 34 | VersionSend | `char client ('B'), uint8 major, minor, revision, char os[]` |
| 9 | ExistingPlayer `'x'` | the extended version answer, to a `VersionGet` with property ids |
| 114 | Flashlight Light | `uint8 sub (0), uint8 player, uint8 on`: asks to switch our light (key F, a tap of the pad's Back) |

The classic rules are stricter in two places: send the position once a second
even when you did not move, and send the orientation whenever it changes at
all (an exact comparison, not a threshold).

The client reports its own hits; the server decides the damage. Servers check
the distance, the line of sight and the fire rate, so a client should only send
hits it really saw.

---

## 7. Extensions our client understands

Servers announce extensions with `ExtInfo` (id 60), as the aosprotocol
specification (https://github.com/zerospades/aosprotocol) describes. Our
client answers with the ones both sides know, never one the server did not
offer: `PlayerProperties` (0x00, packet `0x40`, the exact HP, blocks,
grenades, ammo and score), Damage Markers (0x20, packet `0x60`), Teamplay
(0x30, packet `0x70`: Config, Ping, ESP Mark), Flashlight (0x32, packet
`0x72`: Light, Light State, Light Config; every lit player's beam is drawn,
where it lands and the lamp's glare by the spec's Beam, Lens, Distance and
Fog terms, hidden by blocks and bodies, never a light the server did not
switch on), Player Limit (0xC0), Message Types (0xC1: 3 big, 4 info,
5 warning, 6 error, shown as popups) and Kick Reason (0xC2: a system line
from player 255 before the disconnect is the reason shown). UTF-8 Chat has
no id (section 2.3). The Powerthirst packets are not supported.

Our own server used `HostCommand` (63) and `HostStatus` (64) for bot
management before it moved to protocol 1.0. Packet 64 is also Player
Properties: our client reads it as Player Properties once the server agreed
to that extension, and as `HostStatus` otherwise.

---

## 8. What a client implementer must check

This list is deliberately not a list of our client's faults: that changes with
every release. It is what to verify in any 0.75 client, ours included. The
ngspades-client conformance checklist, kept current, is at
<https://ngspades.github.io/shoot-and-destroy/>.

- Length: every packet's size is checked against its id before decoding.
- Orientation and position: `OrientationData` sets the orientation,
  `PositionData` the position; the thresholds in section 6 decide when to send.
- Values from the wire: teams, weapons, tools, game modes and territory teams
  never index a table unchecked; an unknown mode is read as TC.
- `CreatePlayer` and `ExistingPlayer`: the −2 on `z`, a short packet keeping
  the name, and where a player that has no body yet is placed.
- Block edits: batched per frame, action types 0..5 all handled, the palette
  row stepping at the edges, the block count settled by the server.
- Text: the `0xFF` UTF-8 prefix, names longer than 16 bytes, and the colour
  bytes `0x01`-`0x08` (section 2.3).
- Disconnect reasons: every code in section 1.2, 0, 8, 9, 11 and 61 included,
  shown as a reason and never trusted as a number.
- Position updates: sent at the rate the server expects (about 1 Hz when
  idle), with the exact orientation.
- Other: `SetHP` effects by damage type, `Restock` only while alive, physics
  checks for cells that were already empty.
