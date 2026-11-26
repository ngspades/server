#import "/template.typ": *

== The ports, again

The game is one UDP port (`[server] port`, 32887 for 1.0, 32897 for the 0.75 profile). Browsers and join codes use a second UDP port (`[webrtc] port`, the game's + 1). The remote console is a TCP port that stays on the machine. The table with the firewall commands is in #ch("02-install")[Installing and first start].

The server listens on one IPv6 socket that also answers IPv4: a host or a player with IPv6 alone can play, and a machine without IPv6 falls back to IPv4 (`Listening on UDP port n over IPv4 and IPv6|IPv4 only`). An IPv4 peer arrives mapped (`::ffff:a.b.c.d`) and is unmapped before anything reads it; an IPv6 player is banned by its /64. The LAN broadcast stays IPv4.

== The public list (the master)

```toml
[master]
enabled = true
url = "https://master.shootanddestroy.obex.pw"
country = "BR"
key_file = "master.key"
connect_tokens = true
```

With `[master] enabled`, the server announces itself to our master server every 30 seconds over IPv4 and IPv6 and on every change (name, map, mode, players). The announce is signed with an Ed25519 key, `master.key` in the data folder, made at the first start: it is the server's identity on the list, so back it up and keep it secret. The master probes the server's UDP port to check it is reachable. `leave` is sent on shutdown.

- `url` empty: the server is not listed and no browser can find it.
- `SP_MASTER=0` keeps a run off the list; use it for every test server (tests, benchmarks and the install script's test start do).
- `SP_MASTER_URL` replaces the address and turns the list on; `SP_MASTER_KEY` points at another key.
- The 0.75 profile also announces to the public 0.75 master (`[master075]`, default `67.205.183.163:32886`, 32 players at most). A server in both lists is shown once, as ours.
- `[server] master` is a leftover switch: on a 1.0 server it only logs `Not announcing on the 0.75 master list`.

=== Connect tokens

A player joining from the internet over UDP needs a token from the master, which ngspades-client asks for by itself (one per join, tied to the server's key and the handshake). A flood of joins therefore goes through the master's per-address limit (30 a minute). LAN players, players on this machine, browsers, and every join while the server has not reached the master for two minutes need no token. `[master] connect_tokens = false` turns it off (a server not on the list has it off anyway); `SP_CONNECT_TOKENS=0|1|all` overrides it (`all` asks local addresses too).

== Join codes: playing with a friend

A match started from ngspades-client's menu with "Friends join by code" gets a fresh six-character code (`XXX - XXX`). The server announces it to the master (the row stays off the public list unless the host also ticks "List on the server list"), and a friend types the code in the server list's address box. No port forwarding is needed: the join goes over WebRTC through the master's signalling. A dedicated server can set one with `--join-code` or `SP_JOIN_CODE`, but join codes are meant for listen servers.

== Browsers: WebRTC

Browser players connect with WebRTC DataChannels, the same two channels and the same packets as native clients (`reliable` and `unreliable` map to ENet channels 0 and 1), on one UDP port shared by every peer.

```toml
[webrtc]
enabled = true
# port = 32888                    # default: the game's port + 1
stun = "stun:stun.l.google.com:19302"
```

- It is off by default on a dedicated server. A listen server with a join code turns it on by itself.
- Open the WebRTC UDP port in the firewall as for the game's.
- `stun` tells the server its public address behind a NAT; `""` for none (a server with a public address does not need one). `turn`, `turns`, `turn_username` and `turn_credential` give players behind strict NATs a relay (also `SP_WEBRTC_TURN*`).
- The DTLS certificate is `webrtc.pem` in the data folder, made at the first start.
- The offers come through the master: the browser posts to the master, the server polls for it, answers within 10 seconds.
- A leftover server on the WebRTC port makes every offer fail with `Failed to gather local ICE candidates`: check `lsof -iUDP:32888`.

== What the server limits by itself

The server cannot stop a volume attack (gigabits of UDP fill the machine's link before the program sees a packet), but it keeps one abusive client or a small flood from hurting the match.

#wide-table(
  (auto, auto, auto, auto),
  table.header([*Per player*], [*Rate*], [*Burst*], [*Kicked after (drops in 10 s)*]),
  [packets], [400 / s], [800], [2000],
  [chat lines], [1 / s], [5], [15],
  [commands (GenericCommand)], [20 / s], [40], [200],
)

A client over a limit has the excess dropped, is warned at the first drop and kicked when it keeps going. Bots are exempt; staff are exempt from the chat and command limits.

- *Per address*: `[net] max_connections_per_ip` (default 4) connections within `connection_window_seconds` (default 10); 0 turns the guard off; `SP_MAX_PER_IP` overrides it. Keep it at 4 or more when several players share an address (a household, a LAN party).
- *Joins*: a 1.0 handshake must finish within 5 s (initiation) and 10 s (certificate). Connections not yet in the game are capped at a quarter of `max_players`; at the cap the oldest unfinished one is pushed out, not the newcomer.
- *UDP queries* (the server list's `HELLOINFO`): ngspades-client pads its query to 1200 bytes so the answer amplifies nothing; 5 answers a second per address (burst 10), the short query 0.5 a second (burst 3), 200 answers a second in all. `HELLOLAN` answers the local network only.

Only the per-address guard and `max_players` are settings; the other rates are fixed in the program (`Flood.c`, `Ping.c`).

== Picking a host

#note[
  A volume attack can only be stopped by the network in front of the machine. Pick a provider that filters attacks in its network at no extra cost: OVHcloud (every server sits behind its anti-DDoS; the game dedicated servers add "Anti-DDoS Game", a filter made for UDP game traffic), Hetzner (DDoS protection is included on every server), Path.net based hosts, or Cloudflare Spectrum (TCP and UDP, paid). A home connection works for friends, but it exposes the home address.
]

The server needs very little: 128 players measured at 50-80 MB of memory and about 5% of one core, so the smallest VPS with 1 vCPU and 1 GB is enough. Prefer a location near your players (latency). The bandwidth to each player is about 26 KB/s with 127 bots on a large map; a full server's upload is what that figure times the players.

== Network behaviour worth knowing

- *Fog*: `[server] fog_distance` (32 to 256, 192 shipped) is how far players are seen; enemies are sent only within it plus 8 blocks, so a wallhack cannot exist. 192 against 128 costs about 12.6 against 8.0 KB/s to a client on a large map with 127 bots.
- *Lag compensation* (`[net] lag_compensation`): a hit is judged against the body the shooter could see, rewound by its round trip within `lag_compensation_max_ms` plus `lag_compensation_extra_ms`; death, respawn and teleports are never rewound.
- *ENet*: the server uses enet6 with the packet throttle's deceleration set to 0 and a 15 s minimum, 40 s maximum timeout, so a phone that stalls briefly is not dropped. Encryption is never turned on (it would change the wire and 0.75 clients would not connect).
- *Map transfer*: the stored `ngmap` goes out as it is, paced by what the connection already holds (up to 64 fragments a tick below 64 KiB in flight). A client that has the exact stream answers `map-cached 1` and receives 0 bytes.
