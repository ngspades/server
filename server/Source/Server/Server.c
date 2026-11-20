// Copyright DarkNeutrino 2021
#ifdef _WIN32
/* GetConsoleMode: a real terminal, which isatty() cannot tell from NUL */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <Server/Flood.h>
#include <Server/Rcon.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Legacy/Master075.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Sky.h>
#include <Server/Outfit.h>
#include <Server/Plugins.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/NetStats.h>
#include <Server/Proto10/Session.h>
#include <Server/Proto10/World.h>
#include <Server/Commands/Commands.h>
#include <Server/Bots.h>
#include <Server/HostPackets.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/Sentry.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Helicopter.h>
#include <Server/Proto10/Mount.h>
#include <Server/EventLog.h>
#include <Server/Console.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Map.h>
#include <Server/Anticheat.h>
#include <Server/Audit.h>
#include <Server/Conduct.h>
#include <Server/EscapeWatch.h>
#include <Server/LagComp.h>
#include <Server/NetGuard.h>
#include <Server/Net/Conn.h>
#include <Server/Net/EnetConn.h>
#include <Server/Net/WebRtc.h>
#include <Server/Net/Master.h>
#include <Server/SpawnProtect.h>
#include <Server/GameplayPhysics.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Ping.h>
#include <Server/Player.h>
#include <Server/Server.h>
#include <Server/Structs/GrenadeStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Server/Structs/StartStruct.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <mersenne-twister/MT.h>
#include <Util/Compress.h>
#include <Util/DataStream.h>
#include <Util/Enums.h>
#include <Util/JSONHelpers.h>
#include <Util/Line.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <Util/Queue.h>
#include <Util/Types.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Server/MapStore.h>
#include <stddef.h>
#include <stdio.h>
// Include stdio before readline cause it needs it for some reason
#include <enet/enet.h>
#include <errno.h>
#include <json-c/json.h>
#include <json-c/json_object.h>
#include <json-c/json_types.h>
#include <json-c/json_util.h>
#include <math.h>
#include <pthread.h>
#ifdef SP_WEB_HOST
/* the browser's server (Net/Worker.c): no terminal, no readline, and the
   tick is the worker's main loop */
#include <Server/Net/Worker.h>
#ifdef SP_SWITCH_HOST
/* the Switch's local match (client/switch/localhost.c): the server is a
   thread of the game, so the tick is the native loop below and the game
   is told when the server is up */
extern void sd_host_server_started(void);
#else
#include <emscripten.h>
#endif
#else
#include <readline/history.h>
#include <readline/readline.h>
#endif

#if defined(__APPLE__) && !defined(SP_WEB_HOST)
/* GNU readline global missing from the libedit shipped with macOS */
int rl_catch_signals = 0;
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <tomlc99/toml.h>
#include <Util/TOMLHelpers.h>

server_t        server;
pthread_mutex_t server_lock;

server_t* get_server(void)
{
    /* the one global instance, for calls that cannot take it as an argument */
    return &server;
}

/* whether stdin is a terminal a person can type at: on Windows isatty() is
   also true for NUL (a character device), and a server started with its input
   on NUL ran its console, read EOF at once and shut down (the game's listen
   server hands its child NUL) */
static int _stdin_is_terminal(void)
{
#ifdef _WIN32
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) ? 1 : 0;
#else
    return isatty(STDIN_FILENO) ? 1 : 0;
#endif
}

static void _string_nodes_free(string_node_t* root)
{
    string_node_t *el, *tmp;
    DL_FOREACH_SAFE(root, el, tmp)
    {
        free(el->string);
        DL_DELETE(root, el);
        free(el);
    }
}

static void* _calculate_physics(void)
{
    server.global_timers.update_time = get_nanos();
    if (server.global_timers.update_time - server.global_timers.last_update_time >= (NANO_60TPS)) {
        update_movement_and_grenades(&server);
        server.global_timers.last_update_time = get_nanos();
    }
    return 0;
}

/* a player back to the rest pose, in one place so every reset starts from the
   same basis vectors */
static void _srv_reset_player(server_t* server, player_t* player, uint8_t reset, uint8_t disconnect)
{
    vector3f_t empty   = {0, 0, 0};
    vector3f_t forward = {1, 0, 0};
    vector3f_t height  = {0, 0, 1};
    vector3f_t strafe  = {0, 1, 0};
    init_player(server, player, reset, disconnect, empty, forward, strafe, height);
}

/* the map this load plays: the rotation's next entry, or the one /map forced.
   reset is 1 for a map change, 0 for the first load. */
