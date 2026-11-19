# Hosting a server

This guide is for someone renting a machine to run a public server. The steps
below are automated by `tools/install_server.sh`.

## Quick start with the install script

```
curl -fsSL https://raw.githubusercontent.com/ngspades/server/main/tools/install_server.sh -o install_server.sh && bash install_server.sh
```

The repository is private today, so until it is public run the script from a
checkout and use `--binary <path>` or `--from-source`.

The script runs seven steps:

1. Check this machine (curl, tar, gzip).
2. Get the server (download the newest `ngspades-server-v*` release, or use a
   binary or source build).
3. Maps (the maps download themselves at the first start).
4. Your server's settings (name, mode, bots, port, passwords, public listing).
5. Test start (a short run, not listed on the public list).
6. Run it as a service (systemd on Linux).
7. Done.

It can be rerun: a state file (`.install-state`) resumes where it stopped, and
`--reset` redoes all of it.

Non-interactive example:

```
tools/install_server.sh --yes --name "My Server" --mode ngctf --bots 8 --port 32887 --admin-password 'your-password-here'
```

It refuses any password starting with `changeme`, fills the other four roles
(manager, moderator, guard, trusted) with random ones in `passwords.txt`, and
its test start runs with `SP_MASTER=0` so the server never shows on the
public list.

## Pick a host with network DDoS protection

A volume attack (gigabits of UDP) fills the machine's link before the program
sees a packet, so no setting of the server can stop it: only the network in
front of it can. Pick a provider that filters attacks in its network at no
extra cost:

- OVHcloud: every server sits behind its network anti-DDoS, and its game
  dedicated servers add "Anti-DDoS Game", a filter made for UDP game traffic.
- Hetzner: DDoS protection is included on every server.
- Others: Path.net-based hosts, and Cloudflare Spectrum (TCP/UDP, paid).

A home connection works for friends, but it exposes the home address.

The server needs very little: 128 players measured at 50-80 MB of memory and
~5% of one core, so the smallest VPS with 1 vCPU and 1 GB is enough. Prefer a
location near the players (latency). IPv4 is the default; IPv6 works too, since
the server listens on both.

## Ports

