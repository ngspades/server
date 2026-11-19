# Host commands: `PACKET_HOSTCMD`

Contract shared by the client menu and the server. Anyone working on this
must follow the ids, layout and semantics below exactly.

> Status: the ids and layouts below are the 0.75 packets, which only the
> client still speaks (to vanilla-compatible servers). Our server speaks
> protocol 1.0 only, so the same contract travels as 1.0 `GenericCommand`s
> (`docs/protocol-1.0.md` 10), with the fields as decimal strings in the
> order given here:
>
> - client -> server: `host <command> <arg1> <arg2>` (`player_id` is dropped:
>   the sender is authoritative). The client's `proto10_send_075()` turns the
>   menu's `PACKET_HOSTCMD` into it; the server's bridge hands it to
>   `hostcmd_apply`, the same queue the console uses.
> - server -> client: `host-status <bots> <humans> <autofill> <flags>
>   <difficulty>` (the difficulty is the 1.0-only fifth field), sent
>   by `Proto10/Bridge.c` as a per-tick diff per player (so autofill, joins and
>   console commands reach the menu too). The client replays it into
>   `read_PacketHostStatus`.
>
> - server -> client: `host-bots <team1> <team2>`, each team's bots (1.0
>   only, its own command: a client keeps six parts of a command), sent
>   on a change with `host-status`; the Bots menu shows them.
>
> Without `host-status` the menu says "Servidor sem suporte a bots.": that is
> what a 1.0 server without this support (or a vanilla one) looks like.

## Why a dedicated packet

The host (the player who started the local/listen server, or an admin of a
remote one) manages bots from the ESC menu. Reusing chat would pollute the
chat and expose commands to everyone. A dedicated packet keeps the vanilla
0.75/0.76 clients compatible: unknown packet ids are ignored by vanilla
clients and by vanilla servers, so both directions keep working.

## Ids

| side | id | name |
| --- | --- | --- |
| client -> server | `63` | `PACKET_HOSTCMD` |
| server -> client | `64` | `PACKET_HOSTSTATUS` |

`50` is deliberately avoided: it is the master/statistics callback id used by
SpadesX, so the 60-79 range is used for our extensions.

## `PACKET_HOSTCMD` (client -> server)

```
u8  player_id   (ignored by the server, the sender is authoritative)
u8  command     see the table below
u8  arg1        (meaning depends on the command, 0 when unused)
u8  arg2        (meaning depends on the command, 0 when unused)
```

| command | name | arg1 | arg2 |
| --- | --- | --- | --- |
| 0 | `ADD_BOTS` | count (1..64, a whole team) | team: 0 = blue, 1 = green, 2 = balance |
| 1 | `KICK_BOTS` | 0 | 0 (removes every bot) |
| 2 | `SET_AUTOFILL` | target player count (0 = off) | 0 |
| 3 | `KICK_BOT` | bot player id | 0 |
| 4 | `FILL` | fill to `arg1` players per team | 0 |
| 5 | `DIFFICULTY` | 0 = easy, 1 = normal, 2 = hard | 0 |
| 6 | `TUTORIAL_BOT` | team of the bot (0 = blue, 1 = green) | 0 (one dummy bot on the other team's spawn area: it never moves or fires) |
| 7 | `TEAM_BOTS` | count (0..64) | team (0 = blue, 1 = green): that team gets exactly `count` bots, added or the newest removed (the Bots menu's row) |

Permissions are checked server side: only the player who owns the listen
server (or a console/admin command) may use it. Unauthorised commands are
answered with `HOSTSTATUS` reason `NOT_ALLOWED` and change nothing.

## `PACKET_HOSTSTATUS` (server -> client)

Sent to a player right after they spawn/join, and after every host command, so
the menu can show live numbers.

```
u8  bot_count
u8  player_count      (humans, excluding bots)
u8  autofill_target   (0 = disabled)
u8  flags             bit 0: sender may manage bots (host/admin),
                      bit 1: bots are enabled on this server,
                      bit 2: autofill is enabled
```

## Server semantics

- Bots are server players (`player_t`) flagged with `is_bot`. They appear to
  clients exactly like human players (`CreatePlayer`, `WorldUpdate`, ...), so no
  client change is needed to see them.
- `ADD_BOTS` adds up to the server player limit, balancing teams by default.
- `FILL n` tops both teams up to `n` players each.
- `KICK_BOTS` removes all bots, `KICK_BOT` removes one by player id.
- `SET_AUTOFILL n` keeps `n` players per server as bots: when a human spawns
  and that would exceed the target, bots are removed (on the next bots tick,
  never from inside the spawn: that freed players other loops still walked);
  when a human leaves, a bot is added back. A bot's own spawn never trims, so
  bots the host adds by hand stay. The menu's number only applies while its
  checkbox is on.
- `DIFFICULTY n` sets the range each bot's skill is rolled in and the aim
  error factor, re-rolling the bots already playing. The server starts with
  `[bots] difficulty` from `config.toml` (`SP_BOT_DIFFICULTY` overrides it);
  singleplayer passes the level last picked in the menu, saved in the client's
  `config.ini` as `bot_difficulty`.
- The same operations are available from the server console as
  `addbot [n] [blue|green]`, `kickbots`, `kickbot <id>`, `botfill <n>`,
  `autofill <n>`, so the feature can be tested without the menu.

## Client semantics

- The ESC menu shows a "Bots" section only when `flags` bit 0 is set.
- Controls: add N bots (per team / balanced), fill each team to N, remove all
  bots, autofill toggle, and a live list/count from `HOSTSTATUS`.
- The client must keep working against a vanilla server: no packets are sent
  unless the server announced support (`flags` bit 1).