static const char* _srv_select_map(server_t* server, uint8_t reset)
{
    if (server->s_map.rotation_mode == MAP_ROTATION_RANDOM) {
        uint8_t index = gen_rand_long(&server->rand) % server->s_map.map_count;
        server->s_map.current_map = server->s_map.map_list;
        for (int i = 0; i < index; ++i) {
            if (server->s_map.current_map->next != NULL) {
                server->s_map.current_map = server->s_map.current_map->next;
            } else {
                break;
            }
        }
    } else {
        /* alphabetical and TOML-defined rotations both play the list in order */
        if (reset == 1 && server->s_map.current_map != NULL) {
            if (server->s_map.current_map->next != NULL) {
                server->s_map.current_map = server->s_map.current_map->next;
            } else {
                server->s_map.current_map = server->s_map.map_list;
            }
        } else {
            server->s_map.current_map = server->s_map.map_list;
        }
    }

    /* /map and /restart: load this one now. When it is in the rotation the
       pointer moves too, so the next map is still the following one. */
    const char* load_map = server->s_map.current_map->string;
    if (server->forced_map[0] != '\0') {
        load_map = server->forced_map;
        string_node_t* node;
        DL_FOREACH(server->s_map.map_list, node)
        {
            if (!strcmp(node->string, load_map)) {
                server->s_map.current_map = node;
                break;
            }
        }
    }
    return load_map;
}

/* every player back to a spectator at a map change. init_player wipes the
   team, but the host set each team's bots and a bot's JOINING step does not
   count itself: keep a bot's team and count it here, or the whole lot lands
   on one side off stale counts. */
static void _srv_reset_players(server_t* server)
{
    player_t *player, *tmp;
    server->protocol.num_team_users[TEAM_A] = 0;
    server->protocol.num_team_users[TEAM_B] = 0;
    HASH_ITER(hh, server->players, player, tmp)
    {
        uint8_t team = player->team;
        _srv_reset_player(server, player, 1, 0);
        if (player->is_bot && (team == TEAM_A || team == TEAM_B)) {
            player->team = team;
            server->protocol.num_team_users[team]++;
        }
    }
}

/* a map file next to the VXL, checked the same way server_force_map checks
   both halves of a map before accepting it */
