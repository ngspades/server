#
# Add sources
#

add_library(Server STATIC "")

set(PACKET_HEADERS
    Packets/Packets.h
)

set(PACKET_SOURCES
    Packets/BlockAction.c
    Packets/BlockLine.c
    Packets/ChangeTeam.c
    Packets/ChangeWeapon.c
    Packets/ExistingPlayer.c
    Packets/Grenade.c
    Packets/Hit.c
    Packets/HostCommand.c
    Packets/InputData.c
    Packets/KillAction.c
    Packets/Message.c
    Packets/OrientationData.c
    Packets/PacketManager.c
    Packets/PlayerLeft.c
    Packets/PositionData.c
    Packets/SetColor.c
    Packets/SetHP.c
    Packets/SetTool.c
    Packets/ShortPlayerData.c
    Packets/VersionRequestResponse.c
    Packets/WeaponInput.c
    Packets/WeaponReload.c
)

set(COMMANDS_HEADERS
    Commands/CommandManager.h
    Commands/Commands.h
)

set(COMMANDS_SOURCES
    Commands/Admin.c
    Commands/Ban.c
    Commands/Bots.c
    Commands/Clin.c
    Commands/CommandManager.c
    Commands/Help.c
    Commands/Intel.c
    Commands/Invisible.c
    Commands/Kick.c
    Commands/Kill.c
    Commands/Login.c
    Commands/Mute.c
    Commands/PrivateMessage.c
    Commands/Ratio.c
    Commands/Reset.c
    Commands/Say.c
    Commands/Server.c
    Commands/Stats.c
    Commands/Map.c
    Commands/Crate.c
    Commands/Night.c
    Commands/Sky.c
    Commands/Outfit.c
    Commands/Perk.c
    Commands/Teleport.c
    Commands/Toggles.c
    Commands/Unban.c
    Commands/Ups.c
    Commands/Shutdown.c
)

set(STRUCTS_HEADERS
    Structs/CommandStruct.h
    Structs/EventStruct.h
    Structs/GamemodeStruct.h
    Structs/GrenadeStruct.h
    Structs/IPStruct.h
    Structs/MapStruct.h
    Structs/MovementStruct.h
    Structs/PlayerStruct.h
    Structs/ProtocolStruct.h
    Structs/ServerStruct.h
    Structs/TimerStruct.h
    Structs/StartStruct.h)

set(SERVER_HEADERS
    ${COMMANDS_HEADERS}
    ${PACKET_HEADERS}
    ${STRUCTS_HEADERS}
    Block.h
    EventLog.h
    Bots.h
    BotGoal.h
    BotProjects.h
    Grenade.h
    HostPackets.h
    Nodes.h
    Staff.h
    Console.h
    Events.h
    Server.h
    GameApi.h
    Map.h
    MapMeta/MapMeta.h
    MapMeta/Internal.h
    MapDownload.h
    MapPreview.h
    Gamemodes/Gamemode.h
    Gamemodes/Ctf.h
    Gamemodes/CtfInternal.h
    Gamemodes/NgCtf.h
    Gamemodes/Squads.h
    Gamemodes/Arena.h
    Ping.h
    ParseConvert.h
    Player.h
    Proto10/Session.h
    Proto10/Bridge.h
    Proto10/World.h
    Proto10/Entities.h
    Proto10/GameMode.h
    Proto10/Objects.h
    Proto10/Sentry.h
    Proto10/Mg.h
    Proto10/Mount.h
    Proto10/Helicopter.h
    Proto10/Explosives.h
    Proto10/Rockets.h
    Proto10/Jetpack.h
    Proto10/NetStats.h
    Perks.h
    Classes.h
    LagComp.h
    Conduct.h
    EscapeWatch.h
    Anticheat.h
    Names.h
    Stats.h
    Audit.h
    SpawnProtect.h
    NetGuard.h
    Net/Conn.h
    Net/EnetConn.h
    Net/WebRtc.h
    Net/Master.h
    Net/RtcPeer.h
    ConfigCheck.h
    ConfigKeys.h
    Profile.h
    Legacy/Ext075.h
    Legacy/Master075.h
)

# protocol 1.0 codec shared with the client (docs/protocol-1.0.md 13.0)
set(PROTO10_COMMON_SOURCES
    ${PROJECT_SOURCE_DIR}/../common/proto10/buf.c
    ${PROJECT_SOURCE_DIR}/../common/proto10/codec.c
    ${PROJECT_SOURCE_DIR}/../common/proto10/text.c
)

