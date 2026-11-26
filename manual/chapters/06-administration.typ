#import "/template.typ": *

== Roles and passwords

A player becomes staff with `/login <role> <password>` in ngspades-client's chat. There are five roles, each with a password in `[passwords]`. A higher role holds the permissions of the lower ones.

#wide-table(
  (auto, auto, auto, 1fr),
  table.header([*Role*], [*Config key*], [*Bit*], [*Typically for*]),
  [`trusted`], [`trusted`], [1], [Regular players you trust; the lowest staff level. Few commands need it alone.],
  [`guard`], [`guard`], [2], [Helpers: mute, kick, ban, `/tb`, `/tk`.],
  [`mod`], [`moderator`], [4], [Moderators. Note the name: `/login mod ...`, but the config key is `moderator`.],
  [`admin`], [`admin`], [8], [Administrators: maps, teleports, the sky, crates, perks, `/reset`.],
  [`manager`], [`manager`], [16], [The owner: everything, including `/shutdown`.],
)

An empty password turns that role off. The server warns at every start while a password is still `changeme...`. A plugin may ask for more than the password before the role is held (`on_login`, a second factor). `/logout` drops the role.

#note[
  The host of a listen server (a player who started a match from ngspades-client's menu) owns it: every staff permission is granted without a `/login`. A dedicated server always asks for a login. The console and the remote console run every command with all permissions.
]

== Using commands

In ngspades-client, type the command in the chat with a `/`. Commands are lower-cased before they run (arguments keep their case). Arguments are separated by spaces or tabs, and a pair of quotes wraps one that contains spaces. A player is named by `#<id>` (`/kick #3`; the id is on the scoreboard and in `/bots`). A line is limited to 1000 characters; the server flood-limits chat commands for non-staff.

On the server's own console (a TTY), the prompt `Console>` takes the same commands, with or without the slash. The console is off when the server runs without a terminal (a service, Docker without `-it`); use the remote console below.

```text
/kick #3
/ban #3 60 spawn killing        a ban of 60 minutes with a reason
/pban #3                        permanent
/map Normandie
/say The server restarts in 5 minutes
/addbot 4 blue
```

The ban commands: `/ban #id [minutes] [reason]` (0 or none: for good), and fixed lengths `/hban` (6 hours), `/dban` (a day), `/wban` (a week), `/mban` (a month), `/pban` (for good). `/unban <ip>`, `/undoban`, `/banrange` and `/unbanrange` complete them. Bans are kept by address in `Bans.json` in the data folder (an IPv6 player by its /64), and the kicked player is told. `game.ban` in a plugin writes the same list.

== Every command

The table is generated from the server's command registry. *Who* is the lowest role that may use it.

#include "/gen/commands.typ"

#note[
  Commands from plugins (`game.command`) appear in `/help` after the server's own, with the role the plugin set. A plugin cannot override a server command: a name already taken is refused (`EVT plugin action=command_refused why=taken`). The shipped `server_tools` plugin adds `/spawn`, `/spawn crate` and `/goto <x> <y>` for admins (protocol 1.0 only).
]

Two registered commands are for diagnosis: `/colinfo` dumps a map column, `/profile` (guard or higher) shows the profile, config path, data folder, ports and masters, which it keeps from other players because it names the machine's folders.

== The remote console

A TCP connection that runs the console's commands, for a tool that manages the server: #link("https://github.com/ngspades/sadctl")[sadctl] (which also offers an MCP endpoint for AI agents), a monitor, a script. Nothing else is in the server: no HTTP, no JSON-RPC, no tokens.

```toml
[rcon]
enabled = true
bind = "127.0.0.1"
port = 32890          # the 0.75 profile's config uses 32900
password = "at least sixteen characters"
```

`SP_RCON_PASSWORD` (which also turns it on), `SP_RCON_PORT` and `SP_RCON_BIND` win over the file. With a shorter password the server logs a warning and leaves it off. It is not built on Windows.

#note(kind: "warning")[
  The password is the only authentication, a console command runs with every permission, and the password travels in the clear. *Never open the port to the internet.* Keep `bind` on `127.0.0.1` and, from another machine, use an SSH tunnel: `ssh -L 32890:127.0.0.1:32890 host`.
]

=== The wire

Text, one line at a time (1000 bytes at most).

+ Connect and send `auth <password>`. The answer is `ok`, or `denied` (after a second's pause) and the connection closes.
+ Send one command per line, with or without the leading `/` (`addbot 4 blue`, `/kick #3`, `/say hello`). The answer is what the command printed (up to 64 KB), followed by an EOT byte (`0x04`) and a newline. A command that prints nothing answers `(done, nothing printed)`.
+ The line `status` is the exception: the answer is the server's state as one JSON object (name, map, mode, profile, uptime, score, limits, and every player with id, team, bot, alive, hp, kills, deaths, assists, captures, score, ip and ping).

`/shutdown`, `/login` and `/logout` are refused. A quiet connection is closed after a minute. One client is served at a time; a line runs on the server's tick (5 s at most to answer). Every line is written to the admin audit (`actor=rcon`) and the event log (`EVT rcon`).

```sh
# a try, with netcat
(printf 'auth %s\nstatus\n' "$PW"; sleep 1) | nc 127.0.0.1 32890
```

== The admin audit

Every command, host command and remote console line is appended to `logs/admin.log` in the data folder (and to the event log as `EVT admin`). The rcon password is never written. `[audit] enabled = false` keeps only the event-log line; `[audit] file` moves it.

== Host commands (the bots menu)

A player hosting from ngspades-client changes bots, their difficulty and autofill from the pause menu. On protocol 1.0 this travels as `host <command> <arg1> <arg2>` GenericCommands, and the server pushes `host-status` and `host-bots` back. The contract (ids, layout, semantics) is in #link("https://github.com/ngspades/server/blob/main/docs/host-commands.md")[docs/host-commands.md]; an owner does not need it, the console commands above do the same.

== Conduct, anti-cheat and flood limits

Keep a public server healthy with the sections `[conduct]` (AFK kicks, team-grief points; staff and a listen host are exempt) and `[anticheat]` (counters and a rate-limited log, no ban; `kick_on_protocol_violation` for hard violations). The flood limits (per player: 400 packets a second, one chat line a second with a burst of 5, 20 commands a second) and the per-address connection guard (`[net] max_connections_per_ip`) are described in #ch("07-network")[Network].

== Match control from the keyboard

#table(
  columns: (auto, 1fr),
  stroke: none,
  [`/endmatch [1|2]`], [Ends the match now with the score as it is, or with that team winning; the summary runs, then the next map.],
  [`/extend <minutes>`], [Adds minutes to a match that has a time limit.],
  [`/nextmap`, `/restart`, `/reset`], [Load the next map, reload this one, or reset (the same family).],
  [`/map <Name>`], [Load a map now; downloads one that is not installed.],
  [`/classes on|off`], [Turn the classes on or off for modes that have the option.],
  [`/sky time ...`, `/night on|off`], [The time of day and the weather (1.0), or day and night for 0.75 clients.],
  [`/perk <name> [player]`, `/crate <ammo|health> [player]`], [Grant a perk, or drop a supply crate: for tests and rewards.],
)
