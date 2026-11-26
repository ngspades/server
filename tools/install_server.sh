#!/usr/bin/env bash
# Install the ngspades dedicated server on this machine.
#
# Seven numbered steps, each recorded in <dir>/.install-state so a failed run
# can be resumed by rerunning the same command. The admin password is written
# only into config.toml (chmod 600) and never into the state files.
#
# Usage:
#   tools/install_server.sh                       # interactive
#   tools/install_server.sh --yes --dir /opt/ngspades-server
#   tools/install_server.sh --binary ./server/build/ngspades-server
#   tools/install_server.sh --from-source
#   tools/install_server.sh --version ngspades-server-v0.3.0
#
# The release download uses the public GitHub URL pattern:
#   https://github.com/<repo>/releases/...  (asset *-<platform>.tar.gz)
# The repository is private today, so anonymous downloads fail until it is
# public; --binary <path> or --from-source are the ways meanwhile.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
GAME_CMAKE="$ROOT/common/game.cmake"
if [ -f "$GAME_CMAKE" ]; then
	GAME_NAME=$(sed -n 's/^set(GAME_SERVER_TITLE "\(.*\)")/\1/p' "$GAME_CMAKE")
	SERVER=$(sed -n 's/^set(GAME_SERVER_NAME "\(.*\)")/\1/p' "$GAME_CMAKE")
fi
: "${GAME_NAME:=ngspades-server}"
: "${GAME_SLUG:=ngspades}"
: "${SERVER:=ngspades-server}"
REPO="ngspades/server"
GUIDE_URL="https://github.com/$REPO/blob/main/docs/hosting.md"

# ---- option defaults -------------------------------------------------------
YES=0
CURRENT_STEP=""
DIR=""
BINARY=""
FROM_SOURCE=0
VERSION=""
NAME=""
MODE=""
BOTS=""
PORT=""
ADMIN_PASSWORD=""
RCON_PASSWORD=""
PUBLIC=""
SYSTEMD=""
USER_NAME="$GAME_SLUG"
NO_TEST=0
RESET=0
RCON_PROVIDED=0
USER_PROVIDED=0

usage() {
	cat <<EOF
Usage: $0 [options]

Options:
  --yes, -y              non-interactive: never ask, take flag or default
  --dir <path>           install folder (default /opt/$SERVER as root, else \$HOME/$SERVER)
  --binary <path>        use this server binary instead of downloading
  --from-source          build from source instead of downloading
  --version <tag>        a ngspades-server-v* tag to download instead of the latest
  --name <text>          server name
  --mode <ctf|ngctf|babel|arena|zombie>
  --bots <0..64>         bots on each team
  --port <n>             game port
  --admin-password <pw>  admin password (>= 8 chars)
  --rcon-password <pw>      remote console password (empty = off)
  --public / --private   listed on the public server list (default public)
  --systemd / --no-systemd  (Linux only)
  --user <name>          service user (default $GAME_SLUG)
  --no-test              skip the test start
  --reset                forget state and redo every step
  -h, --help             show this help
EOF
}

need_value() {
	if [ "$2" -lt 2 ]; then
		printf 'error: %s needs a value\n' "$1" >&2
		usage >&2
		exit 2
	fi
}

while [ $# -gt 0 ]; do
	case "$1" in
		--yes|-y) YES=1; shift ;;
		--dir) need_value "$1" "$#"; DIR=$2; shift 2 ;;
		--binary) need_value "$1" "$#"; if [ -f "$2" ]; then BINARY=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); else BINARY=$2; fi; shift 2 ;;
		--from-source) FROM_SOURCE=1; shift ;;
		--version) need_value "$1" "$#"; VERSION=$2; shift 2 ;;
		--name) need_value "$1" "$#"; NAME=$2; shift 2 ;;
		--mode) need_value "$1" "$#"; MODE=$2; shift 2 ;;
		--bots) need_value "$1" "$#"; BOTS=$2; shift 2 ;;
		--port) need_value "$1" "$#"; PORT=$2; shift 2 ;;
		--admin-password) need_value "$1" "$#"; ADMIN_PASSWORD=$2; shift 2 ;;
		--rcon-password) need_value "$1" "$#"; RCON_PASSWORD=$2; RCON_PROVIDED=1; shift 2 ;;
		--public) PUBLIC=1; shift ;;
		--private) PUBLIC=0; shift ;;
		--systemd) SYSTEMD=1; shift ;;
		--no-systemd) SYSTEMD=0; shift ;;
		--user) need_value "$1" "$#"; USER_NAME=$2; USER_PROVIDED=1; shift 2 ;;
		--no-test) NO_TEST=1; shift ;;
		--reset) RESET=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
