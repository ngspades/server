# libdatachannel (https://github.com/paullouisageneau/libdatachannel), the
# server's WebRTC: DataChannels only (no media, no WebSocket), libjuice for
# ICE (the only backend with the UDP mux every peer shares), OpenSSL for
# DTLS. Linked statically, so the binary carries it like ENet.
include(FetchContent)

set(LIBDATACHANNEL_TAG "v0.24.6")

# Homebrew's OpenSSL is keg-only: CMake does not find it by itself
if(APPLE AND NOT OPENSSL_ROOT_DIR AND NOT DEFINED ENV{OPENSSL_ROOT_DIR})
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(COMMAND ${BREW_EXECUTABLE} --prefix openssl@3
                        OUTPUT_VARIABLE BREW_OPENSSL_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(EXISTS "${BREW_OPENSSL_PREFIX}")
            set(OPENSSL_ROOT_DIR "${BREW_OPENSSL_PREFIX}")
        endif()
    endif()
endif()
find_package(OpenSSL REQUIRED)

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG ${LIBDATACHANNEL_TAG}
    GIT_SHALLOW TRUE
    GIT_SUBMODULES deps/plog deps/usrsctp deps/libjuice deps/json
)

set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
# its own static library (and its submodules'), whatever the project builds
set(_sd_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
# a unity build of its C sources clashes (libjuice). Afterwards the variable
# is unset, not restored: on a first configure it was not set yet, and an
# empty one kept the option below from turning the unity build on
set(CMAKE_UNITY_BUILD OFF)
FetchContent_MakeAvailable(libdatachannel)
set(BUILD_SHARED_LIBS "${_sd_shared_libs}")
unset(CMAKE_UNITY_BUILD)