static int _srv_map_file_exists(const char* name, const char* ext, const char* mode)
{
    char path[512];
    map_file_path(path, sizeof path, name, ext);
    FILE* fp = fopen(path, mode);
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static void _server_init(server_t*   server,
                         uint32_t    connections,
                         const char* serverName,
                         const char* team1Name,
                         const char* team2Name,
                         const uint8_t*    team1_color,
                         const uint8_t*    team2_color,
                         uint8_t     gamemode,
                         uint8_t     reset)
{
    server->global_timers.update_time = server->global_timers.last_update_time = get_nanos();
    /* a plugin's game.set_mode(): the next load uses it once */
    if (server->forced_gamemode >= 0) {
        gamemode                    = (uint8_t) server->forced_gamemode;
        server->forced_gamemode     = -1;
    }
    if (reset == 0) {
        server->protocol.num_players = 0;
        /* 64 a match: the 0.75 limit of 32 went with the 0.75 protocol */
        server->protocol.max_players = (connections <= SERVER_MAX_PLAYERS) ? connections : SERVER_MAX_PLAYERS;
    }

    server->protocol.input_flags = 0;

    if (reset == 0) {
        /* SP_RAND_SEED: tests replay the same spawns, intel and tents */
        const char* seed_env = getenv("SP_RAND_SEED");
        server->rand         = seed_rand(seed_env != NULL && *seed_env ? strtoul(seed_env, NULL, 10) : (unsigned long)time(NULL));
    }

    const char* load_map = _srv_select_map(server, reset);
    snprintf(server->map_name, sizeof server->map_name, "%s", load_map);
    LOG_STATUS("Selecting %s as map", server->map_name);

    char vxl_map[256];
    map_file_path(vxl_map, sizeof(vxl_map), load_map, "vxl");

    LOG_STATUS("Loading spawn ranges from map file");
    char map_config_path[256];
    map_file_path(map_config_path, sizeof(map_config_path), load_map, "toml");

    int         team1_start[3];
    int         team2_start[3];
    int         team1_end[3];
    int         team2_end[3];
    int         fog_color[3];
    int         map_size[3];
    uint8_t     capture_limit;

    toml_table_t* parsed;
    toml_table_t* map_table;
    /* the server-only archive (tools/package_server.sh) ships no maps: say
       where they come from before the generic "cannot find" exit */
    FILE* map_config_fp = fopen(map_config_path, "r");
    if (map_config_fp) {
        fclose(map_config_fp);
    } else {
        LOG_ERROR("Map '%s' is not installed: copy its folder from "
                  "https://github.com/ngspades/maps (maps/%s/) into Resources/maps/, "
                  "or start with --maps-dir <folder>",
                  load_map,
                  load_map);
    }
    TOMLH_READ_FROM_FILE(parsed, map_config_path);
    TOMLH_GET_TABLE(parsed, map_table, "map");

    /* [map] */
    TOMLH_GET_INT_ARRAY(map_table, fog_color, "fog_color", 3, ((int[]){128, 232, 255}), 1);
    TOMLH_GET_INT_ARRAY(map_table, map_size, "map_size", 3, ((int[]){512, 512, 64}), 1);
    TOMLH_GET_INT(map_table, capture_limit, "capture_limit", server->capture_limit, 1);

    /* [water_damage] */
    uint8_t water_damage_enabled;
    uint8_t water_damage_per_second;
    TOMLH_GET_BOOL(map_table, water_damage_enabled, "water_damage_enabled", 0, 1);
    TOMLH_GET_INT(map_table, water_damage_per_second, "water_damage_per_second", 5, 1);

    /* [spawnpoints] */
    toml_table_t* spawnpoints_table;
    toml_table_t* team1_spawnpoints_table;
    toml_table_t* team2_spawnpoints_table;
    TOMLH_GET_TABLE(parsed, spawnpoints_table, "spawnpoints");

    TOMLH_GET_TABLE(spawnpoints_table, team1_spawnpoints_table, "team1");
    TOMLH_GET_INT_ARRAY(team1_spawnpoints_table, team1_start, "start", 3, ((int[]){0, 0, 0}), 0);
    TOMLH_GET_INT_ARRAY(team1_spawnpoints_table, team1_end, "end", 3, ((int[]){10, 10, 0}), 0);

    TOMLH_GET_TABLE(spawnpoints_table, team2_spawnpoints_table, "team2");
    TOMLH_GET_INT_ARRAY(team2_spawnpoints_table, team2_start, "start", 3, ((int[]){0, 0, 0}), 0);
    TOMLH_GET_INT_ARRAY(team2_spawnpoints_table, team2_end, "end", 3, ((int[]){10, 10, 0}), 0);

    /* The 0.75 wire has no map size in it (MapStart carries only the
       compressed length) and a vanilla client builds 512 x 512 x 64, and the
       0.75 profile's VXL buffer is sized for a 512 map (Legacy/Join075.c):
       a bigger map would reach those clients truncated or not at all. Refuse
       it like a map that fails to load: no map, the server does not start
       (protocol 1.0 announces map-size in its GameStateHeader, so only the
       0.75 profile has to refuse). */
    if (profile_is_075() && (map_size[0] != 512 || map_size[1] != 512)) {
        LOG_WARNING("Map '%s' is %dx%d: the 0.75 profile plays 512 x 512 maps only", load_map, map_size[0],
                    map_size[1]);
        evt_log(NULL, "map", "action=refused why=size_075 name=%s size=%dx%d", load_map, map_size[0], map_size[1]);
        server->running = 0;
        return;
    }

    if (map_load(server, vxl_map, map_size) == 0) {
        return;
    }
    /* after the map: a script's get_z() reads it */
    char map_meta_path[256];
    map_file_path(map_meta_path, sizeof(map_meta_path), load_map, "txt");
    map_meta_load(server, map_meta_path);

    vector3i_t team1_start_range = {team1_start[0], team1_start[1], 1};
    vector3i_t team1_end_range   = {team1_end[0], team1_end[1], 1};
    vector3i_t team2_start_range = {team2_start[0], team2_start[1], 1};
    vector3i_t team2_end_range   = {team2_end[0], team2_end[1], 1};
    /* a range off the map is brought onto its edge: it used to end the
       server, so a downloaded or a player's map in the rotation took it down
       (Capitol writes -1). The spawn search finds dry ground from there */
    vector3i_t* ranges[4] = {&team1_start_range, &team1_end_range, &team2_start_range, &team2_end_range};
    for (int r = 0; r < 4; r++) {
        if (valid_pos_v3i(server, *ranges[r])) {
            continue;
        }
        int cx = ranges[r]->x < 0 ? 0 : ranges[r]->x >= server->s_map.map.size_x ? server->s_map.map.size_x - 1 : ranges[r]->x;
        int cy = ranges[r]->y < 0 ? 0 : ranges[r]->y >= server->s_map.map.size_y ? server->s_map.map.size_y - 1 : ranges[r]->y;
        LOG_WARNING("Map '%s': spawn range point %d,%d is off the map, moved to %d,%d",
                    server->map_name, ranges[r]->x, ranges[r]->y, cx, cy);
        ranges[r]->x = cx;
        ranges[r]->y = cy;
    }

    if (reset) {
        _srv_reset_players(server);
    }

    server->global_ab = 1;
    server->global_ak = 1;

    server->protocol.spawns[0].from.x = team1_start[0];
    server->protocol.spawns[0].from.y = team1_start[1];
    server->protocol.spawns[0].from.z = team1_start[2];
    server->protocol.spawns[0].to.x   = team1_end[0];
    server->protocol.spawns[0].to.y   = team1_end[1];
    server->protocol.spawns[0].to.z   = team1_end[2];

    server->protocol.spawns[1].from.x = team2_start[0];
    server->protocol.spawns[1].from.y = team2_start[1];
    server->protocol.spawns[1].from.z = team2_start[2];
    server->protocol.spawns[1].to.x   = team2_end[0];
    server->protocol.spawns[1].to.y   = team2_end[1];
    server->protocol.spawns[1].to.z   = team2_end[2];

    server->protocol.color_fog.r = fog_color[0];
    server->protocol.color_fog.g = fog_color[1];
    server->protocol.color_fog.b = fog_color[2];

    server->protocol.color_team[0].r = team1_color[R_CHANNEL];
    server->protocol.color_team[0].g = team1_color[G_CHANNEL];
    server->protocol.color_team[0].b = team1_color[B_CHANNEL];

    server->protocol.color_team[1].r = team2_color[R_CHANNEL];
    server->protocol.color_team[1].g = team2_color[G_CHANNEL];
    server->protocol.color_team[1].b = team2_color[B_CHANNEL];

    memcpy(server->protocol.name_team[0], team1Name, strlen(team1Name));
    memcpy(server->protocol.name_team[1], team2Name, strlen(team2Name));
    server->protocol.name_team[0][strlen(team1Name)] = '\0';
    server->protocol.name_team[1][strlen(team2Name)] = '\0';
    server->protocol.gamemode.score_limit = capture_limit;

    server->protocol.gamemode.water_damage_enabled = water_damage_enabled;
    server->protocol.gamemode.water_damage         = water_damage_per_second;

    memcpy(server->server_name, serverName, strlen(serverName));
    server->server_name[strlen(serverName)] = '\0';
    /* what the map's author wrote in the .txt wins over the .toml */
    map_meta_apply(server);
    gamemode_read_map(server, gamemode, parsed);
    toml_free(parsed);
    gamemode_init(server, gamemode);
    /* the 1.0 objects of the map just left (the MG stands through a death, a
       class or a team change, never through a map), and the new map's
       helicopters at their teams' bases (Proto10/Helicopter.c) */
    proto10_objects_map_start(server);
    heli_map_start(server);
    server->forced_map[0] = '\0';
    lagcomp_init(server);
    spawnprotect_init(server);
    gameplay_physics_init(server);
    netguard_init(server);
    conduct_init(server);
    escape_watch_init(server);
    anticheat_init(server);
    audit_init(server);
}

int server_force_map(server_t* server, const char* name)
{
    if (name == NULL || *name == '\0' || strlen(name) >= sizeof(server->forced_map) || strchr(name, '/') != NULL ||
        strchr(name, '\\') != NULL)
    {
        return 0;
    }
    /* a name a 0.75 server or aos.party has it by: the folder it means. Off
       the network: this runs on the tick (Commands/Map.c, the plugins), and
       a download here would stall everyone */
    char folder[MAP_NAME_MAX + 1];
    if (map_resolve_alias(name, folder, sizeof folder, 0)) {
        name = folder;
    }
    if (!_srv_map_file_exists(name, "vxl", "rb") || !_srv_map_file_exists(name, "toml", "r")) {
        return 0;
    }
    snprintf(server->forced_map, sizeof(server->forced_map), "%s", name);
    return 1;
}

void server_reset(server_t* server)
{
    outfit_map_start(); /* every player's; the teams' last (Player Outfit) */
    if (profile_is_075()) {
        /* a new map: every 0.75 client turns every light off on Map Start */
        ext075_map_start(server);
    }
    /* the same for the 1.0 flashlights: a joiner is only told about the lights
       that are on, so a light left on here would never go off again */
    player_t *light_player, *light_tmp;
    HASH_ITER(hh, server->players, light_player, light_tmp)
    {
        proto10_bridge_light_off(light_player, "map");
    }
    /* and every seat (Proto10/Mount.c): the helicopters are new objects */
    mount_map_start(server);
    _server_init(server,
                 server->protocol.max_players,
                 server->server_name,
                 server->protocol.name_team[0],
                 server->protocol.name_team[1],
                 server->protocol.color_team[0].arr,
                 server->protocol.color_team[1].arr,
                 gamemode_current(server)->id,
                 1);
}

static void* _world_update(void)
{
    player_t *player, *tmp;
    HASH_ITER(hh, server.players, player, tmp)
    {
        on_player_update(&server, player);
        /* the movement event log keeps the cadence of the 0.75 WorldUpdate
           it used to accompany: once per 1/ups seconds per player */
        if (is_past_join_screen(player)) {
            uint64_t time = get_nanos();
            if (time - player->timers.time_since_last_wu >= (uint64_t) (NANO_IN_SECOND / player->ups)) {
                evt_move(player, player->is_bot ? "bot" : "net");
                if (profile_is_075()) {
                    legacy_send_world_update(&server, player);
                }
                player->timers.time_since_last_wu = get_nanos();
            }
        }
    }
    return 0;
}

/* a connection's player leaves the game: its transport's close, or a stale
   ENet peer the same client replaced by connecting again (Net/EnetConn.c) */
void server_conn_closed(server_t* server, conn_t* conn)
{
    uint8_t player_id = conn->player_id;
    if (player_id != CONN_NO_PLAYER) {
        player_t* player;
        HASH_FIND(hh, server->players, &player_id, sizeof(player_id), player);
        if (player == NULL) {
            LOG_ERROR("Server tried to disconnect non existent player!");
            return;
        }
        gamemode_player_out(server, player);
        plugins_on_leave(server, player);
        send_player_left(server, player);
        _srv_reset_player(server, player, 0, 1);
        server->protocol.num_players--;
        server->protocol.num_team_users[player->team]--;
        grenade_t* elt;
        uint32_t   counter = 0;
        DL_COUNT(player->grenade, elt, counter);
        if (counter == 0) {
            HASH_DEL(server->players, player);
            HASH_SORT(server->players, player_sort);
            free(player);
        }
        bots_on_player_leave(server);
    }
}

int server_conn_in_use(server_t* server, const conn_t* conn)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->conn == conn) {
            return 1;
        }
    }
    return 0;
}

