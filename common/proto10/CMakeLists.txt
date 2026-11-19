# Protocol 1.0 codec shared by client/ and server/ (docs/protocol-1.0.md).
#
# The client and server builds compile buf.c and codec.c directly; this file
# only builds the test executable:
#   cmake -S common/proto10 -B common/proto10/build && cmake --build common/proto10/build
#   common/proto10/build/test_proto10
cmake_minimum_required(VERSION 3.10)
project(proto10 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(test_proto10 buf.c codec.c text.c tests/test_proto10.c)
if(UNIX AND NOT APPLE)
    # asinf and the rest: libm is not part of libc on Linux
    target_link_libraries(test_proto10 PRIVATE m)
endif()

# at least as strict as the server (-Werror), plus conversions: this code reads
# bytes from strangers on the internet
target_compile_options(test_proto10 PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
# MinGW ships no sanitizer runtime: Windows runs the same checks without them
if(NOT WIN32)
    target_compile_options(test_proto10 PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(test_proto10 PRIVATE -fsanitize=address,undefined)
endif()