set(SERVER_SOURCES
    ${COMMANDS_SOURCES}
    ${PACKET_SOURCES}
    ${PROTO10_COMMON_SOURCES}
    Proto10/Session.c
    Proto10/Bridge.c
    Proto10/NetStats.c
    Proto10/World.c
    Proto10/Entities.c
    Proto10/GameMode.c
    Proto10/Objects.c
    Proto10/Flags.c
    Proto10/Crates.c
    Proto10/Sentry.c
    Proto10/Mg.c
    Proto10/Mount.c
    Proto10/Helicopter.c
    Proto10/Explosives.c
    Proto10/Rockets.c
    Proto10/Drills.c
    Proto10/Jetpack.c
    Perks.c
    Assists.c
    BlockHealth.c
    LagComp.c
    Conduct.c
    Flood.c
    EscapeWatch.c
    Anticheat.c
    Names.c
    Stats.c
    MapVote.c
    Audit.c
    SpawnProtect.c
    GameplayPhysics.c
    NetGuard.c
    ConfigCheck.c
    ConfigKeys.c
    Classes.c
    Rcon.c
    Sky.c
    Outfit.c
    Block.c
    EventLog.c
    Bots.c
    BotBlocks.c
    BotCombat.c
    BotMelee.c
    BotNav.c
    BotRoles.c
    BotSenses.c
    BotThink.c
    BotPlanner.c
    BotProjects.c
    Grenade.c
    Nodes.c
    Staff.c
    Console.c
    Events.c
    Server.c
    Map.c
    MapMeta/MapMeta.c
    MapMeta/Lex.c
    MapMeta/Eval.c
    MapMeta/Cond.c
    MapMeta/Func.c
    MapDownload.c
    MapPreview.c
    Gamemodes/Gamemode.c
    Gamemodes/Ctf.c
    Gamemodes/NgCtf.c
    Gamemodes/Squads.c
    Gamemodes/Arena.c
    Gamemodes/Zombie.c
    Gamemodes/Creative.c
    Gamemodes/Tdm.c
    Gamemodes/Koth.c
    Gamemodes/Tc.c
    Gamemodes/LuaMode.c
    Net/Conn.c
    Net/EnetConn.c
    Net/WebRtc.c
    Net/RtcPeer.cpp
    Net/Master.c
    Plugins.c
    PluginNet.c
    Ping.c
    ParseConvert.c
    Player.c
    Profile.c
    Legacy/Ext075.c
    Legacy/Master075.c
)

# the browser's server (../../cmake/web.cmake): what opens a socket, calls
# out or reads a terminal is left out, Net/WebHostStubs.c answers for it as
# a server with it turned off, and Net/Worker.c is its one transport (the
# page's MessageChannel)
if(SERVER_WEB_HOST)
    list(REMOVE_ITEM SERVER_SOURCES
        Net/EnetConn.c
        Net/WebRtc.c
        Net/RtcPeer.cpp
        Net/Master.c
        Ping.c
        Console.c
    )
    list(APPEND SERVER_SOURCES Net/Worker.c Net/WebHostStubs.c)
    list(APPEND SERVER_HEADERS Net/Worker.h)
endif()

target_sources(Server
    PRIVATE
        ${SERVER_SOURCES}
    PUBLIC
        ${SERVER_HEADERS}
)

target_link_libraries(Server PRIVATE SpadesXCommon Util ngmap ${SERVER_NET_LIBS})
# Net/RtcPeer.cpp: libdatachannel's C++ API is C++17
target_compile_features(Server PRIVATE cxx_std_17)

# its own unit: added to a unity batch it moved Plugins.c in with MapMeta.c,
# whose static names clash
set_source_files_properties(Rcon.c PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# the same for the network counters: in a batch they moved Drills.c in with
# a file whose _player clashes
set_source_files_properties(Proto10/NetStats.c PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# the 0.75 profile's wire (docs/plan-075-server.md): each file its own unit,
# so the restored senders' static names never meet another file's
file(GLOB LEGACY_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Legacy/*.c)
set_source_files_properties(${LEGACY_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# the glob only set the property: a new Legacy/*.c is built through this
# (Ext075.c and Master075.c are listed above too; CMake keeps one of each)
target_sources(Server PRIVATE ${LEGACY_SOURCES})
# its own unit: its static helpers (_exists, _move...) are common names
set_source_files_properties(MapDownload.c PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# the map script reader's five files share names through Internal.h and keep
# their helpers static: each its own unit
set_source_files_properties(MapMeta/MapMeta.c MapMeta/Lex.c MapMeta/Eval.c MapMeta/Cond.c MapMeta/Func.c PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