void server_conn_connect(server_t* server, conn_t* conn, uint32_t connect_data)
{
    if (!netguard_allow(server, conn)) {
        conn->player_id = CONN_NO_PLAYER;
        conn_disconnect(conn, 0, CONN_CLOSE_NOW);
        return;
    }
    on_new_player_connection(server, conn, connect_data);
}

void server_conn_receive(server_t* server, conn_t* conn, const uint8_t* data, size_t len)
{
    uint8_t   player_id = conn->player_id;
    player_t* player;
    HASH_FIND(hh, server->players, &player_id, sizeof(player_id), player);
    if (player == NULL) {
        LOG_ERROR("Could not find player with ID %hhu", player_id);
        return;
    }
    /* each profile speaks one protocol (on_new_player_connection refuses
       the other): the 0.75 table, or the 1.0 session, whose bridge replays
       into the same 0.75 receive handlers */
    /* a flood is dropped before anything parses it (Flood.c) */
    if (!flood_take(server, player, FLOOD_PACKET)) {
        return;
    }
    if (profile_is_075()) {
        on_packet_received_075(server, player, data, len);
        return;
    }
    proto10_on_packet(server, player, data, len);
}

static void _server_update(server_t* server)
{
#ifdef SP_WEB_HOST
    worker_poll(server);
#else
    conn_enet_poll(server);
    webrtc_poll(server);
#endif
}

