// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/MapStore.h>
#include <Server/Plugins.h>
#include <modes/modes.h>
#include <strings.h>
#include <tomlc99/toml.h>
#include <Server/Bots.h>
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Ctf.h>
#include <Server/Gamemodes/CtfInternal.h>
#include <Server/Proto10/Flags.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Proto10/GameMode.h>
#include <uthash/uthash.h>
#include <proto10/codec.h>
#include <Server/Perks.h>
#include <Server/Proto10/Explosives.h>
#include <stdio.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Player.h>
#include <Server/Server.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Types.h>
#include <Util/Weapon.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

/* a cell for the intel's and the tents' rests: past the top of the world
   is sky, under its bottom solid, and off its sides nothing. A tent at z 0
   (a map built to the top) was moved up to z -1 by a block put beside it,
   and the next look read the map out of bounds and crashed the server
   (111ButteII, Bikini_Atoll_240, Four_way...) */
static int _ctf_solid(server_t* server, int x, int y, int z)
{
    map_store_t* map = &server->s_map.map;
    if (z < 0) {
        return 0;
    }
    if (z >= map->size_z) {
        return 1;
    }
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    return map_solid(map, x, y, z);
}

static uint8_t check_under_tent(server_t* server, uint8_t team)
{
    uint8_t count = 0;
    for (int x = server->protocol.gamemode.base[team].x - 1; x <= server->protocol.gamemode.base[team].x; x++) {
        for (int y = server->protocol.gamemode.base[team].y - 1; y <= server->protocol.gamemode.base[team].y; y++) {
            if (_ctf_solid(server, x, y, server->protocol.gamemode.base[team].z) == 0) {
                count++;
            }
        }
    }
    return count;
}

static uint8_t check_under_intel(server_t* server, uint8_t team)
{
    uint8_t ret = 0;
    if (_ctf_solid(server,
                        server->protocol.gamemode.intel[team].x,
                        server->protocol.gamemode.intel[team].y,
                        server->protocol.gamemode.intel[team].z) == 0)
    {
        ret = 1;
    }
    return ret;
}

static uint8_t check_in_tent(server_t* server, uint8_t team)
{
    uint8_t    ret      = 0;
    vector3f_t checkPos = server->protocol.gamemode.base[team];
    checkPos.z--;
    if (_ctf_solid(server, (int) checkPos.x, (int) checkPos.y, (int) checkPos.z))
    { // the tent cell, then its three neighbours at the same height
        ret = 1;
    } else if (_ctf_solid(server, (int) checkPos.x - 1, (int) checkPos.y, (int) checkPos.z)) {
        ret = 1;
    } else if (_ctf_solid(server, (int) checkPos.x, (int) checkPos.y - 1, (int) checkPos.z)) {
        ret = 1;
    } else if (_ctf_solid(server, (int) checkPos.x - 1, (int) checkPos.y - 1, (int) checkPos.z)) {
        ret = 1;
    }

    return ret;
}

static uint8_t check_in_intel(server_t* server, uint8_t team)
{
    uint8_t    ret      = 0;
    vector3f_t checkPos = server->protocol.gamemode.intel[team];
    checkPos.z--;
    if (_ctf_solid(server, (int) checkPos.x, (int) checkPos.y, (int) checkPos.z)) {
        ret = 1;
    }
    return ret;
}

/* pyspades' vector_collision: a player takes the intel (or rests in the tent)
   from a box round it, not from its exact cell. The box is CTF_PICKUP_RANGE
   across in x and y, and in z only above: z grows downwards and the intel's z
   is the cell it rests on, so a body level with or under that cell is under
   the intel, not on it. A standing body is about 2 to 3 blocks above the cell,
   a crouched one about 1 to 2, and a hop over it reaches ~4. The first version
   asked for the exact cell and an exact z, on integers, so a player running
   over the intel never took it. */
#define CTF_PICKUP_RANGE 3.0f
#define CTF_PICKUP_ABOVE 4.5f

static uint8_t check_player_in_tent(server_t* server, player_t* player)
{
    uint8_t ret = 0;
    if (player->alive == 0) {
        return ret;
    }
    vector3f_t pos  = player->movement.position;
    /* the 2x2 cells are x in [x-1, x] and y in [y-1, y], so its middle is
       tent.x, tent.y */
    vector3f_t tent = server->protocol.gamemode.base[player->team];
    float      dz   = tent.z - pos.z;
    if (fabsf(pos.x - tent.x) < CTF_PICKUP_RANGE && fabsf(pos.y - tent.y) < CTF_PICKUP_RANGE && dz > 0.0f &&
        dz <= CTF_PICKUP_ABOVE)
    {
        ret = 1;
    }
    return ret;
}


static void move_intel_and_tent_down(server_t* server)
{
    while (check_under_intel(server, 0)) {
        vector3f_t newPos = {server->protocol.gamemode.intel[0].x,
                             server->protocol.gamemode.intel[0].y,
                             server->protocol.gamemode.intel[0].z + 1};
        server->protocol.gamemode.intel[0] = newPos;
    }
    while (check_under_intel(server, 1)) {
        vector3f_t newPos = {server->protocol.gamemode.intel[1].x,
                             server->protocol.gamemode.intel[1].y,
                             server->protocol.gamemode.intel[1].z + 1};
        server->protocol.gamemode.intel[1] = newPos;
    }
    while (check_under_tent(server, 0) == 4) {
        vector3f_t newPos = {server->protocol.gamemode.base[0].x,
                             server->protocol.gamemode.base[0].y,
                             server->protocol.gamemode.base[0].z + 1};
        server->protocol.gamemode.base[0] = newPos;
    }
    while (check_under_tent(server, 1) == 4) {
        vector3f_t newPos = {server->protocol.gamemode.base[1].x,
                             server->protocol.gamemode.base[1].y,
                             server->protocol.gamemode.base[1].z + 1};
        server->protocol.gamemode.base[1] = newPos;
    }
}

static void moveIntelAndTentUp(server_t* server)
{
    if (check_in_tent(server, 0)) {
        vector3f_t newTentPos = server->protocol.gamemode.base[0];
        newTentPos.z -= 1;
        server->protocol.gamemode.base[0] = newTentPos;
    } else if (check_in_tent(server, 1)) {
        vector3f_t newTentPos = server->protocol.gamemode.base[1];
        newTentPos.z -= 1;
        server->protocol.gamemode.base[1] = newTentPos;
    } else if (check_in_intel(server, 1)) {
        vector3f_t newIntelPos = server->protocol.gamemode.intel[1];
        newIntelPos.z -= 1;
        server->protocol.gamemode.intel[1] = newIntelPos;
    } else if (check_in_intel(server, 0)) {
        vector3f_t newIntelPos = server->protocol.gamemode.intel[0];
        newIntelPos.z -= 1;
        server->protocol.gamemode.intel[0] = newIntelPos;
    }
}

static uint8_t check_player_on_intel(server_t* server, player_t* player, uint8_t team)
{
    uint8_t ret = 0;
    if (player->alive == 0) {
        return ret;
    }
    vector3f_t pos   = player->movement.position;
    vector3f_t intel = server->protocol.gamemode.intel[team];
    float      dz    = intel.z - pos.z;
    if (fabsf(pos.x - intel.x) < CTF_PICKUP_RANGE && fabsf(pos.y - intel.y) < CTF_PICKUP_RANGE && dz > 0.0f &&
        dz <= CTF_PICKUP_ABOVE)
    {
        ret = 1;
    }
    return ret;
}


/* The intel and the tent are walked onto, and the tent covers the 2x2 cells
   ending at its position (check_player_in_tent), so all four must be land. */
static uint8_t intel_tent_spot_dry(server_t* server, float x, float y)
{
    if (x < 1.f || y < 1.f || x >= server->s_map.map.size_x || y >= server->s_map.map.size_y) {
        return 0;
    }
    for (int cx = (int) x - 1; cx <= (int) x; cx++) {
        for (int cy = (int) y - 1; cy <= (int) y; cy++) {
            if (is_water_column(server, (uint16_t) cx, (uint16_t) cy)) {
                return 0;
            }
        }
    }
    return 1;
}

/* The base's ground. The top block of a column is a roof, a building's upper
   floor, a bridge or a tree top as often as the ground: city's intel lay on
   a roof 21 blocks over the street (92,229,27) and Broville's on a tower
   at the top of the world (92,229,2), and no bot ever reached either (every
   plan partial at the planner's 40000 nodes, not one pickup in a match). A
   ground column has solid under its top block for CTF_GROUND_DEPTH cells
   (a roof, a floor, a bridge or a tree has air close under), and the base's
   ground is the stretch of them joined by steps of one block (a player walks
   it) with the most columns in the team's rectangle: a solid tower's top is
   ground but not joined to the rest */
#define CTF_GROUND_DEPTH  8
#define CTF_GROUND_MARGIN 16 /* columns round the rectangle the stretch may use */

typedef struct
{
    int      x0, y0, w, h;
    int32_t* label; /* the stretch of each column of the box, 0 not ground */
    int8_t*  top;
    int32_t  ground; /* the base's stretch, 0 when the rectangle has none */
} ctf_ground_t;

static int _ground_top(server_t* server, int x, int y)
{
    map_store_t* map = &server->s_map.map;
    if (is_water_column(server, (uint16_t) x, (uint16_t) y)) {
        return -1;
    }
    int top = map_top(map, (uint16_t) x, (uint16_t) y);
    for (int z = top + 1; z <= top + CTF_GROUND_DEPTH && z < map->size_z; z++) {
        if (!map_solid(map, (uint16_t) x, (uint16_t) y, (uint16_t) z)) {
            return -1;
        }
    }
    return top;
}

static void _ground_find(server_t* server, const quad3d_t* rect, ctf_ground_t* g)
{
    memset(g, 0, sizeof(*g));
    int sx = server->s_map.map.size_x, sy = server->s_map.map.size_y;
    int x0 = (int) fminf(rect->from.x, rect->to.x) - CTF_GROUND_MARGIN;
    int y0 = (int) fminf(rect->from.y, rect->to.y) - CTF_GROUND_MARGIN;
    int x1 = (int) fmaxf(rect->from.x, rect->to.x) + CTF_GROUND_MARGIN;
    int y1 = (int) fmaxf(rect->from.y, rect->to.y) + CTF_GROUND_MARGIN;
    x0     = x0 < 0 ? 0 : x0;
    y0     = y0 < 0 ? 0 : y0;
    x1     = x1 >= sx ? sx - 1 : x1;
    y1     = y1 >= sy ? sy - 1 : y1;
    if (x1 < x0 || y1 < y0) {
        return;
    }
    g->x0          = x0;
    g->y0          = y0;
    g->w           = x1 - x0 + 1;
    g->h           = y1 - y0 + 1;
    size_t   n     = (size_t) g->w * g->h;
    int32_t* stack = malloc(n * sizeof(int32_t));
    g->label       = calloc(n, sizeof(int32_t));
    g->top         = malloc(n);
    if (stack == NULL || g->label == NULL || g->top == NULL) {
        free(stack);
        free(g->label);
        free(g->top);
        memset(g, 0, sizeof(*g));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        g->top[i] = (int8_t) _ground_top(server, x0 + (int) (i % g->w), y0 + (int) (i / g->w));
    }
    int32_t next = 0, best_inside = 0;
    for (size_t i = 0; i < n; i++) {
        if (g->top[i] < 0 || g->label[i] != 0) {
            continue;
        }
        int32_t label = ++next, inside = 0, sp = 0;
        g->label[i]   = label;
        stack[sp++]   = (int32_t) i;
        while (sp > 0) {
            int32_t c = stack[--sp];
            int     cx = c % g->w, cy = c / g->w;
            int     wx = x0 + cx, wy = y0 + cy;
            if (wx >= fminf(rect->from.x, rect->to.x) && wx < fmaxf(rect->from.x, rect->to.x) &&
                wy >= fminf(rect->from.y, rect->to.y) && wy < fmaxf(rect->from.y, rect->to.y))
            {
                inside++;
            }
            static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int d = 0; d < 4; d++) {
                int nx = cx + dirs[d][0], ny = cy + dirs[d][1];
                if (nx < 0 || ny < 0 || nx >= g->w || ny >= g->h) {
                    continue;
                }
                int32_t k = ny * g->w + nx;
                if (g->top[k] >= 0 && g->label[k] == 0 && abs(g->top[k] - g->top[c]) <= 1) {
                    g->label[k] = label;
                    stack[sp++] = k;
                }
            }
        }
        if (inside > best_inside) {
            best_inside = inside;
            g->ground   = label;
        }
    }
    free(stack);
}

static void _ground_free(ctf_ground_t* g)
{
    free(g->label);
    free(g->top);
}

/* the 2x2 cells intel_tent_spot_dry checks are all the base's ground */
static uint8_t _ground_spot(const ctf_ground_t* g, float x, float y)
{
    for (int cx = (int) x - 1; cx <= (int) x; cx++) {
        for (int cy = (int) y - 1; cy <= (int) y; cy++) {
            int bx = cx - g->x0, by = cy - g->y0;
            if (bx < 0 || by < 0 || bx >= g->w || by >= g->h || g->label[by * g->w + bx] != g->ground) {
                return 0;
            }
        }
    }
    return 1;
}

/* a script's places (ctf_set_spot): [team][0 intel, 1 tent] */
static struct
{
    uint8_t set;
    float   x, y;
} ctf_spots[2][2];

/* kind: MAP_META_FLAG (the intel) or MAP_META_BASE (the tent) */
static vector3f_t set_intel_tent_spawn_point(server_t* server, uint8_t team, int kind)
{
    vector3f_t position;
    int        slot   = kind == MAP_META_BASE ? 1 : 0;
    uint8_t    script = team < 2 && ctf_spots[team][slot].set;
    /* the map's author fixed it (get_entity_location in the .txt): that
       cell, every round and after every capture, as pyspades does */
    if (!script && map_meta_entity(server, team, kind, &position)) {
        return position;
    }

    quad3d_t* spawn = server->protocol.spawns + team;

    float dx = spawn->to.x - spawn->from.x;
    float dy = spawn->to.y - spawn->from.y;

    /* Same trap as player spawns: the rectangle can be mostly sea (Pinpoint's
       intel landed in the water and no one could walk to it). Keep the
       random pick when it is dry, retry otherwise, then search outwards. A
       script's place: that one, or the dry ground round it */
    uint8_t found = 0;
    if (script) {
        position.x = ctf_spots[team][slot].x;
        position.y = ctf_spots[team][slot].y;
        found      = intel_tent_spot_dry(server, position.x, position.y);
    }
    /* on the base's ground first (_ground_find), then any dry spot as before
       when the rectangle has no ground at all */
    ctf_ground_t ground = {0};
    if (!found) {
        _ground_find(server, spawn, &ground);
    }
    for (int pass = ground.ground ? 0 : 1; pass < 2 && !found; pass++) {
        for (int attempt = 0; attempt < 32 && !found && !script; attempt++) {
            position.x = spawn->from.x + dx * gen_rand(&server->rand);
            position.y = spawn->from.y + dy * gen_rand(&server->rand);
            found      = intel_tent_spot_dry(server, position.x, position.y) &&
                    (pass == 1 || _ground_spot(&ground, position.x, position.y));
        }
        float cx = script ? ctf_spots[team][slot].x : spawn->from.x + dx * 0.5f;
        float cy = script ? ctf_spots[team][slot].y : spawn->from.y + dy * 0.5f;
        for (int r = 2; r <= 128 && !found; r += 2) {
            int steps = 8 + r;
            for (int s = 0; s < steps && !found; s++) {
                float a  = (float) s * 2.f * (float) M_PI / (float) steps;
                float px = cx + cosf(a) * (float) r;
                float py = cy + sinf(a) * (float) r;
                if (intel_tent_spot_dry(server, px, py) && (pass == 1 || _ground_spot(&ground, px, py))) {
                    position.x = px;
                    position.y = py;
                    found      = 1;
                }
            }
        }
    }
    uint8_t on_ground = ground.ground && found && _ground_spot(&ground, position.x, position.y);
    _ground_free(&ground);
    if (!found) {
        LOG_WARNING("No dry spot for team %u intel/tent, placing it in the water", team);
    }
    position.z = map_top(&server->s_map.map, position.x, position.y);
    evt_log(NULL, "ctf_place", "team=%u kind=%s at=%.0f,%.0f,%.0f ground=%u", team, slot ? "tent" : "intel",
            position.x, position.y, position.z, on_ground);
    return position;
}

static void _ctf_base_snapshot(server_t* server);

void ctf_set_spot(server_t* server, uint8_t team, int kind, float x, float y)
{
    if (team > 1) {
        return;
    }
    gamemode_vars_t* gm         = &server->protocol.gamemode;
    int              slot       = kind == MAP_META_BASE ? 1 : 0;
    ctf_spots[team][slot].set   = 1;
    ctf_spots[team][slot].x     = x;
    ctf_spots[team][slot].y     = y;
    vector3f_t p                = set_intel_tent_spawn_point(server, team, kind);
    if (slot == 1) {
        gm->base[team]   = p;
        gm->base[team].x = floorf(p.x);
        gm->base[team].y = floorf(p.y);
    } else if (!gm->intel_held[team]) {
        gm->intel[team] = p;
        _ctf_base_snapshot(server); /* the base the guard mends is round the intel */
    }
    evt_log(NULL, "ctf_spot", "team=%u kind=%s at=%.0f,%.0f,%.0f", team, slot ? "tent" : "intel", p.x, p.y, p.z);
}

/* whether each team's intel lies dropped (not at its base): the state the
   1.0 clients get cannot tell otherwise, both are only a position */
static uint8_t ctf_dropped[2];
/* where each team's intel last lay at its base: a Miner tunnels on there
   while a mate carries it home (it comes back to its spawn area after the
   capture) */
static vector3f_t ctf_home[2];

/* the intel of team changed hands: protocol 1.0 delta and perks */
static void _ctf_event(server_t* server, player_t* player, uint8_t team, p10_gm_event_t event)
{
    if (event == P10_GM_EVENT_PICKUP && !ctf_dropped[team] && player != NULL) {
        ctf_home[team] = player->movement.position;
        ctf_home[team].z += 2.4f;
    }
    ctf_dropped[team] = event == P10_GM_EVENT_DROP;
    proto10_gamemode_intel_event(server, team, event);
    plugins_on_intel(server, player,
                     event == P10_GM_EVENT_PICKUP    ? "pickup"
                     : event == P10_GM_EVENT_CAPTURE ? "capture"
                                                     : "drop");
}

static uint8_t _enemy(const player_t* player)
{
    return player->team == TEAM_A ? TEAM_B : TEAM_A;
}

static void _ctf_pickup(server_t* server, player_t* player)
{
    uint8_t team = _enemy(player);
    if (server->protocol.num_players == 0 || player->has_intel == 1 || server->protocol.gamemode.intel_held[team] == 1) {
        return;
    }
    player->has_intel                                         = 1;
    server->protocol.gamemode.player_intel_team[player->team] = player->id;
    server->protocol.gamemode.intel_held[team]                = 1;
    _ctf_event(server, player, team, P10_GM_EVENT_PICKUP);
}

/* player_intel_team keeps the carrier: the capture event finds it there to
   give the perk progress */
static void _ctf_capture(server_t* server, player_t* player)
{
    uint8_t team = _enemy(player);
    if (server->protocol.num_players == 0 || player->has_intel == 0 || server->protocol.gamemode.intel_held[team] == 0) {
        return;
    }
    player->has_intel                          = 0;
    server->protocol.gamemode.intel_held[team] = 0;
    player->captures++;
    _ctf_event(server, player, team, P10_GM_EVENT_CAPTURE);
}

