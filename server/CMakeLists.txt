cmake_minimum_required(VERSION 3.16)

# CMake 3.29+: a library repeated on the link line (the static Server and
# Util pull json-c, lua, readline and m again) only where the linker needs it;
# Apple's ld warned "ignoring duplicate libraries" at every link
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

# SpadesX
project(SpadesX LANGUAGES C CXX)

# the browser's server (a Web Worker): before any target, so
# every object (ngmap's too) is built with its -pthread; no socket, no
# libcurl, OpenSSL, readline or Lua
if(EMSCRIPTEN)
    include(cmake/web.cmake)
elseif(SWITCH_HOST)
    # the Nintendo Switch's local match: the same server as a static library
    # the game links in and runs on a thread (client/switch/server.cmake)
    include(cmake/switch.cmake)
endif()

# Before anything looks for a library. The release's server
# (.github/workflows/server.yml): every library linked in, so the binary
# runs with nothing installed. Linux builds it with musl on
# Alpine and Windows with MSYS2 (-static: no shared library at all, the C
# and C++ runtimes included); macOS cannot link the system statically, so
# there it links the .a of a prefix built for the deployment target
# (tools/server_deps_macos.sh: OpenSSL, json-c, Lua) and keeps the
# system's libcurl and libedit. The default build is unchanged.
option(SERVER_STATIC "Link every library statically (the release binary)" OFF)
# a PEM file built into the binary (the static Linux release): the CA bundle
# used when the machine has none of its own (PluginNet.c's net_curl_tls)
set(SERVER_CA_BUNDLE "" CACHE FILEPATH "CA certificates (PEM) to build in, for machines without any")
if(SERVER_STATIC)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    # nothing to find at run time: no build machine's folder in the binary
    set(CMAKE_SKIP_RPATH ON)
    if(NOT APPLE)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
        add_link_options(-static)
    endif()
    message(STATUS "SERVER_STATIC: libraries linked statically")
endif()
if(SERVER_CA_BUNDLE)
    file(READ "${SERVER_CA_BUNDLE}" _ca_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ca_hex "${_ca_hex}")
    file(WRITE "${CMAKE_BINARY_DIR}/generated/server_ca_bundle.h"
         "/* generated from ${SERVER_CA_BUNDLE} (SERVER_CA_BUNDLE) */\n"
         "static const unsigned char server_ca_bundle[] = {${_ca_hex}0x00};\n")
    add_compile_definitions(SERVER_CA_BUNDLE)
    include_directories("${CMAKE_BINARY_DIR}/generated")
    message(STATUS "CA bundle built in: ${SERVER_CA_BUNDLE}")
endif()
# the two shipped configs, built in: a server whose root has none writes the
# profile's default (Main.c), so an empty folder (a Docker bind mount, the
# scratch image's volume) starts like an unpacked release
foreach(_cfg config config-075)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/Resources/${_cfg}.toml")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    file(READ "${_src}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
    string(MAKE_C_IDENTIFIER "default_${_cfg}" _name)
    string(APPEND _default_configs "static const char ${_name}[] = {${_hex}0x00};\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/generated/server_default_configs.h"
     "/* generated from Resources/config.toml and config-075.toml (CMakeLists.txt) */\n${_default_configs}")
include_directories("${CMAKE_BINARY_DIR}/generated")

# ngmap codec (milestone 7, protocol-1.0.md 7.5)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/ngmap/ngmap.cmake)

# common/maphash: the map's shape hash, header only. ngmap's INTERFACE already
# puts common/ on the Server library's include path; this keeps the top-level
# target (Main.c) on the same one, as <maphash/maphash.h> asks
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../common")
# netlib: the connection layer's public headers (ngnet/conn.h)
include(${CMAKE_CURRENT_SOURCE_DIR}/../netlib/netlib.cmake)
include_directories("${NGNET_INCLUDE_DIR}")

# the game name, shared with the client (common/game.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/game.cmake)

# Find pthread
find_package(Threads REQUIRED)

# the browser's server (cmake/web.cmake, included above): none of the
# network libraries below
if(NOT SERVER_WEB_HOST)

# WebRTC for the browsers (docs/web.md phase 2): libdatachannel, fetched and
# built here, always. Every server takes browser players, so there is no
# build without it: a failure here is a build error. Before the unity build
# and LTO below, which would apply to its C++ too. Offline builds (Flatpak)
# point FETCHCONTENT_SOURCE_DIR_LIBDATACHANNEL at a checkout with submodules.
include(cmake/libdatachannel.cmake)
endif() # NOT SERVER_WEB_HOST

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# not on the Switch: its libraries are renamed object by object before they
# are linked into the game (client/switch/prefix_libs.sh), which LTO's
# bytecode objects do not allow
if(NOT SERVER_SWITCH_HOST)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()
option(CMAKE_UNITY_BUILD "Enable Unity Build" ON)

option(GIT_SUBMODULES_FETCH "Fetch the required Git submodules" ON)

# Add third party libraries
add_subdirectory(Extern)

# Locate json-c (macOS/Homebrew installs it outside the default search path;
# the web build fetched its own, cmake/web.cmake)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT SERVER_WEB_HOST)
    pkg_check_modules(JSONC QUIET json-c)
endif()
if(JSONC_FOUND AND NOT SERVER_WEB_HOST)
    include_directories(${JSONC_INCLUDE_DIRS})
    link_directories(${JSONC_LIBRARY_DIRS})