void stop_server(void)
{
    server.running = 0;
}

#ifndef _WIN32
static void _set_sigaction(const int sig, const struct sigaction *sigact, const char *sig_name)
{
    /*
    GCC recommends to not replace existing sigaction structs
    if they specify SIG_IGN
    */
    struct sigaction prev_act;
    sigaction(sig, NULL, &prev_act);
    if (prev_act.sa_handler == SIG_IGN) {
        // signal should be ignored - so let's not tamper with it
        return;
    }

    // sigaction returns 0 if success, -1 if error
    if (sigaction(sig, sigact, NULL)) {
        LOG_ERROR("Unable to set sigaction for %s", sig_name);
    }
}
#endif

static void _handle_signal(const int sig)
{
    switch (sig) {
        case SIGINT:
#ifdef SP_WEB_HOST
            stop_server(); /* no console to hand the Ctrl+C to */
#else
            handle_sigint();
#endif
            break;
        case SIGTERM:
            stop_server();
            break;
        default:
            LOG_ERROR("Received unhandled signal %d", sig);
            break;
    }
}

#ifdef _WIN32
/* Windows has no sigaction; signal() covers the two handled here (Ctrl+C in
   a console, SIGTERM raised in-process). The client stops a listen server
   with TerminateProcess, which runs no handler at all. */
static inline void _apply_signal_handlers(void)
{
    signal(SIGINT, _handle_signal);
    signal(SIGTERM, _handle_signal);
}
#else
static inline void _apply_signal_handlers(void)
{
    struct sigaction sigact;
    sigact.sa_handler = _handle_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;

    _set_sigaction(SIGINT, &sigact, "SIGINT");
    _set_sigaction(SIGTERM, &sigact, "SIGTERM");
}
#endif

/* the ticks' measurements (the Tick: lines every 30 s) and their schedule,
   kept between ticks: the native loop and the browser's main loop
   (SP_WEB_HOST, a worker cannot sleep between ticks: its messages only come
   while it is idle) run the same tick */
static struct
{
    uint64_t tick_stats_at, tick_total, tick_max, tick_count;
    uint64_t bots_max, physics_max, net_max; /* the parts of the worst ticks */
    /* summed per part over the window, for the averages of the Tick parts line */
    uint64_t sum_phys, sum_sec[6], sum_bots, sum_mode;
    uint64_t next_tick;
} ticks;

/* what the Tick: line says went out: ENet's own count (with its headers),
   or the worker's (its packets: a MessageChannel adds nothing) */
static size_t _net_peers(void)
{
#ifdef SP_WEB_HOST
    return worker_peer_count();
#else
    return server.host->connectedPeers;
#endif
}

static uint64_t _net_sent(void)
{
#ifdef SP_WEB_HOST
    return worker_sent_bytes();
#else
    return server.host->totalSentData;
#endif
}

static void _net_sent_reset(void)
{
#ifdef SP_WEB_HOST
    worker_sent_reset();
#else
    server.host->totalSentData = 0;
#endif
}

