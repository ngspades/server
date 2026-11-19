# Headless protocol 1.0 tester (see p10probe.c and run_probe.py).
#   cmake -S tools/p10probe -B tools/p10probe/build && cmake --build tools/p10probe/build
#   tools/p10probe/build/p10probe 127.0.0.1 32887 all
cmake_minimum_required(VERSION 3.10)
project(p10probe C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# enet6 (common/enet, SirLynix's ENet: its API takes address types and its
# headers are <enet6/enet.h>; common/enet-compat keeps `#include <enet/enet.h>`).
# It is a submodule, so a fresh worktree may have it empty: point this at a
# populated checkout with -DP10PROBE_ENET_DIR=...
set(P10PROBE_ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../common/enet"
    CACHE PATH "enet6 source directory")
set(P10PROBE_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../common")

if(NOT EXISTS "${P10PROBE_ENET_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ENet fork not found in ${P10PROBE_ENET_DIR}: run "
                        "`git submodule update --init` or set P10PROBE_ENET_DIR")
endif()

add_subdirectory("${P10PROBE_ENET_DIR}" enet)
if(NOT TARGET enet)
    add_library(enet ALIAS enet6)
endif()
find_package(ZLIB REQUIRED)
# the ngmap scenario decodes the map with the shared codec (C++)
include("${P10PROBE_COMMON_DIR}/ngmap/ngmap.cmake")

add_executable(p10probe
    p10probe.c
    "${P10PROBE_COMMON_DIR}/proto10/buf.c"
    "${P10PROBE_COMMON_DIR}/proto10/codec.c")
target_include_directories(p10probe PRIVATE "${P10PROBE_COMMON_DIR}/enet-compat" "${P10PROBE_COMMON_DIR}")
target_compile_options(p10probe PRIVATE -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(p10probe PRIVATE enet ZLIB::ZLIB ngmap)
