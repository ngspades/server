#
# Add sources
#

add_library(Util STATIC "")

set(CHECKS_HEADERS
    Checks/BlockChecks.h
    Checks/PlayerChecks.h
    Checks/PositionChecks.h
    Checks/TimeChecks.h
    Checks/WeaponChecks.h
    Checks/VectorChecks.h
)

set(CHECKS_SOURCES
    Checks/BlockChecks.c
    Checks/PlayerChecks.c
    Checks/PositionChecks.c
    Checks/TimeChecks.c
    Checks/WeaponChecks.c
    Checks/VectorChecks.c
)

set(UTIL_HEADERS
    ${CHECKS_HEADERS}
    Queue.h
    Compress.h
    DataStream.h
    Types.h
    Physics.h
    Line.h
    Log.h
    Nanos.h
    DataDir.h
    Notice.h
    Weapon.h
    Alloc.h
)

set(UTIL_SOURCES
    ${CHECKS_SOURCES}
    Compress.c
    DataStream.c
    Physics.c
    Line.c
    Log.c
    Nanos.c
    DataDir.c
    Notice.c
    Weapon.c
    Alloc.c
)

target_sources(Util
    PRIVATE
        ${UTIL_SOURCES}
    PUBLIC
        ${UTIL_HEADERS}
)

target_link_libraries(Util
    PUBLIC
        SpadesXCommon
    PRIVATE
        z # zlib
        vxl
        mersenne_twister
)
