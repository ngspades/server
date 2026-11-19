# The server compiled to wasm for the browser (docs/web.md
# phase 6): the page starts it in a Web Worker and the client joins it over
# a MessageChannel carrying the WebRTC wire of docs/protocol-1.0.md 3.4
# (Net/Worker.c, the client's workerbridge.js). Included by CMakeLists.txt
# when EMSCRIPTEN, before anything looks for a library.
#
# The same game code, protocol 1.0 only; what opens a socket or calls out is
# left out of this build (Source/Server/CMakeLists.txt): ENet's host
# (Net/EnetConn.c), the UDP queries (Ping.c), libdatachannel (Net/WebRtc.c,
# Net/RtcPeer.cpp), the master (Net/Master.c, which needs OpenSSL and
# libcurl) and the console (Console.c, readline).
# Net/WebHostStubs.c answers their calls the way a server with them turned
# off does. No Lua (no plugins) and no libcurl: MapDownload.c and
# PluginNet.c already build without them (ENABLE_CURL).

set(SERVER_WEB_HOST ON)
add_compile_definitions(SP_WEB_HOST=1)
# every object with atomics: the bots' planner and the ngmap encoder are
# pthreads (the worker spawns nested workers for them), and a shared memory
# wasm cannot link an object built without
add_compile_options(-pthread -sUSE_ZLIB=1)
add_link_options(-pthread -sUSE_ZLIB=1)

# Emscripten's zlib port, as the client's (ngmap.cmake, included before
# this, may have made the target already: the flags above reach it anyway)
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB INTERFACE IMPORTED)
    target_compile_options(ZLIB::ZLIB INTERFACE -sUSE_ZLIB=1)
    target_link_options(ZLIB::ZLIB INTERFACE -sUSE_ZLIB=1)
endif()

# json-c: no Emscripten port, so its sources are fetched (pinned, checked)
# and built here, static
include(FetchContent)
FetchContent_Declare(
    jsonc
    URL https://github.com/json-c/json-c/archive/refs/tags/json-c-0.18-20240915.tar.gz
    URL_HASH SHA256=3112c1f25d39eca661fe3fc663431e130cc6e2f900c081738317fba49d29e298
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
set(DISABLE_WERROR ON CACHE BOOL "" FORCE)
set(DISABLE_EXTRA_LIBS ON CACHE BOOL "" FORCE)
set(_policy_minimum ${CMAKE_POLICY_VERSION_MINIMUM})
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
# never as a unity build (the server's CMAKE_UNITY_BUILD, a cache variable,
# reaches every target made after it): batched with a file that includes
# strerror_override.h first, strerror_override.c's _json_c_strerror called
# strerror, macro'd to itself, and recursed until the worker's stack ran out
# at json-c's first error message ("Maximum call stack size exceeded"; the
# local server died the moment a player joined). The unset puts the cache
# value back for the server's own targets.
set(CMAKE_UNITY_BUILD OFF)
FetchContent_MakeAvailable(jsonc)
unset(CMAKE_UNITY_BUILD)
set(CMAKE_POLICY_VERSION_MINIMUM ${_policy_minimum})
# the server includes <json-c/json.h>, the installed layout: the build
# tree has the headers flat (json.h and json_config.h generated), so they
# are copied under a json-c/ folder of their own
set(JSONC_WEB_INCLUDE ${CMAKE_BINARY_DIR}/jsonc-include)
file(GLOB _jsonc_headers ${jsonc_SOURCE_DIR}/*.h)
file(COPY ${_jsonc_headers} ${jsonc_BINARY_DIR}/json.h ${jsonc_BINARY_DIR}/json_config.h
     DESTINATION ${JSONC_WEB_INCLUDE}/json-c)
include_directories(${JSONC_WEB_INCLUDE})
set(JSONC_FOUND TRUE)
set(JSONC_INCLUDE_DIRS ${JSONC_WEB_INCLUDE})
set(JSONC_LIBRARY_DIRS ${jsonc_BINARY_DIR})
set(SERVER_JSONC_LIBS json-c)

# the link of the worker's program (server.js + server.wasm). INVOKE_RUN 0:
# main runs when the page's argv arrives (web/pre.js), not at load
set(SERVER_WEB_LINK_OPTIONS
    -sENVIRONMENT=worker
    -sINVOKE_RUN=0
    -sEXIT_RUNTIME=1
    -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV
    -sFORCE_FILESYSTEM=1
    # a 512 map, the bots' planner copy and the ngmap encoder's snapshot
    # fit in the first 128 MB; a 1024 map grows it
    -sINITIAL_MEMORY=128MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=1GB
    -Wno-pthreads-mem-growth
    -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB
    # the planner and the ngmap encoder, made before the first tick (a
    # thread past the pool starts a turn of the worker's loop later)
    -sPTHREAD_POOL_SIZE=4
    --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js
    --js-library ${CMAKE_CURRENT_SOURCE_DIR}/web/worker_lib.js
)
