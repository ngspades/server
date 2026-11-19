# ngnet: the connection layer (netlib/README.md). A program includes this
# file and links the sources it lists; it is the library's one build entry
# until the library has a repository and a CMakeLists of its own.
#
#   NGNET_INCLUDE_DIR   the public headers (#include "ngnet/transport.h")
#   NGNET_COMMON_SOURCES   the log hook and the join code, on every build
#   NGNET_ENET_CLIENT_SOURCES   the client's ENet connection (needs ENet's headers)
get_filename_component(NGNET_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(NGNET_INCLUDE_DIR "${NGNET_DIR}/include")
set(NGNET_COMMON_SOURCES
	"${NGNET_DIR}/src/host.c"
	"${NGNET_DIR}/src/p2p.c"
)
set(NGNET_ENET_CLIENT_SOURCES
	"${NGNET_DIR}/src/transport_enet.c"
)