static void _ctf_drop(server_t* server, player_t* player)
{
    uint8_t team = _enemy(player);
    if (server->protocol.num_players == 0 || player->has_intel == 0 || server->protocol.gamemode.intel_held[team] == 0) {
        return;
    }
    if (server->protocol.current_gamemode == GAME_MODE_BABEL) {
        /* back on the tower, and the carrier's own intel with it (it was
           hidden while the enemy one was carried) */
        server->protocol.gamemode.intel[team].x = (float) server->s_map.map.size_x / 2;
        server->protocol.gamemode.intel[team].y = (float) server->s_map.map.size_y / 2;
        server->protocol.gamemode.intel[team].z =
        map_top(&server->s_map.map, server->s_map.map.size_x / 2, server->s_map.map.size_y / 2);
        server->protocol.gamemode.intel[player->team] = server->protocol.gamemode.intel[team];
    } else {
        /* on the floor under the carrier, as pyspades' get_z from its
           height: the column's top block was the roof over a carrier who
           died indoors, and the bots sent for it stood under the roof for
           the rest of the round (Broville, `bot_path action=stuck
           goal=take-intel dist=0.0 floor=52 goal_floor=37`) */
        map_store_t* map = &server->s_map.map;
        int       x   = (int) player->movement.position.x, y = (int) player->movement.position.y;
        int       z   = (int) player->movement.position.z + 1;
        z             = z < 0 ? 0 : z;
        while (z < map->size_z - 1 && !map_solid(map, (uint16_t) x, (uint16_t) y, (uint16_t) z)) {
            z++;
        }
        server->protocol.gamemode.intel[team].x = x;
        server->protocol.gamemode.intel[team].y = y;
        server->protocol.gamemode.intel[team].z = z;
    }
    player->has_intel                                         = 0;
    /* nobody: 0xFF, never a player id (32 was the 0.75 "no player", and a
       carrier with an id from 32 up, with 64 players, read as nobody) */
    server->protocol.gamemode.player_intel_team[player->team] = 0xFF;
    server->protocol.gamemode.intel_held[team]                = 0;

    LOG_INFO("Dropping intel at X: %d, Y: %d, Z: %d",
             (int) server->protocol.gamemode.intel[team].x,
             (int) server->protocol.gamemode.intel[team].y,
             (int) server->protocol.gamemode.intel[team].z);
    _ctf_event(server, player, team, P10_GM_EVENT_DROP);
}

void handleTentAndIntel(server_t* server, player_t* player)
{
    uint8_t team;
    if (player->team == TEAM_A) {
        team = TEAM_B;
    } else {
        team = TEAM_A;
    }
    if (player->team != TEAM_SPECTATOR) {
        uint64_t timeNow = get_game_seconds();
        if (player->has_intel == 0) {

            if (check_player_on_intel(server, player, team) && (!server->protocol.gamemode.intel_held[team])) {
                _ctf_pickup(server, player);
                evt_log(player, "intel", "action=pickup team=%u pos=%.1f,%.1f,%.1f", team, player->movement.position.x,
                        player->movement.position.y, player->movement.position.z);
                if (server->protocol.current_gamemode == GAME_MODE_BABEL) {
                    vector3f_t pos = {0, 0, 64};
                    server->protocol.gamemode.intel[player->team] = pos;
                }
            } else if (check_player_in_tent(server, player) &&
                       timeNow - player->timers.since_last_base_enter_restock >= 15)
            {
                player_restock(server, player);
                player->timers.since_last_base_enter_restock = get_game_seconds();
            }
        } else if (player->has_intel) {
            if (check_player_in_tent(server, player) && timeNow - player->timers.since_last_base_enter >= 5) {
                server->protocol.gamemode.score[player->team]++;
                uint8_t winning = 0;
                if (server->protocol.gamemode.score[player->team] >= server->protocol.gamemode.score_limit) {
                    winning = 1;
                }
                _ctf_capture(server, player);
                evt_log(player, "intel", "action=capture score=%u-%u winning=%u", server->protocol.gamemode.score[0],
                        server->protocol.gamemode.score[1], winning);
                player_restock(server, player);
                player->timers.since_last_base_enter = get_game_seconds();
                if (server->protocol.current_gamemode == GAME_MODE_BABEL) {
                    vector3f_t babelIntelPos = {255, 255, map_top(&server->s_map.map, 255, 255)};
                    server->protocol.gamemode.intel[0] = babelIntelPos;
                    server->protocol.gamemode.intel[1] = babelIntelPos;
                } else {
                    server->protocol.gamemode.intel[team] = set_intel_tent_spawn_point(server, team, MAP_META_FLAG);
                }
                if (winning) {
                    gamemode_match_won(server);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the mode: CTF, and Babel (both intels on a tower in the middle)     */
/* ------------------------------------------------------------------ */

static void _ctf_seats_reset(void);
static void _ctf_base_snapshot(server_t* server);
static void _ctf_posts_reset(void);
static void _ctf_posts_tick(server_t* server);

void _ctf_init(server_t* server)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    memset(ctf_spots, 0, sizeof(ctf_spots)); /* a new map: a script sets them again */
    gm->intel_flags     = 0;
    gm->intel[0]        = set_intel_tent_spawn_point(server, 0, MAP_META_FLAG);
    gm->intel[1]        = set_intel_tent_spawn_point(server, 1, MAP_META_FLAG);
    gm->intel_held[0]   = 0;
    gm->intel_held[1]   = 0;
    for (int t = 0; t < 2; t++) {
        gm->base[t]   = set_intel_tent_spawn_point(server, (uint8_t) t, MAP_META_BASE);
        gm->base[t].x = floorf(gm->base[t].x);
        gm->base[t].y = floorf(gm->base[t].y);
    }
    ctf_dropped[0] = ctf_dropped[1] = 0;
    ctf_home[0]    = gm->intel[0];
    ctf_home[1]    = gm->intel[1];
    _ctf_seats_reset(); /* a new match: the bots choose again */
    _ctf_posts_reset();
    _ctf_base_snapshot(server);
}

static void _babel_stair_plan(server_t* server);
static void _babel_wall_plan(server_t* server);
/* when the map started and when each team's staircase was first whole (game
   time): how long the builders take is the measure of the staircase work */
static uint64_t babel_started, babel_whole_at[2];
/* when each team's wall was first whole */
static uint64_t babel_wall_whole_at[2];
/* each bot's role logged this life (_babel_role_goal): the role is by rank,
   so it changes only when the team's bots do */
static uint8_t babel_role_life[256], babel_role_seen[256];
/* when a waller was last told its cell, and which window it holds: log
   pacing only, and reset at every map (ids are different bots there) */
static uint64_t babel_wall_log_at[256];
static int      babel_window_held[256];

static void _babel_init(server_t* server)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    _ctf_init(server);

    color_t platformColor;
    platformColor.raw = map_meta_babel_color(server, 0xFF00FFFF); /* the map's heavens_color */
    for (int x = 206; x <= 306; ++x) {
        for (int y = 240; y <= 272; ++y) {
            map_set_color(&server->s_map.map, x, y, 1, platformColor.raw);
        }
    }
    /* both intels on top of the tower. The highest point of the map, not 0:
       the platform may not be there in every size */
    for (int t = 0; t < 2; t++) {
        gm->intel[t].z = map_top(&server->s_map.map, 255, 255);
        gm->intel[t].x = round((float) server->s_map.map.size_x / 2);
        gm->intel[t].y = round((float) server->s_map.map.size_y / 2);
    }
    _babel_stair_plan(server);
    _babel_wall_plan(server);
    babel_started = get_nanos();
    babel_whole_at[0] = babel_whole_at[1] = 0;
    babel_wall_whole_at[0] = babel_wall_whole_at[1] = 0;
    memset(babel_role_life, 0, sizeof(babel_role_life));
    memset(babel_role_seen, 0, sizeof(babel_role_seen));
    memset(babel_wall_log_at, 0, sizeof(babel_wall_log_at));
    memset(babel_window_held, 0, sizeof(babel_window_held));
}

void _ctf_player_out(server_t* server, player_t* player)
{
    if (player->has_intel) {
        _ctf_drop(server, player);
    }
}

void _ctf_terrain_changed(server_t* server, int built)
{
    if (built) {
        moveIntelAndTentUp(server);
    } else {
        move_intel_and_tent_down(server);
    }
}

/* the tower's platform */
static int _babel_cell_protected(server_t* server, int x, int y, int z)
{
    (void) server;
    return ((x >= 206 && x <= 306) && (y >= 240 && y <= 272) && (z == 2 || z == 0)) ||
           ((x >= 205 && x <= 307) && (y >= 239 && y <= 273) && (z == 1));
}

static void _ctf_intel_value(server_t* server, uint8_t team, char* out)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    p10_intel        in;
    memset(&in, 0, sizeof in);
    /* intel_held[team]: the intel of team is carried, by the enemy team's
       carrier (_ctf_pickup) */
    int carrier = -1;
    if (gm->intel_held[team] && gm->player_intel_team[1 - team] != 0xFF) {
        in.state = P10_INTEL_CARRIED;
        carrier  = gm->player_intel_team[1 - team];
    } else {
        in.state = ctf_dropped[team] ? P10_INTEL_DROPPED : P10_INTEL_BASE;
    }
    /* where it is and who carries it: its Flag entity (Proto10/Flags.c);
       the key says only its state */
    proto10_flags_set(team == 0 ? P10_FLAG_INTEL1 : P10_FLAG_INTEL2, gm->intel[team].x, gm->intel[team].y,
                      gm->intel[team].z, carrier);
    if (!p10_format_intel(&in, out, GM_KEY_VALUE_LEN)) {
        out[0] = 0;
    }
}

uint32_t _ctf_player_captures(server_t* server, player_t* player)
{
    (void) server;
    return player->captures;
}

void _ctf_state_keys(server_t* server, gm_keys_t* out)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    char             intel[GM_KEY_VALUE_LEN];
    _ctf_intel_value(server, 0, intel);
    gm_keys_put(out, P10_GM_TEAM1_INTEL, "%s", intel);
    _ctf_intel_value(server, 1, intel);
    gm_keys_put(out, P10_GM_TEAM2_INTEL, "%s", intel);
    /* the tents: CommandPost entities */
    for (int t = 0; t < 2; t++) {
        proto10_flags_set(t == 0 ? P10_FLAG_TENT1 : P10_FLAG_TENT2, gm->base[t].x, gm->base[t].y, gm->base[t].z, -1);
    }
}

/* Seats (CTF only, not Babel): the roles a team's bots take, drawn at
   random once and kept for the match, the way players settle in the first
   seconds who does what (by id the same bot guarded in every match, and a
   role moved to another bot whenever ids shifted). A seat is taken from a
   team size on (seat_from: a guard from two bots, then the infiltrator, the
   guard's rifle overwatch, the infiltrator's shotgun cover, a tactical
   camper) and empties only when its bot leaves the team. Humans are not
   counted: they play as they like. The bots without a seat attack, every
   second one by id rushes.

   The seats are said in the team's chat when the bots start playing: the
   guard says it guards the intel and names its overwatch, the infiltrator
   names its cover, the camper says it camps, and the ones named answer.
   Meanwhile the team stands at home looking at whoever talks
   ("briefing"), as players do while they choose. A seat taken later is
   said alone, without the wait. */
enum
{
    SEAT_GUARD,
    SEAT_INFILTRATOR,
    SEAT_OVERWATCH,
    SEAT_COVER,
    SEAT_CAMPER,
    SEATS
};
static const int seat_from[SEATS] = {2, 3, 5, 6, 7};

#define BRIEF_LINES 12

typedef struct
{
    uint8_t  from, to; /* speaker, the bot it names (0xFF none) */
    uint64_t at;       /* when it is sent (the game's clock) */
    char     text[96];
} seat_line_t;

static struct
{
    uint64_t    at;              /* last refresh */
    int         count;           /* the team's bots */
    uint8_t     id[SEATS];       /* player id, 0xFF empty */
    uint8_t     told[SEATS];     /* its bot said it took the seat */
    uint8_t     briefed;         /* the briefing was planned (once a match) */
    uint64_t    brief_end;       /* the team holds at home until then */
    int         lines;           /* queued lines */
    seat_line_t line[BRIEF_LINES];
    uint8_t     look_from, look_to; /* the line said last: everyone looks at its speaker */
} seats[2];

static uint8_t seat_role[256]; /* GM_BOT_* for every bot id, from the last refresh */
static uint8_t seat_camp[256]; /* 1 camper, 2 overwatch */

static void _ctf_seats_reset(void)
{
    memset(seats, 0, sizeof(seats));
    for (int t = 0; t < 2; t++) {
        memset(seats[t].id, 0xFF, sizeof(seats[t].id));
        seats[t].look_from = seats[t].look_to = 0xFF;
    }
}

static int _ctf_seat_of(int t, uint8_t id)
{
    for (int k = 0; k < SEATS; k++) {
        if (seats[t].id[k] == id) {
            return k;
        }
    }
    return -1;
}

static void _ctf_seats_refresh(server_t* server)
{
    uint64_t now = get_nanos();
    if (seats[0].at != 0 && now - seats[0].at < NANO_IN_MILLI * 50) {
        return;
    }
    seats[0].at = seats[1].at = now;
    uint8_t   team_of[256];
    player_t *p, *tmp;
    memset(team_of, 0xFF, sizeof(team_of));
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot && p->state != STATE_DISCONNECTED && (p->team == TEAM_A || p->team == TEAM_B)) {
            team_of[p->id] = p->team == TEAM_A ? 0 : 1;
        }
    }
    for (int t = 0; t < 2; t++) {
        int n = 0;
        for (int id = 0; id < 256; id++) {
            n += team_of[id] == t;
        }
        seats[t].count = n;
        for (int k = 0; k < SEATS; k++) {
            uint8_t id = seats[t].id[k];
            if (id != 0xFF && (team_of[id] != t || n < seat_from[k])) {
                seats[t].id[k]   = 0xFF; /* it left the team, or the team shrank */
                seats[t].told[k] = 0;
            }
        }
        for (int k = 0; k < SEATS; k++) {
            if (seats[t].id[k] != 0xFF || n < seat_from[k]) {
                continue;
            }
            uint8_t free_ids[256];
            int     nfree = 0;
            for (int id = 0; id < 256; id++) {
                if (team_of[id] == t && _ctf_seat_of(t, (uint8_t) id) < 0) {
                    free_ids[nfree++] = (uint8_t) id;
                }
            }
            if (nfree > 0) {
                int pick = (int) (gen_rand(&server->rand) * (float) nfree);
                seats[t].id[k] = free_ids[pick < nfree ? pick : nfree - 1];
                seats[t].told[k] = 0;
            }
        }
        /* the rest attack, every second one by id rushes */
        int rest = 0;
        for (int id = 0; id < 256; id++) {
            if (team_of[id] != t) {
                continue;
            }
            int k        = _ctf_seat_of(t, (uint8_t) id);
            seat_camp[id] = k == SEAT_OVERWATCH ? 2 : k == SEAT_CAMPER ? 1 : 0;
            seat_role[id] = k == SEAT_GUARD         ? GM_BOT_DEFEND
                            : k == SEAT_INFILTRATOR ? GM_BOT_INFILTRATE
                            : k == SEAT_COVER       ? GM_BOT_COVER
                            : k >= 0                ? GM_BOT_ATTACK
                            : (n >= 4 && rest++ % 2 == 1) ? GM_BOT_RUSH
                                                          : GM_BOT_ATTACK;
        }
    }
}

/* The seats are classic CTF's: Babel has none, and where the
   classes play (ngCTF) the class is how a bot plays, so there are no seats,
   no briefing, no rushers and no forward posts there either: every bot
   attacks the intel its class's way (the Miner tunnels, the Rocketeer
   flies, the Marksman shoots from afar). As seats they made the guard
   always a Rocketeer, the campers Marksmen, and held half a team at
   posts */
static int _ctf_seats_on(server_t* server)
{
    return server->protocol.current_gamemode != GAME_MODE_BABEL && !classes_on(server);
}

static uint8_t _ctf_role(server_t* server, player_t* bot)
{
    if (!_ctf_seats_on(server) || (bot->team != TEAM_A && bot->team != TEAM_B)) {
        return GM_BOT_ATTACK;
    }
    _ctf_seats_refresh(server);
    return seat_role[bot->id];
}

static player_t* _ctf_seated(server_t* server, int t, int seat)
{
    player_t* p  = NULL;
    uint8_t   id = seats[t].id[seat];
    if (id != 0xFF) {
        HASH_FIND(hh, server->players, &id, sizeof(id), p);
    }
    return p;
}

/* the team's infiltrator, NULL when it is not playing right now */
static player_t* _ctf_infiltrator(server_t* server, player_t* bot)
{
    _ctf_seats_refresh(server);
    player_t* p = _ctf_seated(server, bot->team == TEAM_A ? 0 : 1, SEAT_INFILTRATOR);
    return p != NULL && p->state == STATE_READY && p->alive ? p : NULL;
}

/* how a name is said in the chat: without the "Bot_" every bot carries */
static const char* _ctf_said_name(player_t* p)
{
    return strncmp(p->name, "Bot_", 4) == 0 && p->name[4] != '\0' ? p->name + 4 : p->name;
}

/* queues a line typed after *after (the line it answers): the time to type
   it (a quick typist, ~16 characters a second) and a pause to think first.
   *after becomes its time. The queue stays in time order: two bots type at
   once (the guard's talk and the infiltrator's) */
static void _ctf_say_later(server_t* server, int t, player_t* from, player_t* to, uint64_t* after, const char* fmt)
{
    if (from == NULL || seats[t].lines >= BRIEF_LINES) {
        return;
    }
    seat_line_t line, *l = &line;
    l->from = from->id;
    l->to          = to != NULL ? to->id : 0xFF;
    if (to != NULL) {
        snprintf(l->text, sizeof(l->text), fmt, _ctf_said_name(to));
    } else {
        snprintf(l->text, sizeof(l->text), "%s", fmt);
    }
    float secs = 0.5f + gen_rand(&server->rand) * 0.9f + (float) strlen(l->text) / 16.0f;
    l->at      = *after + (uint64_t) (secs * (float) NANO_IN_SECOND);
    *after     = l->at;
    int k      = seats[t].lines++;
    while (k > 0 && seats[t].line[k - 1].at > l->at) {
        seats[t].line[k] = seats[t].line[k - 1];
        k--;
    }
    seats[t].line[k] = line;
}

static const char* _ctf_pick(server_t* server, const char* const* options, int n)
{
    int k = (int) (gen_rand(&server->rand) * (float) n);
    return options[k < n ? k : n - 1];
}

#define PICK(server, list) _ctf_pick(server, list, (int) (sizeof(list) / sizeof(list[0])))

static const char* const say_guard[]     = {"i'll guard the intel", "i got intel defense", "ill stay home on the intel",
                                            "guarding our intel"};
static const char* const say_guard_ow[]  = {"%s cover me with your rifle", "%s watch the intel with me, take a rifle",
                                            "%s you're on rifle cover with me"};
static const char* const say_infil[]     = {"i'll sneak in with the smg", "going in quiet, smg",
                                            "i'll infiltrate with the smg"};
static const char* const say_infil_cv[]  = {"%s come with me, bring a shotgun", "%s follow me with the shotgun",
                                            "%s you're with me, shotgun"};
static const char* const say_camper[]    = {"i'll be the tactical camper", "tactical camper here",
                                            "i'll play tactical camper with the rifle"};
static const char* const say_ok[]        = {"ok", "got it", "on it", "k", "sure"};
static const char* const say_late[SEATS] = {"i'll guard the intel now", "i'll go in with the smg now",
                                            "i'll watch the intel with the rifle", "i'll cover the smg",
                                            "i'll be the tactical camper"};

/* what a seat says when its bot takes it after the briefing */
static void _ctf_say_seat(server_t* server, int t, int k, uint64_t after)
{
    player_t* p = _ctf_seated(server, t, k);
    _ctf_say_later(server, t, p, NULL, &after, say_late[k]);
}

static void _ctf_brief_plan(server_t* server, int t, uint64_t now)
{
    player_t* guard  = _ctf_seated(server, t, SEAT_GUARD);
    player_t* ow     = _ctf_seated(server, t, SEAT_OVERWATCH);
    player_t* infil  = _ctf_seated(server, t, SEAT_INFILTRATOR);
    player_t* cover  = _ctf_seated(server, t, SEAT_COVER);
    player_t* camper = _ctf_seated(server, t, SEAT_CAMPER);
    float     sec    = (float) NANO_IN_SECOND;
    /* the guard starts, the infiltrator a moment later typing at the same
       time, the camper somewhere in between */
    uint64_t g = now + (uint64_t) ((0.8f + gen_rand(&server->rand) * 1.2f) * sec);
    uint64_t i = g + (uint64_t) ((0.5f + gen_rand(&server->rand) * 2.5f) * sec);
    uint64_t c = g + (uint64_t) ((2.0f + gen_rand(&server->rand) * 4.0f) * sec);
    if (guard != NULL) {
        _ctf_say_later(server, t, guard, NULL, &g, PICK(server, say_guard));
        if (ow != NULL) {
            _ctf_say_later(server, t, guard, ow, &g, PICK(server, say_guard_ow));
            _ctf_say_later(server, t, ow, NULL, &g, PICK(server, say_ok));
        }
    }
    if (infil != NULL) {
        _ctf_say_later(server, t, infil, NULL, &i, PICK(server, say_infil));
        if (cover != NULL) {
            _ctf_say_later(server, t, infil, cover, &i, PICK(server, say_infil_cv));
            _ctf_say_later(server, t, cover, NULL, &i, PICK(server, say_ok));
        }
    }
    if (camper != NULL) {
        _ctf_say_later(server, t, camper, NULL, &c, PICK(server, say_camper));
    }
    for (int k = 0; k < SEATS; k++) {
        seats[t].told[k] = 1;
    }
    seats[t].briefed = 1;
    if (seats[t].lines > 0) {
        seats[t].look_from = seats[t].line[0].from;
        seats[t].look_to   = seats[t].line[0].to;
        seats[t].brief_end = seats[t].line[seats[t].lines - 1].at + 2 * NANO_IN_SECOND;
        LOG_INFO("Bots of team %d: briefing of %d lines, %.1f s", t + 1, seats[t].lines,
                 (double) (seats[t].brief_end - now) / (double) NANO_IN_SECOND);
    }
}