/* one tick of the game; returns how long it took (real nanos) */
static uint64_t _tick(void)
{
    uint64_t tick_start = get_real_nanos();
    pthread_mutex_lock(&server_lock);
    uint64_t part = get_real_nanos();
    _calculate_physics();
    uint64_t physics_time = get_real_nanos() - part;
    part                  = get_real_nanos();
    /* the parts of the network share, for the Slow tick line */
    uint64_t sec[7];
    sec[0] = get_real_nanos();
    _server_update(&server);
    sec[1] = get_real_nanos();
    _world_update();
    sec[2] = get_real_nanos();
    proto10_bridge_tick(&server);
    sec[3] = get_real_nanos();
    proto10_world_tick(&server);
    sky_tick(&server);
    if (profile_is_075()) {
        /* the terrain as block actions, the moved intel and tents, the
           ZeroSpades extensions' pacing */
        legacy_world_tick(&server);
        ext075_tick(&server);
    }
    sec[4] = get_real_nanos();
    proto10_session_tick(&server);
    sec[5] = get_real_nanos();
    for_players(&server);
    hostcmd_process_queue(&server);
    rcon_tick(&server);
    master_tick(&server);
    if (profile_is_075()) {
        master075_tick(&server); /* the public 0.75 masters */
    }
    sec[6]            = get_real_nanos();
    uint64_t net_time = get_real_nanos() - part;
    part              = get_real_nanos();
    bots_update(&server);
    uint64_t bots_time = get_real_nanos() - part;
    part               = get_real_nanos();
    gamemode_tick(&server);
    escape_watch_tick(&server);
    uint64_t mode_time = get_real_nanos() - part;
    pthread_mutex_unlock(&server_lock);
    uint64_t tick_time = get_real_nanos() - tick_start;
    ticks.sum_phys += physics_time;
    for (int k = 0; k < 6; k++) {
        ticks.sum_sec[k] += sec[k + 1] - sec[k];
    }
    ticks.sum_bots += bots_time;
    ticks.sum_mode += mode_time;
    if (tick_time > 20 * NANO_IN_MILLI) {
        /* one line per stall, its parts: the Tick line only says
           "network and the rest" */
#define SECT_MS(a, b) ((double) (sec[b] - sec[a]) / (double) NANO_IN_MILLI)
        LOG_INFO("Slow tick: %.1f ms (physics %.1f, enet %.1f, world update %.1f, bridge %.1f, terrain %.1f, "
                 "sessions %.1f, players %.1f, bots %.1f, mode %.1f)",
                 (double) tick_time / (double) NANO_IN_MILLI, (double) physics_time / (double) NANO_IN_MILLI,
                 SECT_MS(0, 1), SECT_MS(1, 2), SECT_MS(2, 3), SECT_MS(3, 4), SECT_MS(4, 5), SECT_MS(5, 6),
                 (double) bots_time / (double) NANO_IN_MILLI, (double) mode_time / (double) NANO_IN_MILLI);
#undef SECT_MS
    }
    ticks.tick_total += tick_time;
    ticks.tick_count++;
    if (tick_time > ticks.tick_max) {
        ticks.tick_max = tick_time;
    }
    ticks.bots_max    = bots_time > ticks.bots_max ? bots_time : ticks.bots_max;
    ticks.physics_max = physics_time > ticks.physics_max ? physics_time : ticks.physics_max;
    ticks.net_max     = net_time > ticks.net_max ? net_time : ticks.net_max;
    if (get_real_nanos() - ticks.tick_stats_at >= 30ULL * NANO_IN_SECOND) {
        /* what goes out to each connected client (ENet's own count, with
           its headers): the figure a host's upload must carry */
        double window = (double) (get_real_nanos() - ticks.tick_stats_at) / (double) NANO_IN_SECOND;
        size_t peers  = _net_peers();
        LOG_INFO("Tick: %llu ticks, avg %.2f ms, max %.2f ms (max of: bots %.2f, physics %.2f, network and "
                 "the rest %.2f); out %.1f KB/s per client (%zu clients); flood packets dropped %u",
                 (unsigned long long) ticks.tick_count, (double) ticks.tick_total / (double) ticks.tick_count / (double) NANO_IN_MILLI,
                 (double) ticks.tick_max / (double) NANO_IN_MILLI, (double) ticks.bots_max / (double) NANO_IN_MILLI,
                 (double) ticks.physics_max / (double) NANO_IN_MILLI, (double) ticks.net_max / (double) NANO_IN_MILLI,
                 peers ? (double) _net_sent() / window / 1024.0 / (double) peers : 0.0, peers,
                 flood_dropped_packets());
        /* where an average tick goes (measure first) */
#define AVG_US(v) ((double) (v) / (double) ticks.tick_count / 1000.0)
        LOG_INFO("Tick parts (avg us): physics %.0f, enet %.0f, world update %.0f, bridge %.0f, terrain %.0f, "
                 "sessions %.0f, players %.0f, bots %.0f, mode %.0f",
                 AVG_US(ticks.sum_phys), AVG_US(ticks.sum_sec[0]), AVG_US(ticks.sum_sec[1]), AVG_US(ticks.sum_sec[2]), AVG_US(ticks.sum_sec[3]),
                 AVG_US(ticks.sum_sec[4]), AVG_US(ticks.sum_sec[5]), AVG_US(ticks.sum_bots), AVG_US(ticks.sum_mode));
#undef AVG_US
        ticks.sum_phys = ticks.sum_bots = ticks.sum_mode = 0;
        for (int k = 0; k < 6; k++) {
            ticks.sum_sec[k] = 0;
        }
        /* the same window by packet type and, for the bodies, by distance */
        proto10_netstats_log(window, peers, _net_sent());
        _net_sent_reset();
        ticks.tick_stats_at = get_real_nanos();
        ticks.tick_total = ticks.tick_max = ticks.tick_count = 0;
        ticks.bots_max = ticks.physics_max = ticks.net_max = 0;
    }
    return tick_time;
}