done

# ---- helpers ---------------------------------------------------------------
die() {
	printf 'error: %s\n' "$*" >&2
	if [ -n "${CURRENT_STEP:-}" ]; then
		printf 'error: step "%s" failed; rerun this script to resume there.\n' "$CURRENT_STEP" >&2
	fi
	exit 1
}

step() {
	# step <n> <id> <title> ; returns 1 when already done
	local n=$1 id=$2 title=$3
	CURRENT_STEP="$title"
	if [ -t 1 ]; then
		printf '\033[1m==> [%d/7] %s\033[0m\n' "$n" "$title"
	else
		printf '==> [%d/7] %s\n' "$n" "$title"
	fi
	if [ -f "$STATE_FILE" ] && grep -qx -F -- "$id" "$STATE_FILE" 2>/dev/null; then
		printf '    already done, skipping (--reset to redo)\n'
		return 1
	fi
	return 0
}

step_done() { printf '%s\n' "$1" >> "$STATE_FILE"; }

ask() {
	# ask <var> <prompt> <default>
	local __var=$1 __prompt=$2 __default=$3 __input
	if [ -n "$__default" ]; then
		(printf '%s [%s]: ' "$__prompt" "$__default" >/dev/tty) 2>/dev/null || true
	else
		(printf '%s: ' "$__prompt" >/dev/tty) 2>/dev/null || true
	fi
	read -r __input </dev/tty || __input=""
	if [ -z "$__input" ]; then __input=$__default; fi
	printf -v "$__var" '%s' "$__input"
}

validate_port() {
	case "$1" in ''|*[!0-9]*) return 1 ;; esac
	[ "$1" -ge 1024 ] && [ "$1" -le 65534 ]
}

validate_int() {
	case "$1" in ''|*[!0-9]*) return 1 ;; esac
	return 0
}

gen_pw() {
	local pw
	pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 20) || true
	printf '%s' "$pw"
}

# ---- dir default -----------------------------------------------------------
if [ -z "$DIR" ]; then
	if [ "$(id -u)" -eq 0 ]; then DIR="/opt/$SERVER"; else DIR="$HOME/$SERVER"; fi
fi
mkdir -p "$DIR"
STATE_FILE="$DIR/.install-state"
ANSWERS_FILE="$DIR/.install-answers"

if [ "$RESET" -eq 1 ]; then
	rm -f "$STATE_FILE" "$ANSWERS_FILE"
fi

# read answers back (never source)
if [ -f "$ANSWERS_FILE" ]; then
	while IFS='=' read -r k v; do
		case "$k" in
			name) [ -z "$NAME" ] && NAME=$v ;;
			mode) [ -z "$MODE" ] && MODE=$v ;;
			bots) [ -z "$BOTS" ] && BOTS=$v ;;
			port) [ -z "$PORT" ] && PORT=$v ;;
			public) [ -z "$PUBLIC" ] && PUBLIC=$v ;;
			user) [ "$USER_PROVIDED" -eq 0 ] && USER_NAME=$v ;;
		esac
	done < "$ANSWERS_FILE"
fi

# ---- detect OS/arch (needed by later steps even on resume) -----------------
OS=$(uname -s)
case "$OS" in
	Linux|Darwin) ;;
	*) die "unsupported OS: $OS" ;;
esac
ARCH=$(uname -m)
case "$ARCH" in
	x86_64) ARCH=x86_64 ;;
	aarch64|arm64) ARCH=arm64 ;;
	*) die "unsupported architecture: $ARCH" ;;
esac
if [ "$OS" = "Linux" ]; then
	PLATFORM="linux-$ARCH"
	# read, never sourced: os-release sets NAME and VERSION, which are this
	# script's --name and --version
	os_field() { sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}$/\1/p" /etc/os-release 2>/dev/null | head -1; }
	DISTRO=$(os_field ID)
	ID_LIKE_VAL=$(os_field ID_LIKE)
else
	PLATFORM="macos-$ARCH"
	DISTRO=""
	ID_LIKE_VAL=""
fi

