# Protocol 1.0 dossier

Reference for the **1.0 compatibility layer**: a second, modern wire protocol
that `server/` (SpadesX fork) and `client/` (BetterSpades fork) will speak
**alongside** vanilla 0.75/0.76. Protocol 1.0 starts from the protocol of
OpenSpades' `ngspades` branch ("WorldOfSpades 0.1"), cloned in `ngspades/`.

This document has three kinds of content, always labelled:

- **[ref]**: what the ngspades reference code actually does, byte for byte.
  Source paths are relative to `ngspades/Sources/`.
- **[defect]**: a bug or gap in the reference that 1.0 must not copy.
- **[1.0]**: our protocol, where it differs from or adds to the reference.
  Each [1.0] item says whether it is implemented (and where) or still a
  proposal to confirm before implementing it.

Code examples are C99, because both of our programs are C. They are meant to be
copied, not just read: the primitive codec in section 4 is complete.

---

## 0. Coverage: what we have, what is missing

The state of the code on 2026-10-29 (protocol revision 1.3), checked in
`common/proto10/`, `server/Source/Server/Proto10/` and the client's `proto10*.c`,
not only in this document. Keep it in step: a change that implements one of
the gaps below moves its row from 0.2 to 0.1 in the same commit.

The reference (ngspades) is a prototype: its codec is complete, its gameplay
is not (section 1). So "missing" below means **a part of the reference's
wire format we do not use yet**, never a ngspades feature that worked: most
of what we play (chat, building, damage, grenades, game modes) the reference
never had, and we added it (section 12).

### 0.1 Implemented