/* once a tick: the briefing when the bots start playing, a seat taken later
   said alone, and the lines due sent */
static void _ctf_seats_tick(server_t* server)
{
    if (!_ctf_seats_on(server)) {
        return;
    }
    _ctf_seats_refresh(server);
    if (!bots_playing()) {
        return;
    }
    _ctf_posts_tick(server);
    uint64_t now = get_nanos();
    for (int t = 0; t < 2; t++) {
        if (!seats[t].briefed) {
            _ctf_brief_plan(server, t, now);
        }
        for (int k = 0; k < SEATS; k++) {
            if (!seats[t].told[k] && seats[t].id[k] != 0xFF) {
                seats[t].told[k] = 1;
                _ctf_say_seat(server, t, k, now);
            }
        }
        while (seats[t].lines > 0 && seats[t].line[0].at <= now) {
            seat_line_t l = seats[t].line[0];
            memmove(&seats[t].line[0], &seats[t].line[1], sizeof(seat_line_t) * (size_t) (seats[t].lines - 1));
            seats[t].lines--;
            player_t* p = NULL;
            HASH_FIND(hh, server->players, &l.from, sizeof(l.from), p);
            if (p != NULL && p->state != STATE_DISCONNECTED) {
                bot_say(server, p, 1, l.text);
                seats[t].look_from = l.from;
                seats[t].look_to   = l.to;
            }
        }
    }
}

/* the briefing: the bot stays where it stands, looking at whoever talks
   (the speaker at the one it names, or at its nearest teammate) */
static int _ctf_briefing_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    int t = bot->team == TEAM_A ? 0 : 1;
    if (seats[t].brief_end == 0 || get_nanos() >= seats[t].brief_end || !bot->alive || bot->has_intel) {
        return 0;
    }
    uint8_t   look = seats[t].look_from == bot->id ? seats[t].look_to : seats[t].look_from;
    player_t* at   = NULL;
    if (look != 0xFF && look != bot->id) {
        HASH_FIND(hh, server->players, &look, sizeof(look), at);
    }
    if (at == NULL || !at->alive) {
        float     best = 1e9f;
        player_t *p, *tmp;
        at = NULL;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (p == bot || p->team != bot->team || !p->alive || p->state != STATE_READY) {
                continue;
            }
            float dx = p->movement.position.x - bot->movement.position.x;
            float dy = p->movement.position.y - bot->movement.position.y;
            if (dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                at   = p;
            }
        }
    }
    gm_goal_name(out, "briefing", GM_INTENT_BRIEF);
    out->x       = bot->movement.position.x;
    out->y       = bot->movement.position.y;
    out->z       = bot->movement.position.z + 2.4f;
    out->hold    = 1000.0f;
    out->watch_x = at != NULL ? at->movement.position.x : bot->movement.position.x;
    out->watch_y = at != NULL ? at->movement.position.y : bot->movement.position.y;
    return 1;
}

/* Squad lanes: an attacker squad takes its own way to the enemy intel, so a
   team spreads over the map (every squad went down the middle in a line and
   the sides of a big map stayed empty). The squad's leader, its lowest id
   attacker or rusher alive (not a camper), goes through two waypoints, a
   third and two thirds of the way from our base to the enemy intel, pushed
   to the left, the right or kept in the middle by squad (1 left, 2 right, 3
   middle, ...); the rest of the squad follows it. A new leader starts the
   lane again */
#define LANE_WAYPOINTS 2

static struct
{
    uint8_t    leader;  /* player id, 0xFF none */
    uint8_t    stage;   /* the waypoint the leader heads for, LANE_WAYPOINTS: the intel */
    vector3f_t wp[LANE_WAYPOINTS];
} lanes[2][SQUADS_MAX + 1];

/* dry land near (x, y): a column whose top is above the water, the nearest
   in rings up to 20 cells away; the point itself when there is none. Ground
   first (_ground_top): a waypoint on a roof (city's 277,219,27 over the
   street at 48) was a goal its leader planned partial ways to all round */
static vector3f_t _ctf_land_near(server_t* server, float x, float y)
{
    map_store_t* map = &server->s_map.map;
    /* inside the area the map's author lets players be (Busan_Port's
       boundary damage): a lane pushed 100 blocks sideways left it */
    int ax0, ay0, ax1, ay1;
    if (map_meta_play_area(server, &ax0, &ay0, &ax1, &ay1)) {
        x = fminf(fmaxf(x, (float) ax0 + 3.0f), (float) ax1 - 3.0f);
        y = fminf(fmaxf(y, (float) ay0 + 3.0f), (float) ay1 - 3.0f);
    }
    /* and on the map: a lane pushed sideways off a small map's edge came here
       at x -20, and the fallback below read that column (bloody_omaha) */
    x = fminf(fmaxf(x, 2.0f), (float) map->size_x - 3.0f);
    y = fminf(fmaxf(y, 2.0f), (float) map->size_y - 3.0f);
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r <= 20; r++) {
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) {
                        continue; /* the ring only */
                    }
                    int cx = (int) x + dx, cy = (int) y + dy;
                    if (cx < 2 || cy < 2 || cx >= map->size_x - 2 || cy >= map->size_y - 2) {
                        continue;
                    }
                    int top = map_top(map, (uint16_t) cx, (uint16_t) cy);
                    if (top > 4 && top < map->size_z - 2 && (pass == 1 || _ground_top(server, cx, cy) >= 0)) {
                        return (vector3f_t){cx + 0.5f, cy + 0.5f, (float) top};
                    }
                }
            }
        }
    }
    return (vector3f_t){x, y, (float) map_top(map, (uint16_t) x, (uint16_t) y)};
}

static void _ctf_lane_plan(server_t* server, int t, int squad)
{
    gamemode_vars_t* gm    = &server->protocol.gamemode;
    vector3f_t       from  = gm->base[t];
    vector3f_t       to    = gm->intel[1 - t];
    float            dx    = to.x - from.x, dy = to.y - from.y, d = sqrtf(dx * dx + dy * dy);
    static const int sides[3] = {-1, 1, 0};
    float            side  = (float) sides[(squad - 1) % 3];
    float            width = fminf(fmaxf(d * 0.35f, 20.0f), 100.0f);
    float            px = d > 0.1f ? -dy / d : 0.0f, py = d > 0.1f ? dx / d : 0.0f;
    for (int k = 0; k < LANE_WAYPOINTS; k++) {
        float f            = (float) (k + 1) / (float) (LANE_WAYPOINTS + 1);
        lanes[t][squad].wp[k] = _ctf_land_near(server, from.x + dx * f + px * side * width,
                                               from.y + dy * f + py * side * width);
    }
}

static uint8_t ctf_posted[256]; /* the post a bot holds (team's post index), 0xFF none */

int ctf_bot_has_post(server_t* server, player_t* player)
{
    if (!player->is_bot || server->protocol.current_gamemode == GAME_MODE_BABEL) {
        return 0;
    }
    /* where the classes play only the Marksman camps (it leaves the squad
       like a camper: the lane would wait for it at its spot) */
    if (!_ctf_seats_on(server)) {
        return bot_holds_post(player);
    }
    uint8_t role = _ctf_role(server, player);
    return (role != GM_BOT_ATTACK && role != GM_BOT_RUSH) || bot_holds_post(player);
}

/* the leader is still the squad's: in it, on the team, an attacker without
   a post (dead or alive: a leader is kept until it leaves the squad, and the
   squad waits for it at its waypoint) */
static int _ctf_leads(server_t* server, player_t* p, int t, int s)
{
    return p != NULL && p->is_bot && p->state != STATE_DISCONNECTED && (p->team == TEAM_A ? 0 : 1) == t &&
           (p->team == TEAM_A || p->team == TEAM_B) && p->squad == s && p->squad_team == p->team &&
           !ctf_bot_has_post(server, p) && ctf_posted[p->id] == 0xFF; /* a post's holder leads no lane */
}

/* a Miner tunnels its own way (BotNav.c): following a leader on the
   surface it climbed every hill the lane crossed and nobody saw it dig */
static int _ctf_tunnels(server_t* server, player_t* p)
{
    return p != NULL && p->is_bot && classes_dig(server, p) > 1;
}

/* in a tunnel: solid ground right over the head, three cells thick (z
   grows downwards; the cell over a standing head is (int) position.z, a
   crouched one's a cell higher). The column's top over the head was not
   enough: on hiesville a bot in any house "was in a tunnel" (68 follows a
   match, most 13 to 17 cells under a roof) */
static int _ctf_underground(server_t* server, player_t* p)
{
    int x = (int) p->movement.position.x, y = (int) p->movement.position.y;
    if (x < 0 || y < 0 || x >= (int) server->s_map.map.size_x || y >= (int) server->s_map.map.size_y) {
        return 0;
    }
    int head = (int) p->movement.position.z;
    for (int z = head; z >= head - 2 && z >= 2; z--) {
        if (map_solid(&server->s_map.map, x, y, z)) {
            return map_solid(&server->s_map.map, x, y, z - 1) && map_solid(&server->s_map.map, x, y, z - 2);
        }
    }
    return 0;
}

/* a Miner bot in a tunnel (its own or a mate's) or digging one to its
   goal: busy with its way. Under three cells of ground alone missed the
   ramps and shallow stretches: 163 of its tunnels ended for a hunt */
static int _ctf_tunnelling(server_t* server, player_t* p)
{
    return _ctf_tunnels(server, p) && (bot_is_tunnelling(p) || _ctf_underground(server, p));
}

/* the team's first Marksman bot by id camps; the others attack with the
   rifle. Every Marksman camping held a quarter of the team on hills: 16
   bots, 600 s, 10 seeds, Raindance pickups 5.1 to 1.2, Busan_Port kills
   142 to 96 */
static int _ctf_camping_marksman(server_t* server, player_t* bot)
{
    if (!bot->is_bot || bot->klass != GAME_CLASS_MARKSMAN) {
        return 0;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != bot && p->is_bot && p->team == bot->team && p->klass == GAME_CLASS_MARKSMAN && p->id < bot->id) {
            return 0;
        }
    }
    return 1;
}

#define TUNNEL_MATE_R 24.0f /* a squadmate Miner this close, both under the ground */
#define TUNNEL_WAIT   20.0f /* seconds a bot follows it, a stretch under the ground */

static uint64_t tunnel_until[256]; /* get_nanos() its following ends, 0 on the surface */

/* the squad's Miner (a bot or a person) digging under the ground near a
   bot that is under the ground too: a squad spawn by a Miner lands in its
   tunnel. NULL when the bot is on the surface or no such Miner */
static player_t* _ctf_tunnel_mate(server_t* server, player_t* bot)
{
    /* SP_BOT_TUNNEL_FOLLOW=0 turns it off for a measurement */
    static int off = -1;
    if (off < 0) {
        off = getenv("SP_BOT_TUNNEL_FOLLOW") != NULL && !strcmp(getenv("SP_BOT_TUNNEL_FOLLOW"), "0");
    }
    if (off || !bot->alive || bot->has_intel || classes_dig(server, bot) > 1 || !_ctf_underground(server, bot)) {
        tunnel_until[bot->id] = 0;
        return NULL;
    }
    /* TUNNEL_WAIT at most a stretch under the ground: a Miner digging a long
       way kept its squad behind it for minutes (16 bots, 10 seeds:
       Raindance pickups 4.7 to 2.8 with no limit) */
    uint64_t now   = get_nanos();
    uint8_t  fresh = tunnel_until[bot->id] == 0;
    if (fresh) {
        tunnel_until[bot->id] = now + (uint64_t) (TUNNEL_WAIT * 1e9);
    }
    if (now >= tunnel_until[bot->id]) {
        return NULL;
    }
    player_t *p, *tmp, *best = NULL;
    float     best_d = TUNNEL_MATE_R * TUNNEL_MATE_R;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == bot || !p->alive || p->state != STATE_READY || p->team != bot->team || p->squad != bot->squad ||
            classes_dig(server, p) <= 1 || !_ctf_underground(server, p))
        {
            continue;
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        float d  = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best   = p;
        }
    }
    if (best != NULL && fresh) {
        int top = map_top(&server->s_map.map, (uint16_t) bot->movement.position.x,
                                        (uint16_t) bot->movement.position.y);
        evt_log(bot, "bot_tunnel", "action=follow miner=%u depth=%d", best->id, (int) bot->movement.position.z - top);
    }
    return best;
}

/* once a tick: bots with a post leave their squad, every squad keeps its
   leader (a new one only when it left), and how far along its lane it got */
void _ctf_tick(server_t* server)
{
    _ctf_seats_tick(server);
    squads_census(server);
    uint8_t   best[2][SQUADS_MAX + 1], sneak[2][SQUADS_MAX + 1];
    player_t *p, *tmp;
    memset(best, 0xFF, sizeof(best));
    memset(sneak, 0xFF, sizeof(sneak));
    HASH_ITER(hh, server->players, p, tmp)
    {
        int t = p->team == TEAM_A ? 0 : (p->team == TEAM_B ? 1 : -1);
        if (t < 0 || !p->is_bot || p->state == STATE_DISCONNECTED) {
            continue;
        }
        if (ctf_bot_has_post(server, p)) {
            if (p->squad != 0 || p->squad_team != SQUAD_TEAM_ROLE) {
                squads_leave_for_role(server, p);
            }
            continue;
        }
        /* the lowest id leads; a sneaky one only when nobody else can:
           the squad follows its leader and crawled behind a sneaking one
           (Busan_Port, 16 bots: pickups 5.5 to 2.75 once they joined) */
        if (p->squad != 0 && p->squad <= SQUADS_MAX && p->squad_team == p->team && ctf_posted[p->id] == 0xFF) {
            uint8_t* slot = bot_is_sneaky(p) || _ctf_tunnels(server, p) ? &sneak[t][p->squad] : &best[t][p->squad];
            if (p->id < *slot) {
                *slot = p->id;
            }
        }
    }
    for (int t = 0; t < 2; t++) {
        for (int s = 1; s <= SQUADS_MAX; s++) {
            if (best[t][s] == 0xFF) {
                best[t][s] = sneak[t][s];
            }
        }
    }
    for (int t = 0; t < 2; t++) {
        for (int s = 1; s <= SQUADS_MAX; s++) {
            player_t* leader = NULL;
            uint8_t   id     = lanes[t][s].leader;
            if (id != 0xFF) {
                HASH_FIND(hh, server->players, &id, sizeof(id), leader);
            }
            /* a sneaky leader hands over as soon as another member can lead */
            if (!_ctf_leads(server, leader, t, s) ||
                ((bot_is_sneaky(leader) || _ctf_tunnels(server, leader)) && sneak[t][s] != best[t][s]))
            {
                /* it left the squad (or there was none): the next one leads,
                   on the squad's lane, planned once */
                uint8_t was        = lanes[t][s].leader;
                lanes[t][s].leader = best[t][s];
                leader             = NULL;
                if (best[t][s] != 0xFF) {
                    id = best[t][s];
                    HASH_FIND(hh, server->players, &id, sizeof(id), leader);
                    if (was == 0xFF) {
                        lanes[t][s].stage = 0;
                        _ctf_lane_plan(server, t, s);
                    }
                    evt_log(leader, "bot_lane", "action=leader squad=%d stage=%u", s, lanes[t][s].stage);
                }
            }
            if (leader == NULL || !leader->alive || lanes[t][s].stage >= LANE_WAYPOINTS) {
                continue;
            }
            vector3f_t w = lanes[t][s].wp[lanes[t][s].stage];
            if (fabsf(leader->movement.position.x - w.x) < 8.0f && fabsf(leader->movement.position.y - w.y) < 8.0f) {
                lanes[t][s].stage++;
                evt_log(leader, "bot_lane", "action=waypoint squad=%d stage=%u", s, lanes[t][s].stage);
            }
        }
    }
}

/* an attacker of a squad: its leader heads for the lane's next waypoint, the
   others follow the leader. 0 when it is on its own (no squad, no leader,
   or the lane is done: the intel itself) */
static int _ctf_squad_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    int t = bot->team == TEAM_A ? 0 : 1;
    if (bot->squad == 0 || bot->squad > SQUADS_MAX || bot->squad_team != bot->team) {
        return 0;
    }
    uint8_t id = lanes[t][bot->squad].leader;
    if (id == 0xFF || ctf_bot_has_post(server, bot)) {
        return 0;
    }
    /* a Miner stays in the squad (it respawns by its mates) but goes its own
       way, under the hills, instead of after the leader */
    if (id != bot->id && _ctf_tunnels(server, bot)) {
        return 0;
    }
    /* respawned by a squadmate Miner in its tunnel: go on with it under the
       ground, walking the tunnel it digs, until it comes up. They dug their
       own way up to the leader on the surface instead and left the tunnel
       (and the Miner) behind */
    player_t* miner = _ctf_tunnel_mate(server, bot);
    if (miner != NULL) {
        gm_goal_name(out, "tunnel-follow", 0);
        out->x         = miner->movement.position.x;
        out->y         = miner->movement.position.y;
        out->z         = miner->movement.position.z + 2.4f;
        out->hold      = 3.0f;
        out->walk_only = 1; /* the Miner digs: the plan walks its tunnel */
        out->watch_x   = miner->movement.position.x + miner->movement.forward_orientation.x * 10.0f;
        out->watch_y   = miner->movement.position.y + miner->movement.forward_orientation.y * 10.0f;
        return 1;
    }
    if (id == bot->id) {
        if (lanes[t][bot->squad].stage >= LANE_WAYPOINTS) {
            return 0; /* the lane is done: the intel */
        }
        vector3f_t w = lanes[t][bot->squad].wp[lanes[t][bot->squad].stage];
        gm_goal_name(out, "lead", GM_INTENT_PUSH | GM_INTENT_SQUAD | GM_INTENT_WITH);
        out->x       = w.x;
        out->y       = w.y;
        out->z       = w.z;
        return 1;
    }
    player_t* leader;
    HASH_FIND(hh, server->players, &id, sizeof(id), leader);
    if (leader == NULL) {
        return 0;
    }
    if (!leader->alive) {
        /* the leader is down: the squad waits at its next waypoint (with the
           lane done, the intel without it) */
        if (lanes[t][bot->squad].stage >= LANE_WAYPOINTS) {
            return 0;
        }
        vector3f_t w = lanes[t][bot->squad].wp[lanes[t][bot->squad].stage];
        gm_goal_name(out, "regroup", GM_INTENT_PUSH | GM_INTENT_SQUAD);
        out->x       = w.x;
        out->y       = w.y;
        out->z       = w.z;
        out->hold    = 6.0f;
        out->watch_x = server->protocol.gamemode.intel[1 - t].x;
        out->watch_y = server->protocol.gamemode.intel[1 - t].y;
        return 1;
    }
    gm_goal_name(out, "follow", GM_INTENT_PUSH | GM_INTENT_SQUAD | GM_INTENT_WITH);
    out->x       = leader->movement.position.x;
    out->y       = leader->movement.position.y;
    out->z       = leader->movement.position.z + 2.4f;
    out->hold    = 4.0f;
    out->watch_x = leader->movement.position.x + leader->movement.forward_orientation.x * 10.0f;
    out->watch_y = leader->movement.position.y + leader->movement.forward_orientation.y * 10.0f;
    return 1;
}

/* Who goes after the enemy carrying our intel: the guard, and the few bots
   of the team closest to the carrier (CTF_HUNTERS). Everyone did, so with 64
   a side the whole team ran back after one carrier, and every bot that
   respawned left the enemy intel alone to chase it; the rest keep going for
   the enemy intel (theirs at home is what our capture needs) */
