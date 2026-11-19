#!/bin/sh
# Build the release's Linux server: one static binary (musl, every library
# linked in) that runs on any Linux distribution, old or new, with nothing
# installed. Run it as root inside an Alpine container, the repository
# mounted (the CI does, .github/workflows/server.yml), for the container's
# own architecture:
#
#   docker run --rm -v "$PWD:/src" -w /src alpine:3.22 tools/build_server_static.sh [build dir]
#
# The submodules must be checked out (git submodule update --init).
# The binary lands in <build dir>/ngspades-server (default server/build).
# Alpine's certificates are built in (SERVER_CA_BUNDLE), for the machines
# that have none (the debian and ubuntu container images); a machine's own
# file wins (PluginNet.c's net_curl_tls).
set -eu

build=${1:-server/build}
cd "$(dirname "$0")/.."

apk add --no-cache \
	build-base cmake git linux-headers perl pkgconf file ca-certificates \
	openssl-dev openssl-libs-static zlib-dev zlib-static \
	json-c-dev lua5.4-dev readline-dev readline-static ncurses-dev ncurses-static \
	curl-dev curl-static brotli-static zstd-static libpsl-static libidn2-static \
	libunistring-static nghttp2-static c-ares-dev >/dev/null

# the checkout belongs to another user than the container's root
git -C / config --global --add safe.directory '*'

cmake -S server -B "$build" -DCMAKE_BUILD_TYPE=Release -DSERVER_STATIC=ON -DGIT_SUBMODULES_FETCH=OFF \
	-DSERVER_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
cmake --build "$build" -j"$(nproc)"

server=$(sed -n 's/^set(GAME_SERVER_NAME "\(.*\)")/\1/p' common/game.cmake)
bin="$build/$server"
strip "$bin"
file "$bin"
# a dynamic executable would name its loader here
if file "$bin" | grep -q "dynamically linked"; then
	echo "error: $bin is not static" >&2
	exit 1
fi
ls -l "$bin"
