# Writing a protocol 1.0 server

What a server must do for 1.0 clients, our client included. The Shoot and
Destroy server (`server/`, a SpadesX fork, `Source/Server/Proto10/`) is the
reference implementation and speaks only 1.0. Byte layouts are in the dossier,
[protocol-1.0.md](../protocol-1.0.md), and the section numbers (§) point into
it. `common/proto10/` has the C codec for every packet, with tests, and needs
nothing but a C99 compiler.

Read [client-1.0.md](client-1.0.md) first: it gives the order of the
handshake from the client's side. The gameplay rules (damage, blocks, modes)
are the 0.75 ones in [server-0.75.md](server-0.75.md); 1.0 changes the
packets, not the game.

**Must** marks what clients break without; **should** marks what they cope
without.

---

## 1. Transport

| Setting | Value |
| --- | --- |
| Port | 32887 |
| Connect data | `0x08101919`. Refuse anything else with reason 7, or with 3 to look like a vanilla server |
| Channels | 2: 0 reliable, 1 unreliable sequenced for movement |
| Compression | the ENet range coder |
| Peers | your limit; ours is 128 |
| Framing | `u8 id` + payload; ignore unknown ids (§3.2) |
| Browsers | optional WebRTC DataChannels carrying the same packets (§3.4, `Net/`) |

Disconnect reasons (§3.3): 0 unknown, 1 internal error, 2 full, 3 kicked (send a
`Kick` with the text first, then disconnect with `enet_peer_disconnect_later`),
4 stopped, 5 timeout, 6 malformed packet, 7 protocol mismatch.

**Must**: decode strictly (§4): reject truncated packets, varints over 10
bytes, strings over their limit or with a NUL, and non-UTF-8 names and chat.
Disconnect with 6 on a malformed packet during the handshake; in the game, log
it and drop the packet.

**Our server** refuses a 0.75 or 0.76 connect data with reason 3, exactly as a
vanilla server refuses a version it does not know, so a 0.75 client shows
"wrong protocol" (`Player.c`, `on_new_player_connection`).

---

## 2. The session (**must**)

A connection goes through these states, each with a deadline (§5.2, §5.3):

| State | Waits for | Deadline |
| --- | --- | --- |
| not initiated | `InitiateConnection` | 5 s (`P10_TIMEOUT_INITIATE`) |
| waiting for certificate | `ClientCertificate` | 10 s (`P10_TIMEOUT_CERT`) |
| map transfer, completing | `MapDataAcknowledge` | 300 s |
| game | | ENet's own timeout |

### 2.1 Greeting and InitiateConnection (§6.1, §6.2, §12.1)

1. On connect, send `Greeting`: the string `"Hello"` and 128 random bytes.
2. Read `InitiateConnection`:
   - the protocol name must be exactly `"WorldOfSpades 1.0"`, else reason 7;
   - log the major and minor version; they are informational, and every
     1.0 client gets the whole protocol (§12.1);
   - `mapQuality` 0 means "send VXL", 1..100 means ngmap at that quality,
     clamped to 20..100 (ours ignores it and sends the map's stored ngmap
     q100: protocol-1.0.md 7.1);
   - check the name: UTF-8, at most 63 bytes, unique on the server.
3. Send `ServerCertificate` with `isValid = 0`, and accept the client's with
   `isValid = 0`. Nothing is signed yet (§6.3).

### 2.2 The map (§7)

1. `GameStateHeader`: a `map` with `map-name`, `map-format` (`vxl` or
   `ngmap`), `map-size` and `map-bytes`. Send `vxl` when you cannot encode
   ngmap, whatever the client asked for.
2. `MapData` fragments (4 KiB or so), throttled on what the peer still has
   in flight.
3. `MapDataFinal`, then wait for `MapDataAcknowledge`.
4. **Must**: keep sending `TerrainUpdate` to a client that is still loading.
   The client applies them after decoding the map, so nothing built during the
   download is lost.
