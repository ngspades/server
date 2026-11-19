#!/usr/bin/env bash
# Assemble a server-only release archive from an already built server: the
# binary, its two configs, the start scripts and a README for the person
# hosting it, with no maps (the server downloads the ones it plays,
# MapDownload.c) and no client. The game archive (package_release.sh)
# carries everything; this one is for hosting, and is released on its own
# tags (ngspades-server-v*), since the server is not updated every time the
# client is.
#
#   tools/package_server.sh <version> <platform> [build dir]     e.g. v0.2.0 linux-arm64
#
# Layout:
#   ngspades-server/ngspades-server[.exe], start + start-classic (.sh, .command
#                 or .bat), README.txt
#   ngspades-server/Resources/config.toml, config-075.toml, maps/ (empty), plugins/
#
# Prints the archive path on the last line.
set -euo pipefail

version=${1:?usage: package_server.sh <version> <platform> [build dir]}
platform=${2:?usage: package_server.sh <version> <platform> [build dir]}
root=$(cd "$(dirname "$0")/.." && pwd)
build=${3:-$root/server/build}

name=$(sed -n 's/^set(GAME_SERVER_TITLE "\(.*\)")/\1/p' "$root/common/game.cmake")
server=$(sed -n 's/^set(GAME_SERVER_NAME "\(.*\)")/\1/p' "$root/common/game.cmake")
[ -n "$name" ] && [ -n "$server" ] || { echo "GAME_SERVER_TITLE/GAME_SERVER_NAME not found in common/game.cmake" >&2; exit 1; }
dist="$root/dist"
dir="$dist/$server"

exe=""
case "$platform" in windows*) exe=".exe" ;; esac

rm -rf "$dist"
mkdir -p "$dir/Resources/maps" "$dir/Resources/plugins"

cp "$build/$server$exe" "$dir/"
cp "$root/server/Resources/config.toml" "$root/server/Resources/config-075.toml" "$dir/Resources/"
cp "$root/server/Resources/plugins/README.md" "$root/server/Resources/plugins/server_tools.lua" "$dir/Resources/plugins/"
# the GPL travels with the binary
cp "$root/LICENSE" "$dir/LICENSE"

# the ports the README names, read from the configs it ships: a config's
# [server] port, and its [webrtc] port when set, else the game's + 1
port_of() { sed -n '/^\[server\]/,/^\[/s/^port *= *\([0-9]*\).*/\1/p' "$1" | head -1; }
rtc_of() { sed -n '/^\[webrtc\]/,/^\[/s/^port *= *\([0-9]*\).*/\1/p' "$1" | head -1; }
port10=$(port_of "$dir/Resources/config.toml")
port075=$(port_of "$dir/Resources/config-075.toml")
rtc10=$(rtc_of "$dir/Resources/config.toml")
rtc075=$(rtc_of "$dir/Resources/config-075.toml")
rtc10=${rtc10:-$((port10 + 1))}
rtc075=${rtc075:-$((port075 + 1))}

cat >"$dir/Resources/maps/README.md" <<EOF
# Maps