/* after a tick that took tick_time: how long until the next one is due
   (real nanos, 0 at once) */
static uint64_t _tick_after(uint64_t tick_time)
{
    /* Run at 60 Hz. Spinning on sleep(0) starved every other thread
       (the console could not even grab the lock) and burned a core.
       Each tick has its own time, 1/60 s after the last one, and the
       sleep lasts until then: a sleep that wakes late (macOS wakes a
       few ms late) is made up on the next tick. A full 1/60 s after the
       tick's own work ran at ~45 Hz */
    /* (SP_TIME_SCALE: the game's clock runs faster, and the ticks come
       as much more often; tick times and their log stay real) */
    uint64_t period = (uint64_t) ((double) NANO_60TPS / get_time_scale());
    /* SP_FAST: the next tick at once, its 1/60 s of game time counted
       (Util/Nanos.c); SP_EXIT_AFTER ends the run after that many game
       seconds, a benchmark's end */
    if (get_fast_mode()) {
        static double exit_after = -1.0;
        static uint64_t fast_began, fast_real, fast_count;
        if (exit_after < 0.0) {
            const char* env = getenv("SP_EXIT_AFTER");
            exit_after      = env != NULL ? atof(env) : 0.0;
            fast_began      = get_nanos();
        }
        fast_real += tick_time;
        fast_count++;
        fast_tick();
        if (exit_after > 0.0 && (double) (get_nanos() - fast_began) / NANO_IN_SECOND >= exit_after) {
            /* what a tick costs, for a benchmark (the Tick: line comes
               every 30 real seconds, longer than a fast run) */
            LOG_INFO("Fast run: %llu ticks, avg %.3f ms a tick", (unsigned long long) fast_count,
                     (double) fast_real / (double) fast_count / (double) NANO_IN_MILLI);
            LOG_INFO("Exiting after %.0f game seconds (SP_EXIT_AFTER)", exit_after);
            server.running = 0;
        }
        return 0;
    }
    ticks.next_tick += period;
    uint64_t now_tick = get_real_nanos();
    if (now_tick + period < ticks.next_tick || now_tick > ticks.next_tick + 100ULL * NANO_IN_MILLI) {
        ticks.next_tick = now_tick; /* far behind (a stall) or the clock jumped: start again from now */
    } else if (now_tick < ticks.next_tick) {
        return ticks.next_tick - now_tick;
    }
    return 0;
}

/* the tick is a main loop of Emscripten's (the browser's worker); the
   Switch's local server runs the native loop on a thread of its own */
#if defined(SP_WEB_HOST) && !defined(SP_SWITCH_HOST)
#define SP_WEB_LOOP 1
#endif

#ifdef SP_SWITCH_HOST
/* the game stops its local server (client/switch/localhost.c): the loop
   ends at the next tick and the server shuts down as it does on /shutdown */
void sd_server_request_stop(void)
{
    server.running = 0;
}
#endif

static void _server_shutdown(void)
{
    // Server is shutting down, kick all players
    player_t *player, *tmp;
    HASH_ITER(hh, server.players, player, tmp)
    {
        /* bots have no connection: this crashed every shutdown with bots in it */
        if (player->conn != NULL) {
            conn_disconnect(player->conn, REASON_KICKED, CONN_CLOSE_NOW);
        }
    }

    if (profile_is_075()) {
        master075_stop(&server);
    }
    master_stop();
    webrtc_stop();
    rcon_stop();
    plugins_shutdown(&server);
    free_all_commands(&server);
    free_all_players(&server);

    _string_nodes_free(server.welcome_messages);
    _string_nodes_free(server.s_map.map_list);
    _string_nodes_free(server.periodic_messages);

    map_store_free(&server.s_map.map);

    pthread_mutex_destroy(&server_lock);

    printf("\n");
    LOG_STATUS("Exiting");
#ifndef SP_WEB_HOST
    conn_enet_stop();
#endif
}

