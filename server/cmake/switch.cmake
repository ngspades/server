# The server as a library for the Nintendo Switch's game (client/switch/
# server.cmake): the browser's server (cmake/web.cmake) with the browser taken
# out. A console cannot start a second program beside the game, so the
# game links this in and runs it on a thread: the same 1.0 server, no
# socket (SP_WEB_HOST: Net/Worker.c is its one transport, the game's side of
# it is client/switch/localhost.c), and the game's ENet, master, console and
# Lua stay out as they do in the browser. Included by CMakeLists.txt when
# SWITCH_HOST.

set(SERVER_WEB_HOST ON)
set(SERVER_SWITCH_HOST ON)
add_compile_definitions(SP_WEB_HOST=1 SP_SWITCH_HOST=1)
# an exit() on the server's thread (a map that fails to load) must end the
# server, never the game it lives in: the game's localhost.c ends the thread
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Dexit=sd_server_exit>)
# no stack protector runtime in the homebrew libc
set(SERVER_STACK_PROTECTOR "")

# json-c is devkitPro's own portlib (switch-libjson-c)
set(SERVER_JSONC_LIBS json-c)

# the portlibs' headers (zlib, json-c), after the -I paths: a portlib ENet is
# in there too and the game's own must win. The server's own targets do not
# link ZLIB::ZLIB, as on a desktop where zlib.h is in the default path
include_directories(AFTER SYSTEM $ENV{DEVKITPRO}/portlibs/switch/include)
