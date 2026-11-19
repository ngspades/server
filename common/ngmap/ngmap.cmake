# ngmap codec as a static library for the client and server builds
# (docs/protocol-1.0.md 7.5). Both are C projects, so this enables CXX and
# defines the target `ngmap`; include() it BEFORE the project's own
# add_compile_options, so the C-only flags (-std=gnu11 on the server) never
# reach the C++ sources. The test executable (CMakeLists.txt next to this
# file) keeps compiling the same sources with -Werror and the sanitizers.
#
#   include(${PATH_TO}/common/ngmap/ngmap.cmake)
#   target_link_libraries(<target> PRIVATE ngmap)
#   #include <ngmap/ngmap.h>   (with <repo>/common on the include path)

if(TARGET ngmap)
    return()
endif()

enable_language(CXX)
# a build may give its own zlib target first (the browser's: Emscripten's
# zlib port, which no find_package sees before it is built)
if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
endif()

set(NGMAP_DIR ${CMAKE_CURRENT_LIST_DIR})

add_library(ngmap STATIC
    ${NGMAP_DIR}/ngmap.cpp
    ${NGMAP_DIR}/vxl.cpp
    ${NGMAP_DIR}/jpge.cpp
)
set_target_properties(ngmap PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(ngmap PRIVATE -Wall -Wextra)
# vendored public domain encoder, kept as upstream wrote it
set_source_files_properties(${NGMAP_DIR}/jpge.cpp PROPERTIES COMPILE_OPTIONS -w)
target_include_directories(ngmap INTERFACE ${NGMAP_DIR}/..)
target_link_libraries(ngmap PUBLIC ZLIB::ZLIB)
