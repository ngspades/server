#import "/template.typ": *

== Choose how to get the server

#wide-table(
  (auto, 1fr, 1fr),
  table.header([*Way*], [*Good for*], [*What you need*]),
  [Release archive], [most owners], [Nothing installed: the Linux archives hold one static binary (x86_64 or arm64); macOS and Windows archives link only the system's libraries.],
  [Install script], [a rented Linux machine], [`curl`, `tar`, `gzip`; systemd for the service.],
  [Docker], [containers], [Docker and Compose; the image is about 16 MB, from `scratch`.],
  [Build from source], [developers, other platforms], [CMake 3.16+, a C11 compiler, json-c, OpenSSL, libcurl, zlib, readline; Lua 5.4 for plugins.],
)

#note(kind: "warning")[
  The repository publishes releases on tags named `ngspades-server-v*`. At the time of writing no release has been published yet, so until one is, use the Docker image or build from source (`tools/install_server.sh --from-source` does the build for you).
]

== A release archive

Archives are named `ngspades-server-<version>-<platform>.tar.gz` (`.zip` on Windows) with the platforms `linux-x86_64`, `linux-arm64`, `macos-x86_64`, `macos-arm64` and `windows-x86_64`. Unpack one anywhere. It holds:

```
ngspades-server/
  ngspades-server        the program (.exe on Windows)
  start.sh               starts the 1.0 server  (start.command on macOS, start.bat on Windows)
  start-classic.sh       starts the 0.75 server (--profile 075)
  README.txt             how to start it, the ports read from the configs
  LICENSE
  Resources/config.toml, config-075.toml
  Resources/maps/        empty: the server downloads the maps it plays
  Resources/plugins/     README.md and the server's own plugin, server_tools.lua
```

Start it with the script. On macOS the first start needs "Open Anyway" in System Settings > Privacy & Security, because the server is not signed by a known developer (the `.command` script clears the quarantine flag once it is allowed to run). On Windows, SmartScreen asks once ("More info", "Run anyway") and the firewall asks to allow the server: allow it on private and public networks if players come from the internet.

#note[
  `Resources/config.toml` enables a plugin called `server_tools` (`load = ["server_tools"]`), which the packaging scripts copy next to it. The file is not in the source repository (see the appendix): a server built from a checkout logs `Plugin server_tools: no Resources/plugins/server_tools.lua` and goes on without it.
]

== The install script

On a rented Linux machine the script does everything, in seven resumable steps:

```sh
curl -fsSL https://raw.githubusercontent.com/ngspades/server/main/tools/install_server.sh -o install_server.sh
bash install_server.sh
```

The steps: check the machine (curl, tar, gzip), get the server (the newest `ngspades-server-v*` release, or `--binary <path>`, or `--from-source`), maps (they download themselves at the first start), your settings (name, mode, bots, port, passwords, public listing), a test start (not listed on the public list: `SP_MASTER=0`), a service (systemd) and a summary. A state file, `<dir>/.install-state`, resumes a run that stopped; `--reset` redoes it.

Non-interactive:

```sh
tools/install_server.sh --yes --name "My Server" --mode ngctf --bots 8 \
  --port 32887 --admin-password 'a-long-password'
```

#fields((
  ("--yes", "flag", "Take the defaults; ask nothing."),
  ("--dir <dir>", "path", "Where to install."),
  ("--binary <path>", "path", "Use this program instead of downloading one."),
  ("--from-source", "flag", "Build the server from a checkout."),
  ("--version <tag>", "string", "A release tag, such as `ngspades-server-v0.3.0`."),
  ("--name, --mode, --bots, --port", "values", "The server's name, game mode, bot count and UDP port."),
  ("--admin-password, --rcon-password", "string", "Passwords; the script refuses any that starts with `changeme`, and fills the other four roles with random ones in `passwords.txt`."),
  ("--public, --private", "flag", "List the server on the public list or not."),
  ("--systemd, --no-systemd, --user <name>", "flag", "Install the service (and the user it runs as) or not."),
  ("--no-test", "flag", "Skip the test start."),
  ("--reset", "flag", "Redo every step."),
))

The admin password is written only into `config.toml` (mode 600), never into the state files.

== Docker

From the repository's `server/` folder:

```sh
docker compose up -d                       # the 1.0 server
docker compose --profile classic up -d     # plus the 0.75 server
docker compose logs -f                     # what it says
docker attach ngspades-server              # its console (detach with Ctrl+P, Ctrl+Q)
```

The image is the static Linux binary on `scratch`: no shell, user `1000`, root `/data` (a volume). The first start writes `Resources/config.toml` (`config-075.toml` for the classic one) into the volume, from the copy built into the binary, and downloads the maps it names. Edit the name and the five passwords there, then `docker compose restart`. Each container is capped at one core and 512 MB. The ports to open are UDP 32887 (1.0), 32897 (0.75), and with WebRTC the game's port plus one.

To build the image yourself, from the repository root with the submodules checked out:

```sh
git submodule update --init --recursive
docker build -f server/Dockerfile -t ngspades-server .
```

== Building from source

```sh
git clone --recursive https://github.com/ngspades/server.git
cd server
make server          # server/build/ngspades-server
make test            # the protocol's tests, the 0.75 extensions' tests, both configs
```

Libraries (the first configure also downloads and builds libdatachannel for WebRTC, a couple of minutes):

