# ngmap codec shared by client/ and server/ (docs/protocol-1.0.md, 7.5).
#
# The client and server builds link ngmap.cmake's static library (C++11,
# zlib); this file builds the two executables that run the codec on its own:
#   cmake -S common/ngmap -B common/ngmap/build && cmake --build common/ngmap/build
#   common/ngmap/build/test_ngmap [maps_dir]        the codec's tests
#   common/ngmap/build/ngmap_cli encode|decode ...  one map file at a time
#                                                   (`make menu-map` build it
#                                                   with -DCMAKE_BUILD_TYPE=Release:
#                                                   the times it prints are why)
cmake_minimum_required(VERSION 3.10)
project(ngmap C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# the codec needs C++11; the test uses std::filesystem
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)

set(SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all)

# vendored public domain encoder: kept as upstream wrote it, so only its own
# warnings are relaxed
add_library(ngmap_jpge OBJECT jpge.cpp)
target_compile_options(ngmap_jpge PRIVATE -w -g -O1 ${SANITIZE})

# the codec as the single static library the client and the server link, so
# the tool below runs the code both of them run
include(${CMAKE_CURRENT_SOURCE_DIR}/ngmap.cmake)

add_executable(test_ngmap ngmap.cpp vxl.cpp tests/test_ngmap.cpp tests/c_api_check.c
    $<TARGET_OBJECTS:ngmap_jpge>)
target_link_libraries(test_ngmap PRIVATE ZLIB::ZLIB)

# at least as strict as the server (-Werror), plus conversions: the decoder
# reads bytes from strangers on the internet
target_compile_options(test_ngmap PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
    -g -O1 ${SANITIZE})
target_link_options(test_ngmap PRIVATE -fsanitize=address,undefined)

# the command line tool (tools/ngmap_cli.c): links the same library, so the
# sizes and times it prints are the ones the client and the server pay.
# Built without the sanitizers: it encodes real maps, and the point of the
# numbers is what the game does with them.
add_executable(ngmap_cli tools/ngmap_cli.c)
target_link_libraries(ngmap_cli PRIVATE ngmap)
target_compile_options(ngmap_cli PRIVATE -Wall -Wextra -Werror -Wpedantic -Wshadow -g)

# ngmap_libvxl.h against ngmap_decode + libvxl_create on real maps:
#   common/ngmap/build/libvxl_check <map.ngmap>...
add_executable(libvxl_check tests/libvxl_check.c ${CMAKE_CURRENT_SOURCE_DIR}/../vxl/libvxl.c)
target_include_directories(libvxl_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(libvxl_check PRIVATE ngmap)
target_compile_options(libvxl_check PRIVATE -g -O1 ${SANITIZE})
target_link_options(libvxl_check PRIVATE -fsanitize=address,undefined)