#define CTF_HUNTERS 4

static int _ctf_hunts(server_t* server, player_t* bot, player_t* carrier, uint8_t role)
{
    /* the guard and the overwatch go at once: watching the intel is their
       job, and it just went */
    if (role == GM_BOT_DEFEND || bot_watches_home(bot)) {
        return 1;
    }
    /* a Miner under the ground goes on with its tunnel: it left it to hunt
       a carrier passing near (62 of its tunnels in eight matches ended so,
       a few cells dug) and is no hunter in the open */
    if (_ctf_tunnelling(server, bot)) {
        return 0;
    }
    float     dx = bot->movement.position.x - carrier->movement.position.x;
    float     dy = bot->movement.position.y - carrier->movement.position.y;
    float     mine = dx * dx + dy * dy;
    int       closer = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == bot || !p->is_bot || p->team != bot->team || p->state != STATE_READY || !p->alive ||
            _ctf_tunnelling(server, p)) {
            continue;
        }
        float px = p->movement.position.x - carrier->movement.position.x;
        float py = p->movement.position.y - carrier->movement.position.y;
        if (px * px + py * py < mine && ++closer >= CTF_HUNTERS) {
            return 0;
        }
    }
    return 1;
}

/* floor f in column x, y holds a standing player: solid, three free cells
   over it */
static int _ctf_standable(map_store_t* map, int x, int y, int f)
{
    if (x < 1 || y < 1 || x >= map->size_x - 1 || y >= map->size_y - 1 || f < 3 || f >= map->size_z - 2) {
        return 0;
    }
    return map_solid(map, x, y, f) && !map_solid(map, x, y, f - 1) &&
           !map_solid(map, x, y, f - 2) && !map_solid(map, x, y, f - 3);
}

/* The base as it was: every block within BASE_R columns of each team's
   intel (8 levels above it to 4 below), with its colour, kept when the
   match starts. The guard puts back what is missing (a tunnel's mouth, a
   wall shot through, a grenade's crater), in the colour it had, nearest
   the intel first, when no enemy is near: the intel's walls stay shut. Our
   own walls are not in it (they were air), and neither is what the team
   itself builds */
#define BASE_R  10
#define BASE_UP 8
#define BASE_DN 4
#define BASE_NZ (BASE_UP + BASE_DN + 1)

static struct
{
    uint8_t  set;
    int      x0, y0, z0;
    uint8_t  solid[2 * BASE_R + 1][2 * BASE_R + 1][BASE_NZ];
    uint32_t color[2 * BASE_R + 1][2 * BASE_R + 1][BASE_NZ];
    uint64_t scan_at;          /* the last look for a hole */
    uint8_t  has_hole;         /* the hole to fill and where to stand */
    int      hole[3], stand[3];
    uint32_t hole_color;
    /* a cell filled three times is the guard's own way (its plan dug
       through the block it had just put, two cells in turn, over and
       over): left alone for a minute */
    uint8_t  was_air[2 * BASE_R + 1][2 * BASE_R + 1][BASE_NZ];
    uint8_t  fills[2 * BASE_R + 1][2 * BASE_R + 1][BASE_NZ];
    uint64_t skip_until[2 * BASE_R + 1][2 * BASE_R + 1][BASE_NZ];
} ctf_base[2];

static void _ctf_base_snapshot(server_t* server)
{
    map_store_t* map = &server->s_map.map;
    for (int t = 0; t < 2; t++) {
        vector3f_t intel = server->protocol.gamemode.intel[t];
        ctf_base[t].set      = server->protocol.current_gamemode != GAME_MODE_BABEL;
        ctf_base[t].x0       = (int) intel.x - BASE_R;
        ctf_base[t].y0       = (int) intel.y - BASE_R;
        ctf_base[t].z0       = (int) intel.z - BASE_UP;
        ctf_base[t].has_hole = 0;
        ctf_base[t].scan_at  = 0;
        memset(ctf_base[t].was_air, 0, sizeof(ctf_base[t].was_air));
        memset(ctf_base[t].fills, 0, sizeof(ctf_base[t].fills));
        memset(ctf_base[t].skip_until, 0, sizeof(ctf_base[t].skip_until));
        for (int i = 0; i < 2 * BASE_R + 1; i++) {
            for (int j = 0; j < 2 * BASE_R + 1; j++) {
                for (int k = 0; k < BASE_NZ; k++) {
                    int x = ctf_base[t].x0 + i, y = ctf_base[t].y0 + j, z = ctf_base[t].z0 + k;
                    uint8_t in = x >= 0 && y >= 0 && z >= 0 && x < map->size_x && y < map->size_y && z < 62;
                    ctf_base[t].solid[i][j][k] = in && map_solid(map, x, y, z);
                    ctf_base[t].color[i][j][k] = ctf_base[t].solid[i][j][k] ? map_color(map, x, y, z) : 0;
                }
            }
        }
    }
}

static int _ctf_standable(map_store_t* map, int x, int y, int f);

/* where a player stands to put a block at (hx, hy, hz): a cell beside it
   (never its own column: its body would refuse the block), the cell in
   reach of the eye */
static int _ctf_repair_stand(map_store_t* map, int hx, int hy, int hz, int* sx, int* sy, int* sf)
{
    float best = 1e9f;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            for (int f = hz - 1; f <= hz + 4; f++) {
                int x = hx + dx, y = hy + dy;
                if (!_ctf_standable(map, x, y, f) || f >= 62) {
                    continue;
                }
                float ex = x + 0.5f - (hx + 0.5f), ey = y + 0.5f - (hy + 0.5f), ez = (f - 2.0f) - (hz + 0.5f);
                float d  = sqrtf(ex * ex + ey * ey + ez * ez);
                if (d <= 3.5f && d < best) {
                    best = d;
                    *sx  = x;
                    *sy  = y;
                    *sf  = f;
                }
            }
        }
    }
    return best < 1e9f;
}

/* the hole of team t's base to fill next, looked for once a second */
static int _ctf_base_hole(server_t* server, int t)
{
    map_store_t* map = &server->s_map.map;
    uint64_t  now = get_nanos();
    if (!ctf_base[t].set) {
        return 0;
    }
    if (ctf_base[t].scan_at != 0 && now - ctf_base[t].scan_at < NANO_IN_SECOND &&
        (!ctf_base[t].has_hole ||
         !map_solid(map, ctf_base[t].hole[0], ctf_base[t].hole[1], ctf_base[t].hole[2])))
    {
        return ctf_base[t].has_hole;
    }
    ctf_base[t].scan_at  = now;
    ctf_base[t].has_hole = 0;
    vector3f_t intel = server->protocol.gamemode.intel[t];
    float      best  = 1e9f;
    for (int i = 0; i < 2 * BASE_R + 1; i++) {
        for (int j = 0; j < 2 * BASE_R + 1; j++) {
            for (int k = 0; k < BASE_NZ; k++) {
                int x = ctf_base[t].x0 + i, y = ctf_base[t].y0 + j, z = ctf_base[t].z0 + k;
                if (!ctf_base[t].solid[i][j][k]) {
                    continue;
                }
                uint8_t solid = map_solid(map, x, y, z);
                if (solid && ctf_base[t].was_air[i][j][k] && ++ctf_base[t].fills[i][j][k] >= 3) {
                    ctf_base[t].fills[i][j][k]      = 0;
                    ctf_base[t].skip_until[i][j][k] = now + 60ULL * NANO_IN_SECOND;
                    LOG_INFO("Base of team %d: cell %d,%d,%d filled 3 times, left alone for 60 s", t + 1, x, y, z);
                }
                ctf_base[t].was_air[i][j][k] = !solid;
                if (solid) {
                    continue;
                }
                float dx = x + 0.5f - intel.x, dy = y + 0.5f - intel.y, dz = z - intel.z;
                float d  = dx * dx + dy * dy + dz * dz;
                /* a block needs a solid neighbour: the far end of a tunnel
                   is filled first, its mouth after */
                /* a base at the map's edge has neighbours off it */
                uint8_t held = _ctf_solid(server, x + 1, y, z) || _ctf_solid(server, x - 1, y, z) ||
                               _ctf_solid(server, x, y + 1, z) || _ctf_solid(server, x, y - 1, z) ||
                               _ctf_solid(server, x, y, z + 1) || _ctf_solid(server, x, y, z - 1);
                uint8_t skipped = ctf_base[t].skip_until[i][j][k] > now;
                int     sx, sy, sf;
                if (d >= best || !held || skipped || !gamemode_cell_editable(server, x, y, z) ||
                    !_ctf_repair_stand(map, x, y, z, &sx, &sy, &sf))
                {
                    continue;
                }
                best                  = d;
                ctf_base[t].has_hole  = 1;
                ctf_base[t].hole[0]   = x;
                ctf_base[t].hole[1]   = y;
                ctf_base[t].hole[2]   = z;
                ctf_base[t].stand[0]  = sx;
                ctf_base[t].stand[1]  = sy;
                ctf_base[t].stand[2]  = sf;
                ctf_base[t].hole_color = ctf_base[t].color[i][j][k];
            }
        }
    }
    return ctf_base[t].has_hole;
}

/* the guard mends the base (_ctf_base_hole), the tent first when it has no
   blocks; only with our intel at home */
static int _ctf_repair_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    int t = bot->team == TEAM_A ? 0 : 1;
    if (ctf_dropped[t] || !_ctf_base_hole(server, t)) {
        return 0;
    }
    gamemode_vars_t* gm = &server->protocol.gamemode;
    if (bot->blocks == 0) {
        gm_goal_name(out, "restock", GM_INTENT_HOME);
        out->x     = gm->base[t].x;
        out->y     = gm->base[t].y;
        out->z     = gm->base[t].z;
        out->exact = 1;
        return 1;
    }
    gm_goal_name(out, "repair", GM_INTENT_HOME);
    out->x          = ctf_base[t].stand[0] + 0.5f;
    out->y          = ctf_base[t].stand[1] + 0.5f;
    out->z          = (float) ctf_base[t].stand[2];
    out->exact      = 1;
    out->work       = 1;
    out->work_x     = ctf_base[t].hole[0];
    out->work_y     = ctf_base[t].hole[1];
    out->work_z     = ctf_base[t].hole[2];
    out->work_color = ctf_base[t].hole_color;
    out->watch_x    = gm->base[1 - t].x;
    out->watch_y    = gm->base[1 - t].y;
    return 1;
}

/* The guard's post: a cell 2 to 5 off our intel that a player stands on,
   on the side the enemy comes from, with ground all round it (no edge it
   could step off while it turns and crouches). A fixed point 3 cells off
   was the air beside Raindance's intel tower: the bot planned a bridge
   block there, the cell is protected by the map, and it walked off the top
   and died of the fall, life after life */
static int _ctf_guard_post(server_t* server, vector3f_t intel, vector3f_t from, int* px, int* py, int* pz)
{
    map_store_t* map = &server->s_map.map;
    int       ix = (int) intel.x, iy = (int) intel.y, iz = (int) intel.z;
    float     dx = from.x - intel.x, dy = from.y - intel.y, d = sqrtf(dx * dx + dy * dy);
    float     ux = d > 0.1f ? dx / d : 0.0f, uy = d > 0.1f ? dy / d : 0.0f;
    float     best = -1e9f;
    for (int oy = -5; oy <= 5; oy++) {
        for (int ox = -5; ox <= 5; ox++) {
            float r = sqrtf((float) (ox * ox + oy * oy));
            if (r < 2.0f || r > 5.0f) {
                continue;
            }
            for (int f = iz - 1; f <= iz + 1; f++) {
                int x = ix + ox, y = iy + oy;
                if (!_ctf_standable(map, x, y, f)) {
                    continue;
                }
                int firm = 0;
                for (int ny = -1; ny <= 1; ny++) {
                    for (int nx = -1; nx <= 1; nx++) {
                        firm += (nx || ny) &&
                                (map_solid(map, x + nx, y + ny, f) || map_solid(map, x + nx, y + ny, f + 1));
                    }
                }
                /* behind the intel as the enemy comes: watching the intel it
                   watches them coming (on their side it had its back to them
                   and never saw who took the intel) */
                float score = (float) firm * 3.0f - fabsf(r - 3.0f) - ((float) ox * ux + (float) oy * uy) / r * 2.0f -
                              (float) abs(f - iz);
                if (score > best) {
                    best = score;
                    *px  = x;
                    *py  = y;
                    *pz  = f;
                }
            }
        }
    }
    return best > -1e9f;
}

/* Forward posts (large maps only): bases 300-400 blocks apart, the bots
   ran the whole way and the middle stayed empty. A few of a team's squads
   (one per POST_SPACING blocks between the bases, at most POST_GROUPS, the
   odd ones: never all of them) hold a post: two of the squad's bots stand
   at a high spot on the squad's lane (its first waypoint, a third of the
   way) behind a wall toward the enemy (the camper's) and stay there, no
   escort home. They stay in the squad, so the squad respawns by them, but
   lead no lane. The squad's other two either go on attacking (model
   "attack") or hold a second post at the lane's second waypoint (model
   "second"). SP_BOT_POSTS = off | attack | second picks it for the
   measurements (docs/bots.md, forward posts) */
#define POST_MIN_DIST 200.0f /* bases closer than this: no posts (2Fort ~70) */
#define POST_SPACING  150.0f
#define POST_GROUPS   3
#define POST_MAX      (2 * POST_GROUPS)
#define POST_HUNT     30.0f /* a holder leaves its post for a carrier this close */

static struct
{
    uint8_t    planned;
    int        n;                 /* posts */
    int        squad[POST_MAX];   /* the squad holding it */
    vector3f_t spot[POST_MAX][2]; /* each holder's cell (floor in z) */
    vector3f_t watch[POST_MAX];   /* where its wall faces */
    uint8_t    holder[POST_MAX][2];
} ctf_posts[2];

static int ctf_posts_model = -1; /* 0 off, 1 attack, 2 second */

static void _ctf_posts_reset(void)
{
    memset(ctf_posts, 0, sizeof(ctf_posts));
    memset(ctf_posted, 0xFF, sizeof(ctf_posted));
}

/* a post's place near (x, y): the highest flat dry column within 12 cells
   that two players stand on side by side (ox, oy across the lane) */
static int _ctf_post_site(server_t* server, float x, float y, float ox, float oy, vector3f_t out[2])
{
    map_store_t* map  = &server->s_map.map;
    float     best = -1e9f;
    for (int dy = -12; dy <= 12; dy++) {
        for (int dx = -12; dx <= 12; dx++) {
            int cx = (int) x + dx, cy = (int) y + dy;
            int bx = cx + (int) roundf(ox * 2.0f), by = cy + (int) roundf(oy * 2.0f);
            if (cx < 4 || cy < 4 || cx >= map->size_x - 4 || cy >= map->size_y - 4 || bx < 4 || by < 4 ||
                bx >= map->size_x - 4 || by >= map->size_y - 4)
            {
                continue;
            }
            int ax0, ay0, ax1, ay1;
            if (map_meta_play_area(server, &ax0, &ay0, &ax1, &ay1) &&
                (cx < ax0 + 3 || cy < ay0 + 3 || cx > ax1 - 3 || cy > ay1 - 3))
            {
                continue;
            }
            int z  = map_top(map, (uint16_t) cx, (uint16_t) cy);
            int bz = map_top(map, (uint16_t) bx, (uint16_t) by);
            if (z < 4 || z >= map->size_z - 2 || abs(bz - z) > 1 || !_ctf_standable(map, cx, cy, z) ||
                !_ctf_standable(map, bx, by, bz))
            {
                continue;
            }
            int flat = 0;
            for (int ny = -1; ny <= 1; ny++) {
                for (int nx = -1; nx <= 1; nx++) {
                    flat += abs(map_top(map, (uint16_t) (cx + nx), (uint16_t) (cy + ny)) - z) <= 1;
                }
            }
            if (flat < 8) {
                continue;
            }
            float score = (float) (map->size_z - z) - sqrtf((float) (dx * dx + dy * dy)) * 0.3f;
            if (score > best) {
                best   = score;
                out[0] = (vector3f_t){cx + 0.5f, cy + 0.5f, (float) z};
                out[1] = (vector3f_t){bx + 0.5f, by + 0.5f, (float) bz};
            }
        }
    }
    return best > -1e9f;
}

/* a bot of team t's squad s that holds no seat */
static int _ctf_post_member(server_t* server, uint8_t id, int t, int squad)
{
    player_t* p = NULL;
    HASH_FIND(hh, server->players, &id, sizeof(id), p);
    return p != NULL && p->is_bot && p->state != STATE_DISCONNECTED && (p->team == TEAM_A || p->team == TEAM_B) &&
           (p->team == TEAM_A ? 0 : 1) == t && p->squad == squad && p->squad_team == p->team && _ctf_seat_of(t, id) < 0;
}

/* a bot of the squad that holds no post yet, at random; 0xFF none */
static uint8_t _ctf_post_draw(server_t* server, int t, int squad)
{
    uint8_t   ids[SQUADS_MAX * 4 + 64];
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (n < (int) sizeof(ids) && ctf_posted[p->id] == 0xFF && _ctf_post_member(server, p->id, t, squad)) {
            ids[n++] = p->id;
        }
    }
    if (n == 0) {
        return 0xFF;
    }
    int k = (int) (gen_rand(&server->rand) * (float) n);
    return ids[k < n ? k : n - 1];
}

static void _ctf_posts_plan(server_t* server, int t)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    ctf_posts[t].planned = 1;
    memset(ctf_posts[t].holder, 0xFF, sizeof(ctf_posts[t].holder));
    if (ctf_posts_model < 0) {
        const char* env = getenv("SP_BOT_POSTS");
        ctf_posts_model = env == NULL ? 2 : !strcmp(env, "off") || !strcmp(env, "0") ? 0 : !strcmp(env, "attack") ? 1 : 2;
    }
    vector3f_t from = gm->base[t], to = gm->intel[1 - t];
    float      dx = to.x - from.x, dy = to.y - from.y, d = sqrtf(dx * dx + dy * dy);
    int        groups = d < POST_MIN_DIST || ctf_posts_model == 0 ? 0 : (int) (d / POST_SPACING);
    groups            = groups > POST_GROUPS ? POST_GROUPS : groups;
    float ux = d > 0.1f ? dx / d : 0.0f, uy = d > 0.1f ? dy / d : 0.0f, px = -uy, py = ux;
    /* the squads with bots in them: the odd ones hold posts (1, 3, 5) */
    int       members[SQUADS_MAX + 1] = {0};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->squad >= 1 && p->squad <= SQUADS_MAX && _ctf_post_member(server, p->id, t, p->squad)) {
            members[p->squad]++;
        }
    }
    /* the posts' places, spread over our half of the way (20 to 50%) so
       they fill it evenly: all at the lanes' waypoints, a third and two
       thirds of the way, the strips between the spawns and them stayed
       empty */
    int sqs[POST_GROUPS], nsq = 0, total = 0;
    for (int sq = 1; sq <= SQUADS_MAX && nsq < groups; sq += 2) {
        if (members[sq] >= 3) {
            sqs[nsq++] = sq;
            total += ctf_posts_model == 2 && members[sq] >= 4 ? 2 : 1;
        }
    }
    static const float sides[3] = {-1.0f, 1.0f, 0.0f};
    float width = fminf(fmaxf(d * 0.35f, 20.0f), 100.0f); /* the lanes' */
    for (int g = 0; g < nsq; g++) {
        int sq    = sqs[g];
        int posts = ctf_posts_model == 2 && members[sq] >= 4 ? 2 : 1;
        for (int k = 0; k < posts; k++) {
            /* the squad's two posts a half apart in the order: near, far */
            int        order = k == 0 ? g : nsq + g;
            float      f     = total > 1 ? 0.2f + 0.3f * (float) order / (float) (total - 1) : 0.33f;
            float      side  = sides[(sq - 1) % 3] * width * fminf(f / 0.33f, 1.0f);
            vector3f_t w     = _ctf_land_near(server, from.x + dx * f + px * side, from.y + dy * f + py * side);
            int        i     = ctf_posts[t].n;
            if (i >= POST_MAX || !_ctf_post_site(server, w.x, w.y, px, py, ctf_posts[t].spot[i])) {
                continue;
            }
            ctf_posts[t].squad[i] = sq;
            ctf_posts[t].watch[i] = (vector3f_t){ctf_posts[t].spot[i][0].x + ux * 40.0f,
                                                 ctf_posts[t].spot[i][0].y + uy * 40.0f, 0.0f};
            ctf_posts[t].n++;
            LOG_INFO("Bots of team %d: post %d (squad %d) at %.0f,%.0f,%.0f, %.0f%% of the way", t + 1, i, sq,
                     ctf_posts[t].spot[i][0].x, ctf_posts[t].spot[i][0].y, ctf_posts[t].spot[i][0].z, f * 100.0f);
        }
    }
}