#wide-table(
  (auto, 1fr, 1fr),
  table.header([], [*Debian / Ubuntu*], [*macOS (Homebrew)*]),
  [json-c, OpenSSL, libcurl, zlib, readline], [`libjson-c-dev libssl-dev libcurl4-openssl-dev zlib1g-dev libreadline-dev`], [`json-c openssl@3` (curl, zlib and libedit come with the system)],
  [Lua 5.4 (plugins; optional)], [`liblua5.4-dev`], [`lua`],
)

#note[
  Without Lua the server builds and runs, but every plugin call is a no-op and no Lua game mode exists (`ENABLE_LUA` is off). If the plugins in this manual do nothing, check that the build found Lua 5.4.
]

Other make targets (`make help` lists them):

#fields((
  ("make server", "build", "Release build in `server/build/`."),
  ("make server-asan", "build", "AddressSanitizer + UBSan build in `server/build-asan/`."),
  ("make server-static", "build", "The release's static Linux binary, built in Alpine through Docker."),
  ("make test", "test", "The protocol's codec (fuzzed), the 0.75 extensions, both config files."),
  ("make smoke", "test", "Every game mode played by 16 bots under the sanitizers."),
  ("make probe-proto10", "test", "A protocol 1.0 client against the built server."),
  ("make run, make run-075", "run", "Start the server (1.0 or the 0.75 profile) with `SP_MASTER=0`."),
))

== First start

From the server's folder:

```sh
./ngspades-server                      # protocol 1.0, Resources/config.toml
./ngspades-server --profile 075        # the classic server, Resources/config-075.toml
./ngspades-server --help               # every option
```

The server finds `Resources/` by itself; `--root <dir>` points at another folder. A start from the wrong folder prints `Config file 'Resources/config.toml' not found` and tells you to start from the server's folder or give `--root`. With `--root` and no config in it, the server writes its profile's default config there (copied from the one built into the binary).

On start it prints the profile, the config path, the data folder, the port and the master, downloads the maps of the rotation it lacks, and ends with `Server started`. The console prompt `Console>` appears when a terminal is attached; run in the background (no TTY) it simply has no console, on purpose.

#note(kind: "warning")[
  Before opening the server to the internet change every password in `[passwords]`. The server warns at every start while one is still `changeme...`. A password that anyone can read lets anyone run `/kick`, `/ban` and `/map` (and with the manager's, `/shutdown`).
]

=== Checking the config

```sh
./ngspades-server --check-config                 # validates Resources/config.toml
./ngspades-server --profile 075 --check-config
```

It reports unknown keys and sections of the other profile, notes a missing map as "downloaded at start-up" while downloading is on, and exits.

== Ports and the firewall

#wide-table(
  (auto, auto, 1fr, auto),
  table.header([*Port*], [*Protocol*], [*What*], [*Open?*]),
  [32887], [UDP], [the game (`[server] port`)], [yes],
  [32888], [UDP], [players in a web browser, WebRTC (`[webrtc] port`, the game's + 1), only with `[webrtc] enabled = true`], [yes, when on],
  [32897, 32898], [UDP], [the same two for the 0.75 profile], [yes, if you run it],
  [32890], [TCP], [the remote console (`[rcon]`, off by default; 32900 for the 0.75 profile)], [*never*],
  [none], [TCP], [no TCP port is needed for players], [-],
)

Outgoing, the server needs https to `raw.githubusercontent.com` (the maps) and to the master server (`[master] url`).

```sh
ufw allow 32887/udp
ufw allow 32888/udp      # only with [webrtc] enabled
```

```sh
firewall-cmd --permanent --add-port=32887/udp
firewall-cmd --permanent --add-port=32888/udp
firewall-cmd --reload
```

The provider's own firewall panel (OVH, Hetzner Cloud firewall) must allow the same ports. A changed `port` moves the WebRTC port with it unless `[webrtc] port` is set.

== Folders the server uses

#fields((
  ("Resources/config.toml", "file", "The 1.0 profile's config. `config-075.toml` is the 0.75 profile's."),
  ("Resources/maps/<Name>/", "folder", "One folder per map: `<Name>.vxl` (or `.ngmap`), `<Name>.toml`, optionally the pyspades `<Name>.txt`. Downloads land here, or in the data folder's `maps/` when this one cannot be written."),
  ("Resources/plugins/", "folder", "The plugins' `.lua` files. `data/<name>.json` holds a plugin's `game.store` (in the data folder's `plugins-data/` when `--data` is given)."),
  ("Bans.json", "file", "The ban list, in the data folder."),
  ("master.key", "file", "The server's identity on the public list (an Ed25519 key). Back it up, keep it secret."),
  ("webrtc.pem", "file", "The DTLS certificate, made at the first start."),
  ("logs/", "folder", "`admin.log` (the admin audit), `crash.log`, and the event log when on."),
  ("ngmap/", "folder", "Cache of maps encoded from a VXL, keyed by the VXL's checksum."),
  ("maps/aliases.json", "file", "The maps repository's alias list, fetched again when older than a day."),
))

Keep `Resources/config.toml`, `config-075.toml`, `Bans.json`, `master.key` and `webrtc.pem` when you update (see #ch("08-operations")[Operations]).

== Running as a service

The install script writes a systemd unit. By hand:

```ini
[Unit]
Description=ngspades-server
After=network-online.target

[Service]
User=ngspades
WorkingDirectory=/opt/ngspades-server
ExecStart=/opt/ngspades-server/ngspades-server
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

then `systemctl enable --now ngspades-server` and `journalctl -u ngspades-server -f` for the log. As a service there is no TTY, so there is no console: use the remote console (see #ch("06-administration")[Administration]) or the in-game commands.