This archive ships no maps: when the server starts, it downloads each map
its rotation names (\`maps = [..]\` in \`config.toml\`, \`config-075.toml\`
for the classic server) that is not here yet, from
https://github.com/ngspades/maps, into this folder.
\`[maps] download = false\` in the config turns that off.

Each map is a folder of its own, \`<Name>/<Name>.vxl\` with its
\`<Name>.toml\` (and the pyspades \`<Name>.txt\` when it has one). Any map of
that repository can be copied here by hand too, or kept anywhere and the
server started with \`--maps-dir <folder>\`.
EOF

# the start scripts and how to run them, in the platform's words
case "$platform" in
windows*)
	start="start.bat"
	start_classic="start-classic.bat"
	for f in start start-classic; do
		args=""
		[ "$f" = start-classic ] && args=" --profile 075"
		# a double-clicked .bat closes its window when it ends: keep it open
		# so the last lines (an error) can be read
		printf '@echo off\r\nrem Starts the %s server from this folder.\r\ncd /d "%%~dp0"\r\n%s.exe%s %%*\r\necho.\r\necho The server has stopped.\r\npause\r\n' \
			"$name" "$server" "$args" >"$dir/$f.bat"
	done
	how_start="Double-click start.bat. Windows may ask whether to allow the
server through its firewall: allow it (private and public networks, if
players come from the internet). If Windows SmartScreen says it protected
your PC, click \"More info\", then \"Run anyway\" (the server is not signed)."
	how_stop="Close the window, or type /shutdown in it."
	;;
macos*)
	start="start.command"
	start_classic="start-classic.command"
	for f in start start-classic; do
		args=""
		[ "$f" = start-classic ] && args=" --profile 075"
		cat >"$dir/$f.command" <<EOF
#!/bin/sh
# Starts the $name from this folder (double-click it in Finder).
cd "\$(dirname "\$0")" || exit 1
# a downloaded archive is quarantined, and the server is not signed by a
# known developer: once this script was allowed to run, the rest may too
xattr -dr com.apple.quarantine . 2>/dev/null
./$server$args "\$@"
echo
echo "The server has stopped. Press Enter to close this window."
read -r _
EOF
		chmod +x "$dir/$f.command"
	done
	how_start="Double-click start.command. The first time, macOS refuses it
(\"cannot be opened\" or \"Apple could not verify\", since the server is not
signed by a known developer): open System Settings > Privacy & Security,
scroll down and click \"Open Anyway\" next to start.command, then
double-click it again (on macOS 14 and older, right-click start.command >
Open > Open does the same). Or, in Terminal, once:
    xattr -dr com.apple.quarantine <this folder>"
	how_stop="Close the Terminal window, or type /shutdown in it."
	;;
*)
	start="start.sh"
	start_classic="start-classic.sh"
	for f in start start-classic; do
		args=""
		[ "$f" = start-classic ] && args=" --profile 075"
		cat >"$dir/$f.sh" <<EOF
#!/bin/sh
# Starts the $name from this folder: ./$f.sh (arguments are passed on)
cd "\$(dirname "\$0")" || exit 1
exec ./$server$args "\$@"
EOF
		chmod +x "$dir/$f.sh"
	done
	how_start="In a terminal, in this folder: ./start.sh
(it is one static program: no library or package to install, on any
Linux distribution, x86_64 or arm64 as its archive says). To keep it
running after you log out, start it inside screen or tmux, or as a
systemd service."
	how_stop="Press Ctrl+C, or type /shutdown in its console."
	;;
esac

# Windows reads a .txt with CRLF line ends best; the others take LF
readme="$dir/README.txt"
cat >"$readme" <<EOF
$name ($version, $platform)
$(printf '%*s' "$(( ${#name} + ${#version} + ${#platform} + 12 ))" '' | tr ' ' '=')

1. Unzip this archive anywhere (it is self-contained).

2. Edit Resources/config.toml with any text editor:
   - [server] name: your server's name on the server list.
   - [passwords]: CHANGE ALL FIVE. They are "changeme1".."changeme5"
     in every copy of this archive, so anyone could log in as the
     manager; the server warns at every start until they are changed.
     In the game, log in with: /login <role> <password>
     (for example /login admin <your admin password>).

3. Start it:
$(printf '%s\n%s\n' "$how_start" "$how_stop" | sed 's/^/   /')

   The maps download by themselves the first time (Resources/maps/):
   the server needs the internet then, and a few seconds per map.

4. Let players in: open or forward these UDP ports on your firewall and
   router to this computer:
     UDP $port10   the game
     UDP $rtc10   players in a web browser (WebRTC)
   Players on your own network find the server under LAN in the game's
   server list; with [master] enabled = true (the default) it is also on
   the public list.

The classic server (Ace of Spades 0.75 players and clients)
   $start_classic starts it instead (or as well: both can run at once).
   It reads Resources/config-075.toml (change its passwords too) and
   uses other ports:
     UDP $port075   the game
     UDP $rtc075   web browsers

More: every option is explained in the config files. --help lists the
command line options. Plugins: Resources/plugins/README.md.
https://github.com/ngspades/maps has every map; add names to
"maps = [..]" in the config and they download at the next start.
EOF
case "$platform" in windows*) sed -i 's/$/\r/' "$readme" "$dir/Resources/maps/README.md" ;; esac

case "$platform" in
macos*)
	# the release is linked against the system's libraries only (SERVER_STATIC
	# with tools/server_deps_macos.sh); a build that still names Homebrew's
	# gets them copied next to it
	if otool -L "$dir/$server" | tail -n +2 | grep -v -e '^\s*/usr/lib/' -e '^\s*/System/' | grep -q .; then
		if command -v dylibbundler >/dev/null; then
			dylibbundler -cd -of -b -x "$dir/$server" -d "$dir/libs" -p @executable_path/libs/
		else
			echo "warning: the binary needs non-system libraries and dylibbundler is not installed" >&2
		fi
	fi
	;;
windows*)
	# a static build needs no DLL; any other (MSYS2's) goes next to the .exe,
	# where Windows looks first
	ldd "$dir/$server.exe" | awk '/\/(ucrt64|mingw64)\// {print $3}' | while read -r dll; do
		echo "bundling $dll" >&2
		cp -n "$dll" "$dir/"
	done
	;;
esac

archive_base="$server-$version-$platform"
case "$platform" in
windows*)
	(cd "$dist" && zip -qr "$archive_base.zip" "$server")
	echo "$dist/$archive_base.zip"
	;;
*)
	tar -C "$dist" -czf "$dist/$archive_base.tar.gz" "$server"
	echo "$dist/$archive_base.tar.gz"
	;;
esac