/* once a tick while the bots play: posts planned, a holder that left its
   squad replaced by another of the squad */
static void _ctf_posts_tick(server_t* server)
{
    for (int t = 0; t < 2; t++) {
        if (!ctf_posts[t].planned) {
            _ctf_posts_plan(server, t);
        }
        for (int i = 0; i < ctf_posts[t].n; i++) {
            for (int h = 0; h < 2; h++) {
                uint8_t id = ctf_posts[t].holder[i][h];
                if (id != 0xFF && _ctf_post_member(server, id, t, ctf_posts[t].squad[i])) {
                    continue;
                }
                if (id != 0xFF) {
                    ctf_posted[id] = 0xFF;
                }
                id                        = _ctf_post_draw(server, t, ctf_posts[t].squad[i]);
                ctf_posts[t].holder[i][h] = id;
                if (id != 0xFF) {
                    ctf_posted[id] = (uint8_t) i;
                    player_t* p    = NULL;
                    HASH_FIND(hh, server->players, &id, sizeof(id), p);
                    evt_log(p, "bot_post", "action=assign post=%d squad=%d slot=%d cell=%.0f,%.0f,%.0f", i,
                            ctf_posts[t].squad[i], h, ctf_posts[t].spot[i][h].x, ctf_posts[t].spot[i][h].y,
                            ctf_posts[t].spot[i][h].z);
                }
            }
        }
    }
}

#define POST_COVER  40.0f /* a holder watches round our carrier this close */
#define POST_ATTACK 50.0f /* a squad's leader goes to clear an enemy post this close */
#define POST_PASSED 15.0f /* a post the carrier is this much nearer home than is behind it */

/* post i of team t has a holder alive at it */
static int _ctf_post_manned(server_t* server, int t, int i)
{
    for (int h = 0; h < 2; h++) {
        player_t* p  = NULL;
        uint8_t   id = ctf_posts[t].holder[i][h];
        if (id == 0xFF) {
            continue;
        }
        HASH_FIND(hh, server->players, &id, sizeof(id), p);
        if (p != NULL && p->alive && p->state == STATE_READY) {
            float dx = p->movement.position.x - ctf_posts[t].spot[i][h].x;
            float dy = p->movement.position.y - ctf_posts[t].spot[i][h].y;
            if (dx * dx + dy * dy < 6.0f * 6.0f) {
                return 1;
            }
        }
    }
    return 0;
}

/* the carrier's next stop home: the furthest of our posts still ahead of
   it (nearer home than it by POST_PASSED) that costs at most 40 blocks of
   detour; 0 when it is to go straight to the tent. It ran home alone
   across the open middle; now it runs from post to post, each post
   covering it */
static int _ctf_carrier_stop(server_t* server, player_t* bot, vector3f_t* out)
{
    int        t    = bot->team == TEAM_A ? 0 : 1;
    vector3f_t home = server->protocol.gamemode.base[t];
    float      bx = bot->movement.position.x, by = bot->movement.position.y;
    float      dh   = sqrtf((bx - home.x) * (bx - home.x) + (by - home.y) * (by - home.y));
    float      best = -1.0f;
    for (int i = 0; i < ctf_posts[t].n; i++) {
        vector3f_t c   = ctf_posts[t].spot[i][0];
        float      ph  = sqrtf((c.x - home.x) * (c.x - home.x) + (c.y - home.y) * (c.y - home.y));
        float      pb  = sqrtf((c.x - bx) * (c.x - bx) + (c.y - by) * (c.y - by));
        if (ph > dh - POST_PASSED || pb + ph - dh > 40.0f || !_ctf_post_manned(server, t, i)) {
            continue;
        }
        if (ph > best) {
            best = ph;
            *out = c;
        }
    }
    return best >= 0.0f;
}

/* an enemy post with a holder at it within POST_ATTACK of the bot, the
   nearest */
static int _ctf_enemy_post_near(server_t* server, player_t* bot, vector3f_t* out)
{
    int   e    = bot->team == TEAM_A ? 1 : 0;
    float best = POST_ATTACK * POST_ATTACK;
    int   hit  = 0;
    for (int i = 0; i < ctf_posts[e].n; i++) {
        vector3f_t c  = ctf_posts[e].spot[i][0];
        float      dx = c.x - bot->movement.position.x, dy = c.y - bot->movement.position.y;
        if (dx * dx + dy * dy < best && _ctf_post_manned(server, e, i)) {
            best = dx * dx + dy * dy;
            *out = c;
            hit  = 1;
        }
    }
    return hit;
}

/* a holder's goal: its cell of the post, the wall toward the enemy. A
   carrier of either intel close by, or the enemy intel lying near, comes
   first */
static int _ctf_post_goal(server_t* server, player_t* bot, player_t* carrier, gm_bot_goal_t* out)
{
    int     t = bot->team == TEAM_A ? 0 : 1;
    uint8_t i = ctf_posted[bot->id];
    if (i == 0xFF || i >= ctf_posts[t].n || bot->has_intel) {
        return 0;
    }
    if (carrier != NULL && carrier->alive) {
        float cx = carrier->movement.position.x - bot->movement.position.x;
        float cy = carrier->movement.position.y - bot->movement.position.y;
        if (cx * cx + cy * cy < POST_HUNT * POST_HUNT) {
            return 0;
        }
    }
    gamemode_vars_t* gm    = &server->protocol.gamemode;
    uint8_t          enemy = _enemy(bot);
    if (!gm->intel_held[enemy] && ctf_dropped[enemy]) {
        float ix = gm->intel[enemy].x - bot->movement.position.x, iy = gm->intel[enemy].y - bot->movement.position.y;
        if (ix * ix + iy * iy < 25.0f * 25.0f) { /* CTF_GRAB_NEAR */
            return 0;
        }
    }
    int        s    = ctf_posts[t].holder[i][1] == bot->id ? 1 : 0;
    vector3f_t spot = ctf_posts[t].spot[i][s];
    gm_goal_name(out, "post", GM_INTENT_POST);
    out->x          = spot.x;
    out->y          = spot.y;
    out->z          = spot.z;
    out->watch_x    = ctf_posts[t].watch[i].x;
    out->watch_y    = ctf_posts[t].watch[i].y;
    /* our carrier passing by: the post watches round it, where the ones
       chasing it come from */
    uint8_t   ours = gm->player_intel_team[bot->team];
    player_t* c    = NULL;
    if (gm->intel_held[enemy] && ours != 0xFF) {
        HASH_FIND(hh, server->players, &ours, sizeof(ours), c);
    }
    if (c != NULL && c->alive) {
        float cx = c->movement.position.x - spot.x, cy = c->movement.position.y - spot.y;
        if (cx * cx + cy * cy < POST_COVER * POST_COVER) {
            out->watch_x = c->movement.position.x;
            out->watch_y = c->movement.position.y;
        }
    }
    return 1;
}

/* the CTF loop a player follows: take the enemy intel, carry it into our own
   tent, hunt whoever carries ours. Pickup and capture happen in
   handleTentAndIntel exactly like for a human, so the bot only has to stand
   by the intel's cell (the check is a box round it) */
#define CTF_GRAB_SMG  80.0f /* an SMG bot goes for a dropped enemy intel this far */
#define CTF_GRAB_NEAR 25.0f /* any other bot on its way, this close */

static int _ctf_grabs_dropped(player_t* bot, vector3f_t intel)
{
    if (bot_holds_post(bot) && bot->weapon != WEAPON_SMG) {
        return 0; /* a camper keeps its distance */
    }
    float dx = intel.x - bot->movement.position.x, dy = intel.y - bot->movement.position.y;
    float r  = bot->weapon == WEAPON_SMG ? CTF_GRAB_SMG : CTF_GRAB_NEAR;
    return dx * dx + dy * dy < r * r;
}

/* A flank approach for an attacker without a squad lane on a large map: it
   first walks to a staging point 34..80 cells off the straight base-to-base
   line, then turns for the intel. Picked once per life (player_t.flank_*) so
   a replan does not send it back to the staging point. The squad lanes
   already do this for squadded bots; this covers the rest. */
static int _ctf_flank_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    if (!bot->alive || bot->flank_stage >= 2 || out->role != GM_BOT_ATTACK || bot->has_intel) {
        return 0;
    }
    gamemode_vars_t* gm    = &server->protocol.gamemode;
    uint8_t          enemy = _enemy(bot);
    if (bot->flank_stage == 0) {
        float dx = gm->intel[enemy].x - gm->base[bot->team].x;
        float dy = gm->intel[enemy].y - gm->base[bot->team].y;
        float d  = sqrtf(dx * dx + dy * dy);
        /* one attacker in three, and only where a flank is worth the walk */
        if (d < 140.0f || (bot->id % 3) != 1 || ctf_dropped[enemy]) {
            bot->flank_stage = 2;
            return 0;
        }
        float px = -dy / d, py = dx / d;
        float side = (bot->id & 1) ? 1.0f : -1.0f;
        float off  = 34.0f + (float) ((bot->id * 13) % 47);
        float f    = 0.42f + (float) ((bot->id * 7) % 16) / 100.0f;
        /* on the ground near it (_ctf_land_near), not on a roof there */
        vector3f_t at = _ctf_land_near(server, gm->base[bot->team].x + dx * f + px * off * side,
                                       gm->base[bot->team].y + dy * f + py * off * side);
        bot->flank_x  = at.x;
        bot->flank_y  = at.y;
        bot->flank_stage = 1;
        evt_log(bot, "bot_flank", "action=pick at=%.0f,%.0f off=%.0f side=%.0f", bot->flank_x, bot->flank_y, off,
                side);
    }
    float dx = bot->flank_x - bot->movement.position.x;
    float dy = bot->flank_y - bot->movement.position.y;
    if (dx * dx + dy * dy > 8.0f * 8.0f) {
        gm_goal_name(out, "flank", 0);
        out->x     = bot->flank_x;
        out->y     = bot->flank_y;
        out->z     = map_top(&server->s_map.map, (uint16_t) bot->flank_x, (uint16_t) bot->flank_y);
        out->exact = 0;
        return 1;
    }
    bot->flank_stage = 2;
    evt_log(bot, "bot_flank", "action=pass at=%.0f,%.0f", bot->flank_x, bot->flank_y);
    return 0;
}

int _ctf_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    gamemode_vars_t* gm      = &server->protocol.gamemode;
    uint8_t          enemy   = _enemy(bot);
    player_t*        carrier = NULL;
    if (bot->team == TEAM_SPECTATOR) {
        return 0;
    }
    if (gm->intel_held[bot->team]) {
        uint8_t id = gm->player_intel_team[enemy];
        HASH_FIND(hh, server->players, &id, sizeof(id), carrier);
    }
    memset(out, 0, sizeof(*out));
    out->role     = _ctf_role(server, bot);
    if (_ctf_seats_on(server)) {
        out->seated = 1;
        out->camp   = seat_camp[bot->id];
    } else if (classes_on(server) && _ctf_camping_marksman(server, bot)) {
        /* no seats where the classes play, but the Marksman's way is the
           camper's: high ground on its side with a view of the enemy's
           way (_bot_camp_spot), the rifle from behind its castle wall */
        out->seated = 1;
        out->camp   = 1;
    }
    out->has_home = 1;
    out->home_x   = gm->intel[bot->team].x + 0.5f;
    out->home_y   = gm->intel[bot->team].y + 0.5f;
    out->home_z   = gm->intel[bot->team].z;
    if (out->seated && _ctf_briefing_goal(server, bot, out)) {
        return 1;
    }
    /* a landmine (the Marksman's perk) goes by our intel, where the enemy
       must step: one put down by our spawn waited all match. The bot puts it
       when it is there (_bot_sentry) */
    if (!bot->has_intel && !gm->intel_held[bot->team] && perks_holds(server, bot, P10_PERK_LANDMINE) &&
        explosives_room(bot, P10_PERK_LANDMINE))
    {
        vector3f_t intel = gm->intel[bot->team];
        vector3f_t from  = gm->base[enemy];
        float      dx = from.x - intel.x, dy = from.y - intel.y, d = sqrtf(dx * dx + dy * dy);
        float      bx = intel.x - bot->movement.position.x, by = intel.y - bot->movement.position.y;
        if (bx * bx + by * by < 90.0f * 90.0f) {
            gm_goal_name(out, "plant-mine", GM_INTENT_PLANT);
            out->x       = intel.x + 0.5f + (d > 0.1f ? dx / d * 2.0f : 0.0f);
            out->y       = intel.y + 0.5f + (d > 0.1f ? dy / d * 2.0f : 0.0f);
            out->z       = intel.z;
            out->hold    = 1.5f;
            out->watch_x = from.x;
            out->watch_y = from.y;
            return 1;
        }
    }
    if (_ctf_post_goal(server, bot, carrier, out)) {
        return 1;
    }
    vector3f_t enemy_post;
    if (out->role == GM_BOT_DEFEND && !bot->has_intel && !gm->intel_held[bot->team] &&
        _ctf_repair_goal(server, bot, out))
    {
        return 1;
    }
    if (out->role == GM_BOT_DEFEND && !bot->has_intel && !gm->intel_held[bot->team]) {
        /* a few cells off our intel (in our tent or dropped somewhere), on
           the side the enemy comes from */
        vector3f_t intel = gm->intel[bot->team];
        vector3f_t from  = gm->base[enemy];
        float      dx = from.x - intel.x, dy = from.y - intel.y, d = sqrtf(dx * dx + dy * dy);
        int        px, py, pz;
        gm_goal_name(out, "defend", GM_INTENT_GUARD | GM_INTENT_HOME);
        if (_ctf_guard_post(server, intel, from, &px, &py, &pz)) {
            out->x = px + 0.5f;
            out->y = py + 0.5f;
            out->z = (float) pz;
        } else {
            out->x = intel.x + 0.5f + (d > 0.1f ? dx / d * 3.0f : 0.0f);
            out->y = intel.y + 0.5f + (d > 0.1f ? dy / d * 3.0f : 0.0f);
            out->z = intel.z;
        }
        out->watch_x = from.x;
        out->watch_y = from.y;
        return 1;
    }
    player_t* mate = out->role == GM_BOT_COVER && !bot->has_intel ? _ctf_infiltrator(server, bot) : NULL;
    if (mate != NULL && mate != bot) {
        /* the bodyguard: next to the infiltrator, looking where it looks (it
           has the intel or not: then it is its escort home) */
        gm_goal_name(out, "cover", GM_INTENT_COVER);
        out->x       = mate->movement.position.x;
        out->y       = mate->movement.position.y;
        out->z       = mate->movement.position.z + 2.4f;
        out->watch_x = mate->movement.position.x + mate->movement.forward_orientation.x * 10.0f;
        out->watch_y = mate->movement.position.y + mate->movement.forward_orientation.y * 10.0f;
        out->hold    = 3.0f;
        return 1;
    }
    if (bot->has_intel) {
        /* the tent is the 2x2 cells ending at base.x/base.y, so its centre is
           base.x/base.y itself (check_player_in_tent) */
        vector3f_t stop;
        gm_goal_name(out, "carry-home", 0);
        out->x     = gm->base[bot->team].x;
        out->y     = gm->base[bot->team].y;
        out->z     = gm->base[bot->team].z;
        out->exact = 1;
        out->rush  = 1;
        if (_ctf_carrier_stop(server, bot, &stop)) {
            gm_goal_name(out, "carry-via-post", 0);
            out->x     = stop.x;
            out->y     = stop.y;
            out->z     = stop.z;
            out->exact = 0;
        }
    } else if (carrier != NULL && carrier->alive && _ctf_hunts(server, bot, carrier, out->role)) {
        gm_goal_name(out, "hunt-carrier", GM_INTENT_CHASE);
        out->x    = carrier->movement.position.x;
        out->y    = carrier->movement.position.y;
        out->z    = carrier->movement.position.z + 2.4f;
    } else if (!gm->intel_held[enemy] && ctf_dropped[enemy] && _ctf_grabs_dropped(bot, gm->intel[enemy])) {
        /* the enemy intel lies where its carrier died: the SMG goes for it
           from afar, anyone close by on its way (bots on their lane walked
           past it). Up close is the SMG's fight */
        gm_goal_name(out, "take-intel", GM_INTENT_TAKE | GM_INTENT_PUSH);
        out->x     = floorf(gm->intel[enemy].x) + 0.5f;
        out->y     = floorf(gm->intel[enemy].y) + 0.5f;
        out->z     = gm->intel[enemy].z;
        out->exact = 1;
    } else if (!gm->intel_held[enemy] && (out->role == GM_BOT_ATTACK || out->role == GM_BOT_RUSH) &&
               bot->squad >= 1 && bot->squad <= SQUADS_MAX && bot->squad_team == bot->team &&
               lanes[bot->team == TEAM_A ? 0 : 1][bot->squad].leader == bot->id &&
               _ctf_enemy_post_near(server, bot, &enemy_post))
    {
        /* an enemy post on the squad's way: cleared, not walked past (its
           squad follows the leader there) */
        gm_goal_name(out, "clear-post", 0);
        out->x    = enemy_post.x;
        out->y    = enemy_post.y;
        out->z    = enemy_post.z;
    } else if (!gm->intel_held[enemy] && (out->role == GM_BOT_ATTACK || out->role == GM_BOT_RUSH) &&
               _ctf_squad_goal(server, bot, out))
    {
        /* led along its squad's lane, or leading it */
    } else if (!gm->intel_held[enemy] && (out->role == GM_BOT_ATTACK || out->role == GM_BOT_RUSH) &&
               _ctf_flank_goal(server, bot, out))
    {
        /* an unsquadded attacker takes the long way round first */
    } else if (!gm->intel_held[enemy]) {
        /* pickup is a box of 3 blocks round the intel (check_player_on_intel);
           the exact cell is still the surest place to wait */
        gm_goal_name(out, "take-intel", GM_INTENT_TAKE | GM_INTENT_PUSH);
        out->x     = floorf(gm->intel[enemy].x) + 0.5f;
        out->y     = floorf(gm->intel[enemy].y) + 0.5f;
        out->z     = gm->intel[enemy].z;
        out->exact = 1;
    } else {
        /* a teammate has the enemy intel: every other bot of the team goes
           to it and fights in the open around it, even if it dies for it,
           to keep the enemy off the carrier. (It went to our base: on a big
           map the carrier took minutes to come home, and every bot that
           respawned meanwhile milled about in the base) */
        player_t* ours = NULL;
        uint8_t   id   = gm->player_intel_team[bot->team];
        if (id != 0xFF) {
            HASH_FIND(hh, server->players, &id, sizeof(id), ours);
        }
        /* a Miner keeps digging its way to where the enemy intel lay at
           its base (it comes back to that spawn area after the capture):
           an escort with a shotgun,
           pulled out of its tunnel, ended 80 of its tunnels in eight
           matches after a few cells, and the next attack began from the
           surface again */
        if (_ctf_tunnels(server, bot) && (out->role == GM_BOT_ATTACK || out->role == GM_BOT_RUSH)) {
            gm_goal_name(out, "tunnel-on", GM_INTENT_TAKE | GM_INTENT_PUSH);
            out->x = ctf_home[enemy].x;
            out->y = ctf_home[enemy].y;
            out->z = ctf_home[enemy].z;
            /* there, it waits for the intel to come back: on the spot it
               turned round and round (bot_path action=stuck dist=0.5) */
            out->hold    = 3.0f;
            out->watch_x = ctf_home[enemy].x;
            out->watch_y = ctf_home[enemy].y;
            return 1;
        }
        gm_goal_name(out, "escort", GM_INTENT_WITH);
        out->assault = 1;
        if (ours != NULL && ours->alive) {
            out->x       = ours->movement.position.x;
            out->y       = ours->movement.position.y;
            out->z       = ours->movement.position.z + 2.4f;
            out->hold    = 8.0f;
            out->watch_x = ours->movement.position.x + ours->movement.forward_orientation.x * 10.0f;
            out->watch_y = ours->movement.position.y + ours->movement.forward_orientation.y * 10.0f;
        } else {
            out->x = gm->base[bot->team].x;
            out->y = gm->base[bot->team].y;
            out->z = gm->base[bot->team].z;
        }
    }
    return 1;
}