5. `GameStateFinal`: the world parameters (§7.6), then **every** entity and
   **every** player in the match, each with the Create bit. (The `ngspades`
   reference forgot the players, §11 D6.)
   Our server's `fog-distance` is `[server] fog_distance` (192 in the shipped
   config.toml, 128 without the key, 32 to 256): our client shows bodies up
   to it (the terrain to the player's own view distance), so do not send
   enemies past it (ours cuts them 8 blocks further, `Proto10/Bridge.c`).

**Our server** encodes ngmap on a background thread from a snapshot of the map
and replays the edits made since then as `TerrainUpdate`; encodings are shared
between joiners asking for the same quality (§7.1, `Proto10/Session.c`).

### 2.3 Join (§10)

The client is now watching. It sends a `GenericCommand`:

| Command | Do |
| --- | --- |
| `join <team> <weapon> [<class>]` | create the player and its body entity, spawn it, then send `local-player <id>` to that client |
| `team <n>`, `weapon <n>` | change, as a 0.75 `ChangeTeam`/`ChangeWeapon`: a death, then a spawn |
| `leave` | remove the player, back to watching |

Unknown commands: log and ignore.

**Must**: use the same id for a player and its body entity.

---

## 3. The game

### 3.1 Players and entities (§8)

Each tick, for every entity, compare what changed since the last update (the
delta rule, §8.4) and send one `EntityUpdate` (30) with every non-empty item.
Send `PlayerUpdate` (50) when a name,
team, score, kills or deaths, ping or flags change (§8.11), and `PlayerRemove`
(51) / `EntityRemove` (33) when they go away.

A player body's `Create` carries all three weapon slots and their skins
(§8.4, revision 1.3): the class's primary gun, its second gun (zero
parameters when the class has none) and the gear (the grenades or the
Miner's drill, whose magazine is the count).

Our server ticks at 60 Hz and builds `EntityUpdate` from the same state its
game logic uses (`Proto10/Entities.c`, `Bridge.c`). It sends everything
reliable on channel 0 for now. **Should**: send movement-only items unreliable
on channel 1, as the client already does with its own movement, so a lost
packet does not hold back the ones behind it (§3.1).

**Should**: send player bodies with the compact trajectory, 17 bytes instead
of 37 (§12.7), to every client. It is about all a busy server sends.

### 3.2 Client movement (§8.7, §8.13)

`ClientSideEntityUpdate` (35) carries the client's own body: trajectory, input,
tool, block color. Movement is client-authoritative, so check it:

- drop items for an entity the client does not own;
- refuse a change of trajectory type;
- accept the new trajectory only when it is close to your own extrapolation
  (the reference allows 0.1 block). Otherwise send a correction: an
  `EntityUpdate` with `forced = 1`.

The tool slot of that item is the player's choice of gun (revision 1.3):
`Weapon1` is the primary, `Weapon2` the class's second gun — make the swap
(clip and reserve go with each, refused without a second gun, dead, or
within 0.25 s, and then drop the whole tool change) — and `Weapon3` is the
grenades or the Miner's drill. Do not tell anyone with a command: the tool
slot of the body's entity items tells them which gun it holds. Fire the
rocket launcher and the drill on the press of `toolPrimary` (set in this
update, not in the last), from your copy of the eye and look, after applying
the update's look and tool.

`PlayerAction` (60): `Jump` and `ReloadWeapon` (send `EntityEvent`
`ReloadWeapon` to the others, §8.8).

### 3.3 Combat (§9)

- `HitEntity` (90): the client says its shot hit an entity, with the fire
  position, the hit position and the part. Check it like in 0.75 (fire rate,
  ammo, distance, line of sight) and apply the damage. A gun's damage type is
  `Weapon1` or `Weapon2` (the slot in hand) and `Damage`/`EntityDie` echo it:
  refuse a `Weapon1`/`Weapon2` hit that is not the slot in hand
  (`hit_refused why=slot`).
- `HitTerrain` (91): a shot or the spade (damage type `Dig`) hit a block.
- `Damage` (92) to the victim: amount, type, source.
- `EntityDie` (32) to everyone when it dies. **Our server** sets `param` to the
  0.75 kill kind plus the respawn seconds shifted by 8, and `fromEntity` to the
  killer (§8.9). Our client builds its kill feed from that.

### 3.4 World edits (§9.1, §12.2)

| From the client | Check, then |
| --- | --- |
| `BuildBlock` (93) | holding blocks, in reach, inside the map: place it |
| `BuildLine` (94) | enough blocks, both ends in reach: place the line |
| `ThrowGrenade` (95) | holding a grenade, has one: create a grenade entity with a gravity trajectory |
| `HitTerrain` (91) | damage or destroy the block. **Our server** keeps a gun's block health (100, the gun's or class's damage a pellet), breaks the block at 0 and queues a crack as the cell's health in the tick's `TerrainUpdate` (§9.3, §9.1); the spade's hit is the breaking one, counted by the client |

Send what changed as `TerrainUpdate` (40): every cell created or destroyed by
one action in one packet, so clients apply it whole before running their block
physics. Run the physics on the server too.

### 3.5 Chat (§12.2.1, §12.2.2)

`ChatMessage` (70) from a client: check the length and the UTF-8, then relay it
as `ChatBroadcast` (71) to everyone or to the team. Server notices are
`ChatBroadcast` too.

### 3.6 Game mode (§12.2.3)

`GameModeState` (80) is a string map (`P10_GM_*` keys in `codec.h`):

- a snapshot, starting with the key `mode`, right after `GameStateFinal` and at
  each round start;
- a delta whenever a score, intel, territory or capture progress changes.

The CTF and TC rules are the 0.75 ones (section 3.8 of
[server-0.75.md](server-0.75.md)).

### 3.7 Respawn

Send the respawn delay with the death, in the high byte of `EntityDie`'s
`param` (section 3.3), and spawn the body when it expires. §12.3 proposes a
`RespawnAt` field in `PlayerUpdate` instead; it is not implemented.

---

## 4. Our additions (optional)

These are ours, announced through world parameters, entity flags, map keys and
`GenericCommand`s, and a client that does not know them still plays:

- the hit and kill markers (`hit`, `kill`), spotting (`ping spot`/`spot`),
  assists;
- classes, perks, the jetpack and the sentry gun (§12.4–12.6);
- bot management for the host (`host`, `host-status`, `docs/host-commands.md`).

There is no per-client version filtering: every 1.0 client decodes all of
them, and an addition to the wire changes the server, the client and
`common/proto10/` together.

---

## 5. Checklist

**Must**:

1. ENet on 32887 with the range coder, 2 channels, connect data `0x08101919`.
2. A strict decoder; unknown packet ids, commands and map keys ignored.
3. The session with its deadlines: `Greeting`, `InitiateConnection`
   (protocol name, `mapQuality`), certificates.
4. The map: `GameStateHeader` with `map-format`, `MapData`, `MapDataFinal`,
   `MapDataAcknowledge`, `TerrainUpdate` relayed during the transfer.
5. `GameStateFinal` with every entity and every player.
6. `join`/`local-player`, `team`, `weapon`, `leave`.
7. `EntityUpdate` deltas per tick; `PlayerUpdate`, `PlayerRemove`,
   `EntityRemove`.
8. Checked `ClientSideEntityUpdate`, `PlayerAction`.
9. `HitEntity`, `HitTerrain`, `Damage`, `EntityDie`.
10. `BuildBlock`, `BuildLine`, `ThrowGrenade`, `TerrainUpdate`, block physics.
11. `ChatMessage` → `ChatBroadcast`.
12. `GameModeState` for at least CTF.

**Should**:

- ngmap with a shared, cached encoding.
- Compact trajectories for every player body.
- A listing on a master server (ours, https://master.shootanddestroy.obex.pw:
  signed HTTPS announces).
- `EVT` log lines for every handshake step and refused packet.
