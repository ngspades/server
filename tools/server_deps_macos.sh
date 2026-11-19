#!/usr/bin/env bash
# The macOS release server's libraries, built from source as static
# archives for an old enough macOS: Homebrew's bottles are built for the
# macOS of the machine that made them (a binary linking them requires that
# version), and the release must start on a Mac that has no Homebrew.
#
#   tools/server_deps_macos.sh <prefix>      (MACOSX_DEPLOYMENT_TARGET, default 12.0)
#
# OpenSSL (libdatachannel's DTLS, the master's signatures), json-c and Lua
# 5.4 land in <prefix> as .a only (no .dylib, so the linker can only take
# the archive), with their pkg-config files. libcurl, zlib and readline
# (libedit) are the system's, present on every Mac. Then:
#
#   OPENSSL_ROOT_DIR=<prefix> PKG_CONFIG_PATH=<prefix>/lib/pkgconfig \
#     cmake -S server -B server/build -DSERVER_STATIC=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
set -euo pipefail

prefix=${1:?usage: server_deps_macos.sh <prefix>}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-12.0}
jobs=$(sysctl -n hw.ncpu)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# name url sha256 (a .tar.gz or a .tar.xz)
fetch() {
	local file="$work/$1.${2##*.tar.}"
	curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 -o "$file" "$2"
	echo "$3  $file" | shasum -a 256 -c - >/dev/null
	mkdir -p "$work/$1"
	tar -xf "$file" -C "$work/$1" --strip-components 1
}

fetch openssl https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz \
	967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
fetch json-c https://s3.amazonaws.com/json-c_releases/releases/json-c-0.18.tar.gz \
	876ab046479166b869afc6896d288183bbc0e5843f141200c677b3e8dfb11724
fetch lua https://www.lua.org/ftp/lua-5.4.7.tar.gz \
	9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30

# OpenSSL: libcrypto and libssl only, no shared libraries, no docs; its
# certificates are never read (libcurl is the system's)
(cd "$work/openssl" &&
	./Configure "darwin64-$(uname -m)-cc" no-shared no-tests no-docs no-apps \
		--prefix="$prefix" --libdir=lib "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" >/dev/null &&
	make -j"$jobs" build_libs >/dev/null &&
	make install_dev >/dev/null)

cmake -S "$work/json-c" -B "$work/json-c/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
	-DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DBUILD_APPS=OFF -DDISABLE_EXTRA_LIBS=ON \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DCMAKE_INSTALL_PREFIX="$prefix" \
	-DCMAKE_INSTALL_LIBDIR=lib >/dev/null
cmake --build "$work/json-c/build" -j"$jobs" >/dev/null
cmake --install "$work/json-c/build" >/dev/null

# Lua has no configure: its makefile's macosx target, then the files
# pkg-config's lua5.4 names
make -C "$work/lua/src" -j"$jobs" liblua.a MYCFLAGS="-DLUA_USE_MACOSX -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
	>/dev/null
mkdir -p "$prefix/include/lua5.4" "$prefix/lib/pkgconfig"
cp "$work/lua/src/liblua.a" "$prefix/lib/liblua5.4.a"
cp "$work"/lua/src/{lua.h,luaconf.h,lualib.h,lauxlib.h,lua.hpp} "$prefix/include/lua5.4/"
cat >"$prefix/lib/pkgconfig/lua5.4.pc" <<EOF
prefix=$prefix
Name: Lua
Description: Lua 5.4, static, built by tools/server_deps_macos.sh
Version: 5.4.7
Cflags: -I\${prefix}/include/lua5.4
Libs: -L\${prefix}/lib -llua5.4
Libs.private: -lm
EOF

rm -f "$prefix"/lib/*.dylib
ls -l "$prefix/lib"