/* patrols go round the enemy base */
/* Babel's platform is a slab at z = 1 over x 206..306, y 240..272, the
   intel on it, the ground 40 levels below. The rules (_babel_cell_protected)
   leave a strip one cell wide round it at z = 1 where nothing is built, but
   allow z = 2 there: a player builds a pillar in that strip up to floor 2
   and steps up onto the slab, and with the intel digs the pillar back down.
   The bots do the same (gm_bot_goal_t.climb): the planner alone spent its
   budget on the ground and they fell off bridges into the moat (77 falls
   in 480 s on Nine-Eye_Babel, no pickup) */
#define BABEL_Y0 241
#define BABEL_Y1 271

static int _babel_strip_x(server_t* server, uint8_t team)
{
    return server->protocol.gamemode.base[team].x < server->s_map.map.size_x / 2 ? 205 : 307;
}

static int _babel_floor(player_t* p)
{
    return (int) roundf(p->movement.position.z + 2.4f);
}

/* the ground next to the strip, outward (where a pillar in it started) */
static int _babel_ground(server_t* server, int x, int y)
{
    return map_top(&server->s_map.map, (uint16_t) (x == 205 ? 204 : 308), (uint16_t) y);
}

/* a column of our strip to climb: dry ground, one per bot (by id) so two
   bots do not build in the same column */
static int _babel_ladder(server_t* server, player_t* bot, int* lx, int* ly)
{
    int x = _babel_strip_x(server, bot->team), start = 256 + ((int) (bot->id % 8) - 4) * 3;
    for (int k = 0; k < 2 * (BABEL_Y1 - BABEL_Y0); k++) {
        int y = start + ((k & 1) ? -(k + 1) / 2 : k / 2);
        if (y < BABEL_Y0 || y > BABEL_Y1) {
            continue;
        }
        int top = map_top(&server->s_map.map, (uint16_t) x, (uint16_t) y);
        if (top >= 20 && top < server->s_map.map.size_z - 2) {
            *lx = x;
            *ly = y;
            return 1;
        }
    }
    return 0;
}

/* the nearest pillar, the way down, its top in *lz: our own strip first
   (the carrier went down the enemy's side and had the whole plain to cross);
   one already partly dug counts (only a top at 2 did, and a carrier whose
   pillar someone was digging walked off the slab) */
static int _babel_way_down(server_t* server, player_t* bot, int* lx, int* ly, int* lz)
{
    float best = 1e9f;
    int   own  = _babel_strip_x(server, bot->team);
    for (int side = 0; side < 2 && best >= 1e9f; side++) {
        int x = side ? (own == 205 ? 307 : 205) : own;
        for (int y = BABEL_Y0; y <= BABEL_Y1; y++) {
            int top = map_top(&server->s_map.map, (uint16_t) x, (uint16_t) y);
            if (top < 2 || top > _babel_ground(server, x, y) - 3) {
                continue;
            }
            float dx = x + 0.5f - bot->movement.position.x, dy = y + 0.5f - bot->movement.position.y;
            /* a top flush with the slab first: a drop onto a dug one cell
               pillar carries the body over it */
            float d = dx * dx + dy * dy + (top > 2 ? 400.0f * (top - 2) : 0.0f);
            if (d < best) {
                best = d;
                *lx  = x;
                *ly  = y;
                *lz  = top;
            }
        }
    }
    return best < 1e9f;
}

/* Each team's staircase to the slab, two blocks wide, the way players climb
   Babel together: step k (0 at the top, floor 2 in our strip, next to the
   slab) is a block at z = 2 + k in column x[k], held up by a support block
   under it (z = 3 + k) that touches step k + 1 beside it. Built from the
   ground up by the team's builders, broken from its foot by an enemy, and
   climbed by everyone once whole. A pillar each (the old way) took ~430
   blocks a match and nobody fought over it */
#define BABEL_STAIR_MAX 56

static struct {
    int len;                    /* steps; 0: no staircase on this map */
    int dir;                    /* outward from the slab: -1 west, +1 east */
    int y0;                     /* its lanes: y0 and y0 + 1 */
    int x[BABEL_STAIR_MAX];     /* step k's column */
    int ground[BABEL_STAIR_MAX][2]; /* each lane's ground top there before any block */
} babel_stair[2];

/* the steps down from the strip until a support rests on the ground; the
   lanes nearest the slab's middle whose cells are all free air or ground */
static void _babel_stair_plan(server_t* server)
{
    map_store_t* map = &server->s_map.map;
    for (int t = 0; t < 2; t++) {
        int sx                = _babel_strip_x(server, (uint8_t) t);
        babel_stair[t].len    = 0;
        babel_stair[t].dir    = sx == 205 ? -1 : 1;
        for (int o = 0; o < 12 && babel_stair[t].len == 0; o++) {
            int y0 = 255 + ((o & 1) ? -(o + 1) / 2 : o / 2) * 2;
            int ok = 1, len = 0;
            for (int k = 0; k < BABEL_STAIR_MAX && ok; k++) {
                int x = sx + babel_stair[t].dir * k;
                int g0 = map_top(map, (uint16_t) x, (uint16_t) y0);
                int g1 = map_top(map, (uint16_t) x, (uint16_t) (y0 + 1));
                /* down to the lower lane's ground: in the higher one the
                   last cells are inside it already. Stopped at the higher
                   ground, the lower lane's last support hung in the air and
                   the server refused it (249 times a match) */
                int g  = g0 > g1 ? g0 : g1;
                babel_stair[t].x[k]         = x;
                babel_stair[t].ground[k][0] = g0;
                babel_stair[t].ground[k][1] = g1;
                for (int l = 0; l < 2 && ok; l++) {
                    int gl = l ? g1 : g0;
                    for (int z = k - 1 > 0 ? k - 1 : 2; z <= 3 + k && z < gl; z++) {
                        /* the step, its support and the room over the step */
                        ok = !map_solid(map, x, y0 + l, z) &&
                             (z < 2 + k || gamemode_cell_editable(server, x, y0 + l, z));
                    }
                }
                len = k + 1;
                /* down until the step rests on the ground (its support is
                   the ground itself), one block above it: ending where the
                   support reached the ground left the lowest step two up,
                   which nobody climbs, and the builders stood under it all
                   match (plans of 40000 nodes, a staircase stuck at 34/204) */
                if (g <= 3 + k) {
                    break;
                }
            }
            if (ok && len < BABEL_STAIR_MAX) {
                babel_stair[t].len = len;
                babel_stair[t].y0  = y0;
            }
        }
        LOG_INFO("Babel staircase for team %d: %d steps from x %d, lanes y %d-%d", t, babel_stair[t].len, sx,
                 babel_stair[t].y0, babel_stair[t].y0 + 1);
    }
}

/* One wall per team between its staircase's foot (and its tent) and the
   enemy: it shields what the team stands on from the side the enemy comes
   from, across the ground approach. It used to stand 5 columns outward of
   the foot, on the far side of the staircase, where it shielded nothing the
   enemy crossed: players saw the bots build it behind the staircase, on the
   side away from the enemy. It goes 5 columns from the foot toward the
   enemy's tent, long enough to cover the foot's lanes and the tent's side,
   3 blocks high from each column's own ground, with every third column's top
   cell left open as a window. The top cell is where a standing eye is (the
   castle wall's lesson: a two high gap put the merlons at eye height and
   held 40 of the 70 shots its own wall stopped). The wallers hold the
   windows behind it, looking out at the enemy. Planned once a map like the
   staircase; a cell dug out is put back from the same plan */
#define BABEL_WALL_LEN  20  /* columns across the approach */
#define BABEL_WALL_HIGH 3   /* g-1, g-2, the eye's cell at g-3 */
#define BABEL_WALL_OUT  5   /* columns from the foot toward the enemy: past the
                               guard's post (3 out), which then stands behind
                               the wall and shoots through a window too */
#define BABEL_WALL_EVERY 3  /* every third column is a window */

static struct {
    int n;          /* columns planned; 0: no wall on this map */
    int x;          /* the line's column, the same for all of them */
    int dir;        /* from our side toward the enemy: +1 east, -1 west */
    int y[BABEL_WALL_LEN];
    int ground[BABEL_WALL_LEN]; /* each column's own ground top */
    int window[BABEL_WALL_LEN]; /* 1: its top cell is left open */
} babel_wall[2];

/* a cell of our staircase (a step or the support under it): the wall never
   stands in one, however the two plans meet on a map */
static int _babel_wall_on_stair(int t, int x, int y, int z)
{
    if (y != babel_stair[t].y0 && y != babel_stair[t].y0 + 1) {
        return 0;
    }
    for (int k = 0; k < babel_stair[t].len; k++) {
        if (babel_stair[t].x[k] == x && (z == 2 + k || z == 3 + k)) {
            return 1;
        }
    }
    return 0;
}

static void _babel_wall_plan(server_t* server)
{
    map_store_t* map = &server->s_map.map;
    for (int t = 0; t < 2; t++) {
        babel_wall[t].n   = 0;
        babel_wall[t].dir = 0;
        if (babel_stair[t].len == 0) {
            continue;
        }
        int        len    = babel_stair[t].len;
        int        foot_x = babel_stair[t].x[len - 1], foot_y = babel_stair[t].y0 + 1;
        vector3f_t tent   = server->protocol.gamemode.base[t];
        vector3f_t enemy  = server->protocol.gamemode.base[1 - t];
        /* the side the enemy comes from is our foot's way to its tent: a
           team's base and staircase sit on its own side of the slab, the
           enemy's on the other. Read from the tents, never from `dir`
           alone: `dir` is outward from the slab, which is our side */
        int edir = enemy.x >= (float) foot_x ? 1 : -1;
        int wx   = foot_x + edir * BABEL_WALL_OUT;
        /* cover the foot's lanes and the tent's side; a tent far from the
           foot keeps the wall on the foot */
        int cy = foot_y;
        if (fabsf(tent.y - (float) foot_y) <= (float) (BABEL_WALL_LEN - 2)) {
            cy = (int) roundf((tent.y + (float) foot_y) * 0.5f);
        }
        int start = cy - BABEL_WALL_LEN / 2;
        int end   = start + BABEL_WALL_LEN - 1;
        babel_wall[t].dir = edir;
        if (wx < 1 || wx + 1 >= map->size_x) {
            LOG_INFO("Babel wall for team %d: no room at x %d", t, wx);
            continue;
        }
        int n = 0;
        for (int i = 0; i < BABEL_WALL_LEN; i++) {
            int y = start + i;
            if (y < 0 || y >= map->size_y) {
                continue;
            }
            /* never the tent's own 2x2 cells */
            if (wx >= (int) tent.x - 2 && wx <= (int) tent.x + 1 && y >= (int) tent.y - 2 && y <= (int) tent.y + 1) {
                continue;
            }
            int g = map_top(map, (uint16_t) wx, (uint16_t) y);
            /* dry ground with room over it, and the column behind it dry too
               (that is where its holder stands, on our side) */
            if (g >= map->size_z - 1 || g <= BABEL_WALL_HIGH ||
                map_top(map, (uint16_t) (wx - edir), (uint16_t) y) >= map->size_z - 1)
            {
                continue;
            }
            int ok = 1;
            for (int z = g - 1; z >= g - BABEL_WALL_HIGH && ok; z--) {
                ok = !map_solid(map, (uint16_t) wx, (uint16_t) y, (uint16_t) z) &&
                     !_babel_wall_on_stair(t, wx, y, z) && gamemode_cell_editable(server, wx, y, z);
            }
            if (!ok) {
                continue;
            }
            babel_wall[t].y[n]      = y;
            babel_wall[t].ground[n] = g;
            babel_wall[t].window[n] = i % BABEL_WALL_EVERY == 0;
            n++;
        }
        babel_wall[t].n = n;
        babel_wall[t].x = wx;
        if (n > 0) {
            int windows = 0;
            for (int i = 0; i < n; i++) {
                windows += babel_wall[t].window[i];
            }
            if (windows == 0) {
                /* the lake cut the columns the windows were on: one in the
                   middle, or the holders would have nothing to shoot through
                   (and no goal once the wall is whole) */
                babel_wall[t].window[n / 2] = 1;
            }
        }
        /* the numbers the side is judged by: the middle of the line is
           closer to the enemy's tent than the foot is, and the enemy's way
           to the foot crosses the line (a wall behind the staircase passed
           neither: it was 5 columns past the foot, out of that way) */
        float mid_y  = n > 0 ? ((float) babel_wall[t].y[0] + (float) babel_wall[t].y[n - 1]) * 0.5f : (float) cy;
        float d_wall = (mid_y - enemy.y) * (mid_y - enemy.y) + ((float) wx - enemy.x) * ((float) wx - enemy.x);
        float d_foot = ((float) foot_y - enemy.y) * ((float) foot_y - enemy.y) +
                       ((float) foot_x - enemy.x) * ((float) foot_x - enemy.x);
        int closer = d_wall < d_foot;
        int cross  = 0;
        if (n > 0 && enemy.x != (float) foot_x) {
            float f  = ((float) wx - enemy.x) / ((float) foot_x - enemy.x);
            float yc = enemy.y + f * ((float) foot_y - enemy.y);
            cross    = f > 0.0f && f < 1.0f && yc >= (float) babel_wall[t].y[0] - 0.5f &&
                    yc <= (float) babel_wall[t].y[n - 1] + 0.5f;
        }
        int ok    = n > 0 && closer && cross;
        if (n > 0) {
            evt_log(NULL, "bot_babel",
                    "action=wall_plan team=%d foot=%d,%d tent=%.0f,%.0f enemy_tent=%.0f,%.0f from=%d,%d to=%d,%d "
                    "columns=%d faces=enemy closer=%d cross=%d ok=%d",
                    t, foot_x, foot_y, tent.x, tent.y, enemy.x, enemy.y, wx, babel_wall[t].y[0], wx,
                    babel_wall[t].y[n - 1], n, closer, cross, ok);
        } else {
            evt_log(NULL, "bot_babel",
                    "action=wall_plan team=%d foot=%d,%d tent=%.0f,%.0f enemy_tent=%.0f,%.0f from=%d,%d to=%d,%d "
                    "columns=0 faces=enemy closer=%d cross=%d ok=0",
                    t, foot_x, foot_y, tent.x, tent.y, enemy.x, enemy.y, wx, start, wx, end, closer, cross);
        }
        LOG_INFO("Babel wall for team %d: %d columns at x %d, y %d-%d, facing the enemy at x %.0f", t, n, wx, start,
                 end, enemy.x);
    }
}

/* the next cell to build in lane l (bottom up: a support, then its step),
   0 when the lane is whole */
static int _babel_stair_next(server_t* server, int t, int l, int* x, int* y, int* z, int* k_out)
{
    for (int k = babel_stair[t].len - 1; k >= 0; k--) {
        int cx = babel_stair[t].x[k], cy = babel_stair[t].y0 + l;
        for (int s = 1; s >= 0; s--) {
            int cz = 2 + k + s;
            if (cz >= babel_stair[t].ground[k][l] || map_solid(&server->s_map.map, cx, cy, cz)) {
                continue;
            }
            *x     = cx;
            *y     = cy;
            *z     = cz;
            *k_out = k;
            return 1;
        }
    }
    return 0;
}

/* every cell of team t's staircase is there */
static int _babel_stair_whole(server_t* server, int t)
{
    int x, y, z, k;
    return !_babel_stair_next(server, t, 0, &x, &y, &z, &k) && !_babel_stair_next(server, t, 1, &x, &y, &z, &k);
}

/* the highest block of team t's staircase within the first steps, what a
   breaker on the slab's edge spades: the stair cut at its top, under the
   climbers' feet. From its foot meant crossing the plain under the tower,
   which on Nine-Eye is a lake: the breakers fell in, no block broken. 0 none */
