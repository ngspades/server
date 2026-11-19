#ifndef SERVERSTRUCT_H
#define SERVERSTRUCT_H

#include <Server/MapDownload.h>
#include <Server/Structs/EventStruct.h>
#include <Server/Structs/PhysicsStruct.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ProtocolStruct.h>
#include <Server/Structs/TimerStruct.h>
#include <Util/DataStream.h>
#include <mersenne-twister/MT.h>
#include <Util/Types.h>
#include <enet/enet.h>
#include <flight/flight.h>
#include <signal.h>
#include <stddef.h>

typedef struct server
{
    ENetHost*             host;
    player_t*             players;
    protocol_t            protocol;
    physics_t             physics;
    flight_rules          jetpack; // [jetpack] in config.toml, Server/Proto10/Jetpack.c
    float                 sentry_lifetime; // [sentry] lifetime_seconds, Server/Proto10/Sentry.c
    mt_rand_t             rand;
    uint16_t              port;
    map_t                 s_map;
    global_timers_t       global_timers;
    command_t*            cmds_map;
    command_t*            cmds_list;
    string_node_t*        welcome_messages;
    uint8_t               welcome_messages_count;
    string_node_t*        periodic_messages;
    uint8_t               periodic_message_count;
    uint8_t*              periodic_delays;
    uint8_t               global_ak;
    uint8_t               global_ab;
    uint8_t               capture_limit;
    const char*           manager_passwd;
    const char*           admin_passwd;
    const char*           mod_passwd;
    const char*           guard_passwd;
    const char*           trusted_passwd;
    char                  map_name[MAP_NAME_MAX + 1];
    char                  map_shape[65]; /* the map's shape hash (common/maphash), "" before a load */
    uint8_t*              map_preview; /* the map's top view (MapPreview.c), NULL before a load */
    size_t                map_preview_len; /* its encoded bytes: SDP1, size, zlib RGB */
    char                  server_name[31];
    char                  gamemode_name[7];
    char                  preset_name[32]; /* [presets.<name>], empty none */
    char                  forced_map[MAP_NAME_MAX + 1];/* /map or /restart: the next load */
    int                   forced_gamemode; /* game.set_mode: the next load, -1 none */
    uint64_t              match_started_at; /* game clock, /time and the summary */
    uint32_t              match_extra_seconds; /* /extend, 0 none */
    volatile sig_atomic_t running; // volatile keyword is required to have an access to this variable in any thread
    volatile sig_atomic_t console_enabled; // 0 when running without a terminal (listen server / background)
    uint8_t               aim_assist;      // [server] aim_assist: 0 forbids the clients' gamepad aim assist
    uint16_t              fog_distance;    // [server] fog_distance: the fog's end in blocks (32..256), enemies cut past it
    uint8_t               listen_mode;     // 1 when started with --listen/--server (the local player owns the server)
    uint8_t               listen_private;  // --listen without --public: off the public server list (Master.c)
    char                  join_code[12];   // --join-code: this match's handy join code (Master.c), "" none
    uint8_t               features_on;     // the features the mode turned on for this map (the zombie mode's classes are the host's choice)
    uint8_t               first_infected_flag; // --first-infected: 0 not given, 255 automatic, else that many patient zeros
    uint8_t               classes_flag;    // the host's choice on the command line (--classes 0|1): 0 the mode's own rule, 1 on, 2 off

    /* Host commands are produced by other threads (console) but ENet may only
       be driven from the network thread, so requests are queued here and
       executed inside the tick. */
    struct host_request
    {
        uint8_t command;
        uint8_t arg1;
        uint8_t arg2;
        uint8_t requester_id; /* 255 == console */
        uint8_t used;
    } host_queue[16];

    event_handlers_t event_handlers;
} server_t;

#endif