: "${USER_NAME:=$GAME_SLUG}"

# ---- step 1: check ---------------------------------------------------------
if step 1 check "Check this machine"; then
	missing=0
	for c in curl tar gzip; do
		command -v "$c" >/dev/null 2>&1 || missing=1
	done
	if [ "$missing" -eq 1 ]; then
		if [ "$(id -u)" -eq 0 ] && [ "$OS" = "Linux" ]; then
			if printf '%s %s' "$DISTRO" "$ID_LIKE_VAL" | grep -qE 'debian|ubuntu'; then
				apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq curl ca-certificates tar gzip
			elif printf '%s %s' "$DISTRO" "$ID_LIKE_VAL" | grep -qE 'fedora|rhel'; then
				dnf install -y curl ca-certificates tar gzip
			else
				die "install curl, tar and gzip manually, then rerun"
			fi
		else
			die "missing curl/tar/gzip; install them (or run as root on Linux)"
		fi
	fi
	step_done check
fi

# ---- step 2: get -----------------------------------------------------------
if step 2 get "Get the server"; then
	if [ -n "$BINARY" ]; then
		[ -f "$BINARY" ] || die "binary not found: $BINARY"
		mkdir -p "$DIR/Resources/maps" "$DIR/Resources/plugins"
		cp "$BINARY" "$DIR/$SERVER"
		chmod 755 "$DIR/$SERVER"
		[ -f "$ROOT/server/Resources/plugins/server_tools.lua" ] \
			&& cp "$ROOT/server/Resources/plugins/server_tools.lua" "$DIR/Resources/plugins/"
		true
	elif [ "$FROM_SOURCE" -eq 1 ]; then
		if [ -f "$ROOT/server/CMakeLists.txt" ]; then
			CHECKOUT=$ROOT
		else
			CHECKOUT="$DIR/src"
			if [ ! -d "$CHECKOUT" ]; then
				git clone --recursive "https://github.com/$REPO.git" "$CHECKOUT"
			fi
		fi
		printf 'Building from source (the first configure fetches and builds libdatachannel; a few minutes)...\n'
		if [ "$(id -u)" -eq 0 ] && [ "$OS" = "Linux" ]; then
			if printf '%s %s' "$DISTRO" "$ID_LIKE_VAL" | grep -qE 'debian|ubuntu'; then
				apt-get update -qq
				DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential cmake git pkg-config libssl-dev libjson-c-dev liblua5.4-dev libreadline-dev libcurl4-openssl-dev zlib1g-dev
			elif printf '%s %s' "$ID" "${ID_LIKE:-}" | grep -qE 'fedora|rhel'; then
				dnf install -y gcc gcc-c++ make cmake git pkgconf openssl-devel json-c-devel lua-devel readline-devel libcurl-devel zlib-devel
			fi
		fi
		if [ "$OS" = "Darwin" ]; then
			command -v brew >/dev/null 2>&1 || die "Homebrew is required on macOS (--from-source)"
			brew install cmake pkg-config json-c openssl@3 lua
		fi
		git -C "$CHECKOUT" submodule update --init --recursive
		if command -v nproc >/dev/null 2>&1; then CORES=$(nproc); else CORES=$(sysctl -n hw.ncpu); fi
		cmake -S "$CHECKOUT/server" -B "$CHECKOUT/server/build" -DCMAKE_BUILD_TYPE=Release
		cmake --build "$CHECKOUT/server/build" -j "$CORES"
		mkdir -p "$DIR/Resources/maps" "$DIR/Resources/plugins"
		cp "$CHECKOUT/server/build/$SERVER" "$DIR/$SERVER"
		chmod 755 "$DIR/$SERVER"
		cp "$CHECKOUT/server/Resources/config.toml" "$CHECKOUT/server/Resources/config-075.toml" "$DIR/Resources/"
		cp "$CHECKOUT/server/Resources/plugins/server_tools.lua" "$DIR/Resources/plugins/"
	else
		# download
		releases=$(curl -fsSL "https://api.github.com/repos/$REPO/releases?per_page=100") || die "could not fetch the release list (the repository may still be private; use --binary or --from-source)"
		if [ -n "$VERSION" ]; then
			target_tag=$VERSION
		else
			target_tag=$(printf '%s' "$releases" | grep -o '"tag_name": *"[^"]*"' | sed 's/.*: *"//;s/"$//' | grep '^ngspades-server-v' | head -1) || true
		fi
		[ -n "$target_tag" ] || die "no ngspades-server-v* release found (the repository may still be private; use --binary or --from-source)"
		asset_url=$(printf '%s' "$releases" | awk -v tag="$target_tag" -v plat="$PLATFORM" '
			/"tag_name"/ { current=$0; gsub(/.*"tag_name"[[:space:]]*:[[:space:]]*"/, "", current); gsub(/".*/, "", current) }
			/"browser_download_url"/ {
				url=$0; gsub(/.*"browser_download_url"[[:space:]]*:[[:space:]]*"/, "", url); gsub(/".*/, "", url)
				if (current == tag && url ~ ("-" plat "\\.tar\\.gz$")) print url
			}
		' | head -1) || true
		[ -n "$asset_url" ] || die "no asset for $PLATFORM in $target_tag (the repository may still be private; use --binary or --from-source)"
		tmpdir=$(mktemp -d)
		trap 'rm -rf "$tmpdir"' EXIT
		curl -fsSL "$asset_url" -o "$tmpdir/server.tar.gz" || die "download failed (the repository may still be private; use --binary or --from-source)"
		tar -xzf "$tmpdir/server.tar.gz" -C "$tmpdir"
		[ -d "$tmpdir/$SERVER" ] || die "archive layout unexpected"
		mkdir -p "$DIR"
		for item in "$tmpdir/$SERVER"/*; do
			b=$(basename "$item")
			if [ "$b" = "Resources" ]; then
				mkdir -p "$DIR/Resources"
				for r in "$item"/*; do
					rb=$(basename "$r")
					if { [ "$rb" = "config.toml" ] || [ "$rb" = "config-075.toml" ]; } && [ -f "$DIR/Resources/$rb" ]; then
						continue
					fi
					cp -r "$r" "$DIR/Resources/"
				done
			else
				cp -r "$item" "$DIR/"
			fi
		done
		rm -rf "$tmpdir"
		trap - EXIT
	fi

	# check it runs
	if ! "$DIR/$SERVER" --help >/dev/null 2>&1; then
		die "the server binary does not run on this machine"
	fi
	step_done get
fi

# ---- step 3: maps ----------------------------------------------------------
if step 3 maps "Maps"; then
	printf 'The server downloads the maps of its rotation itself at its first start,\n'
	printf 'from https://github.com/ngspades/maps, into %s/Resources/maps\n' "$DIR"
	printf '(so step 5 needs the internet).\n'
	mkdir -p "$DIR/Resources/maps" || die "cannot create maps folder"
	[ -w "$DIR/Resources/maps" ] || die "maps folder not writable: $DIR/Resources/maps"
	step_done maps
fi

# ---- step 4: config --------------------------------------------------------
if step 4 config "Your server's settings"; then
	# keep-settings check
	keep=0
	if [ -f "$DIR/Resources/config.toml" ] && ! grep -qi 'changeme' "$DIR/Resources/config.toml"; then
		if [ "$YES" -eq 1 ]; then
			keep=1
		else
			keepans=""
			ask keepans "Keep the current settings? (y/n)" "y"
			case "$keepans" in [Yy]*) keep=1 ;; esac
		fi
	fi

	if [ "$keep" -eq 1 ]; then
		# only refresh run.sh
		: "${BOTS:=8}"
		cat > "$DIR/run.sh" <<EOF
#!/bin/sh
cd "\$(dirname "\$0")" || exit 1
exec ./$SERVER --root "\$PWD" --team-bots $BOTS $BOTS "\$@"
EOF
		chmod 755 "$DIR/run.sh"
		{
			printf 'name=%s\n' "${NAME:-}"
			printf 'mode=%s\n' "${MODE:-}"
			printf 'bots=%s\n' "$BOTS"
			printf 'port=%s\n' "${PORT:-}"
			printf 'public=%s\n' "${PUBLIC:-1}"
			printf 'rcon_password_set=%s\n' "$([ -n "$RCON_PASSWORD" ] && echo yes || echo no)"
			printf 'user=%s\n' "$USER_NAME"
		} > "$ANSWERS_FILE"
		step_done config
	else
		# acquire values
		if [ -z "$NAME" ]; then
			if [ "$YES" -eq 1 ]; then NAME="$GAME_NAME"; else ask NAME "Server name" "$GAME_NAME"; fi
		fi
		if [ -z "$MODE" ]; then
			if [ "$YES" -eq 1 ]; then MODE=ngctf; else ask MODE "Gamemode (ctf|ngctf|babel|arena|zombie)" "ngctf"; fi
		fi
		if [ -z "$BOTS" ]; then
			if [ "$YES" -eq 1 ]; then BOTS=8; else ask BOTS "Bots per team" "8"; fi
		fi
		if [ -z "$PORT" ]; then
			if [ "$YES" -eq 1 ]; then PORT=32887; else ask PORT "Port" "32887"; fi
		fi
		if [ -z "$PUBLIC" ]; then
			if [ "$YES" -eq 1 ]; then PUBLIC=1; else
				pubans=""
				ask pubans "List on the public server list? (y/n)" "y"
				case "$pubans" in [Nn]*) PUBLIC=0 ;; *) PUBLIC=1 ;; esac
			fi
		fi
		if [ -z "$ADMIN_PASSWORD" ]; then
			if [ "$YES" -eq 1 ]; then die "admin password required (--admin-password) in --yes mode"; fi
			while :; do
				read -rs -p "Admin password: " _pw1 </dev/tty; (printf '\n' >/dev/tty) 2>/dev/null || true
				read -rs -p "Repeat: " _pw2 </dev/tty; (printf '\n' >/dev/tty) 2>/dev/null || true
				if [ "$_pw1" != "$_pw2" ]; then (printf 'passwords do not match, try again\n' >/dev/tty) 2>/dev/null || true; continue; fi
				ADMIN_PASSWORD=$_pw1
				break
			done
		fi
		if [ "$RCON_PROVIDED" -eq 0 ] && [ "$YES" -eq 0 ]; then
			ask RCON_PASSWORD "Remote console password (empty = off)" ""
		fi

		# validate
		[ "${#NAME}" -ge 1 ] && [ "${#NAME}" -le 60 ] || die "name must be 1..60 chars"
		case "$NAME" in *\"*|*\\*|*$'\n'*) die "name must not contain quote, backslash or newline" ;; esac
		case "$MODE" in ctf|ngctf|babel|arena|zombie) ;; *) die "invalid mode: $MODE" ;; esac
		validate_int "$BOTS" || die "bots must be an integer"
		[ "$BOTS" -ge 0 ] && [ "$BOTS" -le 64 ] || die "bots must be 0..64"
		validate_port "$PORT" || die "port must be an integer 1024..65534"
		if [ -n "$RCON_PASSWORD" ]; then
			[ "${#RCON_PASSWORD}" -ge 16 ] || die "remote console password must be >= 16 chars"
			printf '%s' "$RCON_PASSWORD" | grep -qE '^[A-Za-z0-9._-]+$' || die "remote console password must match [A-Za-z0-9._-]"
		fi
		[ "${#ADMIN_PASSWORD}" -ge 8 ] || die "admin password must be >= 8 chars"
		case "$ADMIN_PASSWORD" in *\"*|*\\*|*\ *) die "admin password must not contain quote, backslash or space" ;; esac
		_lp=$(printf '%s' "$ADMIN_PASSWORD" | tr '[:upper:]' '[:lower:]')
		case "$_lp" in changeme*) die "admin password refused: every copy of the server ships changeme passwords; pick your own" ;; esac

		case "$MODE" in
			ctf) GAMEMODE_ID=0 ;;
			ngctf) GAMEMODE_ID=6 ;;
			babel) GAMEMODE_ID=2 ;;
			arena) GAMEMODE_ID=3 ;;
			zombie) GAMEMODE_ID=4 ;;
		esac

		case "$MODE" in
			ngctf) MAPS_BLOCK='maps = [
    "Raindance",
    "Busan_Port",
]' ;;
			ctf) MAPS_BLOCK='maps = [
    "2Fort_CTF",
    "Raindance",
]' ;;
			babel) MAPS_BLOCK='maps = [
    "Nine-Eye_Babel",
    "babelstairs",
]' ;;
			arena) MAPS_BLOCK='maps = [
    "de_dust",
    "De_Dust2_342",
    "csitaly",
]' ;;
			zombie) MAPS_BLOCK='maps = [
    "Beaulieu_Abbey_Zombie",
    "Zombie_Island",
]' ;;
		esac

		# generate default config if missing
		if [ ! -f "$DIR/Resources/config.toml" ]; then
			(cd "$DIR" && ./"$SERVER" --root "$DIR" --check-config) >/dev/null 2>&1 || true
			[ -f "$DIR/Resources/config.toml" ] || die "config.toml could not be created"
		fi

		# keep an original copy the first time
		if [ ! -f "$DIR/Resources/config.toml.orig" ]; then
			cp "$DIR/Resources/config.toml" "$DIR/Resources/config.toml.orig"
		fi

		PW_MANAGER=$(gen_pw)
		PW_MODERATOR=$(gen_pw)
		PW_GUARD=$(gen_pw)
		PW_TRUSTED=$(gen_pw)

		tmpconfig=$(mktemp)
		MAPS_BLOCK="$MAPS_BLOCK" awk -v name="$NAME" -v port="$PORT" -v gm="$GAMEMODE_ID" \
			-v pw_admin="$ADMIN_PASSWORD" -v pw_manager="$PW_MANAGER" \
			-v pw_moderator="$PW_MODERATOR" -v pw_guard="$PW_GUARD" \
			-v pw_trusted="$PW_TRUSTED" \
			-v public="$PUBLIC" -v rcon_password="$RCON_PASSWORD" '
			BEGIN { section=""; inmaps=0 }
			/^\[/ { inmaps=0; section=$0 }
			{
				if (section == "[server]") {
					if ($0 ~ /^name[[:space:]]*=/) { print "name = \"" name "\""; next }
					if ($0 ~ /^port[[:space:]]*=/) { print "port = " port; next }
					if ($0 ~ /^gamemode[[:space:]]*=/) { print "gamemode = " gm; next }
					if ($0 ~ /^maps[[:space:]]*=[[:space:]]*\[/) {
						print ENVIRON["MAPS_BLOCK"]
						inmaps=1
						next
					}
					if (inmaps) {
						if ($0 ~ /^[[:space:]]*\][[:space:]]*$/) { inmaps=0 }
						next
					}
				}
				if (section == "[passwords]") {
					if ($0 ~ /^manager[[:space:]]*=/) { print "manager   = \"" pw_manager "\""; next }
					if ($0 ~ /^admin[[:space:]]*=/) { print "admin     = \"" pw_admin "\""; next }
					if ($0 ~ /^moderator[[:space:]]*=/) { print "moderator = \"" pw_moderator "\""; next }
					if ($0 ~ /^guard[[:space:]]*=/) { print "guard     = \"" pw_guard "\""; next }
					if ($0 ~ /^trusted[[:space:]]*=/) { print "trusted   = \"" pw_trusted "\""; next }
				}
				if (section == "[master]") {
					if ($0 ~ /^enabled[[:space:]]*=/) { print "enabled = " (public==1 ? "true" : "false"); next }
				}
				if (section == "[rcon]") {
					if ($0 ~ /^enabled[[:space:]]*=/) {
						if (rcon_password == "") print "enabled = false"; else print "enabled = true"
						next
					}
					if ($0 ~ /^password[[:space:]]*=/) {
						if (rcon_password == "") print "password = \"\""; else print "password = \"" rcon_password "\""
						next
					}
				}
				print
			}
		' "$DIR/Resources/config.toml" > "$tmpconfig"
		mv "$tmpconfig" "$DIR/Resources/config.toml"

		{
			printf 'manager: %s\n' "$PW_MANAGER"
			printf 'admin: %s\n' "$ADMIN_PASSWORD"
			printf 'moderator: %s\n' "$PW_MODERATOR"
			printf 'guard: %s\n' "$PW_GUARD"
			printf 'trusted: %s\n' "$PW_TRUSTED"
		} > "$DIR/passwords.txt"
		chmod 600 "$DIR/passwords.txt"
		printf 'Wrote passwords to %s (chmod 600)\n' "$DIR/passwords.txt"

		chmod 600 "$DIR/Resources/config.toml"

		if ! (cd "$DIR" && ./"$SERVER" --root "$DIR" --check-config); then
			die "config check failed"
		fi

		cat > "$DIR/run.sh" <<EOF
#!/bin/sh
cd "\$(dirname "\$0")" || exit 1
exec ./$SERVER --root "\$PWD" --team-bots $BOTS $BOTS "\$@"
EOF
		chmod 755 "$DIR/run.sh"

		{
			printf 'name=%s\n' "$NAME"
			printf 'mode=%s\n' "$MODE"
			printf 'bots=%s\n' "$BOTS"
			printf 'port=%s\n' "$PORT"
			printf 'public=%s\n' "$PUBLIC"
			printf 'rcon_password_set=%s\n' "$([ -n "$RCON_PASSWORD" ] && echo yes || echo no)"
			printf 'user=%s\n' "$USER_NAME"
		} > "$ANSWERS_FILE"

		step_done config
	fi
fi

# ---- read back kept settings (port/bots) if missing -----------------------
if [ -z "${PORT:-}" ] && [ -f "$DIR/Resources/config.toml" ]; then
	PORT=$(sed -n '/^\[server\]/,/^\[/s/^port *= *\([0-9]*\).*/\1/p' "$DIR/Resources/config.toml" | head -1)
fi
: "${BOTS:=8}"

# ---- step 5: test ----------------------------------------------------------
if [ "$NO_TEST" -eq 1 ]; then
	if step 5 test "Test start (not listed)"; then
		printf 'skipped (--no-test)\n'
		step_done test
	fi
else
	if step 5 test "Test start (not listed)"; then
		SP_MASTER=0 "$DIR/run.sh" </dev/null >"$DIR/install-test.log" 2>&1 &
		server_pid=$!
		started=0
		exited_early=0
		for i in $(seq 1 180); do
			if ! kill -0 "$server_pid" 2>/dev/null; then
				exited_early=1
				break
			fi
			if grep -q "Server started" "$DIR/install-test.log" 2>/dev/null; then
				started=1
				break
			fi
			if [ $((i % 5)) -eq 0 ]; then (printf '.' >/dev/tty) 2>/dev/null || true; fi
			sleep 1
		done
		(printf '\n' >/dev/tty) 2>/dev/null || true

		if [ "$exited_early" -eq 1 ] && grep -qiE 'in use|taken|address already' "$DIR/install-test.log" 2>/dev/null; then
			printf 'something already listens on port %s; skipping test\n' "$PORT"
			kill "$server_pid" 2>/dev/null || true
			wait "$server_pid" 2>/dev/null || true
			rm -f "$DIR/install-test.log"
			step_done test
		elif [ "$started" -ne 1 ]; then
			printf '%s\n' "server did not start; last 20 lines:" >&2
			tail -20 "$DIR/install-test.log" >&2
			kill "$server_pid" 2>/dev/null || true
			wait "$server_pid" 2>/dev/null || true
			rm -f "$DIR/install-test.log"
			die "test start failed"
		else
			reply=""
			set +e
			# one read() of the whole datagram: bash's read takes a byte at a
			# time, and a UDP socket drops the rest of a datagram on a short read
			reply_file=$(mktemp)
			exec 3<>/dev/udp/127.0.0.1/"$PORT"
			printf 'HELLOINFO%1191s' '' >&3
			dd bs=65536 count=1 <&3 >"$reply_file" 2>/dev/null &
			dd_pid=$!
			for _ in 1 2 3 4 5 6 7 8 9 10; do
				kill -0 "$dd_pid" 2>/dev/null || break
				sleep 0.5
			done
			kill "$dd_pid" 2>/dev/null
			wait "$dd_pid" 2>/dev/null
			reply=$(cat "$reply_file")
			rm -f "$reply_file"
			exec 3>&- 2>/dev/null
			set -e
			if printf '%s' "$reply" | grep -q '"name"'; then
				printf 'The server answered: %s\n' "$(printf '%s' "$reply" | cut -c1-120)"
			else
				printf 'warning: no answer from the server (port may be filtered)\n'
			fi
			kill "$server_pid" 2>/dev/null || true
			for i in $(seq 1 10); do
				kill -0 "$server_pid" 2>/dev/null || break
				sleep 1
			done
			kill -9 "$server_pid" 2>/dev/null || true
			wait "$server_pid" 2>/dev/null || true
			rm -f "$DIR/install-test.log"
			step_done test
		fi
	fi
fi

# ---- step 6: service -------------------------------------------------------
if step 6 service "Run it as a service"; then
	if [ "$OS" = "Darwin" ]; then
		printf 'On macOS, run %s/run.sh (or double-click start.command from the archive).\n' "$DIR"
		step_done service
	else
		if [ -z "$SYSTEMD" ]; then
			if [ "$YES" -eq 1 ]; then
				if [ "$(id -u)" -eq 0 ] && [ -d /run/systemd/system ]; then
					SYSTEMD=1
				else
					SYSTEMD=0
				fi
			else
				sdans=""
				ask sdans "Install as a systemd service? (y/n)" "y"
				case "$sdans" in [Nn]*) SYSTEMD=0 ;; *) SYSTEMD=1 ;; esac
			fi
		fi
		if [ "$SYSTEMD" -ne 1 ]; then
			printf 'skipping service install\n'
			step_done service
		elif [ "$(id -u)" -ne 0 ]; then
			printf 'Without root, install the service by hand:\n'
			printf '  useradd --system --home-dir %s --no-create-home --shell /usr/sbin/nologin %s\n' "$DIR" "$USER_NAME"
			printf '  chown -R %s: %s\n' "$USER_NAME" "$DIR"
			printf '  (write /etc/systemd/system/%s.service, then systemctl daemon-reload && systemctl enable --now %s)\n' "$SERVER" "$SERVER"
			step_done service
		else
			if ! id "$USER_NAME" >/dev/null 2>&1; then
				nologin=$(command -v nologin 2>/dev/null || true)
				[ -n "$nologin" ] && [ -x "$nologin" ] || nologin=/sbin/nologin
				[ -x "$nologin" ] || nologin=/usr/sbin/nologin
				useradd --system --home-dir "$DIR" --no-create-home --shell "$nologin" "$USER_NAME"
			fi
			chown -R "$USER_NAME": "$DIR"
			protect_home_line="ProtectHome=yes"
			case "$DIR" in
				/home/*|/root/*) protect_home_line="" ;;
			esac
			{
				printf '%s\n' "[Unit]"
				printf 'Description=%s\n' "$GAME_NAME"
				printf '%s\n' "After=network-online.target"
				printf '%s\n' "Wants=network-online.target"
				printf '\n%s\n' "[Service]"
				printf 'User=%s\n' "$USER_NAME"
				printf 'Group=%s\n' "$USER_NAME"
				printf 'WorkingDirectory=%s\n' "$DIR"
				printf 'ExecStart=%s/run.sh\n' "$DIR"
				printf '%s\n' "Restart=on-failure"
				printf '%s\n' "RestartSec=5"
				printf '%s\n' "NoNewPrivileges=yes"
				printf '%s\n' "ProtectSystem=strict"
				printf 'ReadWritePaths=%s\n' "$DIR"
				[ -n "$protect_home_line" ] && printf '%s\n' "$protect_home_line"
				printf '%s\n' "PrivateTmp=yes"
				printf '\n%s\n' "[Install]"
				printf '%s\n' "WantedBy=multi-user.target"
			} > "/etc/systemd/system/$SERVER.service"
			if [ -d /run/systemd/system ]; then
				systemctl daemon-reload
				systemctl enable --now "$SERVER"
			else
				printf 'unit written; systemd is not running here (container?)\n'
			fi
			step_done service
		fi
	fi
fi

# ---- step 7: done ----------------------------------------------------------
# step 7 is never stored and always prints
step 7 finish "Done" || true
printf '\n'
printf 'Folder: %s\n' "$DIR"
printf '\n'
printf 'Start/stop:\n'
if [ -f "/etc/systemd/system/$SERVER.service" ]; then
	printf '  systemctl start|stop|status %s\n' "$SERVER"
	printf '  journalctl -u %s -f\n' "$SERVER"
else
	printf '  %s/run.sh   (Ctrl+C to stop)\n' "$DIR"
fi
printf '\n'
printf 'Open these UDP ports in your firewall/router:\n'
printf '  %s  (game)\n' "${PORT:-32887}"
printf '  %s  (web browsers, WebRTC)\n' "$(( ${PORT:-32887} + 1 ))"
printf 'Remote console port 32890 stays on 127.0.0.1 and must NOT be opened.\n'
printf 'Example (not run): ufw allow %s/udp ; firewall-cmd --permanent --add-port=%s/udp\n' "${PORT:-32887}" "${PORT:-32887}"
printf '\n'
printf 'Passwords: %s\n' "$DIR/passwords.txt"
printf 'In the game, log in with: /login admin <password>\n'
printf '\n'
printf 'Read the hosting guide (DDoS protection, ports, limits, updates): %s\n' "$GUIDE_URL"
printf '\n'