#define BABEL_BREAK_STEPS 3
static int _babel_stair_top(server_t* server, int t, int* x, int* y, int* z)
{
    for (int k = 0; k < babel_stair[t].len && k < BABEL_BREAK_STEPS; k++) {
        for (int s = 0; s <= 1; s++) {
            for (int l = 0; l < 2; l++) {
                int cx = babel_stair[t].x[k], cy = babel_stair[t].y0 + l, cz = 2 + k + s;
                if (cz < babel_stair[t].ground[k][l] && map_solid(&server->s_map.map, cx, cy, cz) &&
                    gamemode_cell_editable(server, cx, cy, cz))
                {
                    *x = cx;
                    *y = cy;
                    *z = cz;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* the bot's place among its team's bots, by id (the same every life), and
   how many bots the team has */
static int _babel_rank(server_t* server, player_t* bot, int* team_bots)
{
    int       rank = 0;
    player_t *p, *tmp;
    *team_bots = 0;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot && p->team == bot->team) {
            (*team_bots)++;
            rank += p != bot && p->id < bot->id;
        }
    }
    return rank;
}

/* How a team of Babel bots divides its jobs: a quarter builds the wall, 15%
   hunts, and the rest raise the staircase (which is where a team's bots all
   used to be). With fewer than 4 bots the whole team builds (a crew of two
   still puts one up; two builders never finished one) and the crew never
   drops under two. At 8 bots a team that is 5 crew, 2 wallers (one of them
   the guard) and a hunter: with a quarter hunting, babelstairs' staircase
   took 172 s with the wallers' help and the crew has to outweigh them. The
   guard is the wallers' last rank, the breaker the crew's (_babel_role_goal,
   _babel_stair_goal) */
#define BABEL_WALL_SHARE 0.25f
#define BABEL_HUNT_SHARE 0.15f

static void _babel_roles(int team_bots, int has_wall, int* wall_n, int* hunt_n, int* crew_n)
{
    *wall_n = *hunt_n = 0;
    *crew_n           = team_bots;
    if (team_bots < 4) {
        return;
    }
    *wall_n = has_wall ? (int) (team_bots * BABEL_WALL_SHARE) : 0;
    *hunt_n = (int) (team_bots * BABEL_HUNT_SHARE);
    if (has_wall && *wall_n < 1) {
        *wall_n = 1;
    }
    if (*hunt_n < 1) {
        *hunt_n = 1;
    }
    while (*wall_n + *hunt_n > team_bots - 2) {
        if (*hunt_n > *wall_n) {
            (*hunt_n)--;
        } else {
            (*wall_n)--;
        }
    }
    *crew_n = team_bots - *wall_n - *hunt_n;
}

enum
{
    BABEL_ROLE_CREW = 0,
    BABEL_ROLE_WALL,
    BABEL_ROLE_HUNT,
};

/* by rank: the wallers first, the hunters, then the crew (whose last rank is
   the breaker's; the guard is the wallers' last) */
static int _babel_role_of(int rank, int wall_n, int hunt_n)
{
    if (rank < wall_n) {
        return BABEL_ROLE_WALL;
    }
    if (rank < wall_n + hunt_n) {
        return BABEL_ROLE_HUNT;
    }
    return BABEL_ROLE_CREW;
}

/* the way down by a whole staircase (ours first): the top step of a lane
   in *x, *y, *z. A carrier stepping off the slab's edge onto a pillar one
   cell wide ran past it in the air and fell: 18 of 23 pickups in four 600 s
   matches ended so, one capture. The steps are two wide, a level apart */
static int _babel_stair_down(server_t* server, player_t* bot, int* x, int* y, int* z)
{
    for (int side = 0; side < 2; side++) {
        int t = side ? !bot->team : bot->team;
        if (t > 1 || babel_stair[t].len == 0) {
            continue;
        }
        for (int l = 0; l < 2; l++) {
            int nx, ny, nz, k;
            if (_babel_stair_next(server, t, l, &nx, &ny, &nz, &k)) {
                continue;
            }
            *x = babel_stair[t].x[0];
            *y = babel_stair[t].y0 + l;
            *z = map_top(&server->s_map.map, (uint16_t) *x, (uint16_t) *y);
            return 1;
        }
    }
    return 0;
}

/* standing on a built step of a staircase (either team's): the plan walks
   it down. The step's block must be there: a carrier on its pillar's top
   with its centre over the next column read as on step 1 and planned home
   from the air */
static int _babel_on_stair(server_t* server, player_t* bot)
{
    int cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    for (int t = 0; t < 2; t++) {
        if (babel_stair[t].len == 0 || (cy != babel_stair[t].y0 && cy != babel_stair[t].y0 + 1)) {
            continue;
        }
        for (int k = 0; k < babel_stair[t].len; k++) {
            if (babel_stair[t].x[k] == cx && map_solid(&server->s_map.map, cx, cy, 2 + k) &&
                _babel_floor(bot) <= 2 + k) {
                return 1;
            }
        }
    }
    return 0;
}

/* The cells of team t's staircase a builder can put now: missing, and with
   a solid face (the server refuses a floating block), lowest first, the
   support before its step, lane 0 before lane 1. Each lane grows only from
   the ground up, but the lanes touch: once one leads, the other fills in
   beside it, so a crew has several cells to work on at once */
#define BABEL_CREW_MAX 6

typedef struct
{
    int x, y, z, k, l;
} babel_cell_t;

static int _babel_stair_cells(server_t* server, int t, babel_cell_t* cells, int max)
{
    map_store_t* map = &server->s_map.map;
    int       n   = 0;
    for (int k = babel_stair[t].len - 1; k >= 0 && n < max; k--) {
        for (int s = 1; s >= 0 && n < max; s--) {
            for (int l = 0; l < 2 && n < max; l++) {
                int cx = babel_stair[t].x[k], cy = babel_stair[t].y0 + l, cz = 2 + k + s;
                if (cz >= babel_stair[t].ground[k][l] || map_solid(map, cx, cy, cz)) {
                    continue;
                }
                int touch = map_solid(map, cx + 1, cy, cz) || map_solid(map, cx - 1, cy, cz) ||
                            map_solid(map, cx, cy + 1, cz) || map_solid(map, cx, cy - 1, cz) ||
                            map_solid(map, cx, cy, cz + 1) || (cz > 0 && map_solid(map, cx, cy, cz - 1));
                if (!touch) {
                    continue;
                }
                cells[n++] = (babel_cell_t) {cx, cy, cz, k, l};
            }
        }
    }
    return n;
}

/* the wall's missing cells: what a waller can put now (a solid face, since
   the server refuses a floating block), the bottom row first, then the row
   over it; a window column's top cell is never on the list (a dug one is) */
typedef struct
{
    int x, y, z, column;
} babel_wall_cell_t;

static int _babel_wall_cells(server_t* server, int t, babel_wall_cell_t* cells, int max)
{
    map_store_t* map = &server->s_map.map;
    int       n   = 0;
    for (int row = 0; row < BABEL_WALL_HIGH && n < max; row++) {
        for (int i = 0; i < babel_wall[t].n && n < max; i++) {
            if (babel_wall[t].window[i] && row == BABEL_WALL_HIGH - 1) {
                continue;
            }
            int cx = babel_wall[t].x, cy = babel_wall[t].y[i], cz = babel_wall[t].ground[i] - 1 - row;
            if (cz < 1 || map_solid(map, (uint16_t) cx, (uint16_t) cy, (uint16_t) cz) ||
                !gamemode_cell_editable(server, cx, cy, cz))
            {
                continue;
            }
            int touch =
                map_solid(map, (uint16_t) (cx + 1), (uint16_t) cy, (uint16_t) cz) ||
                map_solid(map, (uint16_t) (cx - 1), (uint16_t) cy, (uint16_t) cz) ||
                map_solid(map, (uint16_t) cx, (uint16_t) (cy + 1), (uint16_t) cz) ||
                map_solid(map, (uint16_t) cx, (uint16_t) (cy - 1), (uint16_t) cz) ||
                map_solid(map, (uint16_t) cx, (uint16_t) cy, (uint16_t) (cz + 1)) ||
                (cz > 0 && map_solid(map, (uint16_t) cx, (uint16_t) cy, (uint16_t) (cz - 1)));
            if (!touch) {
                continue;
            }
            cells[n++] = (babel_wall_cell_t) {cx, cy, cz, i};
        }
    }
    return n;
}

/* the wall is whole: nothing left to put (the windows were never on the
   list, a dug out window cell is) */
static int _babel_wall_whole(server_t* server, int t)
{
    babel_wall_cell_t cells[BABEL_WALL_LEN * BABEL_WALL_HIGH];
    return babel_wall[t].n == 0 || _babel_wall_cells(server, t, cells, BABEL_WALL_LEN * BABEL_WALL_HIGH) == 0;
}

/* an enemy on the ground within BABEL_WALL_ALERT of the wall: only then is
   a window worth holding. Holding one all match from a whole wall fired no
   shot on Nine-Eye, whose fight is on the slab and under it, 20+ blocks from
   the foot (the wallers now go with the crew, and come back for this) */
#define BABEL_WALL_ALERT 40.0f

static int _babel_wall_enemy_near(server_t* server, player_t* bot, int t)
{
    float     wx = babel_wall[t].x + 0.5f, wy = babel_stair[t].y0 + 1.0f;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || (p->team != TEAM_A && p->team != TEAM_B) || p->state != STATE_READY ||
            !p->alive || p->movement.position.z < 4.0f)
        {
            continue;
        }
        float dx = p->movement.position.x - wx, dy = p->movement.position.y - wy;
        if (dx * dx + dy * dy <= BABEL_WALL_ALERT * BABEL_WALL_ALERT) {
            return 1;
        }
    }
    return 0;
}

/* where the bot behind the wall stands to put cell y (or hold its window):
   our side of the line, at that y, and ±1 or ±2 when a staircase step hangs
   right over it. A wall cell under a step at the staircase's own lanes left
   the bot standing on the step above, 7 cells from the block: the server
   refused every placement (distance 4) and the wall stopped at 9 cells */
static int _babel_wall_stand(server_t* server, int t, int yy, int* sx, int* sy, int* sg)
{
    map_store_t* map = &server->s_map.map;
    int       x   = babel_wall[t].x - babel_wall[t].dir;
    for (int d = 0; d <= 2; d++) {
        for (int s = 0; s < (d == 0 ? 1 : 2); s++) {
            int y = yy + (d == 0 ? 0 : (s ? -d : d));
            if (y < 0 || y >= map->size_y) {
                continue;
            }
            int g = map_top(map, (uint16_t) x, (uint16_t) y);
            if (g >= map->size_z - 1 || g <= BABEL_WALL_HIGH) {
                continue;
            }
            /* the ground itself: a step above it is no stand (the bot would
               stand on the step, out of the block's reach) */
            if (_babel_wall_on_stair(t, x, y, g)) {
                continue;
            }
            *sx = x;
            *sy = y;
            *sg = g;
            return 1;
        }
    }
    return 0;
}

/* a waller's part of the goal: its share of the missing cells, out of blocks
   the tent, and once the wall is whole a window to hold behind it */
static int _babel_wall_goal(server_t* server, player_t* bot, gm_bot_goal_t* out, int t, int wall_rank)
{
    if (babel_wall[t].n == 0) {
        return 0;
    }
    map_store_t* map = &server->s_map.map;
    int       dir = babel_wall[t].dir, wx = babel_wall[t].x;
    /* the team defends the wall as it does the staircase's foot */
    out->has_guard = 1;
    out->guard_x   = babel_stair[t].x[0] + 0.5f;
    out->guard_y   = babel_stair[t].y0 + 1.0f;
    babel_wall_cell_t cells[BABEL_WALL_LEN * BABEL_WALL_HIGH];
    int               ncells = _babel_wall_cells(server, t, cells, BABEL_WALL_LEN * BABEL_WALL_HIGH);
    if (ncells > 0) {
        const babel_wall_cell_t* c = &cells[wall_rank % ncells];
        if (bot->blocks == 0) {
            /* the tent restocks them, as it does the stair crew: harvesting
               beside the wall would take the wall down again */
            gm_goal_name(out, "restock", GM_INTENT_HOME);
            out->x         = server->protocol.gamemode.base[t].x;
            out->y         = server->protocol.gamemode.base[t].y;
            out->z         = server->protocol.gamemode.base[t].z;
            out->exact     = 1;
            out->walk_only = out->keep_terrain = 1;
            return 1;
        }
        /* behind the wall, in the cell's own column: the block in reach */
        int sx, sy, sg;
        if (!_babel_wall_stand(server, t, c->y, &sx, &sy, &sg)) {
            return 0;
        }
        if (bot->alive && get_nanos() - babel_wall_log_at[bot->id] > 5ULL * NANO_IN_SECOND) {
            babel_wall_log_at[bot->id] = get_nanos();
            evt_log(bot, "bot_babel", "action=assign role=wall cell=%d,%d,%d stand=%d,%d wall=%d of %d cells",
                    c->x, c->y, c->z, sx, sy, babel_wall[t].n, ncells);
        }
        gm_goal_name(out, "babel-wall", 0);
        out->x         = sx + 0.5f;
        out->y         = sy + 0.5f;
        out->z         = (float) sg;
        out->exact     = 1;
        out->walk_only = out->keep_terrain = 1;
        out->work      = 1;
        out->work_x    = c->x;
        out->work_y    = c->y;
        out->work_z    = c->z;
        /* the colour of the ground it stands on: a dug one is the current top */
        int gc = babel_wall[t].ground[c->column];
        if (!map_solid(map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) gc)) {
            gc = map_top(map, (uint16_t) c->x, (uint16_t) c->y);
        }
        out->work_color = map_color(map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) gc);
        return 1;
    }
    /* whole: a window of its own, looking out of it, but only while an enemy
       is coming over the ground at the wall; with none there the waller
       joins the crew, or hunts (_babel_role_goal) */
    if (!_babel_wall_enemy_near(server, bot, t)) {
        return 0;
    }
    int windows[BABEL_WALL_LEN], nw = 0;
    for (int i = 0; i < babel_wall[t].n; i++) {
        if (babel_wall[t].window[i]) {
            windows[nw++] = i;
        }
    }
    if (nw == 0) {
        return 0;
    }
    int i  = windows[wall_rank % nw];
    int sx = wx - dir, sy, sg;
    int wy = babel_wall[t].y[i];
    if (!_babel_wall_stand(server, t, wy, &sx, &sy, &sg)) {
        return 0;
    }
    if (bot->alive && babel_window_held[bot->id] != wy) {
        babel_window_held[bot->id] = wy;
        evt_log(bot, "bot_babel", "action=window y=%d stand=%d,%d wall=%d", wy, sx, sy, babel_wall[t].n);
    }
    gm_goal_name(out, "babel-window", GM_INTENT_POST);
    out->x         = sx + 0.5f;
    out->y         = sy + 0.5f;
    out->z         = (float) sg;
    out->hold      = 1.5f;
    out->watch_x   = wx + dir * 20.0f + 0.5f;
    out->watch_y   = wy + 0.5f;
    out->walk_only = out->keep_terrain = 1;
    return 1;
}

/* the guard at the staircase's foot, behind the wall the others raise 5
   columns toward the enemy: one of the wall builders holds it
   (_babel_role_goal), so the crew keeps every builder it had. At 8 bots a
   team the crew's own guard had gone (it needed 6 of a crew of 4) and the
   foot had nobody. It stands on the enemy's side of the foot but behind the
   wall, looking the way the enemy comes. Beside the staircase's lanes, not
   on them: on the lane it stood in the column of the steps the crew was
   raising (its body refused the block), and babelstairs' crew stalled at
   27 of 210 cells for 80 s */
static void _babel_guard_goal(gm_bot_goal_t* out, server_t* server, int t)
{
    int       len = babel_stair[t].len, dir = babel_wall[t].dir;
    map_store_t* map = &server->s_map.map;
    int       x   = babel_stair[t].x[len - 1] + dir * 3;
    int       y   = babel_stair[t].y0 + 2;
    if (map_top(map, (uint16_t) x, (uint16_t) y) >= map->size_z - 1) {
        y = babel_stair[t].y0 - 1; /* wet there: the other side of the lanes */
    }
    out->has_guard = 1;
    out->guard_x   = babel_stair[t].x[0] + 0.5f;
    out->guard_y   = babel_stair[t].y0 + 1.0f;
    gm_goal_name(out, "stair-guard", 0);
    out->x       = x + 0.5f;
    out->y       = y + 0.5f;
    out->z       = 0.0f;
    out->hold    = 3.0f;
    out->watch_x = out->x + dir * 20.0f;
    out->watch_y = out->y;
    out->climb   = 0;
    /* on foot and with no spade: its path to the post dug through the
       staircase beside it and a support gone took forty cells of it down
       (team 1 stalled at 128 and never got up again) */
    out->walk_only = out->keep_terrain = 1;
}

static int _babel_stair_goal(server_t* server, player_t* bot, gm_bot_goal_t* out, int up)
{
    int t = bot->team == TEAM_A ? 0 : 1, e = 1 - t;
    if (babel_stair[t].len == 0 || bot->has_intel) {
        return 0;
    }
    int len = babel_stair[t].len, dir = babel_stair[t].dir;
    /* what the whole team defends: our staircase's top, where the enemy
       spades it from the slab */
    out->has_guard = 1;
    out->guard_x   = babel_stair[t].x[0] + 0.5f;
    out->guard_y   = babel_stair[t].y0 + 1.0f;
    int x, y, z, k;
    int whole = _babel_stair_whole(server, t);
    /* Until our staircase is whole the crew builds it, and once the wall is
       whole its builders join the crew (they held a window all match and
       fired no shot); the guard stays at the foot until the staircase is
       done. With the others off to their pillars, players saw bots raising
       towers of blocks beside a staircase they did not help with, and a crew
       of all of them puts it up in 30 to 150 s. Then the breaker (from 4
       crew, the crew's last rank) goes up it to cut the enemy's, and the
       rest climbs it to the intel. Each builder takes another of the cells
       that can be put now; more builders than such cells share them */
    int team_bots;
    int rank = _babel_rank(server, bot, &team_bots);
    int wall_n, hunt_n, crew_n;
    _babel_roles(team_bots, babel_wall[t].n > 0, &wall_n, &hunt_n, &crew_n);
    int helpers = 0;
    if (!whole && babel_wall[t].n > 0 && _babel_wall_whole(server, t)) {
        helpers = wall_n - (wall_n >= 2 ? 1 : 0); /* the guard's rank stays at the foot */
    }
    int crew_eff = crew_n + helpers;
    int crew_rank;
    if (rank < wall_n) {
        crew_rank = crew_n + rank; /* a waller, after the crew's own ranks */
    } else {
        crew_rank = rank - wall_n - hunt_n;
    }
    /* the breaker is the crew's last rank: the wallers are not in it (they
       are off hunting by the time it is whole) */
    int breaker = whole && crew_n >= 4 ? crew_n - 1 : -1;
    int crew    = whole ? 0 : crew_eff;
    babel_cell_t cells[BABEL_CREW_MAX];
    int          ncells = crew_rank >= 0 && crew_rank < crew ? _babel_stair_cells(server, t, cells, BABEL_CREW_MAX) : 0;
    if (ncells > 0) {
        const babel_cell_t* c = &cells[crew_rank % ncells];
        x = c->x, y = c->y, z = c->z, k = c->k;
        if (bot->blocks == 0) {
            /* out of blocks: the tent restocks them. Harvesting with the
               spade on the staircase took the next lane's steps */
            gm_goal_name(out, "restock", GM_INTENT_HOME);
            out->x     = server->protocol.gamemode.base[t].x;
            out->y     = server->protocol.gamemode.base[t].y;
            out->z     = server->protocol.gamemode.base[t].z;
            out->exact = 1;
            out->climb = 0;
            out->walk_only = out->keep_terrain = 1; /* down off the staircase without spading it */
            return 1;
        }
        /* a builder: on the step below the missing cell (on the ground
           beyond the foot for the lowest), the block in reach. A cell
           filled in beside the other lane stands on that lane's step when
           its own is not there yet (reach is 4) */
        int sx    = babel_stair[t].x[0] + dir * (k + 1); /* the column outward of the cell */
        int sy    = y;
        if (k + 1 < len && !map_solid(&server->s_map.map, sx, sy, 3 + k)) {
            int other = babel_stair[t].y0 + (1 - c->l);
            if (map_solid(&server->s_map.map, sx, other, 3 + k)) {
                sy = other;
            }
        }
        static uint64_t assigned_at[256];
        if (get_nanos() - assigned_at[bot->id] > 5ULL * NANO_IN_SECOND) {
            assigned_at[bot->id] = get_nanos();
            evt_log(bot, "bot_stair", "action=assign cell=%d,%d,%d k=%d stand=%d,%d crew=%d of %d cells pos=%.1f,%.1f,%.1f",
                    x, y, z, k, sx, sy, crew, ncells, bot->movement.position.x, bot->movement.position.y,
                    bot->movement.position.z);
        }
        gm_goal_name(out, "stair-build", 0);
        out->x    = sx + 0.5f;
        out->y    = sy + 0.5f;
        out->z    = k + 1 < len ? (float) (3 + k) /* on step k + 1 */
                                : (float) map_top(&server->s_map.map, (uint16_t) sx, (uint16_t) sy);
        out->exact  = 1;
        out->climb  = 0;
        /* on foot: a plan or a jam that digs went through the staircase */
        out->walk_only = out->keep_terrain = 1;
        out->work      = 1;
        out->work_x    = x;
        out->work_y    = y;
        out->work_z    = z;
        return 1;
    }
    if (crew_rank == breaker && babel_stair[e].len > 0 && _babel_stair_top(server, e, &x, &y, &z) &&
        _babel_floor(bot) > 2) {
        /* up our staircase first, like everyone (a pillar of its own was a
           tower of blocks beside it) */
        up = 1;
    } else if (crew_rank == breaker && babel_stair[e].len > 0 && _babel_stair_top(server, e, &x, &y, &z)) {
        /* a breaker on the slab: at its edge over the enemy's staircase,
           the block below and beside it in the spade's reach */
        int ex      = babel_stair[e].x[0] - babel_stair[e].dir; /* the slab's edge column */
        gm_goal_name(out, "stair-break", 0);
        out->x      = ex + 0.5f;
        out->y      = y + 0.5f;
        out->z      = 1.0f;
        out->exact  = 1;
        out->climb  = 0;
        out->rush   = 1; /* across the slab without stopping: it fought every
                            enemy in sight on the way and never broke a block */
        out->work   = 2;
        out->work_x = x;
        out->work_y = y;
        out->work_z = z;
        return 1;
    }
    if (whole && up) {
        /* up the staircase to its top step; the step onto the slab follows */
        int top    = map_top(&server->s_map.map, (uint16_t) babel_stair[t].x[0],
                                           (uint16_t) (babel_stair[t].y0 + (bot->id & 1)));
        /* only from below the top step (floor 2): on it, half way through
           the hop onto the slab and on the slab itself the bot is there.
           Asked by column, it sent bots back down from the slab's edge
           (9000 flips with onto-slab) and from the slab (13000 with
           take-intel), and nobody took the intel */
        if (top == 2 && _babel_floor(bot) > 2) {
            gm_goal_name(out, "stair-up", 0);
            out->x     = babel_stair[t].x[0] + 0.5f;
            out->y     = babel_stair[t].y0 + (bot->id & 1) + 0.5f;
            out->z     = 2.0f;
            out->exact = 1;
            out->climb = 0;
            out->walk_only = out->keep_terrain = 1; /* up its steps, never through them */
            return 1;
        }
    }
    if (!whole && up && _babel_floor(bot) > 2 && _babel_on_stair(server, bot)) {
        /* on the staircase when the enemy's breaker cut its top: wait on
           this step while the crew puts it back. Going on, or off to the
           pillars, walked into the cut or off the side: 130-180 falls a
           match once the staircases were whole */
        gm_goal_name(out, "stair-wait", 0);
        out->x         = (int) bot->movement.position.x + 0.5f;
        out->y         = (int) bot->movement.position.y + 0.5f;
        out->z         = (float) _babel_floor(bot);
        out->exact     = 1;
        out->climb     = 0;
        out->walk_only = out->keep_terrain = 1;
        return 1;
    }
    return 0;
}

/* a target a hunter cannot reach on foot: a column under (or on) the slab,
   where the way across is Nine-Eye's lake and the planner's 40000 node cap
   hands back partial plans, or a place up a staircase past the sixth step
   (the climb is the crew's work, and the hunter has no spade to follow).
   Chasing those kept the hunters out of the fight entirely (Ctf.c's Babel
   notes: the slab is x 206..306, y 240..272) */