| Id | Packet | State |
| --- | --- | --- |
| 1-4 | Greeting, InitiateConnection, Server/ClientCertificate | done; the protocol name is `WorldOfSpades 1.0`, the version is the protocol revision (12.1); the ServerCertificate is always `isValid = 0`; the ClientCertificate carries the master's connect token where the server asks for one (6.3.1) |
| 5 | Kick | done |
| 6, 7, 10, 11 | GameStateHeader, MapData, MapDataAcknowledge, MapDataFinal | done; ngmap encoded off the tick and cached, VXL when the client asks for quality 0 (7.5); fragments sent while the connection holds less than 64 KiB (D9, 7.2) |
| 8 | GameStateFinal | done, with every player (D6 fixed) |
| 9 | GenericCommand | done; the reference's `join`, `leave` (to the spectators) and `local-player`, and ~40 of ours (10, 12.5, 12.6) |
| 30 | EntityUpdate | done; a delta per receiver, the rate by distance, the fog cut, the compact trajectory (12.7); a body's health only to its owner and its team (0 to the others) |
| 31 | EntityEvent | ReloadWeapon, Jump (a body's jump to the clients that have it, `proto10_bridge_jump`), Explode, and our Fire (sentry) and UsePerk |
| 32 | EntityDie | done for players (D3 fixed); `param` = 0.75 kill reason, respawn seconds << 8 (8.9) |
| 33 | EntityRemove | done (D5 fixed) |
| 35 | ClientSideEntityUpdate | done; 30 Hz, unreliable on channel 1, reliable before an action (D7, D8 fixed) |
| 40 | TerrainUpdate | done, batched a tick; the health byte carries block damage (9.3) |
| 50 | PlayerUpdate | Create, Flags (isAdmin: the staff's badge on the scoreboard), Score, and our Team, Stats, Ping, Perks |
| 51 | PlayerRemove | done |
| 60 | PlayerAction | Jump (with the zombie leap's charge), ReloadWeapon, Suicide (the pause menu's Respawn, the same death as `/kill`), our UsePerk |
| 90 | HitEntity | done, through `game_player_hit`; `hitPosition` is the point the pellet met, checked on the target's body (lag compensated) and in line of sight from `firePosition` (9.2) |
| 91 | HitTerrain | done; spade, dig and every pellet of a gun, server-side block health (9.3) |
| 92 | Damage | done, to the victim only (D4 fixed); typed Fall, Grenade, Melee (a spade's or a claw's hit), Weapon1/Weapon2 by the slot in hand, our Sentry; Weapon3, Block, Poison and the vehicles' types are not used |
| 70, 71, 80, 93, 94, 95 | ChatMessage, ChatBroadcast, GameModeState, BuildBlock, BuildLine, ThrowGrenade | ours (12.2) |

Also done:
- **Objectives as entities**: each intel a `Flag`, riding its carrier through
  `parentEntityId`, each tent a `CommandPost`, KotH's hill and TC's
  territories `Checkpoint`s;
  GameModeState keeps what they cannot say (12.2.3).
- **Entity types**: Player, Grenade (Gravity trajectory), Rocket (the
  Commando's, Linear), and ours: Sentry, Landmine, Dynamite, Drill, the supply crates (12.5,
  12.6, 12.6.1).
- **Trajectories**: Constant, Linear, Gravity, Player, and our compact
  Player (12.7).
- **Inventory (revision 1.3)**: Weapon1 the class's gun, Weapon2 the second
  gun, Weapon3 the gear (grenades or the drill, our weapon type Gear); every
  Create carries the three `WeaponParameters` and the body and weapon skins,
  and the client plays by them: a body's guns are named by its weapon skins,
  and the fire interval, magazine and reload time are the server's
  (`weapon_server_params`, the client's `weapon.c`; its tables only for a
  0.75/0.76 server), and so is the raise time: a gun taken out fires after
  `raiseTime` (`GAME_WEAPON_RAISE_MS`, 500), which the client waits and the
  server enforces (`player_gun_raised`, 150 ms of slack; `EVT shot_refused
  why=raise`; bots wait it too);
  a gun change is the Tool field, no command (8.4, 12.6). The rocket
  launcher and the drill fire from the trigger, no command (12.6).
- **Player input**: analogue movement (a stick pushed part way walks at
  `push / 127` of a walk on the ground, on both sides' physics; keys and bots
  127), stand and crouch, primary, secondary, sprint,
  chat (the chat box open: "..." over the head on the other clients),
  and our jetpack bit.
- **World parameters**: the reference's five keys (the physics ones the
  server's `[gameplay]`, played by the client), and ours (7.6).
- **Entity flags**: `fly` on a body that flies free (creative), which the
  client predicts by; `playerClip` and `weaponClip` set, unread.
- **Defects**: D1 to D11 and D13 fixed (section 11); D12 open (G14).

### 0.2 Missing or partial

Ranked by what a player gains, then by effort (S small, M medium, L large).

| # | Reference part | What we do now | What is missing | Effort |
| --- | --- | --- | --- | --- |
| G14 | Certificates (D12) | `isValid = 0`, except the connect token (6.3.1) | accounts: identity, bans by key, stats | L |
| G15 | PlayerStance `Prone` | decoded, refused | prone (not in Jagex's game: models, hit boxes, bots) | L |
| G16 | `Vehicle` entity, `RigidBody` trajectory | none (the reference's are stubs too) | only if the design wants vehicles | L |

The rest when a feature asks for it.

---

## 1. Background and status

- `ngspades` was yvt's 2013–2014 rewrite of OpenSpades as a new game ("World
  of Spades") with its own server. Last commit: 2014-07-19 (`31d859b`). It was
  abandoned; the final commits are about a map/object editor.
- It is a **prototype**. The codec (`Server/Shared.{h,cpp}`) is complete for
  the packets it defines, but the gameplay behind them is mostly missing:
  - `PlayerEntity` has real movement physics (~420 lines).
  - `Grenade`, `Rocket`, `Flag`, `CommandPost`, `Checkpoint` and `Vehicle`
    entities are 33-line stubs.
  - The server rejects or ignores `PlayerAction`, `HitEntity`, `HitTerrain`
    and `EntityDie` from clients (`SPNotImplemented()`, `Server/Connection.cpp`).
  - Chat, building and damage notification are `TODO`s in `Server/Shared.h`.
- Therefore: **the wire format is the valuable part**. Gameplay semantics for
  1.0 come from our own server (SpadesX), not from ngspades.

### Main differences from 0.75

| Topic | 0.75 | WorldOfSpades 0.1 [ref] |
| --- | --- | --- |
| Integers | fixed width (u8/u16/u32) | LEB128 varints, zigzag for signed |
| Strings | NUL-terminated or fixed size | length-prefixed bytes (≤ 1 MiB) |
| Handshake | ENet connect data = version (3/4) | connect magic + Greeting/Initiate/certificates, protocol name and version, locale, nonces |
| Map | zlib VXL | "ngmap": deflated column geometry + JPEG-compressed colours, quality picked by the client |
| World model | one packet per subject (WorldUpdate, GrenadePacket, Intel…) | generic **entities** with delta-encoded updates |
| Movement | positions sampled every update | **trajectories** (constant, linear, gravity, player, rigid body) the client extrapolates |
| Player vs body | same thing, 8-bit id | player record (name, score, flags) separate from its entity |
| Input | 8 booleans | analogue `xmove`/`ymove` (−127..127), stance stand/crouch/**prone**, primary/secondary, sprint, chat |
| Weapons | 3 hardcoded | `WeaponParameters` sent by the server (fire rate, damage, magazine, reload, penetration…), 3 weapon slots + melee + block |
| Damage | KillAction with a kill type | `HitEntity`/`HitTerrain` with fire and hit positions, `Damage` with a 13-value damage type |
| Disconnect | ENet data 1..3 | ENet data with its own 8-value enum, plus free-text `Kick` |
| Extensibility | none | `GenericCommand` (list of strings) |

---

## 2. Coexistence with 0.75/0.76

**Update (2026-10-23):** the server has a second profile,
`--profile 075`, that speaks only 0.75/0.76 (docs/plan-075-server.md; its
wire in `server/Source/Server/Legacy/`). The default profile is unchanged:
1.0 only, as decided below. One server is one protocol; a mixed match is
still out of scope.

**Decision (2026-10-03):** the server of this repository becomes **1.0
only**; the client keeps speaking **1.0, 0.75 and 0.76** (1.0 with our
server, 0.75/0.76 with public servers). Until 1.0 reaches parity (milestones
5 and 6) the server accepts both protocols in the same match, as described
below; milestone 8 (section 14) is the switch.

The hard rules in `CONTRIBUTING.md` still apply: our client must keep joining vanilla
servers and our server must keep accepting vanilla clients. **The protocol is
chosen per connection, at ENet connect time, from the 32-bit connect data:**

| ENet connect data | Protocol | Who sends it |
| --- | --- | --- |
| `3` | 0.75 | vanilla 0.75 clients, BetterSpades (`VERSION_075`, the client's `network.h`) |
| `4` | 0.76 | 0.76 clients, BetterSpades (`VERSION_076`) |
| `0x08101919` | WorldOfSpades / 1.0 | ngspades client (`protocol::connectMagic`) |

There is no collision: the value is a free 32-bit field and vanilla only uses
small numbers.

- **Server [1.0]**: `on_new_player_connection` (`server/Source/Server/Player.c`)
  currently rejects anything but `VERSION_0_75` with
  `REASON_WRONG_PROTOCOL_VERSION`. It becomes a dispatch on `event->data`, and
  the peer remembers its protocol for every later send and receive (section 13).
- **Client [1.0]**: `network_connect` (the client's `network.c`) tries 0.75 and
  then 0.76. A 1.0-capable client tries `0x08101919` first. A vanilla server
  answers it with a disconnect carrying reason 3 ("wrong protocol"), and the
  client falls back to 0.75. A 1.0 server either completes the handshake or
  rejects it with `ProtocolMismatch` (section 3.3).
- **Never mix.** A peer speaks exactly one protocol for its whole life.
  Packet id 9, for example, means `GenericCommand` in 1.0 and something
  unrelated in 0.75.

---

## 3. Transport

### 3.1 ENet setup [ref]

| Setting | Server (`Server/Host.cpp`) | Client (`NGClient/Host.cpp`) |
| --- | --- | --- |
| Port | `0x810` (2064), hardcoded `TODO` | given by the user |
| Peers / channels | 256 peers, 1 channel | 2 peers, 1 channel |
| Compression | `enet_host_compress_with_range_coder` | same |
| Connect data | must equal `0x08101919` | `enet_host_connect(host, &addr, 1, 0x08101919)` |
| Packets | all `ENET_PACKET_FLAG_RELIABLE` on channel 0 | same |

Vanilla AoS also uses the ENet range coder, so our existing ENet hosts are
already configured correctly. SpadesX creates its host with 2 channels; ENet
negotiates down to what the peer asks for, so a 1-channel 1.0 client works.

**[1.0]** Keep the port (32887) and the host shared with 0.75: the dispatch on
connect data makes a second port unnecessary.

**[1.0]** Movement updates (`EntityUpdate` with only trajectory/input, and
`ClientSideEntityUpdate`) go on channel 1, **unreliable sequenced**. Everything
else stays reliable on channel 0. The reference sends the local player's state
reliably every frame (`NGClient/NetworkPlayer.cpp`), which builds an ever
growing queue as soon as a packet is lost.

### 3.2 Packet framing [ref]

One ENet packet = one protocol packet:

```
+---------+--------------------------------+
| u8 type | payload (type specific)        |
+---------+--------------------------------+
```

Unknown type ids decode to `NULL` (`Packet::Decode`). The server logs the
failure and drops the packet; the client disconnects itself (`Kicked`).
**[1.0]**: both sides ignore unknown ids, which is what makes it possible to
add packets later without bumping the protocol name.

### 3.3 Disconnect reasons [ref]

Sent as the ENet disconnect data (`enet_peer_disconnect(peer, reason)`):

| Value | `DisconnectReason` | Meaning |
| --- | --- | --- |
| 0 | `Unknown` | |
| 1 | `InternalServerError` | exception while accepting the peer |
| 2 | `ServerFull` | `sv_maxClients` reached |
| 3 | `Misc` | a `Kick` packet with the text was sent first |
| 4 | `ServerStopped` | server shutting down |
| 5 | `Timeout` | handshake step took too long (section 5.3) |
| 6 | `MalformedPacket` | decode/handler error, or wrong connect magic |
| 7 | `ProtocolMismatch` | protocol name in `InitiateConnection` differs |

**Careful:** vanilla uses the same ENet field with a different table
(1 = banned, 2 = connection limit, 3 = wrong protocol). The meaning depends on
the protocol the connection was opened with. The one ambiguous case is
connecting with the 1.0 magic: a vanilla server answers `3` (wrong protocol),
while a 1.0 server would use `3` only after sending a `Kick`. **[1.0]**: a
client that receives `3` without a preceding `Kick` while still in the
handshake treats it as "not a 1.0 server" and falls back to 0.75.

### 3.4 WebRTC transport [1.0]

The browser cannot send UDP, so it reaches our server over WebRTC
DataChannels (docs/web.md phases 2 and 3). The packets are the same; only
what carries them changes. **Implemented** in the server
(`server/Source/Server/Net/WebRtc.c`, `Net/RtcPeer.cpp`, libdatachannel)
and its test client (`tools/rtcprobe`); the web client implements the same.
Only protocol 1.0 goes over WebRTC. **Any server that wants browser players
turns it on** (`[webrtc] enabled = true`, `SP_WEBRTC_ENABLED`, default
false): a match hosted from the game with a join code turns it on by
itself, and a dedicated server that sets it takes browsers too. A server
that leaves it off is listed with `webrtc: false` and takes only native
clients.

**Signalling.** Through the master server (its README is the API): the
browser posts its offer to `POST /v1/servers/{id}/offer`, the game server
gets it from its long poll and answers through `POST /v1/answer` within
10 s. ICE is **not trickled**: the browser waits for
`iceGatheringState == "complete"` before posting, and the server answers
only once its gathering is complete, so each SDP carries every candidate.
The server's DTLS certificate is self-signed; its fingerprint is in the
answer (and in the list, `dtls_fingerprint`).

**Channels.** The browser creates both before making its offer:

| label | options | carries |
| --- | --- | --- |
| `reliable` | the defaults (reliable, ordered) | ENet channel 0 |
| `unreliable` | `ordered: false, maxRetransmits: 0` | ENet channel 1 |

Any other label is closed by the server. The server, like over ENet,
sends everything on `reliable`; it reads both.

**Messages.** A **text** message is control, a **binary** one is a game
packet:

- Once both channels are open, the client's first message is the text
  `connect <n>` on `reliable`, `<n>` being the ENet connect data in
  decimal (`connect 135272729` for `0x08101919`). Only the 1.0 magic is
  accepted; anything else gets the refusal ENet would give (`3`, wrong
  protocol, for `connect 3`).
- The server ends a connection with the text `disconnect <reason>` on
  `reliable`, `<reason>` being the number ENet would put in its disconnect
  data (section 3.3, or vanilla's table for a refusal before the
  handshake: `3` wrong protocol, `4` full, `1` banned), then closes the
  peer.
- Every binary message is exactly one protocol 1.0 packet, the bytes ENet
  carries (section 3.2), on the channel of the table; **at most 16 KB**
  (browsers take 64 to 256 KB; the map's fragments are 8 KB). The server
  logs `EVT webrtc action=too_big` if it ever sends a larger one.
- A binary message before `connect` is dropped; any other text is ignored
  (`EVT webrtc action=text_ignored`).

**Closing.** The browser closing its peer (or its channels) is the ENet
disconnect: the player leaves at once. The server waits up to 2 s for its
queued messages (the `disconnect` text last) before it closes.

**Server side.** Every WebRTC peer shares one UDP port (libjuice's ICE UDP
mux): `[webrtc] port`, by default the game's port + 1 (`SP_WEBRTC_PORT`),
and `[webrtc] stun` (`SP_WEBRTC_STUN`, `""` none) to learn its public
address behind a NAT. A peer is a `conn_t` like an ENet peer
(`Net/Conn.h`): the game sends through `conn_send` and never knows which
transport it uses. Logged as `EVT webrtc action=offer|answer|channel|
connected|closing|closed why=`.

---

## 4. Primitive encoding

All multi-byte fixed-width values are **little endian**.

| Name | Encoding [ref] |
| --- | --- |
| `u8`, `u16`, `u32` | fixed width, little endian |
| `f32` | IEEE-754 single, bit pattern written as `u32` |
| `vec3` | `f32 x, f32 y, f32 z` (12 bytes) |
| `varint` | unsigned LEB128: 7 bits per byte, low group first, bit 7 = "more follows" |
| `svarint` | zigzag then `varint`: `n >= 0 → 2n`, `n < 0 → 2(−1−n)+1` |
| `bytes` | `varint length` + raw bytes; decoders reject length > 1 MiB |
| `string` | `bytes`; the decoder truncates at the first NUL (it goes through `c_str()`) |
| `map` | repeated `string key, string value`, terminated by an empty key; writers skip empty keys |
| `color` | 3 bytes in the order **B, G, R** (`WriteColor` writes `z, y, x` of an `IntVector3(r,g,b)`) |
| `blockcoord` | `svarint x, svarint y, svarint z` |
| `rotation` | `vec3`: quaternion `x,y,z` with `w ≥ 0` (negated if `w < 0`); the decoder rebuilds `w = sqrt(1 − x² − y² − z²)` |

Varint examples: `0 → 00`, `127 → 7F`, `128 → 80 01`, `300 → AC 02`.
Zigzag examples: `0 → 00`, `−1 → 01`, `1 → 02`, `−2 → 03`.

**[defect]** `ReadVariableInteger` accumulates into a `uint32_t` even though it
returns 64 bits, and keeps shifting past 32 (undefined behaviour). Values of
2³² and above are corrupted, and an endless run of `0x80` bytes is never
rejected. **[1.0]**: decode into 64 bits, reject more than 10 bytes, and require
fields documented as 32-bit to fit in 32 bits.

**[defect]** `string` silently drops everything after a NUL. **[1.0]**:
reject strings that contain NUL, and require valid UTF-8 for player names and
chat. Names are CP437 in 0.75, so the server converts them at the protocol
boundary.

### 4.1 Reference codec in C

```c
/* proto10_buf.h - primitive codec for protocol 1.0 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    int error; /* sticky: once set, every read returns 0 */
} p10_reader;

typedef struct {
    uint8_t* data;
    size_t size;
    size_t cap;
    int error; /* sticky: set when cap is exceeded */
} p10_writer;

static int p10_need(p10_reader* r, size_t n)
{
    if (r->error || r->size - r->pos < n) {
        r->error = 1;
        return 0;
    }
    return 1;
}

static uint8_t p10_read_u8(p10_reader* r)
{
    return p10_need(r, 1) ? r->data[r->pos++] : 0;
}

static uint16_t p10_read_u16(p10_reader* r)
{
    if (!p10_need(r, 2))
        return 0;
    uint16_t v = (uint16_t) (r->data[r->pos] | (r->data[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}

static uint32_t p10_read_u32(p10_reader* r)
{
    if (!p10_need(r, 4))
        return 0;
    const uint8_t* p = r->data + r->pos;
    r->pos += 4;
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static float p10_read_f32(p10_reader* r)
{
    uint32_t u = p10_read_u32(r);
    float f;
    memcpy(&f, &u, sizeof f); /* no union punning, no strict aliasing issues */
    return f;
}

/* unsigned LEB128, at most 10 bytes (64 bits). the reference decoder keeps
   only 32 bits; see the [defect] note in section 4 */
static uint64_t p10_read_varint(p10_reader* r)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        uint8_t b = p10_read_u8(r);
        if (r->error)
            return 0;
        v |= (uint64_t) (b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
    }
    r->error = 1; /* more than 10 bytes */
    return 0;
}

static int64_t p10_read_svarint(p10_reader* r)
{
    uint64_t u = p10_read_varint(r);
    return (u & 1) ? -1 - (int64_t) (u >> 1) : (int64_t) (u >> 1);
}

/* returns a pointer into the packet (not NUL terminated) and its length */
static const uint8_t* p10_read_bytes(p10_reader* r, size_t* len)
{
    uint64_t n = p10_read_varint(r);
    if (n > 1024 * 1024 || !p10_need(r, (size_t) n)) {
        r->error = 1;
        *len = 0;
        return NULL;
    }
    const uint8_t* p = r->data + r->pos;
    r->pos += (size_t) n;
    *len = (size_t) n;
    return p;
}

/* copies a string into out (always NUL terminated); rejects embedded NULs */
static void p10_read_string(p10_reader* r, char* out, size_t out_size)
{
    size_t len;
    const uint8_t* p = p10_read_bytes(r, &len);
    if (!p || memchr(p, 0, len) || len >= out_size) {
        r->error = 1;
        out[0] = '\0';
        return;
    }
    memcpy(out, p, len);
    out[len] = '\0';
}

typedef struct { float x, y, z; } p10_vec3;

static p10_vec3 p10_read_vec3(p10_reader* r)
{
    p10_vec3 v;
    v.x = p10_read_f32(r);
    v.y = p10_read_f32(r);
    v.z = p10_read_f32(r);
    return v;
}

static int p10_at_end(const p10_reader* r)
{
    return !r->error && r->pos == r->size;
}

/* ---- writer ---- */

static void p10_write_raw(p10_writer* w, const void* d, size_t n)
{
    if (w->error || w->cap - w->size < n) {
        w->error = 1;
        return;
    }
    memcpy(w->data + w->size, d, n);
    w->size += n;
}

static void p10_write_u8(p10_writer* w, uint8_t v)
{
    p10_write_raw(w, &v, 1);
}

static void p10_write_u16(p10_writer* w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t) v, (uint8_t) (v >> 8)};
    p10_write_raw(w, b, 2);
}

static void p10_write_u32(p10_writer* w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)};
    p10_write_raw(w, b, 4);
}

static void p10_write_f32(p10_writer* w, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    p10_write_u32(w, u);
}

static void p10_write_varint(p10_writer* w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        p10_write_u8(w, v ? (uint8_t) (b | 0x80) : b);
    } while (v);
}

static void p10_write_svarint(p10_writer* w, int64_t v)
{
    p10_write_varint(w, v < 0 ? (((uint64_t) (-1 - v)) << 1) | 1 : ((uint64_t) v) << 1);
}

static void p10_write_bytes(p10_writer* w, const void* d, size_t n)
{
    p10_write_varint(w, n);
    p10_write_raw(w, d, n);
}

static void p10_write_string(p10_writer* w, const char* s)
{
    p10_write_bytes(w, s, strlen(s));
}

static void p10_write_vec3(p10_writer* w, p10_vec3 v)
{
    p10_write_f32(w, v.x);
    p10_write_f32(w, v.y);
    p10_write_f32(w, v.z);
}
```

**Usage rule.** Check `r.error` once, after the whole packet has been decoded,
and check `p10_at_end` for packets that have no trailing list. A packet that
fails either check is malformed: the server disconnects with
`MalformedPacket`.

---

## 5. Packet catalogue

### 5.1 Ids [ref]

Direction: S→C = server to client, C→S = client to server.

| Id | Name | Dir | Section |
| --- | --- | --- | --- |
| 1 | `Greeting` | S→C | 6.1 |
| 2 | `InitiateConnection` | C→S | 6.2 |
| 3 | `ServerCertificate` | S→C | 6.3 |
| 4 | `ClientCertificate` | C→S | 6.3 |
| 5 | `Kick` | S→C | 6.4 |
| 6 | `GameStateHeader` | S→C | 7.1 |
| 7 | `MapData` | S→C | 7.2 |
| 8 | `GameStateFinal` | S→C | 7.4 |
| 9 | `GenericCommand` | both | 10 |
| 10 | `MapDataAcknowledge` | C→S | 7.3 |
| 11 | `MapDataFinal` | S→C | 7.3 |
| 30 | `EntityUpdate` | S→C | 8.6 |
| 31 | `EntityEvent` | S→C | 8.8 |
| 32 | `EntityDie` | S→C | 8.9 |
| 33 | `EntityRemove` | S→C | 8.10 |
| 35 | `ClientSideEntityUpdate` | C→S | 8.7 |
| 40 | `TerrainUpdate` | S→C | 9.1 |
| 50 | `PlayerUpdate` | S→C | 8.11 |
| 51 | `PlayerRemove` | S→C | 8.12 |
| 60 | `PlayerAction` | C→S | 8.13 |
| 90 | `HitEntity` | C→S | 9.2 |
| 91 | `HitTerrain` | C→S | 9.3 |
| 92 | `Damage` | S→C | 9.4 |
| 70 | `ChatMessage` **[1.0]** | C→S | 12.2.1 |
| 71 | `ChatBroadcast` **[1.0]** | S→C | 12.2.2 |
| 80 | `GameModeState` **[1.0]** | S→C | 12.2.3 |
| 93 | `BuildBlock` **[1.0]** | C→S | 12.2.4 |
| 94 | `BuildLine` **[1.0]** | C→S | 12.2.5 |
| 95 | `ThrowGrenade` **[1.0]** | C→S | 12.2.6 |

Ids must be below 128 (the decode table has 128 slots). **[1.0]** reserves
70–79 for chat and messages, 80–89 for game modes, 93–99 for combat additions
and 100–127 for our own extensions (section 12).

### 5.2 Connection state machine

```
client                                         server
  | ENet connect (data = 0x08101919)  -------->   | NotInitiated (10 s)
  |                <------------------  Greeting |
  | InitiateConnection  ------------------------> | check protocol name
  |                <---------  ServerCertificate | WaitingForCertificate (60 s)
  | ClientCertificate  -------------------------> |
  |                <-----------  GameStateHeader | MapTransfer (300 s)
  |                <-----------  MapData × N     |
  |                <-----------  MapDataFinal    | CompletingMapTransfer
  | MapDataAcknowledge  ------------------------> |   (after the client decoded the map)
  |                <-----------  GameStateFinal  | Game
  | GenericCommand "join"  ---------------------> | creates player + entity, spawns
  |                <-----  GenericCommand "local-player" <id>
  |                <-----  EntityUpdate / PlayerUpdate / TerrainUpdate ...
```

`OnWorldChanged` (new round, map change) restarts the transfer at
`GameStateHeader` for every connection in `Game`. The player is dropped, so the
client has to `join` again.

### 5.3 Handshake timeouts [ref]

Each pre-game state has a deadline, after which the server disconnects with
`Timeout`: `NotInitiated` 10 s, `WaitingForCertificate` 60 s, the map transfer
states 300 s. There is no timeout in `Game`, where ENet's own peer timeout
applies.

---

## 6. Session packets

### 6.1 `Greeting` (1, S→C) [ref]

| Field | Type | Notes |
| --- | --- | --- |
| magic | `string` | always `"Hello"`; the client rejects anything else |
| nonce | `bytes` | server nonce, 128 random bytes |

```
01  05 48 65 6C 6C 6F  80 01 <128 bytes>
```

### 6.2 `InitiateConnection` (2, C→S) [ref]

| Field | Type | Notes |
| --- | --- | --- |
| protocolName | `string` | must equal `"WorldOfSpades 0.1"` byte for byte, else `ProtocolMismatch` |
| majorVersion | `u16` | client version (informational) |
| minorVersion | `u16` | |
| revision | `u16` | |
| packageString | `string` | e.g. `"OpenSpades 0.0.12"`, ≤ 256 bytes |
| environmentString | `string` | OS/CPU description, ≤ 1024 bytes |
| locale | `string` | e.g. `"pt_BR"`, ≤ 256 bytes |
| playerName | `string` | |
| nonce | `bytes` | client nonce, 256 random bytes |
| mapQuality | `u8` | 1..100; the server clamps it to 20..100 (`cg_mapQuality`). **[1.0]** 0 = "send VXL" |

**[1.0] Map format negotiation.** The reference client always sends 1..100
and always gets ngmap. We give the value 0, which the reference never sends,
the meaning "this client cannot decode ngmap, send `map-format=vxl`"; any
value 1..100 means "ngmap is fine, at this quality" and the server encodes at
the value clamped to 20..100. This needs no codec or packet change, keeps an
OpenSpades client on ngmap, and lets a client without the codec (or a test)
ask for VXL. The server still answers VXL to a 1..100 client when ngmap is
not possible (encoding failed, map above the ngmap limits), so a client must
honour `map-format` in the GameStateHeader (7.1), not assume what it asked
for. **Our server no longer reads `mapQuality`**: every join gets the map's
stored ngmap at quality 100 as it is (7.1), so our client sends 100 and has
no map quality setting.

The session nonce is `serverNonce || clientNonce`. It is what certificates sign
(6.3).

```c
typedef struct {
    char protocol_name[257];
    uint16_t major, minor, revision;
    char package[257];
    char environment[1025];
    char locale[257];
    char player_name[64]; /* [1.0] limit, the reference has none */
    uint8_t nonce[256];
    size_t nonce_len;
    uint8_t map_quality;
} p10_initiate;

static int p10_decode_initiate(const uint8_t* d, size_t n, p10_initiate* out)
{
    p10_reader r = {d, n, 1, 0}; /* pos 1: skip the type byte */
    size_t len;
    const uint8_t* nonce;

    p10_read_string(&r, out->protocol_name, sizeof out->protocol_name);
    out->major = p10_read_u16(&r);
    out->minor = p10_read_u16(&r);
    out->revision = p10_read_u16(&r);
    p10_read_string(&r, out->package, sizeof out->package);
    p10_read_string(&r, out->environment, sizeof out->environment);
    p10_read_string(&r, out->locale, sizeof out->locale);
    p10_read_string(&r, out->player_name, sizeof out->player_name);
    nonce = p10_read_bytes(&r, &len);
    if (nonce && len <= sizeof out->nonce) {
        memcpy(out->nonce, nonce, len);
        out->nonce_len = len;
    } else {
        r.error = 1;
    }
    out->map_quality = p10_read_u8(&r);
    return p10_at_end(&r); /* 1 = ok */
}
```

### 6.3 `ServerCertificate` (3, S→C) and `ClientCertificate` (4, C→S) [ref]

Same layout for both:

| Field | Type | Notes |
| --- | --- | --- |
| isValid | `u8` | 0 = no certificate; then nothing follows |
| certificate | `bytes` | only if `isValid != 0` |
| signature | `bytes` | session nonce signed with the private key; only if `isValid != 0` |

**[ref]** Both sides always send `isValid = 0`. Nothing is validated, and the
certificate format was never defined.

**[1.0]** Keep the packets and send `isValid = 0` for now. They are the place
to add authenticated accounts later (for example an Ed25519 key per player),
without changing the handshake.

The client may send `ClientCertificate` right after `InitiateConnection`
without waiting for the server certificate. The reference client does wait.

#### 6.3.1 The connect token (ours)

Our server admits a 1.0 player over UDP from a non-local address only with
a token from our master: a flood of joins then has to ask the master, one
per connection, under its per address limit. After the Greeting the client
posts `{"address": "ip:port", "nonce": base64}` (the server's address it
connected to, the Greeting's 128 byte nonce) to the master's `POST
/v1/token`, and sends the answer as its `ClientCertificate` (`isValid` 1):

| Part | Bytes |
| --- | --- |
| certificate | `"SADT"`, version `u8` 1, the server's Ed25519 public key (32), expiry `u64` little endian (unix seconds, 60 s on), nonce length `u8`, the nonce |
| signature | Ed25519 by the master over the certificate (64) |

The server learns the master's public key from every announce's answer
(`token_key`) and checks the format, its own key, the expiry, the nonce of
this connection and the signature; a refusal is a Kick "Join this server
from the server list" (`EVT proto10 token action=ok|refused why=format|
server|expired|nonce|signature|none`). Not asked: a local address, a WebRTC
connection (its offer went through the master), a server that has not
heard the key for 120 s (a master down locks nobody out), `[master]
connect_tokens = false`. The client waits up to 5 s for the master before
sending an empty certificate (`CEVT connect_token action=ask|got|failed|skip`).

### 6.4 `Kick` (5, S→C) [ref]

| Field | Type |
| --- | --- |
| reason | `string` (shown to the player) |

The server sends `Kick` and then disconnects with `Misc` (3), using
`enet_peer_disconnect_later` so the packet is delivered first.

---

## 7. Game state transfer

### 7.1 `GameStateHeader` (6, S→C) [ref]

| Field | Type |
| --- | --- |
| properties | `map` |

Starts a transfer. The reference sends an empty map. **[1.0]** puts the map
description here, so the client can show it and allocate memory before the
data arrives:

| Key | Example | Meaning |
| --- | --- | --- |
| `map-name` | `Pinpoint` | |
| `map-format` | `ngmap` or `vxl` | how to decode the `MapData` stream (7.5) |
| `map-size` | `512,512,64` | width, height, depth |
| `map-bytes` | `1834211` | total stream size, for a progress bar (optional) |
| `map-crc32` | `3f2a91c7` | CRC32 (zlib's) of the exact `MapData` stream, 8 hex digits, and the client's cache key with `map-bytes` (optional: a server that sends none never gets an answer) |

`map-format` is `ngmap` on our server, whatever `mapQuality` said: the map's
stored stream at quality 100 (the maps repository's `.ngmap`, or the map's
encoding made once at load and kept), sent as it is, the cells changed since
the load following as `TerrainUpdate`. It is `vxl` only for a map above
ngmap's limits or an encoding that failed. The reference server followed
the rule of 6.2 (`vxl` for 0, `ngmap` at that quality for 1..100).

**A client that already holds the stream** answers the header with the
`GenericCommand` `map-cached <1>` (section 10) and the server sends no
`MapData` at all: straight to `MapDataFinal` (7.3) and on, exactly as after
the last fragment. It answers `map-cached 0` when it does not, and the
fragments follow as below. The server waits for the answer for 3 real
seconds (`P10_TIMEOUT_MAP_ANSWER`, `Session.c`) and then sends the map
anyway, so a client that does not speak the command only loses those 3 s.

The key is the stream, not the map: the CRC32 and length of the bytes the
server would send (`map-crc32`, `map-bytes`), which is what 0.76's MapStart
does with its map's CRC32. Two qualities of one map are two streams and two
cache files, and so is the same map after an edit made before its snapshot.
The client keeps them in the player's folder as
`cache/maps/<CRC32>-<bytes>.ngmap` or `.vxlz` (the client's `userdata.c`, the
same folder and the same LRU trim as the 0.76 cache). A demo records its map,
so a recording client never answers from the cache, and neither does a
listen server's own map (it is on the disk already). The header's stream is
still followed by the TerrainUpdate of the edits made since the snapshot;
the client keeps those until its map is ready whether the stream arrived or
came from the cache.

Our server (`Server/Proto10/Session.c`) encodes ngmap on a background thread
from a VXL snapshot taken when the ClientCertificate arrives (~0.3 s for
Raindance, ~1.25 s for Gridwar, so the 60 Hz tick must not wait). Nothing is
sent until the encoding is done, because a second GameStateHeader during a
transfer is not allowed: that is what lets a failed encoding still fall back
to a VXL transfer. Every terrain edit made after the snapshot is logged on
the encoding and replayed to the joiner as TerrainUpdate right after its
GameStateHeader (the client keeps those until the map is decoded, 7.3).
Joiners asking for the same quality share one encoding, and the result is
cached per (map, quality) while fewer than 4096 cells changed since the
snapshot; a new map (`map_load`) drops the cache. `map-bytes` is therefore
always exact.

### 7.2 `MapData` (7, S→C) [ref]

| Field | Type |
| --- | --- |
| fragment | `bytes` (up to 4096 bytes in the reference) |

The concatenated fragments form the map stream. The server encodes on a
background thread, stops when more than 1 MiB is queued, and only sends while
the peer has less than 64 KiB pending.

**[defect]** `HostPeer::GetPendingBytes()` always returns 0, so the 64 KiB
throttle does nothing. **[1.0] done**: `conn_pending` (ENet's bytes in flight
and queued, WebRTC's buffered amount), fragments while under 64 KiB, at most
64 a tick (`P10_MAP_PENDING_MAX`, `P10_MAP_FRAGMENTS_MAX`, `Session.c`). The
end of a transfer logs `EVT <player> proto10 map_sent bytes= ticks= ms=`.
A `map-cached 1` answer sends no fragment at all (7.1) and logs
`EVT <player> proto10 map_cached hit=1 bytes= crc= why=answer` instead; the
same line with `hit=0` and `why=answer|timeout` says the map is being sent.

### 7.3 `MapDataFinal` (11, S→C) and `MapDataAcknowledge` (10, C→S) [ref]

Both are empty. The server sends `MapDataFinal` after the last fragment. The
client decodes the whole map, then answers with `MapDataAcknowledge`. Only
after the acknowledge does the server send `GameStateFinal`, so the snapshot is
taken as late as possible.

`TerrainUpdate` packets that arrive during the transfer are kept by the client
and applied, in order, after the map is decoded (`savedMapEdits`,
`NGClient/NetworkClient.cpp`). The server must therefore **keep** broadcasting
terrain edits to connections that are still loading.

Ours does: `TerrainUpdate` goes to every 1.0 connection in `map_transfer`,
`completing_map` or `game`. The client (the client's `proto10_world.c`) keeps the
packets it receives before `MapDataFinal` and applies them right after
decoding the map, before `MapDataAcknowledge`.

### 7.4 `GameStateFinal` (8, S→C) [ref]

| Field | Type |
| --- | --- |
| properties | `map` (world parameters, section 7.6) |
| entityCount | `varint` |
| entities | `entityCount` × entity update item (8.4), each with the `Create` bit |
| playerCount | `varint` |
| players | `playerCount` × player update item (8.11), each with the `Create` bit |

**[defect]** The server never fills `players` (`FinalStateTransfer`, which
sends `0`), so a joining client does not learn about players already in the
match until each of them changes. **[1.0]**: send every player.

After this packet the connection is in `Game`.

### 7.5 Map formats

#### `ngmap` [ref] (`Client/GameMap_ngmap.cpp`)

The whole stream is a **zlib** stream (`DeflateStream` calls `deflateInit`,
so it has the 2-byte zlib header and the Adler-32 trailer, not raw deflate).
Decompressed:

```
u32  signature = 0x1145148A
u16  width
u16  height
u16  depth          (≤ 64: columns are stored as 64-bit solid masks)
u8   quality        (1..100)
geometry            one entry per column, rows in boustrophedon order:
                    for y in 0..h-1: x goes 0..w-1 on even y, w-1..0 on odd y
u32  jpegLength
u8[jpegLength]      JPEG image holding the lossy colour tiles
color blocks        to the end of the stream
```

Column entry: a format byte, then the data.

- **RLE** (`format == 0`): a list of `u8`. Bit z of the column's solid mask is
  1 when voxel z is solid (z grows downwards, `z = 0` is the top). Each byte is
  a z at which solidity toggles. The **first** byte carries `0x80` when the
  column starts solid at `z = 0`. The **last** byte is always `depth`, which
  terminates the column.
- **Bitmap** (`format & 1`): bits 1..4 of the format byte say which 16-voxel
  slices are non-empty. Each non-empty slice follows as a `u16` mask, top
  slice first. The encoder picks whichever of the two is shorter.

Colour section: the map is cut into 8×8×8 cells. Only voxels that can be
seen (solid with at least one empty neighbour) need a colour, and cells
without such voxels are skipped. Within a column of cells, odd-x cells run z
from the bottom up. Each emitted cell has a format byte (`BlockFormat |
sub-format`):

| Format | Value | Sub-formats |
| --- | --- | --- |
| Constant | `0x00` | one colour for every visible voxel |
| LinearX/Y/Z | `0x01`–`0x03` | `0x00` gradient between 2 colours, `0x20` raw 1D image |
| PlanarX/Y/Z | `0x04`–`0x06` | `0x00` 4-corner interpolation, `0x10` lossy (a tile inside the JPEG), `0x20` raw 2D image |
| Volumetric | `0x07` | `0x00` 8-corner interpolation, `0x10` raw voxels |

The encoder tries the cheapest format first and accepts it when the error
stays under a threshold derived from `quality` (`diversityLimit = 34 −
quality/3`).

**[1.0]**: do not re-implement ngmap from this description. Port
`GameMap_ngmap.cpp` (about 1,900 lines of C++), or keep it as a C++
translation unit with a C interface (`ngmap_encode`/`ngmap_decode`), and test
it against the reference with round trips on real maps. It needs zlib (we have
it) and a JPEG codec (the reference uses `jpge.cpp` for encoding and SDL_image
for decoding; `stb_image` can decode).

**Implementation** (milestone 7, codec only): `common/ngmap/`, a C++ port of
`GameMap_ngmap.cpp` behind a C API (`ngmap.h`: `ngmap_encode` VXL bytes →
ngmap, `ngmap_decode` ngmap → VXL bytes, `ngmap_strerror`), with its own VXL
reader/writer, `jpge.cpp` (vendored from the reference) and `stb_image.h`
(vendored, public domain) for the JPEG tiles. Built into the client, the
server and `tools/p10probe` through `common/ngmap/ngmap.cmake` (a static
library, C++11, included before the server's C-only flags). The decoder streams the inflate output, rejects maps above
2048×2048×64 and more than 64M coloured voxels before allocating, and bounds
the JPEG by the number of coloured cells. Details the description above gets
wrong or leaves out:

- the colour threshold is squared: `diversityLimit = (34 − quality/3)²`,
  compared with the squared RGB range of a cell;
- which voxels get a colour is computed identically by both sides and must
  stay bit-exact (`ComputeNeedsColor`, ported with its quirks: out-of-map
  columns are air, the layer `d − 1` counts as solid, and for depths under
  ~24 earth bits leak into real voxels);
- JPEG tiles are laid out bottom-up and each tile is stored upside down,
  because the reference's `JpegWriter` writes the bitmap bottom-up;
- `Volumetric` sub-format `0x00` (8-corner) is listed but was never
  implemented by either side; only `0x10` (raw) exists.

`make test-ngmap` (ASan/UBSan) round-trips the 10 maps in
`server/Resources/maps/`: geometry is exact at every quality; at q100 every
visible voxel's colour is within 1 per channel (100% exact on 8 maps, 99.8%
on Olympic); at q80 the mean error is 0.04–2.5 per channel, with isolated
voxels off by up to ~190 (JPEG and gradient fits on high-contrast cells).
Size against the zlib-9 VXL: q100 44–111% (Pinpoint is the one larger), q80
17–76% (Raindance 853 KB → 149 KB, Clock_Town_arena 738 KB → 153 KB,
Gridwar 1.65 MB → 751 KB). Malformed input (every truncation up to 4 KiB,
then strided; 9,000 random bit flips in the stream and in the inflated
payload; hostile headers) is always rejected or decodes to a well-formed VXL,
with no sanitizer report. Nothing was tested against a running OpenSpades.

#### `vxl` [1.0]

The stream is the zlib-compressed VXL, exactly the bytes 0.75 sends in
`MapChunk`. This lets milestone 1 (section 14) ship without ngmap. The server
already has this encoder, so a 1.0 connection costs nothing extra.

### 7.6 World parameters [ref] (`Game/World.cpp`)

All values are strings, parsed with `stof`:

| Key | Default | Meaning |
| --- | --- | --- |
| `player-jump-vel` | `0.36` | upward velocity added on jump |
| `fall-damage-vel` | `0.58` | landing speed where fall damage starts |
| `fall-damage-fatal-vel` | `1` | landing speed that kills |
| `fog-distance` | `96` | |
| `fog-color` | `0.6,0.7,0.9` | r,g,b in 0..1 |

**[defect]** An unknown key makes `WorldParameters::Update` throw ("Unknown
property"), which disconnects the client. **[1.0]**: ignore unknown keys,
which makes this the natural place for new server-driven settings. 1.0 adds `gamemode` (the mode's wire name), `team1-name`,
`team1-color`, `team2-name`, `team2-color`, `capture-limit` and `aim-assist`,
replacing what 0.75 sends in `StateData`, and the perks' keys (`jetpack-fuel`,
`sentry-lifetime`: 12.4, 12.5). `fog-distance` is the server's `[server]
fog_distance`. No `respawn-time`: the respawn delay rides each death
(`EntityDie.param`, 8.9). The reference's three physics keys are the
server's own figures, `[gameplay] jump_velocity` and `fall_damage_velocity`
in config.toml (0.36 and 0.58, the 0.75 profile's always; fatal is where a
fall takes 100), and the client plays by the first two (`player_jump_velocity`,
`player_fall_damage_velocity`, reset when a match is left; `CEVT
world_physics`). A class's own jump still wins.

---

## 8. Entities and players

### 8.1 Concepts [ref]

- A **player** (`game::Player`) is a participant: id, name, score, flags. It
  exists whether or not it has a body.
- An **entity** (`game::Entity`) is anything simulated in the world. A player's
  body is a `Player` entity owned by that player.
- **[ref] rule:** a player's body entity id **equals** its player id. The
  server links the entity with the player id explicitly
  (`World().LinkEntity(pe, *GetId())` in `Game/Player.cpp`; `LinkEntity`
  insists that player entity ids are explicit), and the client finds the body
  with `FindEntity(player id)` (`NetworkPlayer::GetEntity`). Other entities get
  automatically allocated ids. **[1.0]**: keep the rule, and allocate
  non-player ids from a range that never collides with player ids (players
  0..255, other entities from 256 up).
  **Implemented (milestone 6)**: grenade entities take ids from 256 to
  65791 and wrap (`Server/Proto10/Entities.c`; a grenade lives 3 s, so a
  reused id never meets a live one). The client keeps ids >= 256 in its own
  small table (the client's `proto10_entities.c`); `players[]` stays the
  model for 0..255.
- **Bots** (our rule: bots are players) are player records whose entity no
  client owns, simulated by the server. No special case is needed on the wire.

### 8.2 Coordinates [ref]

Same axes as the map and as our server: `x`, `y` horizontal, **`z` grows
downwards**, `z = 0` is the top of the world. Gravity is `(0, 0, +32)` blocks/s²
(`World::GetGravity`), and a jump subtracts `player-jump-vel` from `velocity.z`.
Positions are in blocks; velocities in blocks per second.

This is **not** our client's internal layout (`pos.y` is up and mirrored,
`internal_y = 63 − packet_z`; see `CONTRIBUTING.md`). Convert at the protocol
boundary exactly as for 0.75.

### 8.3 Enumerations [ref] (`Game/Constants.h`)

```c
enum p10_entity_type  { P10_ENT_PLAYER, P10_ENT_GRENADE, P10_ENT_ROCKET, P10_ENT_COMMAND_POST,
                        P10_ENT_FLAG, P10_ENT_CHECKPOINT, P10_ENT_VEHICLE };
enum p10_event_type   { P10_EV_JUMP, P10_EV_RELOAD_WEAPON, P10_EV_SUICIDE, P10_EV_EXPLODE };
enum p10_death_type   { P10_DEATH_UNSPECIFIED, P10_DEATH_PLAYER, P10_DEATH_EXPLODE };
enum p10_tool_slot    { P10_TOOL_MELEE, P10_TOOL_BLOCK, P10_TOOL_WEAPON1, P10_TOOL_WEAPON2,
                        P10_TOOL_WEAPON3 };
enum p10_stance       { P10_STANCE_STAND, P10_STANCE_CROUCH, P10_STANCE_PRONE };
enum p10_weapon_type  { P10_WEAPON_BULLET, P10_WEAPON_SHOTGUN, P10_WEAPON_ROCKET,
                        P10_WEAPON_GEAR }; /* [1.3] ours, see 8.4 */
enum p10_hit_type     { P10_HIT_UNSPECIFIED, P10_HIT_TORSO, P10_HIT_HEAD, P10_HIT_LIMB };
enum p10_damage_type  { P10_DMG_UNKNOWN, P10_DMG_SUICIDE, P10_DMG_FALL, P10_DMG_POISON,
                        P10_DMG_MELEE, P10_DMG_DIG, P10_DMG_BLOCK, P10_DMG_WEAPON1,
                        P10_DMG_WEAPON2, P10_DMG_WEAPON3, P10_DMG_GRENADE,
                        P10_DMG_VEHICLE_EXPLOSION, P10_DMG_VEHICLE };
enum p10_block_create { P10_BLOCK_CREATE_UNSPECIFIED, P10_BLOCK_CREATE_PLAYER };
enum p10_block_destroy{ P10_BLOCK_DESTROY_UNSPECIFIED, P10_BLOCK_DESTROY_DAMAGE,
                        P10_BLOCK_DESTROY_COLLAPSE };
```

Mapping to 0.75 concepts (for the server's translation layer):

| 0.75 | 1.0 |
| --- | --- |
| spade | `P10_TOOL_MELEE` (digging is `HitTerrain` with `P10_DMG_DIG`) |
| block | `P10_TOOL_BLOCK` |
| gun | `P10_TOOL_WEAPON1`, the class's primary gun (outside a class, the 0.75 gun), with the class's `WeaponParameters` |
| second gun | **[1.3]** `P10_TOOL_WEAPON2`, the class's secondary gun (the revolver); a class without one carries it as zero parameters (magazine 0), and the server refuses the slot |
| grenade | **[1.3]** `P10_TOOL_WEAPON3` (revision 1.3: Weapon2 is the class's second gun; both programs map the 0.75 grenade tool to Weapon3) |
| kill type headshot / weapon / spade / grenade / fall | `HitType` + `DamageType` in `Damage`/`EntityDie` |

### 8.4 Entity update item [ref]

Used by `EntityUpdate`, `ClientSideEntityUpdate` and `GameStateFinal`.

```
varint  entityId
u8      mask              which optional parts follow, in this order:
  bit 0 Create       -> u8 entityType
                        if entityType == Player:
                            bytes bodySkin
                            bytes weaponSkin[3]
                            weaponParams[3]
  bit 1 Flags        -> u8 entityFlags
  bit 2 Trajectory   -> trajectory (8.5)
  bit 3 PlayerInput  -> u8 inputFlags, i8 xmove, i8 ymove
  bit 4 Tool         -> u8 toolSlot
  bit 5 BlockColor   -> color (B,G,R)
  bit 6 Health       -> u8 health
```

Entity flags: bit 0 `playerClip` (players collide with it), bit 1
`weaponClip` (bullets hit it), bit 2 `fly`, **[1.1]** bit 3 `jetpack` (this
player body wears an equipped jetpack, 12.4). Bits 4–7 are rejected.

**[1.3] The three weapon slots.** A Player's `Create` fills all three
`weaponParams`: `[0]` the primary gun (the class's, or the 0.75 gun outside a
class), `[1]` the secondary gun (all zero, magazine 0, when the class has
none), `[2]` the gear — the grenades, or the Miner's drill — with `type` 3 =
Gear (`P10_WEAPON_GEAR`: ours, the free value of the 2-bit type field), its
magazine the grenades or the drill's charges and its fire interval 1000 ms.
`weaponSkin[i]` is that weapon's name (`rifle`, `smg`, `shotgun`, `rocket`,
`revolver`, empty for none; the gear `grenade` or `drill`) and `bodySkin` is
`class/<name>` (`class/commando`) in a classes match, else `default`. The
decoder refuses a skin over 32 bytes or with a byte outside
`a-z 0-9 _ / -` (empty is fine), and a `toolSlot` above 4.

Input flags: bit 0 `toolPrimary`, bit 1 `toolSecondary`, bit 2 `chat`
(typing, used to show a chat bubble), bit 3 `sprint`, **[1.1]** bit 4
`jetpack` (`P10_IN_JETPACK`: thrust wanted, 12.4), bits 6–7 `stance`. Bit 5
is unused and rejected by the decoder, as is stance 3.
Every 1.0 peer reads both bits; nothing is stripped for anyone (12.1).
`xmove` is strafe and `ymove` is forward, both −127..127; the physics divides
by 127 and clamps the length to 1.

Weapon parameters (9 bytes):

```
u8   flags   bit 0 fullAutomatic, bit 1 reloadsSlow (one round at a time),
             bit 2 penetrative, bits 3-4 weaponType
u16  fireInterval   ms between shots
u16  reloadTime     ms
u16  raiseTime      ms to bring the weapon up
u8   maxDamage      percent of 100 hp
u8   magazineSize   rounds
```

The field order on the wire is **not** the struct order (`reloadTime` and
`raiseTime` come before `maxDamage`). Follow the table above.

```c
typedef struct {
    uint8_t full_auto, slow_reload, penetrative, type;
    uint16_t fire_interval_ms, reload_ms, raise_ms;
    uint8_t max_damage, magazine;
} p10_weapon_params;

static void p10_write_weapon(p10_writer* w, const p10_weapon_params* p)
{
    p10_write_u8(w, (uint8_t) ((p->full_auto ? 1 : 0) | (p->slow_reload ? 2 : 0) |
                               (p->penetrative ? 4 : 0) | ((p->type & 3) << 3)));
    p10_write_u16(w, p->fire_interval_ms);
    p10_write_u16(w, p->reload_ms);
    p10_write_u16(w, p->raise_ms);
    p10_write_u8(w, p->max_damage);
    p10_write_u8(w, p->magazine);
}

/* the 0.75 rifle as our server sends it (_weapon_params in
   server/Source/Server/Proto10/Bridge.c): max damage is its body shot, 49 */
static const p10_weapon_params P10_RIFLE = {
    .full_auto = 0, .slow_reload = 0, .penetrative = 0, .type = P10_WEAPON_BULLET,
    .fire_interval_ms = 500, .reload_ms = 2500, .raise_ms = 500,
    .max_damage = 49, .magazine = 10,
};
```

Delta rule [ref] (`Server/ServerEntity.cpp`): the server remembers what it last
broadcast and sends only the fields whose value changed. The first item for an
entity carries `Create` plus every field. An item with `mask == 0` is never
sent.

`ServerEntity::Serialize()` fills type, flags, trajectory and health for every
entity. `PlayerServerEntity::Serialize()` adds input, tool, block colour, and
the skins and weapon parameters in the `Create` part. Ownership is not on the
wire: it follows from the id rule in 8.1. **[1.0]**: add an explicit owner
field later if vehicles (several entities driven by one player) need it.

### 8.5 Trajectory [ref]

```
u8      type | 0x80 if a parent entity follows
vec3    origin
vec3    velocity
if type in {Constant, Linear, Gravity, RigidBody}:
    rotation angle            (compact quaternion, section 4)
    vec3     angularVelocity  (rotation axis × radians per second)
if type == Player:
    vec3     eulerAngle       (orientation of the view)
if type & 0x80:
    varint   parentEntityId   (the trajectory is relative to that entity)
```

| Value | Type | Client-side motion |
| --- | --- | --- |
| 0 | `Constant` | does not move |
| 1 | `Linear` | `origin += velocity·dt` |
| 2 | `Gravity` | `origin += velocity·dt + ½·g·dt²`, `velocity += g·dt`, `g = (0,0,32)` |
| 3 | `Player` | the player physics (`PlayerEntity`) driven by the last input |
| 4 | `RigidBody` | free body with collisions (not implemented) |

**Player orientation [1.0].** For `Player` trajectories, `euler.x` is the
pitch (positive looks down, since z grows downwards), `euler.z` the yaw and
`euler.y` the roll (always 0). The view direction in map coordinates is
`front = (−cos(x)·sin(z), cos(x)·cos(z), sin(x))`, which is the reference's
`Rz(yaw)·Rx(pitch)` applied to `(0, 1, 0)`. `common/proto10/orient.h`
converts both ways. Player velocities are in the AoS physics units both of
our programs already use (not blocks per second); they are informational, the
receiver trusts positions.

The comments in `Constants.h` give the gravity term as `− ½G·t²`. The code
(`Entity.cpp`) adds `+½·g·dt²` with `g` pointing down (+z). **The code is
normative.**

Sending a grenade as a `Gravity` trajectory once lets every client simulate
the whole arc. In 0.75 that took a `GrenadePacket` plus client-side physics
outside the protocol.

```c
typedef struct {
    uint8_t type;
    p10_vec3 origin, velocity;
    p10_vec3 rotation;        /* compact quaternion (x,y,z), w >= 0 */
    p10_vec3 angular_velocity;
    p10_vec3 euler;           /* Player only */
    int has_parent;
    uint32_t parent;
} p10_trajectory;

enum { P10_TRAJ_CONSTANT, P10_TRAJ_LINEAR, P10_TRAJ_GRAVITY, P10_TRAJ_PLAYER, P10_TRAJ_RIGID_BODY };

static void p10_write_trajectory(p10_writer* w, const p10_trajectory* t)
{
    p10_write_u8(w, (uint8_t) (t->type | (t->has_parent ? 0x80 : 0)));
    p10_write_vec3(w, t->origin);
    p10_write_vec3(w, t->velocity);
    if (t->type == P10_TRAJ_PLAYER) {
        p10_write_vec3(w, t->euler);
    } else {
        p10_write_vec3(w, t->rotation);
        p10_write_vec3(w, t->angular_velocity);
    }
    if (t->has_parent)
        p10_write_varint(w, t->parent);
}

static int p10_read_trajectory(p10_reader* r, p10_trajectory* t)
{
    uint8_t b = p10_read_u8(r);
    t->type = b & 0x7f;
    t->has_parent = (b & 0x80) != 0;
    if (t->type > P10_TRAJ_RIGID_BODY)
        return 0; /* the reference raises "Unknown trajectory type" */
    t->origin = p10_read_vec3(r);
    t->velocity = p10_read_vec3(r);
    if (t->type == P10_TRAJ_PLAYER) {
        t->euler = p10_read_vec3(r);
    } else {
        t->rotation = p10_read_vec3(r);
        t->angular_velocity = p10_read_vec3(r);
    }
    if (t->has_parent)
        t->parent = (uint32_t) p10_read_varint(r);
    return !r->error;
}

/* client-side extrapolation of a Gravity trajectory (grenade arc) */
static p10_vec3 p10_gravity_position(const p10_trajectory* t, float dt)
{
    p10_vec3 p = t->origin;
    p.x += t->velocity.x * dt;
    p.y += t->velocity.y * dt;
    p.z += t->velocity.z * dt + 0.5f * 32.0f * dt * dt;
    return p;
}
```

### 8.6 `EntityUpdate` (30, S→C) [ref]

```
u8   forced        1 = apply even to entities this client owns
item*              entity update items until the end of the packet
```

The server builds one `EntityUpdate` per tick with every entity whose delta is
non-empty, and broadcasts it. A client ignores the parts of items for entities
it owns, unless `forced` is set or the item has `Create`. That is how the
server corrects a client it disagrees with.

An item for an unknown id **without** `Create` is ignored, with a log line.

### 8.7 `ClientSideEntityUpdate` (35, C→S) [ref]

```
item*   entity update items until the end of the packet (never with Create)
```

The client reports the state of the entity it owns: trajectory, input when it
changed, tool and block colour. Movement stays client-authoritative, as in
0.75, but the server checks it:

- Items for entities the client does not own are dropped (logged).
- A change of trajectory **type** is refused and corrected.
- `TryUpdateTrajectory` (`Server/ServerEntity.cpp`) accepts the new trajectory
  only if the reported position is within **0.1 block** of the server's
  extrapolation along the old velocity (the along-track error is allowed up to
  ±0.1), and the velocity differs by less than 0.1. Otherwise the server
  sends back a `forced` correction.

**[defect]** The visitor ends with `SPNotImplemented()`, which throws, so
the server kicks the client after processing its first update. **[1.0]**:
remove it.

**[defect]** The client sends the full state (no delta) **reliably every
frame** (`NetworkPlayer::Update`, "TODO: reduce send interval"). **[1.0]**:
send at a fixed rate (30 Hz), unreliable sequenced on channel 1, and include the
input whenever it changed.

**[1.0] anti-cheat:** the 0.1-block window is a good starting point. Our
server already has 0.75's position checks; reuse their tolerances and log
rejected updates with an `EVT` line (`CONTRIBUTING.md`, logging).

### 8.8 `EntityEvent` (31, S→C) [ref]

```
varint  entityId
u8      eventType   (Jump, ReloadWeapon, Suicide, Explode)
varint  param       event-specific, 0 if unused
```

**[ours]** `ReloadWeapon` on a player entity is sent to every client but the
reloader when the server accepts a reload (`proto10_bridge_reload`); the
client plays the reload sound on that player and refills its view of the
clip, which is what lets it keep drawing that player's shots.

### 8.9 `EntityDie` (32, S→C) [ref]

```
varint  entityId
u8      deathType
varint  param
optional damage info:
  u8    damageType | 0x80 if fromEntity follows
  vec3  firePosition
  vec3  hitPosition
  varint fromEntity   (only with 0x80)
```

The damage info is present when bytes remain after `param`.

**[1.0] param of a player death:** `reason | respawn << 8`, where `reason`
is the 0.75 kill type (0 weapon, 1 headshot, 2 melee, 3 grenade, 4 fall,
5 team change, 6 weapon change) and `respawn` the seconds until the respawn.
Carrying the 0.75 reason as is lets a 1.0 kill feed say exactly what a 0.75
one says. `fromEntity` is the killer, omitted for suicides. A gun death's
`damageType` names the gun in hand, `Weapon1` or `Weapon2` (9.2).

**[defect]** The decoder reads the `damageType` byte **before** checking for
the end of the packet, but the encoder writes nothing when there is no damage
info. A death without damage info therefore fails with "packet truncated".
**[1.0]**: test for the end of the packet first, then read the byte.

This is the packet behind the **kill feed**: victim = `entityId`, killer =
`fromEntity`, weapon/headshot from `damageType` and the last `Damage`.

### 8.10 `EntityRemove` (33, S→C) [ref]

```
varint  entityId
```

**[defect]** `EntityRemovePacket::Decode` constructs an `EntityDiePacket`
(copy and paste error), so a decoded remove is dispatched as a death. Our C
code dispatches on the type byte and does not have this problem; just do not
port it.

### 8.11 `PlayerUpdate` (50, S→C) and player update item [ref]

The packet is a list of items until the end of the packet:

```
varint  playerId
u8      mask
  bit 0 Create  -> string name
  bit 2 Flags   -> u8 playerFlags   (bit 0 isAdmin)
  bit 1 Score   -> varint score
```

The fields are written in the order **Create, Flags, Score**, which is not the
bit order (Score is bit 1, Flags is bit 2). Follow the order above.

**[1.0]** (implemented) adds to the mask, written after Score in this order: bit 3 `Team`
(`u8`: 0 team 1, 1 team 2, 255 spectator), bit 4 `Kills`/`Deaths` (`varint`,
`varint`), bit 5 `Ping` (`varint` ms). These replace 0.75's `ExistingPlayer`,
`ChangeTeam` and the scoreboard fields.

**Mask bits are not self-describing.** A decoder that does not know a bit
cannot tell how many bytes belong to it, and the next item in the list is then
misread. Until the game is public, a new bit simply goes into the protocol and
both programs change together (12.1); once it is public, new bits will need a
revision both sides agree on. The same holds for entity update items (8.4).

### 8.12 `PlayerRemove` (51, S→C) [ref]

```
varint*   player ids until the end of the packet
```

### 8.13 `PlayerAction` (60, C→S) [ref]

```
u8      eventType   (Jump, ReloadWeapon, Suicide)
varint  param
```

A client request to perform an action on its own entity. The server answers
with an `EntityEvent` once it accepts it. **[ref]** The server does not
implement it yet.

---

## 9. World interaction

### 9.1 `TerrainUpdate` (40, S→C) [ref]

A list of edits until the end of the packet. Positions are deltas from a
cursor that starts at `(0,0,0)`:

```
for each edit:
    i8  dx            if dx == -128 (0x80): the cursor moves:
                          varint cx, varint cy, varint cz   (absolute)
                          i8 dx                             (re-read)
    i8  dy
    i8  dz
    position = cursor + (dx, dy, dz)
    u8  health        0 = the block is destroyed, otherwise it is created
    if health != 0:
        u8  red
        u8  green
        u8  blue                 (OpenSpades stores health << 24 | b << 16 | g << 8 | r
                                  and writes the low three bytes first)
        u8  createType
    else:
        u8  destroyType
```

The encoder moves the cursor only when a delta does not fit in −127..127. It
then emits the absolute cursor and a zero delta. **The cursor does not follow
the edits**: deltas are always relative to the last explicit cursor.

**Careful:** this is red, green, blue, the **opposite** of the `color`
primitive (blue, green, red) used for an entity's block colour (8.4). Both
orders come from the reference (`GameMap::GetColor` keeps red in the low byte,
`NetPacketWriter::WriteColor` writes blue first).

Example: create an orange block (r=255, g=128, b=0) at (10, 20, 30) with
health 100, by a player:

```
28  0A 14 1E  64  FF 80 00  01
```

Destroy the block at (300, 5, 63) because its health reached zero. The x delta
does not fit, so the cursor moves first:

```
28  80 AC 02 05 3F  00 00 00  00  01
```

The absolute cursor coordinates are **unsigned** varints. Map coordinates are
never negative, so this is fine, but do not use it for anything else.

Implementation: `p10_start_terrain_update`, `p10_put_map_edit`,
`p10_begin_terrain_update` and `p10_next_map_edit` in
`common/proto10/codec.c`. The decoder adds the delta in 64 bits and rejects
positions outside `int32_t`, so a hostile cursor cannot overflow.

**[defect]** There is **no client → server packet to place a block**.
`HitTerrain` covers damage only, and construction is a `TODO`. **[1.0]** adds
`BuildBlock` and `BuildLine` (12.2.4, 12.2.5).

Edits made during one server tick should be batched into one `TerrainUpdate`.
This is where 1.0 wins over 0.75 when a grenade or a collapse removes dozens of
blocks: 0.75 needs one packet per block or a `BlockLine`.

**Our server (milestone 5, `Server/Proto10/World.c`):** every place that
changes the map calls `proto10_world_cell()` right after its `mapvxl_set_*`
call (builds, block lines, spade/gun/dig destroys, grenade explosions, the
collapse of floating blocks in `Nodes.c`). Once per tick the changed cells are
sorted, de-duplicated, read back from the map and sent as one `TerrainUpdate`
(split above 16 KiB). The packet therefore carries the map's real state, not a
replay: a cell built and dug in the same tick goes out once, as air. Causes:
`createType Player` for builds, `destroyType Damage` for spade, gun, dig and
grenade, `Collapse` for detached cells and `Unspecified` for resyncs; the
health byte is the block's health, `100 - damage taken` (block health, 9.3),
so a crack is an edit of a solid cell with health under 100, batched with the
tick's other edits ([1.0], the use ngspades designed the byte for; it replaced
the crack in the tick's `TerrainUpdate`'s health byte). 0.75 clients compute collapses and
grenade craters
themselves; 1.0 clients are told every cell and turn cells marked `Collapse`
into falling structures. A map reset (`/reset`, a win) restarts the 1.0 map
transfer: new `GameStateHeader`, map, `GameStateFinal`, then `join` again.
Gameplay packets that were in flight when the new transfer started are
dropped (`packet_dropped why=map_download`), not treated as a protocol error.

### 9.2 `HitEntity` (90, C→S) [ref]

```
varint  entityId     who was hit
u8      hitType      Torso / Head / Limb
u8      damageType   which tool (Weapon1..3, Melee, ...)
vec3    firePosition where the shot started
vec3    hitPosition  where it landed
```

Hit detection stays on the client, as in 0.75 (`HitPacket`), but now the server
receives both endpoints. That allows a cheap plausibility check (line of sight
from `firePosition`, `firePosition` close to the shooter, distance and fire
rate against the `WeaponParameters`) instead of trusting a bare player id.

**[1.3]** A gun's `damageType` is `Weapon1` or `Weapon2`, the slot the shot is
fired from: `Weapon1` the primary gun, `Weapon2` the class's second gun. Only
the gun in hand fires (`player->gun_slot` on the server): the server refuses a
`Weapon1` or `Weapon2` hit that is not that slot (`hit_refused why=slot`), so a
client sends the slot it fires with and nothing else. `HitTerrain` is not
checked that way: its `Weapon1`..`Weapon3` all mean a gun's hit on a block
(9.3).

**[1.0] our checks:** our client sends as `hitPosition` the point its pellet
met on the body (map coordinates). The server refuses a gun's hit whose
`firePosition` is more than 4 blocks from the shooter's eye
(`why=fire_position_far`), whose `hitPosition` is more than 3 blocks plus the
lag tolerance from the target's body as the shooter saw it (`lagcomp_rewind`,
`why=hit_position_far`), or whose way from `firePosition` to 0.3 short of
`hitPosition` crosses a solid block (`physics_line_clear`, every cell walked,
`why=hit_blocked`). A spade's hit has no impact point and is not checked so.

### 9.3 `HitTerrain` (91, C→S) [ref]

```
blockcoord  block        (zigzag varints)
u8          damageType   Dig, Weapon1..3, ...
vec3        firePosition
vec3        hitPosition
```

The block has health (the high byte of its stored colour, 9.1). The server
subtracts damage and broadcasts a `TerrainUpdate` with
`destroyType = Damage` when the health reaches zero. 0.75 hides block health
inside the client; 1.0 makes it server state.

**[1.0] decision (milestone 5, block health later):** our server keeps a
gun's block health; the spade keeps the 0.75 model. A gun sends a
`HitTerrain` (`Weapon1`..`Weapon3` all mean a bullet here: the slot in hand
is not checked for a block, 9.2) for **every** pellet that meets a block; the
server takes the class's `block_damage` or the gun's own (rifle 50, SMG 34,
shotgun 20 a pellet, revolver 34: `game_weapon_block_damage`,
`common/classes/classes.h`, the client's table too) from the cell's 100
(`GAME_BLOCK_BREAK`, `server/Source/Server/BlockHealth.c`). At 100 taken
the block breaks as before (a `TerrainUpdate` with `destroyType = Damage`);
below it the cell is queued with `proto10_world_cell` and goes out in the
tick's `TerrainUpdate` as a solid cell whose health is `100 - damage taken`,
which draws the crack on every 1.0 client, the
shooter's prediction included. A cell untouched for 10 s
(`GAME_BLOCK_HEAL_SECONDS`, when the client stops drawing a crack) is whole
again, and so is one the map changes (`proto10_world_cell`), a block of
another colour, and every cell when a map loads. The spade still counts on
the client and sends the hit that breaks the block. The bots' planner does
not know cracks. The 0.75 profile is unchanged (the client counts every
gun, the server keeps nothing). Each hit is applied through
`game_player_block_action` (what a 0.75 `BlockAction` parses into), so the existing rules decide
(tool, distance, `BLOCK_DELAY` or fire rate, the indestructible bottom
layers, game mode checks); a plugin's `on_destroy` hears only the hit that
breaks. The gun's fire rate is a schedule on 1.0 (`block_action_weapon_
checks_paced`, `WeaponChecks.c`): a hit is taken up to 50 ms before the
gun's delay since the last one is up, so shots the network bunched up are
not refused as rapid fire, and a client firing faster than its gun falls
behind and is:

| `damageType` | 0.75 action | tool the server requires |
| --- | --- | --- |
| `Melee` (spade, primary) | `DESTROY_ONE` | spade |
| `Dig` (spade, secondary) | `DESTROY_THREE` (the cell, one above, one below) | spade |
| `Weapon1`..`Weapon3` (a bullet) | `DESTROY_ONE` | gun |
| anything else | refused | |

This follows the reference's names (the spade's primary is a melee hit, its
secondary digs). `firePosition` must be within 4 blocks of the player's eye,
as for `HitEntity`. A refused hit is logged (`dig_refused ... why=`) and
answered with a `TerrainUpdate` carrying the cell's real state. Server-side
block health can come later without a packet change.

### 9.4 `Damage` (92, S→C) [ref]

```
varint  entityId                  the entity that took the damage
u8      damageType | 0x80 if fromEntity follows
varint  fromEntity                (only with 0x80)
varint  toEntity
vec3    firePosition
vec3    hitPosition
u8      amount                    hp removed
```

**[defect]** `Generate()` sets the `0x80` bit when there is a `fromEntity`, but
never writes the `fromEntity` varint. The decoder then reads `toEntity` as
`fromEntity` and goes out of step. **[1.0]**: write it, in the position
shown above.

The client uses `Damage` for the hit indicator (direction from
`firePosition`), screen effects and the local hp. The authoritative hp is the
entity's `Health` field in `EntityUpdate`. A gun's damage is `Weapon1` or
`Weapon2`, by the slot in hand (9.2); `EntityDie` says the same (8.9).

---

## 10. `GenericCommand` (9, both)

```
bytes*   parts until the end of the packet
```

A list of strings: `parts[0]` is the command, the rest are arguments. Unknown
commands are logged and ignored on both sides.

Commands used by the reference:

| Dir | Command | Meaning |
| --- | --- | --- |
| C→S | `join` | create my player and spawn it (only in `Game`) |
| C→S | `leave` | remove my player, back to spectating |
| C→S | `map-cached <0\|1>` | my answer to the `GameStateHeader`'s `map-crc32` (7.1): `1` I hold the stream already, send no `MapData`; `0` send it. Allowed in the map's window, before the game; the server waits 3 s for it |
| S→C | `local-player <id>` | you control player `<id>` (and entity `<id>`); with no argument you control nothing |

Ours add (decimal string arguments):

| Dir | Command | Meaning |
| --- | --- | --- |
| C→S | `join <team> <weapon>` | the team and weapon menus' choice; a fourth part `<class>` in a classes mode (12.6) |
| C→S | `team <n>` | the team screen in the game (","): 0, 1, or 255 for the spectators; through `game_player_change_team`. Our client sends the reference's `leave` for the spectators |
| C→S | `weapon <n>` | the weapon screen in the game ("."): 0 rifle, 1 SMG, 2 shotgun; through `game_player_change_weapon` |
| C→S | `host <cmd> <arg1> <arg2>` | bot management from the menu (`docs/host-commands.md`), permission checked by the server |
| S→C | `host-status <bots> <humans> <autofill> <flags> <difficulty>` | what the menu's Bots section shows; sent per player whenever it changes (a client also accepts it without `<difficulty>`) |
| S→C | `perk-refused <perk> <why>` | a perk use was refused (12.5) |
| S→C | `perk-activated <perk>` | the server put the jetpack on you (12.5) |
| S→C | `perk-next <perk>` | which perk the `P10_PU_PERKS` progress counts towards (the HUD bar label); sent before each perk state update |
| S→C | `sentry-spot <entity>` | a sentry just found a target (not a switch between targets, at most every 2 s a sentry): the client plays its warning beep |
| S→C | `hit <victim> <amount> <head>` | a shot (or grenade, or your sentry) of yours hurt `<victim>` for `<amount>`, `<head>` 1 for a headshot: the hit marker. Never for a miss or for hurting yourself |
| S→C | `hit-protected <victim>` | a shot of yours met `<victim>` while its spawn protection keeps it from harm: no damage, a marker of its own so the shot is not read as lost |
| S→C | `kill <victim>` | your damage killed `<victim>` (the kill marker; sent instead of `hit` for the killing blow) |
| C→S | `ping <info\|danger> x y z [n]` | a mark for the team at x y z (map coordinates); `n` is a phrase of `common/pings/pings.h` (0 "Enemy spotted" ... 5 "Build here", the quick chat's Mark ring), sent as its number so each client shows it in its own language and no free text lands on a marker. Checked for the kind, the team, the position, the phrase and a 0.2 s rate |
| S→C | `ping <player> <info\|danger> x y z [n]` | a teammate's mark, relayed to the rest of the team, `n` as it came |
| C→S | `ping spot <target> x y z` | spot the enemy under the crosshair (map coordinates); checked for the mode (perks), the team, sight from the pinger's eye and a 0.5 s rate |
| S→C | `spot <pinger> <target> x y z` | to the pinger's team, the pinger included: mark `<target>` for 8 s, following it |
| S→C | `spot-refused <why>` | to the pinger: `mode`, `team`, `target`, `not_visible`, `rate`, `spectator` (the client pings the spot instead) |
| C→S | `use` | board the nearest usable thing in reach of my own team (a helicopter's seat, pilot's seat first, or a machine gun's one gunner seat), or leave what I sit on (12.8) |
| S→C | `mount <player> <entity> <seat>` | `<player>` sat down in `<seat>` (0 the pilot or the gunner) of entity `<entity>`; `mount <player> -1 0` when it left. Sent on every change and for everyone mounted when a client enters the game (after the lights) |
| S→C | `use-refused <why>` | to the asker: `none` (nothing in reach), `team` (only the other team's), `full` (every seat taken), `busy` (the MG has a gunner), `dead`, `wrecked`, `rate` |
| C→S | `squad <n>` | join squad `<n>` of my team (0 = alone), the pause menu's squad row |
| S→C | `squads <size>` | when a 1.0 client enters the game: the squads' size, 0 when squads are off |
| S→C | `squad <player> <n>` | `<player>` is in squad `<n>` (0 none): every squad when a client enters the game, then each change |
| S→C | `squad-refused <why>` | to the asker: `full` for a `squad <n>` with no room, else a refused `squad-rally` (`no_squad`, `mode`, `dead`, `not_leader`, `too_close`, `no_ground`) |
| C→S | `squad-rally` | drop the squad's rally point where we stand (the squad's leader only) |
| S→C | `squad-rally <leader> x y z` | to the squad: it respawns there first for 60 s |
| S→C | `restock` | to the player: health, blocks, grenades and ammo refilled (the tent, a plugin's `p:restock`) |
| S→C | `wallet <n>` | to the owner: its block wallet as the server keeps it, on every change (a spawn, a restock, a build's spend, a dig's refund, a plugin's `p:blocks`) |
| S→C | `fog <r> <g> <b>` | a new fog colour, 0..255 each (a plugin's `p:fog` / `game.fog`) |
| S→C | `host-bots <team1> <team2>` | to the host: the bots each team has now (the Bots menu's rows) |
| S→C | `kill-assist <killer> <assister>` | to everyone: `<assister>` hurt the victim of `<killer>` in the 10 s before it died (`Server/Assists.c`); the client tallies it on the scoreboard |
| S→C | `match-player <player> <kills> <deaths> <assists> <stat>` | to everyone when a match ends (every mode, `gamemode_match_won`), one per player in a team, before `match-end`: its figures for the match and the mode's own one (`stat`, named by `match-end`; 0 when the mode has none) |
| S→C | `match-end <team1\|team2\|draw> <score1> <score2> <seconds> <countdown> <stat> <next>` | to everyone after the `match-player` lines: the result, the team scores, the match's length, the seconds until the next map loads (the server reloads then; nothing scores meanwhile), the mode's column (`captures`: CTF, ngCTF, Babel; `hill`: seconds on the hill, koth; `none`) and the next map's name (`-` unknown). The client shows the summary until the countdown ends (the client's `matchsummary.c`); a client reads up to 8 parts of a command |
| S→C | `vote-start <seconds> <count>` | right after `match-end`, when the rotation has at least two maps besides the one just played: the next map's vote is open for `seconds` (the summary is then 14 s longer: `match-end`'s countdown says so, and `next` is `-`), `count` (1..5) options follow |
| S→C | `vote-map <index> <name> <size> <percent>` | one per option, after `vote-start`: the map's folder name, its side in blocks (512 or 1024) and the share (0..100) of the matches this server has played that were on it (the data folder's `map-stats.txt`) |
| S→C | `vote-tally <votes0> [<votes1> ...]` | the votes each option has, one number per option: after the options and to everyone at every change |
| C→S | `vote <index>` | a human's vote (spectators too, bots never), 0-based; the last one counts; refused when no vote is open or the index is out of range (`EVT mapvote action=refused`) |
| S→C | `vote-result <index>` | the vote closed `MAP_VOTE_RESULT_SECONDS` (3) before the next map loads: the winner, the most voted (a tie or no vote draws at random among the tied or all options); `-1` when the map could not be loaded (the rotation goes on). The server loads it next (`server_force_map`) |
| C→S | `light <0\|1>` | my flashlight off/on: the 1.0 form of 0.75's aosprotocol Flashlight (the client's `flashlight.c`). Checked for a bot, the team, being alive, an unchanged state and a 0.2 s rate, then by the plugins' `on_flashlight`; the state is kept per player and never invented by the client |
| S→C | `light <player> <0\|1>` | `<player>`'s light went on or off: to every 1.0 client, its owner included; it is one flag, and a body coming back from past the fog must already know it. Sent for a joiner for each light already on when it enters the game. Nothing is sent when a light goes off with its body: each client turns it off itself on that player's death, new body, removal and a new map, and so does the server's copy |
| S→C | `sky <minutes> <rate> <weather> <intensity> <moonlight>` | the time of day and the weather (`common/sky/sky.h`, the ideas of the aosprotocol specification's Daytime and Weather): minutes since midnight (0..1440, a decimal), the minutes of the day that pass in a real second (0: the time stands; the client runs it on itself), `clear`, `rain`, `snow`, `fog` or `storm`, its intensity and the night's moonlight, 0..100 each. Sent when a client enters the game, to everyone on a change and every 20 s while the day passes. The client lights the world by it (dark at night, warm at dusk, lightning in a storm), greys the fog and draws rain or snow; rain, snow and fog bring the fog nearer. A client that never gets it plays by day in clear weather |
| S→C | `outfit <player> <outfit> <colour>` | how a player looks (the numbers of the aosprotocol specification's draft Player Outfit, zerospades/aosprotocol pull request #14: 0 soldier, 7 miner, 64 zombie, 255 its team's; the client draws any other as 0). `<colour>` is the team whose colour the body wears, 0 or 1, or 255 its own. Cosmetic only. Sent when a client enters the game (every player not in its team's outfit) and on a change; a player's outfit is reset when it leaves and at every map, on both sides |
| S→C | `team-outfits 0 <outfit> 1 <outfit>` | the outfit of each team's players whose own is 255; lasts for the connection. Sent when a client enters the game once any was set, and on a change |
| C→S | `device <platform> <input>` | what I play on, for the scoreboard: `<platform>` one of `desktop android iphone console browser`, `<input>` one of `kbm pad touch` (`common/devices/devices.h`). Sent once in the game and again when the last input device changes. An unknown word is refused (`EVT device action=refused why=value`), an unchanged pair dropped |
| S→C | `device <player> <platform> <input>` | `<player>`'s device, to every 1.0 client; a joiner gets every known one when it enters the game. A player that never said (a bot) has none: the scoreboard draws no icon. The ping beside it is PlayerUpdate's `ping_ms` (bots 0) |

A refused `light` is logged on the server (`EVT light action=refused on= why=bot|team|dead|same|rate|plugin|value`) and not answered: the client sends the request and draws nothing new until the server's relay arrives, so a refused request changes nothing on screen (the 0.75 path works the same way).

**[1.0] usage policy:** `GenericCommand` is good for rare, human-readable
actions: `join <team> <weapon>`, `team <n>`, `weapon <n>`, `vote ...`, and our
existing host commands (`docs/host-commands.md`, which can move here
unchanged as `host <cmd> <args...>`). Frequent or structured data gets its own
packet id.

```c
static void p10_send_generic(ENetPeer* peer, int argc, const char* const* argv)
{
    uint8_t buf[512];
    p10_writer w = {buf, 0, sizeof buf, 0};
    p10_write_u8(&w, 9);
    for (int i = 0; i < argc; i++)
        p10_write_string(&w, argv[i]);
    if (w.error || peer == NULL) /* bots have no peer */
        return;
    enet_peer_send(peer, 0, enet_packet_create(buf, w.size, ENET_PACKET_FLAG_RELIABLE));
}

/* server: tell the client which player it controls */
static void p10_send_local_player(ENetPeer* peer, uint32_t player_id)
{
    char id[16];
    snprintf(id, sizeof id, "%u", (unsigned) player_id);
    const char* argv[] = {"local-player", id};
    p10_send_generic(peer, 2, argv);
}
```

---

## 11. Defects summary

All the reference defects 1.0 has to fix or avoid:

| # | Where | Defect | 1.0 fix |
| --- | --- | --- | --- |
| D1 | `PacketReader::ReadVariableInteger` | 32-bit accumulator, no length limit | 64-bit, max 10 bytes |
| D2 | `ReadString` | truncates at NUL | reject NUL, require UTF-8 |
| D3 | `EntityDiePacket::Decode` | reads the damage type before checking for end of packet | check first |
| D4 | `DamagePacket::Generate` | sets `0x80` but never writes `fromEntity` | write it |
| D5 | `EntityRemovePacket::Decode` | builds an `EntityDiePacket` | dispatch by type |
| D6 | `FinalStateTransfer` | `players` list never filled | send all players |
| D7 | `ClientSideEntityUpdate` handler | ends with `SPNotImplemented()`, kicks the client | remove |
| D8 | `NetworkPlayer::Update` | full state, reliable, every frame | 30 Hz, unreliable sequenced, delta |
| D9 | `HostPeer::GetPendingBytes` | always 0, so map throttling does nothing | real value from ENet: done, `conn_pending` (ENet's bytes in flight and queued, WebRTC's buffered amount), fragments while under 64 KiB, at most 64 a tick (7.2) |
| D10 | server | `PlayerAction`, `HitEntity`, `HitTerrain` not implemented | implement on SpadesX gameplay |
| D11 | protocol | no packet to build blocks, no chat | added (12.2) |
| D12 | certificates | never validated | keep `isValid = 0` until accounts exist |
| D13 | `WorldParameters::Update` | unknown key throws, client disconnects | ignore unknown keys |

D3–D5 change the bytes on the wire compared to the reference. Since nobody
runs the ngspades client, **1.0 does not need to stay compatible with
"WorldOfSpades 0.1"**; the protocol name changes (12.1) so the two can never be
confused.

---

## 12. What 1.0 adds [1.0]

None of this exists in ngspades. Everything in this section is implemented;
section 0 says what of the reference is still unused.

### 12.1 Identity

- Connect magic: keep `0x08101919` (the selector in section 2).
- `protocolName`: `"WorldOfSpades 1.0"`. The server accepts exactly this
  name; `"WorldOfSpades 0.1"` gets `ProtocolMismatch`.
- `majorVersion`/`minorVersion` carry the protocol revision (currently 1.3:
  the jetpack bits of 12.4, the compact trajectory of 12.7 and the three
  weapon slots of 8.4), no longer the client build. They are
  **informational**: the server logs them and our master shows them, but
  negotiates nothing on them. The game has not been released, so there is
  no older 1.0 client to keep working: every 1.0 peer speaks the whole
  protocol, and both programs change together. (Until 2026-10-22 the server
  stripped the 1.1 bits and kept the float trajectory for peers announcing
  an older minor; that was removed.)
- Unknown packet ids, unknown `GenericCommand`s and unknown `map` keys are
  ignored by both sides, so those can be added freely. New mask bits and new
  trailing fields cannot be skipped by a decoder that does not know them
  (see 8.11), so they change both programs at once.

### 12.2 New packets

None of these exist in ngspades. Their wire format is fixed and implemented in
`common/proto10/codec.{h,c}` (encoders, decoders, golden vectors below,
round-trip, truncation and fuzz tests); the gameplay behind them is part of
milestones 5 and 6.

| Id | Name | Dir | Section | Replaces in 0.75 |
| --- | --- | --- | --- | --- |
| 70 | `ChatMessage` | C→S | 12.2.1 | `ChatMessage` |
| 71 | `ChatBroadcast` | S→C | 12.2.2 | `ChatMessage`, server notices |
| 80 | `GameModeState` | S→C | 12.2.3 | `StateData` mode part, `IntelPickup/Capture/Drop`, `TerritoryCapture`, `ProgressBar` |
| 93 | `BuildBlock` | C→S | 12.2.4 | `BlockAction` build |
| 94 | `BuildLine` | C→S | 12.2.5 | `BlockLine` |
| 95 | `ThrowGrenade` | C→S | 12.2.6 | `GrenadePacket` |

#### 12.2.1 `ChatMessage` (70, C→S)

```
u8      channel     0 all, 1 team
string  text        UTF-8, at most 255 bytes
```

The decoder rejects (malformed packet): a channel other than 0 or 1, text
longer than 255 bytes (`P10_CHAT_MAX_TEXT`), text that is not valid UTF-8, and
text containing NUL. A client cannot use channel 2 (system): that would let it
forge server notices. The encoder sets the writer error instead of producing
such a packet.

Example: `gg` to the team:

```
46  01  02 67 67
```

Implementation: `p10_chat_message`, `p10_encode_chat_message`,
`p10_decode_chat_message`.

#### 12.2.2 `ChatBroadcast` (71, S→C)

```
varint  fromPlayer  player id, 0xFFFFFFFF (P10_CHAT_FROM_SERVER) = the server
u8      channel     0 all, 1 team, 2 system
string  text        UTF-8, at most 255 bytes
```

Same text limits as `ChatMessage`; the decoder rejects channels above 2 and a
`fromPlayer` that does not fit in 32 bits. Channel 2 replaces 0.75's server
notices (the client shows them without a name). 0.75 messages are CP437 and
can grow past 255 bytes once converted to UTF-8: the server shortens them on a
character boundary before sending.

Examples: player 3 says `hi` to everyone; the server announces `Round over`:

```
47  03  00  02 68 69
47  FF FF FF FF 0F  02  0A 52 6F 75 6E 64 20 6F 76 65 72
```

Implementation: `p10_chat_broadcast`, `p10_encode_chat_broadcast`,
`p10_decode_chat_broadcast`.

**Text encoding at the boundary (milestone 5):** both our programs keep CP437
inside (the 0.75 message handler, the client's chat line and bitmap font) and
convert only where a 1.0 chat packet is read or written, with
`common/proto10/text.{c,h}`: `p10_cp437_to_utf8` (cuts on a character
boundary to fit 255 bytes) and `p10_utf8_to_cp437` (characters CP437 lacks
become `?`). The server hands the text to `game_player_chat`, the same
function a 0.75 `ChatMessage` parses into, so mute, spam, team and `/command`
rules are the 0.75 ones; the chat it relays and every server notice (`send_server_notice`,
`broadcast_server_notice`) also go to 1.0 clients as `ChatBroadcast`
(notices on channel 2 from `P10_CHAT_FROM_SERVER`). Player names are still
passed through unconverted (to do).

#### 12.2.3 `GameModeState` (80, S→C)

```
map     state       key/value strings, like GameStateHeader (7.1)
```

The game mode is described with a string map so that new modes and keys never
need a protocol revision (unknown keys are ignored, 12.1). Encode with
`p10_start_game_mode_state`, `p10_put_map_entry`… and `p10_put_map_end`;
decode with `p10_begin_game_mode_state` and `p10_next_map_entry`. The key
names are macros in `codec.h` (`P10_GM_*`).

**Snapshots and deltas.** A packet whose **first** entry is `mode` is a
snapshot: the client forgets all game mode state, then applies the entries.
Any other packet is a delta: each key replaces its previous value, keys not
present keep theirs, and an **empty value removes** the key. The server sends a
snapshot right after `GameStateFinal` and whenever a round starts, and a delta
whenever something changes (a score, an intel, a territory, the capture
progress).

**Value formats.** Numbers are decimal (an optional `-`, digits, no spaces, no
`+`). Lists of numbers are comma separated with no spaces, for example
`300,200,61`; `p10_parse_ints(value, out, count)` parses exactly `count` of
them, each within `int32`. Positions are **block cells** in map coordinates
(8.2: `z` grows downwards). Teams are numbered like the player `Team` field
(8.11): `0` team 1, `1` team 2, `255` (`P10_GM_TEAM_NEUTRAL`) nobody.

| Key | Value | Meaning |
| --- | --- | --- |
| `mode` | `ctf`, `ngctf`, `tc`, or another lowercase name | the game mode; starts a snapshot. `ngctf` is CTF with the new mechanics (perks, spotting, assists, squad spawn and rally), drawn like `ctf`. A client that does not know the mode shows the scores only |
| `team1-name`, `team2-name` | text | team names |
| `team1-color`, `team2-color` | `r,g,b` (0..255) | team colours |
| `team1-score`, `team2-score` | decimal | team scores (CTF: captures) |
| `capture-limit` | decimal | CTF captures needed to win |
| `team1-intel`, `team2-intel` | `base`, `dropped`, `carried` | the state of the intel **belonging** to that team (team 1 defends `team1-intel`); where it is and who carries it are its `Flag` entity's (below) |
| `territory-count` | decimal `N` | TC: number of territories |
| `territory<i>` | `holder,capturing,permille,rate` | TC: territory `i` (`0` ≤ `i` < `N`, decimal without leading zeros: `territory0`, `territory12`): its holder (`0` team 1, `1` team 2, `2` nobody), the team that would take it, that team's share of the progress (0..1000) and its change a second (see below). Its place is its `Checkpoint` entity, id `P10_OBJECTIVE_ID_BASE + 5 + i` |
| `progress` | `territory,team,permille,rate` | not sent by our server any more (each `territory<i>` carries its own); a client may still read it |
| `arena-state` | `waiting`, `countdown`, `fight`, `end` | Arena (`mode` = `arena`): waiting for a player on each team, behind the gates before a round, fighting (the dead wait for the next round), or the pause after one |
| `arena-round` | decimal | Arena: the round number, from 1 |
| `arena-time` | decimal | Arena: seconds left in the countdown or the fight |
| `arena-winner` | `team1`, `team2`, `draw` | Arena: who won the last round; absent before the first. The team scores count rounds won, `capture-limit` the rounds that win the map |
| `zombie-team` | `1` or `2` | Mode `zombie` (its rounds use the `arena-*` keys): the zombies' team |
| `zombie-humans` | integer | Mode `zombie`: humans alive (or about to spawn) |
| `koth-radius` | decimal | King of the hill (`mode` = `koth`): the hill's radius in blocks (a player in the cylinder over it, from 4.5 blocks under `koth-floor` up to the sky, is on it). The hill itself is a `Checkpoint` entity whose origin is the ground cell of its middle (below) |
| `koth-floor` | decimal | King of the hill: the map z of the cylinder's floor, the deepest floor a player stands on inside the ring (the hill itself may be on a roof or a tree). An older client ignores it |
| `koth-state` | `fight`, `break` | Koth: a round is on, or the pause after one (the hill has already moved; nothing scores) |
| `koth-round` | decimal | Koth: the round number, from 1 |
| `koth-holder` | `team1`, `team2`, `contested`, `none` | Koth: who is on the hill this second; only a team alone on it scores |
| `koth-on` | `a,b` | Koth: players of team 1 and team 2 on the hill |
| `koth-points` | `a,b,limit` | Koth: this round's points of each team (a second held alone is a point) and the points that win the round. The team scores count rounds won, `capture-limit` the rounds that win the map |
| `koth-winner` | `team1`, `team2` | Koth: who won the last round; absent before the first |

The server leaves out a key with nothing to say: an Arena match sends no
intel, the other modes no `arena-*` key. Babel is sent as `ctf` (its intels
are drawn the same way).

**The objectives are entities** (ngspades' design: `Flag` and `CommandPost`,
8.3). Each intel is a `Flag` entity and each tent a `CommandPost`, created
for every 1.0 client (and in the GameStateFinal snapshot) with no owner and
the team they belong to (both types carry owner and team on their Create, like
the objects of 12.5), a Constant trajectory whose origin is the cell they rest
on (map coordinates, whole numbers), and, for a carried intel, the carrier as
the trajectory's `parentEntityId` (the origin is then meaningless). Their ids
are fixed: `0x7FFF0000` + 0 team 1's intel, 1 team 2's, 2 team 1's tent, 3
team 2's, 4 KotH's hill, 5 + i territory i (`P10_OBJECTIVE_*` in `codec.h`,
`server/Source/Server/Proto10/Flags.c`). A mode
without them (the arena, the zombie mode) has none, and they are removed
when the mode changes.

**Which type for what.** ngspades never said what a `Checkpoint` is (an
empty class, `Game/CheckpointEntity.h`, added in June 2014 next to
`CommandPost` and `Flag`; Ben Aksoy's 1.0 alpha protocol had `FLAG` and
`BASE (CP)`, no checkpoint). Ours: a `CommandPost` is a team's fixed base
(the tents), a `Checkpoint` a place taken by standing on it, neutral
(`P10_TEAM_SPECTATOR`): KotH's hill and territory control's territories. Who holds a checkpoint is not its team (a Create
field): it stays in GameModeState (`koth-holder`).

GameModeState keeps only what the entities cannot say: the intel's state word
(`p10_parse_intel`, `p10_format_intel`, struct `p10_intel`, whose `x, y, z`
and `carrier` the reader fills from the `Flag`):

| Value | State |
| --- | --- |
| `base` | resting at its spawn cell (the `Flag`'s origin) |
| `dropped` | dropped on the ground (the `Flag`'s origin) |
| `carried` | carried (the `Flag`'s `parentEntityId`) |

**Order.** The server sends an objective's EntityUpdate before the
GameModeState that goes with it, in the same tick: a client reading `carried`
already knows the carrier, and a capture's score arrives after the intel is
back at its base. Our client keeps the carrier of a `carried` intel until the
state changes, so the capture or the drop is credited to the right player
(`objectives_fill` in the client's `proto10_gamemode.c`).

A territory's `permille` is the capturing team's share when the key was sent
and `rate` its change in permille a second (negative when the capture
reverts, 0 when it is stalled or at an end). The client extrapolates
`permille + rate × elapsed` from when that key came, clamped to 0..1000,
exactly as 0.75's `ProgressBar` does, so the server sends a territory's key
only when its holder, capturing team or rate changes, and every 2 s while
it moves (a joiner's snapshot is never further behind). Everything is an
integer, so no client ever parses a float in a locale-dependent way. Our
client draws the bar of the moving territory it stands in (within 20
blocks, `proto10_gamemode_tc_progress`).

Example: start of a CTF snapshot, `mode=ctf`, `team1-score=3`:

```
50  04 6D 6F 64 65  03 63 74 66  0B 74 65 61 6D 31 2D 73 63 6F 72 65  01 33  00
```

A full CTF snapshot as the server would send it:

```
mode=ctf  team1-name=Blue  team2-name=Green  team1-color=0,0,255  team2-color=0,255,0
team1-score=2  team2-score=0  capture-limit=10
team1-intel=carried  team2-intel=dropped
```

and a TC delta when team 1 starts taking territory 1, held by team 2, a
quarter of the way in with one player more than the defenders:

```
territory1=1,0,250,50
```

**Implemented (milestone 6)**, `Server/Proto10/GameMode.c` and
the client's `proto10_gamemode.c`:

- The server computes the values from the variables 0.75's `StateData`
  reads (`server->protocol.gamemode`), so both protocols describe the same
  match. It sends: the snapshot to a client right after its `GameStateFinal`;
  a snapshot to everyone from `gamemode_init` (every round start); a delta
  from the 0.75 `IntelPickup`/`IntelDrop`/`IntelCapture` senders once their
  state is updated (so a capture's delta goes out before the round reset it
  may trigger in the same call); and a delta check every tick for anything
  else (the 0.75 `MoveObject` cases: intel and tents following the terrain,
  the intel respawning after a capture). Deltas never contain `mode`.
- The 0.75 state cannot tell an intel at its spawn from a dropped one, so the
  server remembers the drop (`dropped:`) until the next pickup, capture or
  round start.
- Babel is sent as `ctf` (as 0.75's `StateData` does). Territory control is
  `tc` (`Gamemodes/Tc.c`): `territory-count`, every `territory<i>` and a
  `Checkpoint` entity per territory.
- The client applies a snapshot straight into `gamestate` (like
  `StateData`). For a delta it derives the 0.75 event from the transition and
  replays it through the 0.75 handler, so messages, sounds and the score
  bonus are the 0.75 ones: intel becomes `carrier:N` → `IntelPickup`;
  `carrier:N` → not carried with the carrier's team score up →
  `IntelCapture` (winning when the score reaches `capture-limit`); otherwise
  `dropped:` → `IntelDrop`; a territory's holder changes → `TerritoryCapture`;
  the bar → `ProgressBar` (`progress = permille / 1000`,
  `rate = permille per second / 50`, since 0.75 extrapolates
  `0.05 × rate` per second). The server's values then overwrite what the
  handlers counted.

#### 12.2.4 `BuildBlock` (93, C→S)

```
blockcoord  block    svarint x, y, z (zigzag, must fit in int32)
color       color    B, G, R (the `color` primitive of section 4, like the entity block colour)
```

Example: block (256, 255, 62) with r=0x11, g=0x22, b=0x33:

```
5D  80 04 FE 03 7C  33 22 11
```

#### 12.2.5 `BuildLine` (94, C→S)

```
blockcoord  from
blockcoord  to
color       color
```

Example: a line from (10, 20, 30) to (10, 20, 25), red:

```
5E  14 28 3C  14 28 32  00 00 FF
```

The codec only checks the layout (coordinates within `int32`). The line length,
the block count and the cells on the line are the server's business, as with
0.75's `BlockLine`.

The server answers builds with `TerrainUpdate`, using `createType = Player` and
the colour it accepted. It applies its existing rules: `BLOCK_DELAY`, the
solid-neighbour check, the block count (see `CONTRIBUTING.md`). A refused build is
answered with a `TerrainUpdate` that restores the cell, so the client's
prediction is undone, and logged with the reason.

Ours replays `BuildBlock` and `BuildLine` as the 0.75 `SetColor` (when the
colour differs) plus `BlockAction build` / `BlockLine` through the 0.75 receive
handlers; the tool must already be the block (the client reports its tool in a
`ClientSideEntityUpdate` right before each build or dig). Refusals are logged
as `build_refused` / `build_line_refused` with `why=`.

**Careful:** `BuildBlock`/`BuildLine` send the colour as B, G, R, while
`TerrainUpdate` (9.1) sends R, G, B. Both use `p10_color`; only the codec
cares about the order.

Implementation: `p10_blockcoord`, `p10_build_block`, `p10_build_line`,
`p10_encode_build_block`, `p10_decode_build_block`, `p10_encode_build_line`,
`p10_decode_build_line`.

#### 12.2.6 `ThrowGrenade` (95, C→S)

```
vec3    origin      where the grenade leaves the hand
vec3    velocity    blocks per second
f32     fuse        seconds until it explodes
```

The decoder rejects NaN and infinite values in any field and a negative fuse
(0 is accepted: it explodes at once). These floats go straight into the
server's physics, so this check is part of the codec, not left to each handler.
The encoder sets the writer error for the same values.

Example: origin (1, 2, 3), velocity (0, 0, −1), fuse 3 s:

```
5F  00 00 80 3F  00 00 00 40  00 00 40 40
    00 00 00 00  00 00 00 00  00 00 80 BF
    00 00 40 40
```

Thrown grenades become entities with a `Gravity` trajectory and an
`EntityEvent Explode` when the fuse runs out. Rockets and vehicles follow the
same pattern later.

**Implemented (milestone 6).** The server replays `ThrowGrenade` as the 0.75
grenade packet (`receive_grenade_packet`): velocity divided by 32 (0.75 units
are blocks per 1/32 s), and the tool, sprint, rate limit, fuse cap (3 s),
speed and position checks are the 0.75 ones. Every grenade the server arms,
whoever threw it (1.0, 0.75, a bot), is announced to 1.0 clients:

1. `EntityUpdate` item: `Create` + `Flags` (0) + `Trajectory`, type
   `Grenade`, a `Gravity` trajectory with the position and the velocity in
   blocks per second (0.75 velocity × 32; gravity 32 blocks/s² matches the
   0.75 grenade physics).
2. When the fuse runs out: an `EntityUpdate` with a `Constant` trajectory at
   the server's final position (clients simulate the arc and the bounces,
   but explode it where the server says), then `EntityEvent Explode`
   (param 0), then `EntityRemove`.
3. A grenade freed without exploding (the thrower's reset) only gets
   `EntityRemove`.

None of these go to the **thrower**: its client already shows the grenade
from the moment it threw it, as on 0.75 where the server does not echo the
grenade packet. The fuse is not on the wire: a client keeps the grenade until
`Explode` (it uses a long fallback fuse locally). Grenades in flight are part
of the `GameStateFinal` entity list. There is no owner field (8.4), so a
client shows other players' grenades in the neutral colour. Damage, craters
and the kill feed (`EntityDie` reason 3) come from the server as before.

Implementation: `p10_throw_grenade`, `p10_encode_throw_grenade`,
`p10_decode_throw_grenade`.

### 12.3 Respawn and the kill feed

- Respawn countdown: the seconds until the respawn ride the death,
  `EntityDie.param >> 8` (8.9); the client shows the timer and the server
  spawns the entity when it expires. (An earlier proposal put a `RespawnAt`
  in `PlayerUpdate` bit 6; that bit is the perks' now, 12.5.)
- Kill feed: the client derives it from `EntityDie` + `PlayerUpdate` names.
  `EntityDie.param & 0xFF` is the 0.75 kill reason (1 a headshot, 2 melee,
  3 grenade, ...), so no packets need correlating; `damageType` names the
  gun slot.

### 12.4 Jetpack perk (revision 1.1)

The first 1.0-only mechanic. Its bits (input bit 4, entity flag bit 3) go to
every 1.0 peer (12.1).

**Rules.**
- A perk, not standard equipment: nobody has it until the server grants it
  (`jetpack_grant()` in `server/Source/Server/Proto10/Jetpack.h`, called by
  the perk system). It lasts for the rest of that life and is cleared at the
  next spawn. A grant arrives with a full tank. Bots never get it (they have
  no pilot for the input).
- While equipped, holding jump **in the air** thrusts upward (keyboard space
  or the gamepad jump button; no extra binding). The client only raises the
  input once jump has been held for 0.25 s in the air or was pressed again
  mid-air, so a tapped jump stays a normal jump.
- Thrust: 86.4 blocks/s² (the retail release's jump pack), climb capped at
  28 blocks/s (the cap never brakes a faster motion, such as a fresh jump),
  horizontal acceleration 0.5 of walking while thrusting (top speed twice a
  run) instead of the plain airborne 0.1, and 0.2 while gliding between
  pulses with the jetpack worn and fuel left. No thrust above z = −2 (the
  server's `valid_player_pos` accepts no position above z = −4). The retail
  mover shaped its climb with a per-frame drag and left only its emergent
  ceiling (~54 blocks/s); the cap is what that physics plays as, at every
  frame length.
- Fuel: the whole tank is 1.333 s. Each press pays a one-time ignition bite
  of 0.133 s (held input still blocked by the ceiling stays a press and pays
  when the flight starts), then thrust drains 1.2 s. Regen refills the tank
  in 10 s while not thrusting, in the air as well as on the ground, but
  never during the 2 s after taking a hit (every damage, falls and water
  included). The retail release's fall-distance reset has no counterpart:
  fall damage is the vertical speed at landing, so thrusting during a fall
  already cancels it.
- Fall damage is unchanged and comes from the vertical speed at landing, the
  same formula as before: braking with the jetpack just before landing avoids
  it, falling with an empty tank does not.
- Server-configurable in the optional `[jetpack]` table of `config.toml`
  (`enabled`, `fuel_seconds`, `regen_seconds`, `thrust` in blocks/s²,
  `max_rise_speed` in blocks/s, `air_control`, `glide_control`,
  `ignition_cost_seconds`, `refill_delay_seconds`).
  `test_grant_on_spawn = true`
  (or `SP_JETPACK_TEST=1` in the server environment) grants it to every human
  on every spawn: **test only**.

**Wire.**
- `GameStateFinal` world parameters, only when the server has the jetpack:
  `jetpack-fuel` (s, whole tank, ignition included), `jetpack-regen` (s),
  `jetpack-thrust` (blocks/s²), `jetpack-max-rise` (blocks/s),
  `jetpack-air-control`, `jetpack-glide-control`, `jetpack-ignition` (s,
  the per-press bite),
  `jetpack-refill-delay` (s after a hit), `jetpack-ceiling` (z).
  The client predicts with exactly these numbers; without them it never
  thrusts. Unknown keys are ignored by older clients.
- Equipped: entity flag bit 3 (`P10_EF_JETPACK`) on the player's body, sent
  with the normal delta rule (8.4), the owner's body included.
- Thrust: input bit 4 (`P10_IN_JETPACK`). The owner sends it in
  `ClientSideEntityUpdate` (with every input, 30 Hz); the server echoes it in
  `EntityUpdate` for the other clients only while the tank has fuel, so their
  flames go out when it runs dry.
- Fuel is not on the wire: the owner integrates it with the same step as the
  server (`common/flight/flight.h`), from the full tank of the grant.
- In the creative mode (free flight, no jetpack) the same bit is the held
  jump: up (`flight_free`), echoed to the others while held.

**Physics.** `common/flight/flight.h` (`flight_step`) is one header shared by
the server (`physics_move_player`) and the client (`player_move`), run in map
coordinates before gravity and friction. The server simulates the thrust
from the input bit, so its own physics does not pull a flying player down
between position reports and hands out no fall damage for a flight it did
not see. Positions stay client-authoritative and pass through the 0.75
`receive_position_data` checks unchanged: a jump of 6 blocks or more between
two reports is still corrected (forced `EntityUpdate`). Other clients
simulate other bodies with an unlimited tank, gated by the echoed bit. (The
helicopter has a flight model of its own, `common/vehicles/heli.h`, 12.8.)

`EVT ... jetpack action=grant|start|stop|full` (stop with
`why=released|empty|landed|ceiling`, start/stop with the position) and
`grant_refused`/`input_refused` with the reason.

### 12.5 Perks and the sentry gun

The second 1.0-only mechanic, and the first **new entity type**: `P10_ENT_SENTRY`
is id 7, after the reference's seven (`Player`..`Vehicle`), so no older client
can ever decode it by accident. Like the jetpack it is a *perk*: nobody has it
until the server grants it, it is used from the HUD, and a kill with it is
credited to the owner.

**Earning rules (one counter per perk, per life).**
- `player_t.perk_points[P10_PERK_COUNT]`, not one shared pool, so "jetpack only
  for a capture" and "sentry after 5 kills" can coexist. Every event
  (`kill`, `intel_pickup`, `intel_capture`) adds **that perk's own weight** to
  **that perk's own counter**; a weight of 0 means the event earns nothing for
  it. A perk is earned when its counter reaches its own threshold.
- Progress is per life by default (`reset_on_death = true`). A perk with
  `reset_on_death = false` keeps its counter **and** its unused charge across
  deaths, which makes it a once-per-round unlock. A **sentry earned and not
  put down yet** keeps its charge through death for everyone (`perks_on_kill`
  logs `action=kept why=death perk=sentry`): the owner places it when he is
  back.
- A threshold of `points = 0` grants the perk at every spawn: the free-perk
  path, and the quick way to test one.
- **Nothing is pressed to get a perk.** The jetpack is equipped the moment
  it is earned (or the moment its owner is alive again) and is never shown
  to the client as held. The sentry stays in the inventory until the player
  places it: a fifth slot of the client's tool bar (wheel, LB/RB, key 5),
  shown with its ghost, and the fire button places it where the player
  aims. An earlier `activation` setting ("instant"/"button") is gone; the
  server ignores the key with a warning.
- Only the *cheapest* perk not yet granted this life is announced to the HUD,
  so the wire keeps a single progress byte (below).

**Config.** `Resources/config.toml`, all keys optional:

```toml
[perks]                    # weights one event is worth, inherited by every perk
kill_points          = 1   # weapon, headshot, spade, grenade
intel_pickup_points  = 1
intel_capture_points = 3

[perks.jetpack]            # one optional table per perk (perk id -> name)
points           = 3       # threshold; 0 = free on every spawn
enabled          = true    # false: it can never be earned nor used
reset_on_death   = true    # false: progress survives death (once per round)
# kill_points / intel_pickup_points / intel_capture_points override [perks];
# 0 = this event earns nothing for this perk

[perks.sentry]
points = 5
enabled = true
```

- The weights + threshold pair expresses the owner's "or" without an extra
  mode: jetpack only for a capture is `points = 1, intel_capture_points = 1,
  kill_points = 0, intel_pickup_points = 0`; jetpack after X kills is
  `points = X, kill_points = 1, intel_capture_points = 0`; "3 kills **or** 1
  capture" is `points = 3, kill_points = 1, intel_capture_points = 3`.
- The old flat keys `[perks] jetpack_points` / `sentry_points` still set a
  threshold when `[perks.<name>].points` is absent. `SP_PERK_JETPACK` /
  `SP_PERK_SENTRY` / `SP_PERK_MG` override the thresholds for tests. Unknown
  keys and an absent `[perks]` table leave the defaults (kill 1, pickup 1,
  capture 3; jetpack at 3, sentry at 5, mg at 4, landmine at 3, dynamite at 4).
- The Commando's `P10_PERK_MG` (2) is the machine gun of 12.8: placed like the
  sentry from the same tool bar slot, it stays where it is whatever its owner
  does. (The revive it replaced, `Server/Revive.c`, is gone.)

**Wire.**
- `PlayerUpdate` item `P10_PU_PERKS` (bit 6), 3 × u8: `perk_points` (progress
  of the perk the HUD is waiting for), `perk_inventory` (`1 << id` for each
  perk held), `perk_next` (that perk's threshold, 0 = none left). Sent to the
  **owner only**; per-perk counters stay server-side.
- `PlayerAction P10_EV_USE_PERK`, `param = p10_perk_param(id, x, y, z)`:
  places the sentry, the MG, the landmine or the dynamite, perk id in bits 0-7
  and the block cell it stands on in bits 8-39 (x 8-19, y 20-31, z 32-39, map
  coordinates). The jetpack is refused with `automatic`.
- The jetpack, worn by the server, tells the owner with `generic_command
  "perk-activated" jetpack` (the HUD shows "JETPACK ACTIVATED") and logs
  `EVT ... perk action=used perk=jetpack auto=1`. `perk_inventory` never
  carries the jetpack: it is the sentry, the MG, the landmine and the dynamite
  that wait there for their owner to place them.
- A refused use answers the owner with `generic_command "perk-refused" <name>
  <why>` and logs `EVT ... perk action=use_refused perk=<name> why=<token>`.
  A refused *placement* keeps the perk in the inventory (try elsewhere); a
  successful one clears the bit.
- Using the jetpack goes through `jetpack_grant()` (12.4); using the sentry
  goes through `sentry_place()`, the MG through `mg_place()` (12.8) and the
  landmine or dynamite through `explosives_place()` (12.6).

**The sentry gun** (`Server/Proto10/Sentry.c`, objects in `Proto10/Objects.c`).
- Placement: the owner aims at a floor block within `SENTRY_PLACE_RANGE` (5)
  and confirms. `sentry_cell_ok` requires the cell in the map, not the water
  layer, solid ground at `z` with free space at `z - 1` and `z - 2`, visible
  from the eye, no other object within 2.5 and no player standing in the cells
  it would fill. One sentry per owner. Refusals carry a token:
  `out_of_map`, `water`, `no_ground`, `no_room`, `too_far`, `not_visible`,
  `occupied`, `player_in_the_way`, `already_have_one`, `too_many_objects`,
  `not_alive`. **The client's preview mirrors these rules**
  (the client's `sentry.c`, `sentry_cell_valid`): change both together.
- Behaviour: with nothing to shoot (or while its owner is dead) it keeps
  watching: the head sweeps 0.8 rad/s from side to side over the half
  circle in front of it (its owner's look when placed, `home_yaw`), and
  levels out (`_sentry_scan`). It never turns round: an enemy behind that
  half circle is not a target (`_sentry_in_arc`). The aim goes out as the trajectory rotation
  at most 20 times a second; the client eases the drawn head toward it
  (`front_shown`). It turns at 3 rad/s towards the nearest visible enemy within
  `SENTRY_RANGE` (48), aims at the chest or, if only the head shows over a
  wall, the head, and fires every 0.5 s once the barrel is within 0.12 rad.
  The shot really travels off the barrel's line (spread 0.04 rad, growing with
  distance) and only what that ray meets is hit. 20 damage body, 40 head,
  through `send_set_hp` with the **owner as attacker** and `KILL_REASON_SENTRY`
  (7, after the 0.75 reasons 0..6), so kills, score and the kill feed credit
  the owner. It rests while its owner waits to respawn.
- Health: `SENTRY_HEALTH` (200), sent as a normal `P10_EU_HEALTH` delta; the
  label draws `health/200`.
- **Lifetime.** `[sentry] lifetime_seconds` (default 60, `SP_SENTRY_LIFETIME`
  overrides, `0` = it lasts forever). Health drains linearly with the time
  since `o->created` and **only ever goes down**, so the bar drains visibly and
  shooting it only makes it die faster. The check runs before the owner check,
  so it still expires while its owner is dead. On zero,
  `proto10_objects_destroy(..., explode = 1, "expired")` reuses the normal path
  (the `Explode` event the client renders, then `EntityRemove`), logged
  `EVT ... action=explode why=expired`.
- World parameter `sentry-lifetime` (seconds) in `GameStateFinal`, so the
  client's label can show the remaining seconds: the health bar doubles as the
  clock (`health / 200 * lifetime`). The same number the server will expire it
  on.
- Logging: `EVT ... sentry action=place|idle|target|lost_target|fire|explode`
  with `entity=`, and `why=owner_dead|expired` where it applies; a refused
  placement is `perk action=use_refused perk=sentry why=<token>`.
- A sentry kill earns no perk progress (`reason > 3` is refused in
  `perks_on_kill`), or one perk would feed the next one.

**Gamepad.** Both perks are used from the HUD with one remappable button (the
sentry shows a placement preview before confirming, per `CONTRIBUTING.md` rule 6);
no chat command is needed to play. Spectators and the dead cannot use a perk
(`why=not_alive`).

### 12.6 Classes (ngCTF, and the zombies when the host allows them)

A mode with `GM_FEAT_CLASSES` (ngCTF, and the zombie mode when the host
turns them on: never classic CTF, Babel or Arena) gives every player one of
four classes, the table in `common/classes/classes.h` (`game_class_get(n)`,
shared by both programs):
0 Commando, 1 Rocketeer, 2 Marksman, 3 Miner. The server's side is
`server/Source/Server/Classes.c`. In the zombie mode the survivors pick
one and the zombies keep their hands (the mode keeps a player without a
gun out of the classes: `classes_of` gives it nothing). A mode whose classes
own the perks (`GM_FEAT_CLASS_PERKS`) uses config.toml's perk rules, not its
own: every class earns its own streak perk (the zombie mode's own jetpack
is only for the matches without classes).

**Wire** (GenericCommands, decimal arguments):

| Dir | Command | Meaning |
| --- | --- | --- |
| S→C | `classes 1` | the mode has classes; sent when a 1.0 client enters the game (nothing is sent in a mode without them) |
| S→C | `class <player> <n>` | `<player>`'s class: to everyone on every change, and, right after `classes 1`, one per connected player to the client entering the game. It can arrive before the PlayerUpdate that creates the player |
| C→S | `class <n>` | pick a class. Dead or joining: taken at the next spawn. Alive: the player dies and respawns with it, like a weapon change (kill reason 6). Refused (`EVT class action=refused why=mode|bad_value`) in a mode without classes or for an unknown class; nothing is answered |
| C→S | `join <team> <weapon> <class>` | the class rides the join as a fourth part; three parts mean the Commando. In a classes mode the weapon part is ignored |
| C→S | `weapon <n>` | refused in a classes mode (`weapon_refused why=classes`): the class decides the gun |

**What a class changes** (the server's rules, which the client predicts):
- The gun is the class's `weapon`, sent as the body's first weapon slot
  (8.4, revision 1.3): its parameters name the gun, so a client reads every
  player's gun from there (the Commando's rocket launcher is described
  below), and its skin carries the model's name.
The figures are the first Steam Ace of Spades' (port of its `CLASS_*`
tables and class loadouts, `shared/constants.py` and `class_data.py`):
health 100, 70, 70, 85; walk 0.7 (5.6 blocks/s) for all four; sprint 1.4,
1.1, 1.45, 1.4; jump 0.432, 0.36, 0.54, 0.432; wallet 200/1000,
500/1500, 400/1000, 0/1000 (start/max); fall 10 blocks then full damage at
40, 10, 30, 40 (100, 10, 100, 100); head shots taken x1.0, x1.5, x1.5,
x0.5; water drag 8, 12, 8, 8; the melee tool a pickaxe (0.6 s, 40) for the
Commando, Rocketeer and Marksman, a superspade (0.6 s, 50) for the Miner.

- Health: hp stays 0..100 on the wire. Every damage to a player (hits,
  grenades, falls, water) is scaled by `100 / health`, rounded up
  (`(d * 100 + health - 1) / health`): a rifle body shot (49) takes 49 off a
  Commando (100), 70 off a Rocketeer (70), 57.6 off a Miner (85). A head
  shot is multiplied by the class's `headshot` first (the Marksman's and
  Rocketeer's bodies take one and a half, the Miner's a half). The HUD shows
  `hp * health / 100`.
- Ground speed: in `physics_move_player` the acceleration factor `f`, after
  the airborne / crouch / aim-or-sneak factors (and the zombie's), is
  multiplied by `speed` when the player is **not airborne**. Sprinting
  **replaces** that walk multiplier with the class's `sprint` (retail
  semantics: they do not stack); without a class the classic `x1.3` stays.
  The client's mover predicts the same (`classes_sprint`).
- Water: the friction divisor is `class_water * dt + 1` while wading
  (`water_friction`: 8, 8, 8, 12), the classic 6 for everyone else.
- Jump: the jump's upward velocity is the class's `jump` instead of -0.36.
- Blocks: a spawn gives the class's `blocks` (the wallet's start), and the
  wallet holds `blocks_max`: a restock (the tent's `restock`) fills it to
  the max and every dug cell gives one back up to it. The server owns the
  figure and tells its owner each change with `wallet <n>` (protocol 1.0
  has no block count anywhere else): the client's own prediction follows,
  so a Miner (0 at spawn) can build after digging.
- Falls: a classed player's fall damage is the distance flown down, not
  the landing speed (`Util/Physics.c` accumulates the downward
  displacement and a move up past 0.1 clears it): linear from `fall_min`
  to `fall_max` (`fall_dmg` there and beyond), times `fall_water` when the
  landing was in water. A classless player keeps the classic quadratic on
  the landing speed.
- Melee: `HIT_TYPE_MELEE` takes the class's `melee` (40 or 50) instead of
  the classic 80; `swing` is its interval, checked by the server's melee
  rate.
- Grenades: `gear_count` when `gear` is `GAME_GEAR_GRENADES`, else 0 (the
  Miner's drill, below). A restock gives the same.
- Bots pick a class through the same path (`classes_pick`, `by=bot`).

**Perks with classes.** Each class earns one streak perk with ngCTF's perk
machinery (12.5: kill, pickup and capture weights, thresholds, per life):
Commando the machine gun (`P10_PERK_MG` 2, 12.8), Rocketeer the sentry (1),
Marksman the landmine (`P10_PERK_LANDMINE` 3), Miner the dynamite
(`P10_PERK_DYNAMITE` 4). A player earns, holds and is told (`perk-next`,
`P10_PU_PERKS`) about its class's perk only. The jetpack is not a perk with
classes: the Rocketeer wears it from every spawn (`jetpack_on_spawn`, the
same entity flag as the jetpack perk; no `perk-activated` is sent), nobody
else ever has it. The MG, the landmine and the dynamite exist only with
classes.
Thresholds by default: landmine 3, dynamite 4 (`[perks.landmine]`,
`[perks.dynamite]` in config.toml, `SP_PERK_LANDMINE` / `SP_PERK_DYNAMITE`
for tests, 0 = at every spawn).

**The landmine and the dynamite** (`server/Source/Server/Proto10/Explosives.c`)
are held in the inventory like the sentry (the fifth tool bar slot) and put
down the same way: `PlayerAction P10_EV_USE_PERK` with
`p10_perk_param(3 or 4, x, y, z)`, the floor cell, under the sentry's rules
(`sentry_cell_ok`: the client's preview is the same). Refusals answer
`perk-refused <landmine|dynamite> <why>`: the sentry's tokens, `class` (not
your class's perk), and `already_have_one` (a player has at most 2
landmines down, and 1 dynamite).

They are objects of the sentry's registry, so the wire is the sentry's:
an `EntityUpdate` Create with owner and team, a Constant trajectory at the
middle of the floor cell's top (`x + 0.5, y + 0.5, z`, map coordinates),
`P10_EU_HEALTH`, the flag `P10_EF_WEAPON_CLIP` (bullets hit them).

| Entity | Type | Health | Box (half width, height above the floor) | Behaviour |
| --- | --- | --- | --- | --- |
| landmine | `P10_ENT_LANDMINE` 8 | 30 | 0.4, 0.2 | armed 1.5 s after it is put down; then an enemy whose feet come within 1.5 blocks (horizontally) sets it off. It rests while its owner is dead |
| dynamite | `P10_ENT_DYNAMITE` 9 | 60 | 0.4, 0.6 | goes off 4 s after its Create (the client can show the fuse from it) |

Shot or blasted to 0 health, either goes off the next tick (blasts chain a
tick apart); **any hit sets one off at once**, whatever the gun (an SMG's 29
against a landmine's 30 left it standing before), and a bullet of its own
team's counts, and so does its owner's (only a sentry refuses a friendly shot:
`entity_hit refused why=friendly`).
A player who takes another class loses the landmine and the dynamite they
placed (`landmine|dynamite action=remove why=owner_class`); the sentry stays,
it wears out by itself. Going off is an `EntityEvent Explode` on the object, then its
`EntityRemove`; the crater comes as a `TerrainUpdate`, the hurt players'
`Damage`/`EntityDie` as `P10_DMG_GRENADE` fired from the object's position
(kill reason 8, `KILL_REASON_EXPLOSIVE`: the kill is the owner's and earns
no perk progress). The blast is a grenade's (`4096 / d^2` in sight, at most
100, then the class's health scaling) centred half a block over the floor;
the landmine clears a 3 x 3 x 3 crater, the dynamite deals 1.5 times the
damage and clears 5 x 5 x 5, every cell the mode allows
(`gamemode_cell_editable`). An enemy's landmine is drawn like any
other object (the server sends it to everyone; it was hidden past 6 blocks
before, which made a mine nobody could see).

Logged as `EVT landmine|dynamite action=place|armed|trigger|explode why=`,
`EVT explosion cause= at= crater= cleared= hurt=`, `EVT class
action=jetpack`, and the bots' `bot_perk action=landmine|dynamite`.

**The rocket launcher** (the Commando's gun, `GAME_WEAPON_ROCKET` 3,
`server/Source/Server/Proto10/Rockets.c`). The Commando's Player entity
carries it as weapon params `type P10_WEAPON_ROCKET`, magazine 1,
`fire_interval_ms` 800, `reload_ms` 2500, `max_damage` 100. Clip 1, reserve
4 (a spawn and a restock give 1 + 4). The reload is the usual one
(PlayerAction reload, 2.5 s, the trigger released, like the rifle's).
Holding the trigger spends nothing: a rocket is spent only by

| Dir | Command | Meaning |
| --- | --- | --- |
| C→S | (no command) | **fire: the trigger**, ngspades' way: the press of PlayerInput's `toolPrimary` (a ClientSideEntityUpdate whose input has it and the previous one did not) with the rocket launcher in hand launches from the server's copy of the eye, a block ahead along its copy of the look. The client sends that update reliable at the moment it fires, and with the launcher in hand sets the bit only for a press it fired on (held until release), so the server never fires a press the client did not and the clips agree |
| S→C | `grenade-refused <why>` | to the thrower: `sprinting`, `rate` (450 ms between two throws), `spade_nade` (a right click with the spade in the last second) or `plugin`. The throw was not made: the client takes back the grenade it showed and its count |
| S→C | `rocket-refused <why>` | to the shooter: `weapon` (not a rocket launcher), `not_alive`, `tool` (the gun is not in hand), `reloading`, `empty`, `sprinting`, `rate` (0.8 s between two), `direction`, `origin_far`, `too_many`. The clip was not spent |
| S→C | `rocket-push <vx> <vy> <vz>` | to the shooter of a rocket that went off within 4 blocks of its chest: add this to your velocity (the 0.75 physics' units: a jump is vz -0.36). The server applied it to its own copy of the body |

A rocket is an entity of type `P10_ENT_ROCKET` (2, the reference's; no
owner part), sent to **everyone, its shooter included** (the client does
not predict it): `EntityUpdate` Create with flags 0 and a `P10_TRAJ_LINEAR`
trajectory, origin the muzzle, velocity `dir * 75` blocks a second (BattleSpades'
and the retail game's `ROCKET_SPEED`), no
gravity. The server flies it in 0.25 block steps and it goes off on the
first solid block (the blast on the near side of it), a body point of a
living player other than its shooter within 0.7 (hip `+0.1`, `+0.9`,
`+1.7` in z), an enemy object's box, the map's side, or after 3 s. Going
off is what a grenade does: an `EntityUpdate` with a Constant trajectory
where it went off, `EntityEvent Explode`, `EntityRemove`. The blast is
`explosion_blast`: a grenade's damage times 0.5 (times 0.25 for its shooter),
kill reason 3 (grenade, `P10_DMG_GRENADE`; it earns perk progress like a
gun kill), and a 3 x 3 x 3 crater. Logged as `EVT rocket
action=fire|refused|explode|push` (`why=block|player|object|edge|water|
lifetime`) and `EVT explosion cause=rocket`. The bots fire it through the
same `rockets_fire` at an enemy's feet, never within 7 blocks.

**The Miner's spade and drill** (`server/Source/Server/Classes.c`).
- The spade's dig (HitTerrain `P10_DMG_DIG`, the 0.75 `DESTROY_THREE`)
  clears, for a class with `dig` 3, the whole 3 x 3 x 3 cube round the cell
  hit (the usual column of three and its eight neighbours on each level).
  The centre column keeps the usual checks (rate, protection: refused
  whole); each other cell goes only where it is solid, above the water
  layer (z < 62) and editable for the mode and the map. The cells arrive as
  one `TerrainUpdate`; the client predicts nothing. `EVT class action=dig3
  cell= cleared=`.
- The drill is the Miner's gear in the grenade slot (0.75 tool 3,
  `Weapon3` on the wire), `gear_count` (5) charges a life, given back by a
  restock (the tent). It has no grenades: `classes_grenades` is 0. The bit
  flies at `GAME_DRILL_SPEED` 40 blocks a second, falls at
  `GAME_DRILL_GRAVITY` 45 and lives `GAME_DRILL_LIFETIME` 3 s (BattleSpades'
  `DRILL_FLYING_SPEED`, `DRILL_GRAVITY_MULTIPLIER` 1.5 on the retail
  projectile gravity of 30, `DRILL_LIFESPAN`), then bores `GAME_DRILL_DEPTH`
  12 blocks, losing `GAME_DRILL_DRAG` 4% of its speed a block.

| Dir | Command | Meaning |
| --- | --- | --- |
| C→S | (no command) | fire a drill bit: the trigger's press with the drill in hand (`Weapon3`, a Miner), as the rocket's above; the bit leaves the eye along the server's copy of the look. Refused within 20 blocks (`DRILL_BASE_R`) of the player's own tent or of its own intel where it lies. At most one a second |
| S→C | `class-info <n> <key> <value>` | a class's figure as the server plays it (`game_class_set`'s keys: `health`, `speed`, `jump`, `weapon`, `blocks`, `blocks_max`, `gear`, `gear_count`, `jetpack`, `dig`, `perk`, `swing`, `melee`, `title`, `blurb`, `enabled`, `secondary`, `block_damage`, `spawn_perk`, `dig_delay`, `sprint`, `water_friction`, `headshot`, `fall_min`, `fall_max`, `fall_dmg`, `fall_water`): every key of every class after `classes 1`, a changed one to everyone on a change (config.toml's `[classes.<name>]`, a plugin's `game.set_class`). A client keeps its own table and predicts the speed, the sprint, the water drag, the jump and the falls from it |
| S→C | `drill-charges <n>` | to the owner: the drill's charges left, on every change (spawn, use, restock, a class change) |
| S→C | `drill-refused <why>` | to the owner: `mode`, `class`, `not_alive`, `tool` (not the gear slot), `no_charges`, `rate`, `direction`, `base` (its own base), `plugin`. No charge was spent |

**The gun in hand [1.3].** The class's second gun (`secondary`: the revolver,
weapon 4, a sidearm the Commando and the Marksman carry) is a tool slot, not a
command: a client asks for the other gun by sending that slot in its
`ClientSideEntityUpdate` (`Weapon2` for the second, `Weapon1` for the first),
the server swaps the clip and reserve with each (`classes_select_gun` into
`classes_swap_gun`), and every client reads another player's gun from its
body's Tool field (`CEVT class action=gun`). A refusal drops the whole tool
change: `EVT class action=swap_refused why=mode|no_second_gun|not_alive|rate`
names the cause and `EVT proto10 tool_refused` records the dropped change.
The grenades, or the Miner's drill, are `Weapon3`. Revision 1.3 removed the
two GenericCommands that did this, `gun-swap` (C→S) and
`gun <player> <weapon>` (S→C).

The bit is an entity, `P10_ENT_DRILL` (10, owner and team), sent to
everyone (`Proto10/Drills.c`; the figures are `GAME_DRILL_*` in
`common/classes/classes.h`). It leaves at 28 blocks a second spinning; in
the air its trajectory is Gravity (24 blocks/s², down), in the ground
Linear: it bores straight on, clearing the 3 x 3 x 3 cube round every
block it enters (a hole 3 wide and 3 high), each cell where the mode and
the map allow it, and loses 4% of its speed a block. A new trajectory comes
when it goes into or out of the ground, every 0.15 s while it bores and
every 0.5 s in the air. It stops (an EntityRemove: the client puffs dust)
after 12 blocks of ground, at a cell nothing may dig, at the water's level
(z 62), at the edge of its own base, under 4 blocks a second or after 4 s.
The hole arrives as `TerrainUpdate`s. Logged as `EVT class action=drill
by=client|bot eye= dir= charges=`, `action=drill_stop why= dug= cleared=`
and `action=drill_refused why=`. Bots of the Miner use it where they would
spade their way through a wall, once out of their base and under a roof
(the spade starts the tunnel; `_bot_dig`, `bot_path action=drill`).

The Miner's spade swings with the left button every `swing` seconds of its
class (0.12; the others 0.2, `GAME_SWING`), so it breaks a block in about
a quarter of a second; the right button's dig keeps its second.

The Miner's shotgun breaks a block with one shot: `block_damage` (0..100,
0 the gun's own: rifle 50, SMG 34, shotgun 20 a pellet) is what a pellet
of the class's guns does to a block of 100 (`GAME_BLOCK_BREAK`), and the
Miner's is 100. The server keeps the block's health (9.3): the client
sends every pellet that meets a block (`HitTerrain`, Weapon1) and draws
its guess (`classes_block_damage` into `map_damage`), the server takes
`block_damage` from the cell and breaks it at 100. One shot may hit as
many blocks as it has pellets (8 for the shotgun): the first hit of a shot
goes through the gun's checks (fire rate, clip, the spade's timers) and
opens a 100 ms window (`GUN_SHOT_WINDOW`, `Block.c`); the others of that
shot within it are counted up to the pellets instead of being refused as
rapid fire (and reported to the staff). The 0.75 profile, where the client
counts and sends only the breaking hit, keeps `classes_gun_breaks` (the
pellets times `block_damage` / 100) there. Each cell still
passes the mode (`GM_RULE_NO_DESTROY`), the map's protected sectors and
rules, `gamemode_cell_editable` and the water layer. Logged as `EVT class
action=gun_break cell= weapon= shot_block=<k> of=<n>`, `EVT block_damage
cell= hp= damage= by=<weapon>` (each gun hit) and the client's `CEVT
block_hit by=gun weapon= pellet= x= y= z= damage= predicted=` and `CEVT
block_damage x= y= z= damage= by=server` (`CEVT block_break` on a 0.75
server).

Logged as `EVT class action=pick class=<name> by=client|join|bot alive=`,
`EVT class action=spawn class= weapon= blocks= grenades= health=`, and the
`EVT gamemode` line's `classes=1`.

### 12.6.1 Supply crates

Ben Aksoy's alpha listed `AMMO_CRATE` and `HEALTH_CRATE` among its entity
types; ours are `P10_ENT_AMMO_CRATE` 11 and `P10_ENT_HEALTH_CRATE` 12
(after the drill, so `P10_ENT_HAS_OWNER` gives them an owner and a team on
the wire: always `P10_NO_OWNER` and the spectators).

When one comes: in a match where the classes play (`gamemode_has
GM_FEAT_CLASSES`), a kill made with 4 or more living players (both teams,
bots too, the killer counted) within 12 blocks of the killer has a 15%
chance to drop one, ammo or health at even odds, over a dry column within 8
blocks of the killer, and every 40 s one falls where players died
lately: the server keeps a heat map of the deaths (cells of 24 x 24
columns, each death 1, halved every 60 s) and drops a crate within 6
blocks of the mean death of a cell with heat 2 or more, the cell drawn by
its heat, at most 4 of those out (`EVT crate action=hot heat= hottest=` or
`hot_skipped why=cold|full`). The zombie mode has a rule of its own
(`gamemode_ops_t.crate_rule`), classes or not: a zombie killed by a human
drops one 20% of the time, 12 to 24 blocks from the killer (it has to go
and get it), at most one every 20 s and 3 lying about. An admin's `/crate <ammo|health> [player]` and a
plugin's `p:drop_crate(kind)` / `game.drop_crate(x, y, kind)` drop one on a
player or a spot in any mode of the 1.0 profile (a reward). At most 16 at
once; the 0.75 profile has none.

The wire: an `EntityUpdate` Create with a **Constant** trajectory from 30
blocks over the ground (map coordinates, z down), and nothing more while it
falls: the client runs the retail game's `Crate.update` itself
(`common/crates/drop.h`), as the retail client does: gravity 30 blocks/s^2,
the parachute opens 10 blocks over the ground and multiplies the speed by
0.75 on every 1/60 s step (2 blocks a second), is released at 2 blocks, and
the crate bounces at 0.5 of an impact over 2. The server runs the same steps
for the position its pickup needs. A joiner gets the crates in
GameStateFinal's snapshot as they are (one still in the air starts its fall
again from there). The client draws the model (`kv6/crate_ammo.kv6`,
`crate_health.kv6`, and the parachute `kv6/crate_chute.kv6`, made by
`tools/make_crates.py`) while the chute is out, and on the ground hovering
0.35 over it, bobbing and turning.

Taking it: the first living player in a team whose body stands at it (1.5
blocks across, feet on its ground) and **needs** it: health below 100 for
the health crate (set to 100), a reserve not full for the ammo crate (both
guns' reserves and the drill's charges filled, as the tent's restock does
for ammo). A full player walks over it and leaves it for a mate. The
taker's client is told `crate ammo|health` (GenericCommand, S→C), and
everyone gets the crate's `EntityRemove`. Untaken, it goes 120 s after it
landed (`EntityRemove`).

| Dir | Command | Meaning |
| --- | --- | --- |
| S→C | `crate <ammo\|health>` | to the taker: the crate's reserves or health are now full |

Logged as `EVT crate action=drop|land|take|taken|expire|drop_skipped kind=
entity= by=kill|command|plugin why=full|no_ground` and the client's `CEVT
crate action=create|land|take`.

### 12.7 Compact Player trajectory (revision 1.2)

**Why.** Measured with 127 bots, EntityUpdate is 99% of what the server
sends, and a Player item is 39-40 bytes, 37 of them the trajectory: nine
32 bit floats for the origin, the velocity and the euler angles (8.5).

**Wire.** A new trajectory type, `PlayerCompact` (5, `P10_TRAJ_PLAYER_COMPACT`),
17 bytes instead of 37:

| field | type | value |
|---|---|---|
| type | `u8` | 5, bit 7 = has parent (as 8.5) |
| origin x, y | `u16` each | position * 64 (1/64 of a block, 0 to 1023.98) |
| origin z | `i16` | position * 64 rounded down (-512 to 511.98): up in the world, so a standing body never arrives inside its floor |
| velocity x, y, z | `i16` each | velocity * 1024 (+-32) |
| pitch | `i16` | euler.x * 32767 / (pi/2) |
| yaw | `i16` | euler.z * 32767 / pi |
| parent | `varint` | when bit 7 is set |

Roll (euler.y) is not carried: a player's is always 0. The decoder hands
the trajectory on as an ordinary `Player` one, so nothing past the codec
knows the form exists.

**Who gets it.** Every 1.0 peer (12.1): the server converts every body's
trajectory (`_compact` in `Proto10/Bridge.c`). The encoder falls back to the
float Player form when a value does not fit (a map past 1024, a velocity
past 32, a roll), so the request is always safe. Clients send their own
trajectory in floats: the server validates movement with it. A 0.75/0.76
connection never meets any of this.

### 12.8 The machine gun, the helicopter and mounting

Two things a 1.0-only match has that 0.75 never did: the Commando's machine gun
and a helicopter per team. Both are objects of the sentry's registry
(`Proto10/Objects.c`), shot through the same path as the sentry, and a player
mans either of them with the `use` GenericCommand (`Proto10/Mount.c`). Nothing
in this section exists in the 0.75 profile.

**Seats and `use`.**
- `use` from a player on nothing looks for the nearest thing in reach of
  **its own team**, a helicopter first (`GAME_HELI_USE_RANGE` 3, measured to
  its box): its first free seat, seat 0 (the pilot) first; then an MG
  (`GAME_MG_USE_RANGE` 2.5) that has no gunner. A press while mounted leaves.
- Refusals to the asker: `none` (nothing in reach), `team` (only the other
  team's), `full` (every seat taken), `busy` (the MG has a gunner), `dead`
  (spectating, or not alive), `wrecked`, `rate` (0.25 s between two presses).
- The server keeps the seat in `player_t.mount_entity` (an object id,
  `P10_NO_OWNER` on nothing) and `mount_seat`; every change is a `mount
  <player> <entity> <seat>` line to every 1.0 client, `mount <player> -1 0`
  when the player leaves it, and a joiner is told every seat taken after the
  lights (`proto10_bridge_mounts_to`).
- **A mounted player's physics does not run**: the seat's eye holds its body
  every tick (the client draws the body there and the pilot's camera is the
  cabin). It cannot shoot, dig, build or throw from a seat (`hit_refused
  why=mounted`, `block action=refused why=mounted`, `grenade
  action=refused why=mounted`). The jetpack does not fire there either: the
  same input bit is the helicopter's lift.
- A death, a team change, leaving the server and a map start take the player
  off (`EVT mount action=dismount why=death|team|left|map|use|wreck|gone`).
- **Piloting.** The pilot's ordinary input drives it: forward/back and
  left/right are the hull's thrust, the held jump climbs (the PlayerInput
  jetpack bit, or a PlayerAction Jump) and the crouch key descends; its look's
  yaw is the heading. The flight is `common/vehicles/heli.h` (`heli_step`), the
  same integrator the client predicts its own helicopter with.
- **The MG's gunner** aims the barrel with its look, within `GAME_MG_YAW_ARC`
  (90 degrees either side of the way it was placed) and between
  `GAME_MG_PITCH_UP` and `GAME_MG_PITCH_DOWN`, and the trigger fires it every
  `GAME_MG_FIRE_INTERVAL` (0.1 s, the SMG's rate).

**The machine gun** (`Server/Proto10/Mg.c`, figures in `common/mg/mg.h`).
- Placed as the Commando's streak perk: `PlayerAction P10_EV_USE_PERK` with
  `p10_perk_param(P10_PERK_MG, x, y, z)` on a floor block, under the sentry's
  cell rules (`sentry_cell_ok` at the same 5 blocks; the client's preview is
  the same), one at a time an owner (`already_have_one`), refused `mode` where
  the perk does not exist.
- **It stands where it was put whatever its owner does**: a death, a class
  change or a team change leave it standing (unlike the landmine and the
  dynamite, `proto10_objects_owner_class_changed`); it goes when it is
  destroyed, when its owner leaves the server and at a map start.
- The entity: `P10_ENT_MG` (13), owner the placer and its team; a Constant
  trajectory at the swivel, its rotation the barrel's (yaw, pitch);
  `P10_EU_HEALTH` as the percent of `GAME_MG_HEALTH` (250); the
  `P10_EF_WEAPON_CLIP` flag, so bullets hit it.
- Its rounds are a hitscan from `GAME_MG_MUZZLE` (1.8 blocks along the
  barrel): the SMG's damage by the part hit (`GAME_MG_DAMAGE_BODY` 24, head
  75, limb 16), its block damage through block health, and enemies' sentries,
  MGs, mines, dynamite and helicopters. The gunner is the attacker (its kill,
  its score), kill reason `KILL_REASON_MG` (9, `P10_DMG_WEAPON1` to the
  victim), the damage's drawn source the MG itself. Each round kicks the
  barrel up 2 degrees and 1.5 to a side, easing back, and is an `EntityEvent
  P10_EV_FIRE` on the MG whose parameter's bit 0 says the round met
  something.
- Unmanned, the barrel levels out; any teammate may man it, and its owner
  need not be alive for it.

**The helicopter** (`Server/Proto10/Helicopter.c`,
`common/vehicles/heli.h`).
- **One per team at its base, on a 1024 x 1024 map only** (the same test the
  client reads: `map-size` 1024, 1024). At every map start the server looks
  for the nearest column to the team's spawn rectangle whose ground is flat
  all over the hull, faces it toward the other team's base and spawns one
  there (`EVT heli action=base`, `action=spawn`). A site it cannot find leaves
  that team none.
- The entity: `P10_ENT_HELICOPTER` (14), owner `P10_NO_OWNER`, its team; a
  **Linear** trajectory whose origin is the skids' bottom middle and whose
  velocity is in blocks a second; the rotation is the hull's heading,
  nose-down and roll as one quaternion (`p10_rotation_from_euler`'s
  Rz(yaw) * Rx(pitch), rolled about the body's own forward axis). The rotation
  turns heli.h's heading (0 = +x) into the euler convention (0 = +y) first.
  `P10_EU_HEALTH` is the percent of `GAME_HELI_HEALTH` (400), 0 while it is a
  wreck. The trajectory goes out at 20 Hz to everyone (on a change of
  position, heading or health) and 60 Hz to its occupants.
- `GAME_HELI_SEATS` (3): 0 the pilot, 1 and 2 the passengers. Nobody fires
  from it.
- Its flight is `heli_step` every tick with the pilot's input, clipped one
  axis at a time against the map with its box (`GAME_HELI_HALF_LENGTH` 5.5
  along the nose, `GAME_HELI_HALF_WIDTH` 1.8, `GAME_HELI_HEIGHT` 4.5): a wall
  zeroes that way's speed, the map's edge is a wall, and a downward move that
  meets the ground is a landing (`heli_impact`: over `GAME_HELI_SAFE_LANDING`
  6 blocks/s it costs `GAME_HELI_CRASH_DAMAGE` 25 of health for each block/s
  more). It climbs no higher than z 3.
- **Hits** come through the sentry's path (bullets, spades, the MG's rounds,
  rockets, grenades and explosives), enemies only. At 0 health it is a
  **wreck**: no lift, gravity, `EVT heli action=wreck`, and its occupants die
  at once (kill reason `KILL_REASON_EXPLOSIVE` 8, the last enemy who hit it
  credited, `EVT heli action=crash`). The wreck falls, and on the ground it
  blows up (`P10_EV_EXPLODE` then the `EntityRemove`,
  `action=explode why=wreck`); `GAME_HELI_RESPAWN` (10 s) later a new one
  stands at the base (`action=respawn`).
- **A helicopter's occupants cannot be hit by bullets** (`hit_refused
  why=in_vehicle`; the bots do not even pick one as a target): the hull is
  between them and the shot. The MG's gunner has no such cover.
- Logged as `EVT heli action=base|spawn|spawn_failed|mount|dismount|damage|
  crash|wreck|explode|respawn` and `EVT mount
  action=mount|dismount|refused|state player= entity= seat= why=`.

**Checked.** Four bots on Villages_1024 at 20x: a bot boarded a helicopter
(seat 0), its body followed the seat, an enemy's shot at it was refused
(`why=in_vehicle`), the pilot's keys flew it 11 blocks, a wreck killed the
occupant aboard (death reason 8) and a new one stood at the base 10 s later;
an MG that bot placed fired every 0.2 s at what its gunner looked at and
stopped when it left. A 512 map spawns no helicopter (`EVT heli` absent).

---

## 13. Implementation notes for our code

### 13.0 Shared code (`common/proto10/`)

Client and server live in one repository, so the wire code is written once:

- `common/proto10/buf.{c,h}`: section 4.1.
- `common/proto10/codec.{c,h}`: one encode/decode pair per packet, plain C99,
  no dependency on either program (no `player_t`, no client globals, no ENet:
  it works on byte buffers).
- `common/proto10/tests/`: golden vectors and round-trip tests, built as their
  own executable and run with the sanitizers.

Both CMake builds add `${CMAKE_CURRENT_SOURCE_DIR}/../common/proto10` to
their sources. The code must build with the server's `-Werror` and with the
client's flags. Code that knows about one program stays in that program.

### 13.1 Server (`server/`, SpadesX, C)

- New directory `Source/Server/Proto10/` (on top of `common/proto10/`):
  - `Session.c`: the handshake state machine (5.2) with its timeouts.
  - `Bridge.c`: translates between `player_t`/map state and 1.0 packets.
- `player_t` gets `uint8_t protocol` (`PROTO_075`, `PROTO_10`), set in
  `on_new_player_connection` from `event->data`. The send helpers (`send_*`)
  look at it and emit 0.75 or 1.0. **Keep the NULL-peer early return in every
  helper**, because bots have no peer (see `CONTRIBUTING.md`).
- Bots stay unaware of protocols: they are `player_t`s and are announced to
  1.0 clients through the same `Bridge` (a `PlayerUpdate` with Create and an
  `EntityUpdate` with a `Player` trajectory).
- The game tick already runs at 60 Hz. Build 1.0's per-tick `EntityUpdate`
  and `TerrainUpdate` batches there, from the same state 0.75's
  `send_world_update` reads, so both protocols always describe the same world.
- Log every handshake step and every refused 1.0 packet with `EVT`, including
  the reason. Example: `EVT 1234 p3:Deuce proto10 state=map_transfer bytes=81920`.

```c
/* Player.c: choose the protocol from the ENet connect data */
#define PROTO10_CONNECT_MAGIC 0x08101919u

void on_new_player_connection(server_t* server, ENetEvent* event)
{
    uint8_t protocol;
    switch (event->data) {
        case VERSION_0_75:
            protocol = PROTO_075;
            break;
        case PROTO10_CONNECT_MAGIC:
            protocol = PROTO_10;
            break;
        default:
            enet_peer_disconnect_now(event->peer, REASON_WRONG_PROTOCOL_VERSION);
            return;
    }
    /* ... existing ban check and slot allocation, unchanged ... */
    player->protocol = protocol;
    if (protocol == PROTO_10)
        proto10_session_begin(server, player); /* sends Greeting, arms the 10 s timeout */
    /* the 0.75 path continues exactly as today */
}
```

### 13.2 Client (`client/`, BetterSpades, C)

- `src/proto10.c` holds the client session (handshake and packet handling),
  on top of `common/proto10/`. `network.c` keeps a `network_protocol` variable and routes
  received packets either to the existing 0.75 handlers or to the 1.0 ones.
- Connection order: 1.0 magic, then 0.75, then 0.76 (section 2). Until 1.0
  can play a whole match, the 1.0 attempt only happens for `aos10://`
  addresses or `SP_PROTO10=1`; everything else connects exactly as before. A
  server browser entry can remember which one worked.
- Map: both formats. `map-format=vxl` goes through the 0.75 loader;
  `ngmap` is decoded with `ngmap_decode` into VXL bytes and loaded with the
  same `map_vxl_load` (`network_load_map_ngmap`). The client logs the format,
  the size, download and decode times.
- Entities: keep the existing `players[]` array as the client-side model;
  1.0 updates write into it (applying the axis conversion in 8.2). Grenades
  and future entities get a small `entities[]` table keyed by id, with
  trajectories extrapolated per frame.
- Our kill feed and minimap (`hud.c`) do not care where their data came from:
  feed them from `EntityDie` exactly as from 0.75's `KillAction`.

### 13.3 Tests

- **Golden vectors**: one hex dump per packet type, encoded by our C code and
  checked against the reference C++. The reference codec is nearly self-contained
  (`Server/Shared.cpp` plus `Core/ENetTools.cpp` and the headers they include);
  building it standalone with stubs for `Core/Debug.h` and the version macros
  is the first task of milestone 1, not yet verified. The vectors in this document (Greeting, `TerrainUpdate`,
  varints) are the first ones.
- **Round trips**: decode(encode(x)) == x for every packet, with random
  masks; decoders must reject truncated packets and every one-byte truncation
  of a valid packet.
- **Interop**: a test where `tools/sptest/harness.py` starts the server and
  connects a 1.0 client and a 0.75 client to the **same** match, and checks
  that both see each other and the bots (screenshots plus `EVT` lines).

---

## 14. Milestones [1.0]

1. **Codec**: buffer + every packet in sections 6–10 with D1–D5 fixed, golden
   vectors and round-trip tests. No gameplay. **Done**: `common/proto10/`,
   tested with `make test-proto10` (golden vectors, round trips, truncation
   and trailing-byte rejection, 500k fuzz inputs under ASan/UBSan).
2. **Handshake + VXL map**: a 1.0 client reaches `Game` on our server with
   `map-format=vxl` and sees the world. 0.75 clients unaffected (regression:
   join with a vanilla client in the same run). **Done**: verified with the
   harness, a 1.0 client (`--proto10`) and a 0.75 client sharing one match
   with bots (`--join`); the 1.0 client logged no stray 0.75 packet.
3. **Players and movement** (**done**: `Server/Proto10/Bridge.c`, client
   `proto10.c`; a 1.0 client joins from the team menu, moves, and is seen
   moving by a 0.75 client in the same match; bots are visible on 1.0):
   `PlayerUpdate`, `EntityUpdate`/
   `ClientSideEntityUpdate` with Player trajectories, `local-player`, `join`,
   bots visible on both protocols.
4. **Combat** (**done**: hits are replayed through the 0.75 `receive_hit_packet`, deaths and damage hooked in `send_kill_action_packet`/`send_set_hp`; verified: a bot killed a 1.0 player, the 1.0 kill feed showed it and the player respawned; a 1.0 player's own hit on someone is not tested yet): `HitEntity`, `Damage`, `EntityDie`, respawn; kill feed on 1.0.
5. **Building** (**done**: `Server/Proto10/World.c`, client `proto10_world.c`,
   `common/proto10/text.c`; verified with a 0.75 and two 1.0 clients in one
   match: a 1.0 client's build, spade destroy and dig reach the 0.75 client, a
   0.75 client's builds reach the 1.0 clients, chat (all and team, CP437
   characters intact) works both ways, a 1.0 client joining mid-build applies
   the edits made during its map download, and `/reset` re-sends the map to
   1.0 clients): `HitTerrain`, `BuildBlock`/`BuildLine`, `TerrainUpdate`
   batching, chat. HitTerrain mapping in 9.3.
6. **Grenades and game modes** (**done**: `Server/Proto10/Entities.c`,
   `Server/Proto10/GameMode.c`, client `proto10_entities.c`,
   `proto10_gamemode.c`; details in 12.2.3 and 12.2.6; verified with a 0.75
   and two 1.0 clients and a bot in one CTF match: a 1.0 client's grenade
   explodes on the server and reaches the 0.75 client, 0.75 and 1.0 grenades
   reach the other 1.0 clients as entities and explode there, a bot's intel
   pickup and capture show on the 1.0 client with the 0.75 messages and the
   score, and a 1.0 client joining while the bot carries the intel gets
   `carrier:` and the scores in its snapshot. Not verified: a round won by
   capture over 1.0 (the bots rarely bring the intel home), TC (no server
   support)): `Gravity` entities, CTF/TC state (`ThrowGrenade`,
   `GameModeState`).
7. **ngmap**: port the codec, negotiate quality, keep VXL as fallback.
   **Done**: codec in `common/ngmap/` (7.5); negotiation by `mapQuality`
   (6.2: 0 = VXL); server encodes in the background with a per-quality cache
   and edit replay (7.1), falls back to VXL when encoding fails
   (`SP_NGMAP_FAIL=1` forces that path); client decodes ngmap and has a
   "Map quality" setting. Verified: `make probe-proto10` scenarios
   `handshake` (VXL) and `ngmap` (two joins, the second from the cache, each
   stream decoded with `ngmap_decode` and walked as VXL); harness joins on
   Pinpoint, Raindance and Gridwar render the map; a client joining after
   grenade craters gets the cached map plus the 36 crater cells, the same set
   the first client saw. Join (ClientCertificate to GameStateFinal, local
   network) and stream size, q80 ngmap vs VXL: Pinpoint 49 KB vs 136 KB
   (0.31 s fresh vs 0.17 s); Raindance 152 KB vs 873 KB (0.66 s fresh, 0.22 s
   cached vs 0.76 s); Gridwar 766 KB vs 1.07 MB (1.81 s fresh, 0.51 s cached
   vs 0.65 s). On a LAN a fresh ngmap join is slower than VXL by the encoding
   time; ngmap pays off on slow links and once cached.
8. **The switch** (**done**; probe scenario `vanilla` now expects the
   refusal, harness default and `--singleplayer` connect over 1.0,
   `--vanilla` falls back to 0.75) (after 5 and 6): the server refuses connect data 3 (0.75)
   with "wrong protocol"; the client tries 1.0 first, then 0.75, then 0.76;
   singleplayer and `--host` use 1.0 directly; the server is no longer
   announced on the 0.75 master list; `make vanilla-server` keeps the
   client's 0.75 path testable. The server's 0.75 send code is **removed**
   (the receive handlers stay as the internal gameplay API).
9. **1.0-only features**: sentry gun (a new entity type, not one of the
   reference's seven ids), jetpack (player physics on both sides plus an
   input bit, protocol revision 1.1), helicopter (Vehicle entity, riding with
   parentEntityId).
   **Jetpack done** (12.4): revision 1.1, a perk behind `jetpack_grant()`.
   **Sentry done** (12.5): `Server/Proto10/Sentry.c` plus the generic object
   records in `Proto10/Objects.c`, placed from the HUD preview, with a
   configurable lifetime after which an untouched one explodes.
   **Perk earning done** (12.5): one progress counter per perk with
   configurable weights, thresholds, `enabled` and `reset_on_death`, so the
   server owner decides how each perk is earned.
   **Helicopter done** (12.8): two per team at its base on a 1024 map, a pilot
   and two passengers, flown by the pilot's keys with `common/vehicles/heli.h`
   on both sides, hit like a sentry, and a wreck that falls, blows up and
   comes back. (An earlier plan dropped it as unplayable with a gamepad; the
   new-assets work, after the AoS 1.0 beta client, brought it back: `use`
   boards it and the ordinary movement keys fly it.)

---

## Appendix A: source map of the reference

| Topic | File (in `ngspades/Sources/`) |
| --- | --- |
| Packet classes and ids | `Server/Shared.h` |
| Wire encoding of every packet | `Server/Shared.cpp` |
| Fixed-width primitives | `Core/ENetTools.{h,cpp}` |
| Game enums and structs | `Game/Constants.h` |
| World parameters | `Game/World.cpp` |
| Server handshake, map transfer, validation | `Server/Connection.cpp` |
| Server ENet host, disconnect codes | `Server/Host.cpp` |
| Server tick, delta broadcast | `Server/Server.cpp`, `Server/ServerEntity.cpp` |
| Client handshake and packet handling | `NGClient/NetworkClient.cpp` |
| Client ENet host | `NGClient/Host.cpp` |
| Client state upload | `NGClient/NetworkPlayer.cpp` |
| Player physics | `Game/PlayerEntity.cpp` |
| Trajectory integration | `Game/Entity.cpp` |
| ngmap codec | `Client/GameMap_ngmap.cpp` |
| Compact quaternion | `Core/Math.h` (`EncodeRotation`) |