#ifdef SP_WEB_LOOP
/* how often the worker's loop looks whether a tick is due: a timer of the
   worker (no animation frames there), a few times a tick so the 60 Hz
   schedule keeps its time */
#define WEB_FRAME_RATE 250

static void _web_frame(void)
{
    if (!server.running) {
        emscripten_cancel_main_loop();
        _server_shutdown();
        emscripten_force_exit(0);
        return;
    }
    if (!get_fast_mode() && get_real_nanos() < ticks.next_tick) {
        return;
    }
    _tick_after(_tick());
}
#endif

void server_start(server_args args, int startup_bots)
{
    server.global_timers.time_since_start = get_nanos();
    LOG_STATUS("Welcome to %s", GAME_SERVER_TITLE);
    LOG_STATUS("Initializing ENet");

    if (enet_initialize() != 0) {
        LOG_ERROR("Failed to initalize ENet");
        exit(EXIT_FAILURE);
    }
    atexit(enet_deinitialize);

    if (pthread_mutex_init(&server_lock, NULL) != 0) {
        LOG_ERROR("Server mutex failed to initialize");
        exit(EXIT_FAILURE);
    }

#ifdef SP_WEB_HOST
    /* no socket: the page hands its connections over (Net/Worker.c) */
    LOG_STATUS("Creating the browser's server (port %d is only its name)", args.port);
    server.host = NULL;
#else
    LOG_STATUS("Creating server at port %d", args.port);

    server.host = conn_enet_start(&server, args.port, args.connections, args.channels, args.in_bandwidth,
                                  args.out_bandwidth);
    if (server.host == NULL) {
        LOG_ERROR("Failed to create server");
        exit(EXIT_FAILURE);
    }
#endif

    server.port = args.port;

    LOG_STATUS("Intializing server");
    server.players                = NULL;
    server.running                = 1;
    server.console_enabled        = _stdin_is_terminal();
    server.s_map.map_list         = args.map_list;
    server.s_map.map_count        = args.map_count;
    server.s_map.rotation_mode    = args.map_rotation_mode;
    server.welcome_messages       = args.welcome_message_list;
    server.welcome_messages_count = args.welcome_message_list_len;
    server.periodic_messages      = args.periodic_message_list;
    server.periodic_message_count = args.periodic_message_list_len;
    server.periodic_delays        = args.periodic_delays;
    server.capture_limit          = args.capture_limit;
    server.forced_gamemode        = -1;
    snprintf(server.preset_name, sizeof server.preset_name, "%s", args.preset_name != NULL ? args.preset_name : "");
    /* before the first map: its plugins' game.command must see which names
       the server has */
    command_populate_all(&server);
    _server_init(&server,
                 args.connections,
                 args.server_name,
                 args.team1_name,
                 args.team2_name,
                 args.team1_color.arr,
                 args.team2_color.arr,
                 args.gamemode,
                 0);

    /* no init_packets: the 0.75 dispatcher is gone, the bridge calls the
       receive handlers directly */
    jetpack_init(&server);
    sentry_init(&server);
    bots_init(&server);
    rcon_start(&server);
    webrtc_start(&server);
    master_start(&server);
    if (profile_is_075()) {
        master075_start(&server); /* the public 0.75 masters */
    }

    /* the master list belongs to 0.75 clients, which cannot join a 1.0
       server; the "master" config key is still read so old configs load */
    if (args.master && !profile_is_075()) {
        LOG_WARNING("Not announcing on the 0.75 master list: this server speaks protocol 1.0 only");
    }
    server.manager_passwd = args.manager_password;
    server.admin_passwd   = args.admin_password;
    server.mod_passwd     = args.mod_password;
    server.guard_passwd   = args.guard_password;
    server.trusted_passwd = args.trusted_password;

    if (server.running) {
        LOG_STATUS("Server started");
#ifdef SP_SWITCH_HOST
        sd_host_server_started();
#endif
    }

#ifndef SP_WEB_HOST
    rl_catch_signals = 0;
    pthread_t console;
    pthread_create(&console, NULL, server_console, (void*) &server);
    pthread_detach(console);
#endif

    _apply_signal_handlers();

    /* bots requested at startup are created here, inside the server lifecycle,
       so they exist while the tick runs (creating them after server_start()
       returns happens during shutdown and they never reach the game). */
    server_add_startup_bots(startup_bots);

    ticks.tick_stats_at = get_real_nanos();
    ticks.next_tick     = get_real_nanos();
#ifdef SP_WEB_LOOP
    /* the worker's own loop calls the tick (_web_frame) and server_start
       never returns, as the native loop never did until the shutdown */
    emscripten_set_main_loop(_web_frame, WEB_FRAME_RATE, 1);
#else
    while (server.running) {
        uint64_t wait = _tick_after(_tick());
        if (wait > 0) {
            struct timespec frame = {(time_t) (wait / NANO_IN_SECOND), (long) (wait % NANO_IN_SECOND)};
            nanosleep(&frame, NULL);
        }
    }
    _server_shutdown();
#endif
}