static int _babel_hunt_unreachable(player_t* p)
{
    if (p->movement.position.x >= 206.0f && p->movement.position.x <= 306.0f && p->movement.position.y >= 240.0f &&
        p->movement.position.y <= 272.0f)
    {
        return 1;
    }
    int      floor = _babel_floor(p);
    int      cx = (int) p->movement.position.x, cy = (int) p->movement.position.y;
    for (int t = 0; t < 2; t++) {
        if (babel_stair[t].len == 0 || (cy != babel_stair[t].y0 && cy != babel_stair[t].y0 + 1)) {
            continue;
        }
        for (int k = 7; k < babel_stair[t].len; k++) {
            if (babel_stair[t].x[k] == cx && floor >= 2 + k) {
                return 1;
            }
        }
    }
    return 0;
}

/* a hunter's goal: the nearest enemy on the ground it can reach on foot.
   The slab is no place for a hunter: under it is Nine-Eye's lake (the plans
   know it is deadly water), and one up there, a carrier or a camper, is not
   chased. Nobody on the ground: the ground in front of the enemy's
   staircase's foot, where its wall stands and where they come over on
   foot. Once the staircase is whole they go on hunting: only a carrier
   goes up */
static int _babel_hunt_goal(server_t* server, player_t* bot, gm_bot_goal_t* out, int t)
{
    player_t *p, *tmp, *best = NULL;
    float     best_d = 1e18f;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || (p->team != TEAM_A && p->team != TEAM_B) || p->state != STATE_READY ||
            !p->alive || p->movement.position.z < 4.0f || _babel_hunt_unreachable(p))
        {
            continue;
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        if (dx * dx + dy * dy < best_d) {
            best_d = dx * dx + dy * dy;
            best   = p;
        }
    }
    float gx, gy, gz;
    if (best != NULL) {
        gx = best->movement.position.x;
        gy = best->movement.position.y;
        gz = best->movement.position.z + 2.4f;
    } else {
        int e = 1 - t;
        if (babel_stair[e].len > 0) {
            /* out past the enemy's wall, on their side of the foot */
            int foot = babel_stair[e].x[babel_stair[e].len - 1] + babel_stair[e].dir * (BABEL_WALL_OUT + 2);
            vector3f_t at = _ctf_land_near(server, (float) foot + 0.5f, (float) babel_stair[e].y0 + 1.5f);
            gx = at.x;
            gy = at.y;
            gz = at.z;
        } else {
            gx = server->protocol.gamemode.base[e].x;
            gy = server->protocol.gamemode.base[e].y;
            gz = (float) map_top(&server->s_map.map, (uint16_t) gx, (uint16_t) gy);
        }
    }
    gm_goal_name(out, "babel-hunt", GM_INTENT_CHASE);
    /* a hunter chases its own target: not the guard ranking of the goal it
       may have come from (a waller's stair goal sets it) */
    out->has_guard = 0;
    out->x    = gx;
    out->y    = gy;
    out->z    = gz;
    return 1;
}

/* the wall's and the hunter's goals, before the CTF's (1 when it set one).
   A team's bots used to all raise the staircase; a team should look like a
   team: some build the wall, some go out to kill, the rest raise the steps.
   A waller is not idle once its wall is whole: it goes with the crew, hunts
   when the staircase is too, and comes back to mend a cell or to a window
   when the enemy comes over the ground at the wall */
static int _babel_role_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    int t = bot->team == TEAM_A ? 0 : 1;
    int team_bots;
    int rank = _babel_rank(server, bot, &team_bots);
    int wall_n, hunt_n, crew_n;
    _babel_roles(team_bots, babel_wall[t].n > 0, &wall_n, &hunt_n, &crew_n);
    int role = _babel_role_of(rank, wall_n, hunt_n);
    /* once a life (the role is by rank, so a bot keeps it; a joining or
       leaving bot renumbers the ranks). A dead bot's goal is the respawn's
       and logs nothing */
    if (bot->alive && (!babel_role_life[bot->id] || babel_role_seen[bot->id] != role)) {
        babel_role_life[bot->id] = 1;
        babel_role_seen[bot->id] = (uint8_t) role;
        evt_log(bot, "bot_babel", "action=role role=%s crew=%d wall=%d hunt=%d of %d",
                role == BABEL_ROLE_WALL ? "wall" : role == BABEL_ROLE_HUNT ? "hunt" : "crew", crew_n, wall_n, hunt_n,
                team_bots);
    }
    if (bot->has_intel) {
        return 0; /* a carrier runs home like everyone */
    }
    if (role == BABEL_ROLE_WALL) {
        /* the guard is the wallers' last rank, and needs two wallers so one
           can build (8 bots a team): the crew has no guard since the roles,
           and a team of 8, the case the roles were measured on, had none.
           At 4 bots a team the single waller builds */
        if (wall_n >= 2 && rank == wall_n - 1 && babel_stair[t].len > 0 && !_babel_stair_whole(server, t)) {
            _babel_guard_goal(out, server, t);
            return 1;
        }
        if (_babel_wall_goal(server, bot, out, t, rank)) {
            return 1; /* a missing cell, or a window with the enemy coming */
        }
        /* whole and nothing coming: with the crew until the staircase is
           whole, then out hunting */
        if (!_babel_stair_whole(server, t) && _babel_stair_goal(server, bot, out, 0) == 1) {
            return 1;
        }
    } else if (role != BABEL_ROLE_HUNT) {
        return 0;
    }
    /* GM_BOT_RUSH is the core's run and gun (the field `rush` is the
       carrier's: it walks past fights). They shoot on the move and never
       stop for a wall. On foot and with no spade: a hunter chasing a builder
       up the steps dug through its own team's staircase (the hunters' digs
       at the foot took team 0's from 72 cells to 8 in one match), and the
       staircase is the crew's work */
    _babel_hunt_goal(server, bot, out, t);
    out->role         = GM_BOT_RUSH;
    out->walk_only    = 1;
    out->keep_terrain = 1;
    return 1;
}

/* up on the slab, a pillar or the staircase: a bot there finishes the way
   down before any role sends it off (_ctf_bot_goal's pillar and stair
   checks, below). A role change (a teammate leaving renumbers the ranks)
   turned a climber stepping off its slab's edge into a waller ordered to walk
   back across the map, and it walked off the pillar */
static int _babel_high(server_t* server, player_t* bot)
{
    int fz = _babel_floor(bot);
    if (fz <= 3) {
        return 1; /* the slab, or the top steps just under it */
    }
    int cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    if ((cx == 205 || cx == 307) && cy >= BABEL_Y0 && cy <= BABEL_Y1 && fz < _babel_ground(server, cx, cy) - 1) {
        return 1; /* partway up a pillar of the strip */
    }
    return _babel_on_stair(server, bot);
}

static int _babel_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    int high = _babel_high(server, bot);
    if (high && bot->alive) {
        /* a waller whose stand cell is under the staircase's lowest steps
           steps onto them on its way and was sent to stair-wait, its wall
           never finished: on our foot's own steps it is no climber to bring
           down */
        int t = bot->team == TEAM_A ? 0 : 1, team_bots;
        int rank = _babel_rank(server, bot, &team_bots);
        int wall_n, hunt_n, crew_n;
        _babel_roles(team_bots, babel_wall[t].n > 0, &wall_n, &hunt_n, &crew_n);
        if (rank < wall_n && babel_wall[t].dir != 0 && babel_stair[t].len > 0 && _babel_floor(bot) > 3) {
            int fx = babel_stair[t].x[babel_stair[t].len - 1];
            int d  = ((int) bot->movement.position.x - fx) * babel_wall[t].dir;
            int cy = (int) bot->movement.position.y;
            if (d >= 0 && d <= BABEL_WALL_OUT && (cy == babel_stair[t].y0 || cy == babel_stair[t].y0 + 1)) {
                high = 0;
            }
        }
    }
    if (!high && _babel_role_goal(server, bot, out)) {
        return 1;
    }
    if (!_ctf_bot_goal(server, bot, out)) {
        return 0;
    }
    /* a carrier on the far side of the tower, hunted from the ground: the
       way across is under the slab, which on Nine-Eye is a lake (16 hunters
       drowned in eight 600 s matches). Up our pillar to the slab instead */
    if (out->name != NULL && !strcmp(out->name, "hunt-carrier") && _babel_floor(bot) > 3 &&
        (bot->movement.position.x < 256.0f) != (out->x < 256.0f)) {
        gm_goal_name(out, "hunt-up", 0);
        out->x     = 256.5f;
        out->y     = 256.5f;
        out->z     = 1.0f;
        out->exact = 0;
    }
    /* any goal up on the slab goes by our pillar, not only the intel: a
       carrier to hunt or to escort up there sent bots straight at it under
       the slab, into Nine-Eye's moat, and one climbing down its pillar
       stepped off it halfway when its goal turned to a carrier above */
    int up    = (out->name != NULL && !strcmp(out->name, "take-intel")) ||
             (out->x >= 206.0f && out->x <= 307.0f && out->y >= 240.0f && out->y <= 273.0f && out->z > 0.0f &&
              out->z <= 3.0f);
    /* once on its way up its pillar it keeps climbing a while: a goal on
       the slab that moves (a carrier at the slab's edge) flipped it between
       up and down halfway, 250 goal changes a bot in a match */
    static uint64_t up_at[256];
    {
        uint64_t now = get_nanos();
        int      px = (int) bot->movement.position.x, py = (int) bot->movement.position.y;
        int      pillar = (px == 205 || px == 307) && py >= BABEL_Y0 && py <= BABEL_Y1 && _babel_floor(bot) >= 2 &&
                     _babel_floor(bot) < _babel_ground(server, px, py) - 1;
        int      mid = pillar && px == _babel_strip_x(server, bot->team) && _babel_floor(bot) > 2;
        if (up) {
            up_at[bot->id] = now;
        } else if (mid && !bot->has_intel && up_at[bot->id] != 0 && now - up_at[bot->id] < 10ULL * NANO_IN_SECOND) {
            up = 1;
        } else if (pillar) {
            /* either team's pillar: a carrier goes down the enemy's when ours
               is gone. Read as off it between two digs, it planned back up to
               the slab, jumped and landed beside the pillar: most carriers
               died so (18 of 23 pickups in four 600 s matches) */
            /* on its way down, down to the ground: a carrier halfway down
               put bridge blocks out of its pillar at 19 and fell off them */
            gm_goal_name(out, "climb-down", 0);
            out->x     = px + 0.5f;
            out->y     = py + 0.5f;
            out->z     = (float) _babel_ground(server, px, py);
            out->exact = 1;
            out->climb = 2;
            out->rush  = 0;
            return 1;
        }
    }
    int stair = _babel_stair_goal(server, bot, out, up);
    if (stair == 1) {
        return 1;
    }
    if (stair == 2) {
        up = 1; /* a breaker on its way up to the slab */
    }
    int cx    = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    int fz    = _babel_floor(bot);
    int strip = (cx == 205 || cx == 307) && cy >= BABEL_Y0 && cy <= BABEL_Y1;
    int lx, ly;
    /* in a column of our strip, the pillar's top decides, not the bot's
       height: in the air at the top of a jump it read as floor 2, the goal
       went to the intel, the block was never put and it fell back to 3 */
    int own = strip && cx == _babel_strip_x(server, bot->team);
    int top = own ? map_top(&server->s_map.map, (uint16_t) cx, (uint16_t) cy) : 0;
    if (up && (own ? top > 2 : fz > 2)) { /* up to floor 2, the step onto the slab */
        if (own) {
            lx = cx;
            ly = cy; /* on its way up this column */
        } else if (!_babel_ladder(server, bot, &lx, &ly)) {
            return 1;
        }
        gm_goal_name(out, cx == lx && cy == ly ? "climb" : "to-ladder", 0);
        out->x     = lx + 0.5f;
        out->y     = ly + 0.5f;
        out->z     = cx == lx && cy == ly ? 2.0f : (float) map_top(&server->s_map.map, lx, ly);
        out->exact = 1;
        out->climb = cx == lx && cy == ly ? 1 : 0;
        return 1;
    }
    if (up && own && top == 2 && fz <= 3) {
        /* on its pillar's top: the one step onto the slab, straight in. While
           it waited for a plan to the intel it drifted into the next column
           of the strip, read it as a ladder to climb and walked off */
        gm_goal_name(out, "onto-slab", 0);
        out->x     = (cx == 205 ? 206 : 306) + 0.5f;
        out->y     = cy + 0.5f;
        out->z     = 1.0f;
        out->exact = 1;
        out->climb = 0;
        return 1;
    }
    if (!up) {
        /* standing on that column's own top (on the slab's edge, x read as
           the strip's and it dug at the protected cell, then walked off) */
        int   ctop   = strip ? map_top(&server->s_map.map, (uint16_t) cx, (uint16_t) cy) : 0;
        float ox     = bot->movement.position.x - (cx + 0.5f), oy = bot->movement.position.y - (cy + 0.5f);
        /* falling the one cell each dig takes, it is still on its way down
           (read as off the pillar, it walked off between two digs) */
        /* on the slab's edge with its centre over the pillar it read as on
           top (floor 1, the pillar's top 2) and dug at the protected slab */
        int on_top = strip && fabsf(ox) < 0.5f && fabsf(oy) < 0.5f &&
                     (ctop == fz || (bot->airborne && ctop - fz == 1));
        if (on_top && fz < _babel_ground(server, cx, cy)) {
            gm_goal_name(out, "climb-down", 0);
            out->x     = cx + 0.5f;
            out->y     = cy + 0.5f;
            out->z     = (float) _babel_ground(server, cx, cy);
            out->exact = 1;
            out->climb = 2;
            out->rush  = 0;
            return 1;
        }
        int lz;
        if (fz > 1 && _babel_on_stair(server, bot)) {
            return 1; /* on the steps: the mode's goal, planned down them */
        }
        if (fz <= 4 && _babel_stair_down(server, bot, &lx, &ly, &lz)) {
            gm_goal_name(out, "to-stair-top", 0);
            out->x     = lx + 0.5f;
            out->y     = ly + 0.5f;
            out->z     = (float) lz;
            out->exact = 1;
            out->climb = 0;
            out->rush  = 0;
        } else if (fz <= 4 && _babel_way_down(server, bot, &lx, &ly, &lz)) {
            gm_goal_name(out, "to-ladder-top", 0);
            out->x     = lx + 0.5f;
            out->y     = ly + 0.5f;
            out->z     = (float) lz;
            out->exact = 1;
            out->climb = 0;
            out->rush  = 0; /* a carrier running onto a one cell pillar ran past it */
        }
    }
    return 1;
}

/* the intel's carrier (for the bots: gamemode_player_carries) */
int _ctf_player_carries(server_t* server, player_t* player)
{
    (void) server;
    return player->has_intel;
}

void _ctf_patrol_anchor(server_t* server, player_t* bot, float* x, float* y)
{
    vector3f_t base = server->protocol.gamemode.base[_enemy(bot)];
    *x              = base.x;
    *y              = base.y;
}

/* a spawn faces the enemy intel: where it lies, its base while carried */
int _ctf_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    vector3f_t intel = server->protocol.gamemode.intel[_enemy(player)];
    *x               = intel.x;
    *y               = intel.y;
    return 1;
}

/* classic CTF: the 0.75 rules as they were. Squads stay (the bots' lanes
   and the yellow names), but a respawn is at the team's spawn */
/* the escape watch may mark an intel carrier who seals itself away */
int _ctf_escape_watch_player(server_t* server, player_t* player)
{
    (void) server;
    return player->has_intel;
}

const gamemode_ops_t ctf_ops = {
.id              = GAME_MODE_CTF,
.name            = "ctf",
.spawn_facing    = _ctf_spawn_facing,
.wire_name       = "ctf",
.init            = _ctf_init,
.tick            = _ctf_tick,
.player_step     = handleTentAndIntel,
.player_out      = _ctf_player_out,
.terrain_changed = _ctf_terrain_changed,
.state_keys      = _ctf_state_keys,
.summary_stat    = "captures",
.player_stat     = _ctf_player_captures,
.bot_goal        = _ctf_bot_goal,
.patrol_anchor   = _ctf_patrol_anchor,
.player_carries  = _ctf_player_carries,
/* squads ([squads] in config.toml): no mate spawn without GM_FEAT_SQUAD_SPAWN */
.spawn_point     = squads_spawn_point,
.client_in_game  = squads_client_in_game,
.player_command  = squads_command,
.escape_watch_player = _ctf_escape_watch_player,
};

/* no perks: a jetpack would fly straight up the tower */
/* the staircases' and the walls' progress, every 10 s, and the moment each is
   first whole: cells built of the cells planned (a step and its support per
   lane; the wall's 3-high columns less their windows) */
static void _babel_tick(server_t* server)
{
    static uint64_t logged_at;
    uint64_t        now = get_nanos();
    int             built[2] = {0, 0}, total[2] = {0, 0};
    int             wbuilt[2] = {0, 0}, wtotal[2] = {0, 0};
    for (int t = 0; t < 2; t++) {
        for (int k = 0; k < babel_stair[t].len; k++) {
            for (int l = 0; l < 2; l++) {
                for (int s = 0; s <= 1; s++) {
                    int cz = 2 + k + s;
                    if (cz >= babel_stair[t].ground[k][l]) {
                        continue;
                    }
                    total[t]++;
                    built[t] += map_solid(&server->s_map.map, babel_stair[t].x[k], babel_stair[t].y0 + l, cz);
                }
            }
        }
        if (total[t] > 0 && built[t] == total[t] && babel_whole_at[t] == 0) {
            babel_whole_at[t] = now;
            LOG_INFO("Babel staircase of team %d whole after %.0f s (%d cells)", t,
                     (double) (now - babel_started) / NANO_IN_SECOND, total[t]);
        }
        for (int i = 0; i < babel_wall[t].n; i++) {
            for (int row = 0; row < BABEL_WALL_HIGH; row++) {
                if (babel_wall[t].window[i] && row == BABEL_WALL_HIGH - 1) {
                    continue; /* the loophole is left open */
                }
                int cz = babel_wall[t].ground[i] - 1 - row;
                wtotal[t]++;
                wbuilt[t] += map_solid(&server->s_map.map, (uint16_t) babel_wall[t].x,
                                             (uint16_t) babel_wall[t].y[i], (uint16_t) cz);
            }
        }
        if (wtotal[t] > 0 && wbuilt[t] == wtotal[t] && babel_wall_whole_at[t] == 0) {
            babel_wall_whole_at[t] = now;
            LOG_INFO("Babel wall of team %d whole after %.0f s (%d cells)", t,
                     (double) (now - babel_started) / NANO_IN_SECOND, wtotal[t]);
        }
    }
    /* a bot's role is logged once a life: its next life logs it again */
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot && !p->alive) {
            babel_role_life[p->id] = 0;
        }
    }
    if (now - logged_at >= 10ULL * NANO_IN_SECOND) {
        logged_at = now;
        LOG_INFO("Babel staircases at %.0f s: team 0 %d/%d, team 1 %d/%d", (double) (now - babel_started) / NANO_IN_SECOND,
                 built[0], total[0], built[1], total[1]);
        LOG_INFO("Babel walls at %.0f s: team 0 %d/%d, team 1 %d/%d", (double) (now - babel_started) / NANO_IN_SECOND,
                 wbuilt[0], wtotal[0], wbuilt[1], wtotal[1]);
    }
}

/* Babel's maps (Main.c drops the others from the rotation): one named so,
   one of its catalog (common/modes/modes.h) or one tagged babel ([map]
   modes). Without it --gamemode babel played the tower's rules on the
   rotation's CTF map, where there is no slab to climb */
static int _babel_map_fits(const char* map_name)
{
    for (const char* c = map_name; *c; c++) {
        if (strncasecmp(c, "babel", 5) == 0) {
            return 1;
        }
    }
    const struct game_mode_info* mode = game_mode_find("babel");
    for (int k = 0; k < game_mode_map_count(mode); k++) {
        if (strcmp(mode->maps[k], map_name) == 0) {
            return 1;
        }
    }
    return gamemode_map_tagged(map_name, "babel");
}

const gamemode_ops_t babel_ops = {
.id              = GAME_MODE_BABEL,
.name            = "babel",
.wire_name       = "ctf",
.init            = _babel_init,
.tick            = _babel_tick,
.player_step     = handleTentAndIntel,
.player_out      = _ctf_player_out,
.terrain_changed = _ctf_terrain_changed,
.cell_protected  = _babel_cell_protected,
.state_keys      = _ctf_state_keys,
.summary_stat    = "captures",
.player_stat     = _ctf_player_captures,
.bot_goal        = _babel_bot_goal,
.patrol_anchor   = _ctf_patrol_anchor,
.player_carries  = _ctf_player_carries,
.escape_watch_player = _ctf_escape_watch_player,
.map_fits        = _babel_map_fits,
};