endif()

# Lua for game modes as scripts (Gamemodes/LuaMode.c): optional, a server
# built without it simply finds no script mode (the web build has none yet)
if(PkgConfig_FOUND AND NOT SERVER_WEB_HOST)
    pkg_check_modules(LUA QUIET lua5.4)
    if(NOT LUA_FOUND)
        pkg_check_modules(LUA QUIET lua-5.4)
    endif()
endif()
if(LUA_FOUND)
    message(STATUS "Lua ${LUA_VERSION}: game modes in Lua enabled")
    add_compile_definitions(ENABLE_LUA)
    include_directories(${LUA_INCLUDE_DIRS})
    link_directories(${LUA_LIBRARY_DIRS})
else()
    message(STATUS "Lua 5.4 not found: no game modes in Lua")
endif()

# libcurl: the master server's calls (Net/Master.c, required: every server
# lists itself and answers browsers) and the plugins' game.http, both on
# PluginNet.c's threads
if(SERVER_WEB_HOST)
    # the worker calls out to nothing: no master, no map downloads
    set(CURL_LIBRARIES "")
    set(SERVER_READLINE_LIBS "")
    set(SERVER_NET_LIBS "")
else()
find_package(CURL REQUIRED)
message(STATUS "libcurl ${CURL_VERSION_STRING}: the master server and game.http for plugins")
add_compile_definitions(ENABLE_CURL)
include_directories(${CURL_INCLUDE_DIRS})

set(SERVER_JSONC_LIBS json-c)
set(SERVER_READLINE_LIBS readline)
set(SERVER_NET_LIBS datachannel OpenSSL::Crypto)
endif()
# a static library does not bring its own dependencies: pkg-config's static
# lists name them (libcurl's TLS, compression and IDN libraries, readline's
# ncurses). macOS keeps the system's libcurl and libedit
if(SERVER_STATIC AND NOT APPLE AND PkgConfig_FOUND)
    pkg_check_modules(CURL_PC QUIET libcurl)
    if(CURL_PC_FOUND)
        set(CURL_LIBRARIES ${CURL_PC_STATIC_LIBRARIES})
        link_directories(${CURL_PC_STATIC_LIBRARY_DIRS})
    endif()
    add_compile_definitions(CURL_STATICLIB)
    pkg_check_modules(READLINE_PC QUIET readline)
    if(READLINE_PC_FOUND)
        set(SERVER_READLINE_LIBS ${READLINE_PC_STATIC_LIBRARIES})
        link_directories(${READLINE_PC_STATIC_LIBRARY_DIRS})
    endif()
endif()
if(SERVER_STATIC AND JSONC_FOUND)
    set(SERVER_JSONC_LIBS ${JSONC_STATIC_LIBRARIES})
endif()
if(SERVER_STATIC AND LUA_FOUND)
    set(LUA_LIBRARIES ${LUA_STATIC_LIBRARIES})
    link_directories(${LUA_STATIC_LIBRARY_DIRS})
endif()

# Add main target
if(NOT DEFINED SERVER_STACK_PROTECTOR)
    set(SERVER_STACK_PROTECTOR -fstack-protector-strong)
endif()
add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wno-error=unused-but-set-parameter -Wno-error=pedantic
                    $<$<COMPILE_LANGUAGE:C>:-std=gnu11> ${SERVER_STACK_PROTECTOR})
if(SERVER_SWITCH_HOST)
    # devkitPro's GCC 15 warns (maybe-uninitialized, format-truncation) where
    # the native toolchains' do not: the release's -Werror gates the desktop
    # builds, and a console build only has to run
    add_compile_options(-Wno-error)
    # linked into the game, whose own main it is not (Main.c: sd_server_main)
    add_library(SpadesX STATIC "")
else()
    add_executable(SpadesX "")
endif()

target_link_libraries(SpadesX
    PRIVATE
        Server
        Util
        enet
        tomlc99
        vxl
        m
        ${SERVER_JSONC_LIBS}
        ${SERVER_READLINE_LIBS}
        Threads::Threads
        ${LUA_LIBRARIES}
        ${CURL_LIBRARIES}
        ${SERVER_NET_LIBS}
)
if(EMSCRIPTEN)
    target_link_options(SpadesX PRIVATE ${SERVER_WEB_LINK_OPTIONS})
    set_property(TARGET SpadesX APPEND PROPERTY LINK_DEPENDS
                 ${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js ${CMAKE_CURRENT_SOURCE_DIR}/web/worker_lib.js)
endif()

if(JSONC_FOUND)
    target_include_directories(SpadesX PRIVATE ${JSONC_INCLUDE_DIRS})
    target_link_directories(SpadesX PRIVATE ${JSONC_LIBRARY_DIRS})
endif()

add_subdirectory(Source)

# the target keeps the SpadesX name of the fork; the binary is the game's
# (common/game.cmake)
set_target_properties(SpadesX PROPERTIES OUTPUT_NAME "${GAME_SERVER_BINARY}")
if(EMSCRIPTEN)
    # the page starts it as server.js (the client's workerbridge.js)
    set_target_properties(SpadesX PROPERTIES OUTPUT_NAME server SUFFIX ".js")
endif()

if(NOT SERVER_SWITCH_HOST)
set_target_properties(SpadesX Util Server
  PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION true
)
endif()