| Port | Protocol | What | Open to the internet? |
| --- | --- | --- | --- |
| 32887 | UDP | the game (`[server] port`) | yes |
| 32888 | UDP | players in a web browser, WebRTC (`[webrtc] port`, default the game's + 1): only with `[webrtc] enabled = true` | yes, when it is on |
| 32897, 32898 | UDP | the same two for the classic 0.75 profile (`--profile 075`, `config-075.toml`), only if you run it | yes |
| 32890 | TCP | the remote console (`[rcon]`, off by default, `bind = "127.0.0.1"`) | NO, never open it; it stays on the machine (the 0.75 profile's is 32900). Reach it through an SSH tunnel (`ssh -L 32890:127.0.0.1:32890 host`), see [docs/rcon.md](rcon.md) |
| (none) | TCP | no TCP port is needed for players | n/a |

Outgoing: https to GitHub (the maps download) and to the master server
(`[master] url`).

A changed `port` moves the WebRTC port with it unless `[webrtc] port` is
set. Browsers need `[webrtc] enabled = true` (default false): a match hosted
from the game with a join code turns it on by itself, and a dedicated server
that wants browser players sets it, opening the game's port + 1. A server
that leaves it off takes only native clients.

Firewall examples:

```
ufw allow 32887/udp
ufw allow 32888/udp   # only with [webrtc] enabled (browser players)
```

```
firewall-cmd --permanent --add-port=32887/udp
firewall-cmd --permanent --add-port=32888/udp   # only with [webrtc] enabled
firewall-cmd --reload
```

The provider's own firewall panel (OVH, Hetzner Cloud firewall) must also allow
these ports.

## What the server already limits

The server cannot stop a volume attack, but it keeps a single abusive client or
a small flood from hurting the match.

Joins: a player from the internet needs a connect token from the master (the
game asks for it by itself, one per join), so a flood of joins goes through the
master's per address limit (30 a minute). LAN players, players on this machine,
browsers and every join while the server has not reached the master for two
minutes need none. `[master] connect_tokens = false` turns it off (a server not
on the list has it off anyway).

Per player (token buckets: a rate per second and a burst; a client over it has
the excess dropped, is warned on the first drop and kicked when it keeps going;
bots are exempt, and staff are exempt from the chat and command limits):

| what | rate | burst | kicked after (drops in 10 s) |
| --- | --- | --- | --- |
| packets | 400/s | 800 | 2000 |
| chat lines | 1/s | 5 | 15 |
| commands (GenericCommand) | 20/s | 40 | 200 |

Per address: `[net] max_connections_per_ip` (default 4) connections within
`connection_window_seconds` (default 10); 0 turns the guard off; `SP_MAX_PER_IP`
overrides it. Keep 4 or more when several players share one address (a
household, a LAN party).

Joins: a 1.0 handshake must finish in 5 s and 10 s per step; connections not
yet in the game are capped at a quarter of `max_players`, and at that cap the
oldest unfinished one is pushed out, not the newcomer.

UDP queries (the server list's `HELLOINFO`): the client pads its query to 1200
bytes so the answer amplifies nothing; 5 answers a second per address (burst
10), the short unpadded query 0.5 a second (burst 3), 200 answers a second in
all; `HELLOLAN` answers the local network only.

Of these only the per-address connection guard and `max_players` are settings;
the per-player and query rates are fixed in the program
(`server/Source/Server/Flood.c`, `Ping.c`) and changed by editing them and
rebuilding.

Also the game's own rules that matter to a public host: `[conduct]` (AFK and
team griefing kicks) and `[anticheat]` (counters and logs,
`kick_on_protocol_violation`), both explained in `config.toml`.

## Admin and passwords

Five roles in `[passwords]` (manager, admin, moderator, guard, trusted);
`/login <role> <password>` in the game. The server warns at every start while
one is still `changeme*`. Bans are stored in `Bans.json`. See
[docs/rcon.md](rcon.md) for the remote console.

Every server ships `Resources/plugins/server_tools.lua`, which the shipped
`config.toml` loads: the admin role gets `/spawn` (a helicopter on dry
ground in front of you), `/spawn crate` (a random ammo or health supply
crate falling from the sky in front of you) and `/goto <x> <y>` (the middle
of the 16 block chunk x, y, 0 based). They need protocol 1.0. The host of a
listen server enters as admin, so they work in a local match without
`/login`.

## The public list

`[master] enabled` (default true) lists the server on our list; a browser
joins it only with `[webrtc] enabled = true`. `SP_MASTER=0` keeps a test
run off it. `master.key` in the
server's folder is its identity on the list: back it up and keep it secret.

## Docker instead

See README's "The server in Docker" section. The two compose commands are:

```
cd server
docker compose up -d
docker compose --profile classic up -d
```

The same ports apply (UDP 32887, plus 32888 with `[webrtc] enabled`; 32897
and 32898 for the classic profile). Each server's root is a volume (`/data`).

## Updating

Installed by the script: rerun it with `--reset` (it downloads the newest
`ngspades-server-v*` release and keeps your `config.toml`; answer "keep the current
settings"), then:

```
systemctl restart ngspades-server
```

From an archive: unpack the new one over the folder but keep
`Resources/config.toml`, `config-075.toml`, `Bans.json`, `master.key` and
`webrtc.pem`.

Docker, from the checkout's `server/` folder:

```
git pull
docker compose build
docker compose up -d
```

## Troubleshooting

- Players cannot join: check both firewalls (machine and provider) allow UDP.
- The logs: `journalctl -u ngspades-server -f`, or the console.
- A map that does not download: the machine needs https to
  `raw.githubusercontent.com`, or copy the map folder into `Resources/maps/`.
- `--check-config` reports unknown keys.