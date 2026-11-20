#include <Server/Plugins.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Nodes.h>
#include <Server/Proto10/World.h>
#include <Server/Packets/Packets.h>
#include <Server/Proto10/Entities.h>
#include <Server/Proto10/Objects.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <Util/Types.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Server/EventLog.h>
#include <Server/Grenade.h>
#include <stdlib.h>
#include <math.h>

vector3i_t* getGrenadeNeighbors(vector3i_t pos)
{
    static vector3i_t neighArray[54];

    // Lookup table for neighbor offsets
    static const int offsets[54][3] = {
    {0, 0, -2}, {1, 0, -2}, {-1, 0, -2}, {0, 1, -2}, {0, -1, -2}, {1, 1, -2}, {1, -1, -2}, {-1, 1, -2}, {-1, -1, -2},
    {0, -2, 0}, {1, -2, 0}, {-1, -2, 0}, {0, -2, 1}, {0, -2, -1}, {1, -2, 1}, {1, -2, -1}, {-1, -2, 1}, {-1, -2, -1},
    {0, 2, 0},  {1, 2, 0},  {-1, 2, 0},  {0, 2, 1},  {0, 2, -1},  {1, 2, 1},  {1, 2, -1},  {-1, 2, 1},  {-1, 2, -1},
    {-2, 0, 0}, {-2, 1, 0}, {-2, -1, 0}, {-2, 0, 1}, {-2, 0, -1}, {-2, 1, 1}, {-2, 1, -1}, {-2, -1, 1}, {-2, -1, -1},
    {0, 0, 2},  {1, 0, 2},  {-1, 0, 2},  {0, 1, 2},  {0, -1, 2},  {1, 1, 2},  {1, -1, 2},  {-1, 1, 2},  {-1, -1, 2}};

    for (int i = 0; i < 54; i++) {
        neighArray[i].x = pos.x + offsets[i][0];
        neighArray[i].y = pos.y + offsets[i][1];
        neighArray[i].z = pos.z + offsets[i][2];
    }

    return neighArray;
}

uint8_t get_grenade_damage(server_t* server, player_t* damaged_player, grenade_t* grenade)
{
    return explosion_damage_at(server, damaged_player, grenade->position);
}

uint8_t explosion_damage_at(server_t* server, player_t* damaged_player, vector3f_t at)
{
    double     diffX      = fabs(damaged_player->movement.position.x - at.x);
    double     diffY      = fabs(damaged_player->movement.position.y - at.y);
    double     diffZ      = fabs(damaged_player->movement.position.z - at.z);
    vector3f_t playerPos  = damaged_player->movement.position;
    vector3f_t grenadePos = at;
    if (diffX < 16 && diffY < 16 && diffZ < 16 &&
        physics_can_see(server, playerPos.x, playerPos.y, playerPos.z, grenadePos.x, grenadePos.y, grenadePos.z) &&
        at.z < 62)
    {
        double diff = ((diffX * diffX) + (diffY * diffY) + (diffZ * diffZ));
        if (diff == 0) {
            return 100;
        }
        return (int) fmin((4096.f / diff), 100);
    }
    return 0;
}

/* BattleSpades' (retail) blast geometry, its explosionDamageManager: the
   falloff is (R^2 - d^2) / R^2 with d measured from the explosion to the
   body centre (the eye + 0.75 standing, +1.25 crouched), and three sight
   rays to the eye and +0.9 / +1.8 down the body standing (+0.45 / +0.9
   crouched) are weighted 0.5 / 0.3 / 0.2; the sum of the clear rays'
   weights multiplies the damage, each ray spanning 10%..110% of the way
   from the explosion to the sight point. A player crouched behind a low
   wall keeps the head ray's 0.2 where a single eye ray saw nothing. The
   grenade's 4096 / d^2 curve capped at 100, halved for a rocket, never
   killed in one hit; the retail rocket (140, R 6) kills within ~3.2
   blocks */
static float _explosion_retail_los(server_t* server, player_t* p, vector3f_t at)
{
    static const float offsets[2][3] = {{0.0f, 0.9f, 1.8f}, {0.0f, 0.45f, 0.9f}};
    static const float weights[3]   = {0.5f, 0.3f, 0.2f};
    const float*      off          = offsets[p->crouching != 0];
    float             fraction     = 0.0f;
    for (int k = 0; k < 3; k++) {
        float vx  = p->movement.position.x - at.x;
        float vy  = p->movement.position.y - at.y;
        float vz  = p->movement.position.z + off[k] - at.z;
        float len = sqrtf(vx * vx + vy * vy + vz * vz);
        if (len < 1e-6f) {
            fraction += weights[k]; /* the blast sits on the sight point */
            continue;
        }
        /* the stock ray starts 10% of the way along and runs the segment's
           own length more: 10%..110% of explosion to sight point */
        long hx, hy, hz;
        if (!physics_cast_ray(server, at.x + vx * 0.1f, at.y + vy * 0.1f, at.z + vz * 0.1f, vx / len, vy / len,
                              vz / len, len, &hx, &hy, &hz))
        {
            fraction += weights[k];
        }
    }
    return fraction;
}

static long _explosion_retail_at(server_t* server, player_t* p, vector3f_t at, float peak, float radius)
{
    float body = p->crouching ? 1.25f : 0.75f;
    float dx   = p->movement.position.x - at.x, dy = p->movement.position.y - at.y,
          dz = p->movement.position.z + body - at.z;
    float d2 = dx * dx + dy * dy + dz * dz, r2 = radius * radius;
    if (d2 >= r2 || at.z >= 62) {
        return 0;
    }
    float los = _explosion_retail_los(server, p, at);
    if (los <= 0.0f) {
        return 0;
    }
    return (long) (peak * (r2 - d2) / r2 * los + 0.5f);
}

static int _explosion_blast(server_t*  server,
                            player_t*  attacker,
                            vector3f_t pos,
                            int        crater,
                            float      damage_scale,
                            float      self_scale,
                            uint8_t    kill_reason,
                            const char* cause,
                            float      peak,
                            float      radius)
{
    map_store_t* map = &server->s_map.map;
    plugins_on_explode(server, attacker, pos.x, pos.y, pos.z);
    int       hurt = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive) {
            continue;
        }
        float scale = p == attacker ? self_scale : damage_scale;
        long  value = radius > 0.0f ? (long) (_explosion_retail_at(server, p, pos, peak, radius) * scale + 0.5f)
                                    : (long) (explosion_damage_at(server, p, pos) * scale + 0.5f);
        if (value > 0) {
            send_set_hp(server, attacker, p, value, 1, kill_reason, 5, 1, pos);
            hurt++;
        }
    }
    /* the crater: every cell of the cube the mode lets go (a protected cell
       stays, the rest goes), never the water layer */
    int cleared = 0;
    int cx = (int) floorf(pos.x), cy = (int) floorf(pos.y), cz = (int) floorf(pos.z);
    if (crater > 0 &&
        plugins_on_destroy(server, attacker, cx, cy, cz, cause))
    {
        for (int z = cz - crater; z <= cz + crater; z++) {
            for (int x = cx - crater; x <= cx + crater; x++) {
                for (int y = cy - crater; y <= cy + crater; y++) {
                    if (z < 0 || z >= 62 || x < 0 || y < 0 || x >= map->size_x || y >= map->size_y ||
                        !map_solid(map, x, y, z) || !gamemode_cell_editable(server, x, y, z))
                    {
                        continue;
                    }
                    map_set_air(map, x, y, z);
                    proto10_world_cell(server, x, y, z, P10_WORLD_CAUSE_PLAYER);
                    cleared++;
                }
            }
        }
        /* what hung on the cleared cube falls: the shell round it */
        if (cleared > 0) {
            int r = crater + 1;
            for (int z = cz - r; z <= cz + r; z++) {
                for (int x = cx - r; x <= cx + r; x++) {
                    for (int y = cy - r; y <= cy + r; y++) {
                        if (abs(x - cx) != r && abs(y - cy) != r && abs(z - cz) != r) {
                            continue;
                        }
                        vector3i_t n = {x, y, z};
                        if (z < 62 && valid_pos_v3i(server, n)) {
                            check_node(server, n);
                        }
                    }
                }
            }
        }
    }
    proto10_objects_grenade(server, attacker, pos);
    if (cleared > 0) {
        gamemode_terrain_changed(server, 0);
    }
    evt_log(attacker, "explosion", "cause=%s at=%.1f,%.1f,%.1f crater=%d cleared=%d hurt=%d", cause, pos.x, pos.y,
            pos.z, crater, cleared, hurt);
    return cleared;
}

int explosion_blast(server_t*  server,
                    player_t*  attacker,
                    vector3f_t pos,
                    int        crater,
                    float      damage_scale,
                    float      self_scale,
                    uint8_t    kill_reason,
                    const char* cause)
{
    return _explosion_blast(server, attacker, pos, crater, damage_scale, self_scale, kill_reason, cause, 0.0f, 0.0f);
}

int explosion_blast_retail(server_t*  server,
                           player_t*  attacker,
                           vector3f_t pos,
                           int        crater,
                           float      peak,
                           float      radius,
                           float      self_scale,
                           uint8_t    kill_reason,
                           const char* cause)
{
    return _explosion_blast(server, attacker, pos, crater, 1.0f, self_scale, kill_reason, cause, peak, radius);
}

/* every player a blast could reach (within 16 blocks on each axis) and what
   it did: the distance, whether the sight line from the body to the blast was
   clear, and the damage. A blast that hurts nobody says why in the log */
static void _grenade_blast_log(server_t* server, player_t* thrower, player_t* p, vector3f_t at, uint8_t value)
{
    vector3f_t pos = p->movement.position;
    float      dx = fabsf(pos.x - at.x), dy = fabsf(pos.y - at.y), dz = fabsf(pos.z - at.z);
    if (dx >= 16 || dy >= 16 || dz >= 16) {
        return;
    }
    int see = physics_can_see(server, pos.x, pos.y, pos.z, at.x, at.y, at.z) ? 1 : 0;
    evt_log(thrower, "grenade_blast", "victim=%s at=%.2f,%.2f,%.2f d=%.1f sight=%d damage=%d", p->name, at.x, at.y,
            at.z, sqrtf(dx * dx + dy * dy + dz * dz), see, value);
}

void handle_grenade(server_t* server, player_t* player)
{
    grenade_t* grenade;
    grenade_t* tmp;
    if (player->state == STATE_DISCONNECTED) {
        grenade_t* elt;
        uint32_t   counter = 0;
        DL_COUNT(player->grenade, elt, counter);
        if (counter == 0) {
            HASH_DELETE(hh, server->players, player);
            free(player);
            return;
        }
    }
    DL_FOREACH_SAFE(player->grenade, grenade, tmp)
    {
        if (grenade->sent) {
            physics_move_grenade(server, grenade, &server->physics);
            if ((get_nanos() - grenade->time_since_sent) / 1000000000.f >= grenade->fuse) {
                uint8_t allowToDestroy = 0;
                if (!(gamemode_player_rules(server, player) & GM_RULE_NO_DESTROY) &&
                    gamemode_grenade_can_destroy(server, grenade->position) &&
                    plugins_on_destroy(server, player, (int) grenade->position.x, (int) grenade->position.y,
                                       (int) grenade->position.z, "grenade"))
                {
                    allowToDestroy = 1;
                }
                plugins_on_explode(server, player, grenade->position.x, grenade->position.y, grenade->position.z);
                player_t *connected_player, *tmp_player;
                HASH_ITER(hh, server->players, connected_player, tmp_player)
                {
                    if (connected_player->state == STATE_READY) {
                        uint8_t value = get_grenade_damage(server, connected_player, grenade);
                        _grenade_blast_log(server, player, connected_player, grenade->position, value);
                        if (value > 0) {
                            send_set_hp(server, player, connected_player, value, 1, 3, 5, 1, grenade->position);
                        }
                    }
                }
                float x = grenade->position.x;
                float y = grenade->position.y;
                for (int z = grenade->position.z - 1; z <= grenade->position.z + 1; ++z) {
                    if (z < 62 &&
                        (x >= 0 && x <= server->s_map.map.size_x && x - 1 >= 0 && x - 1 <= server->s_map.map.size_x &&
                         x + 1 >= 0 && x + 1 <= server->s_map.map.size_x) &&
                        (y >= 0 && y <= server->s_map.map.size_y && y - 1 >= 0 && y - 1 <= server->s_map.map.size_y &&
                         y + 1 >= 0 && y + 1 <= server->s_map.map.size_y))
                    {
                        if (allowToDestroy && (z >= 0 && z < server->s_map.map.size_z)) {
                            // This is cause casting float to int produces an edge case where
                            // float < 0 rounds to value closer to 0. Which for -0.(>0) is bad
                            int x_rounded = floorf(x - 1);
                            int y_rounded = floorf(y - 1);
                            for (int X = x_rounded; X < x_rounded + 3; ++X) {
                                for (int Y = y_rounded; Y < y_rounded + 3; ++Y)
                                { // I hate nested loops as any other C dev but here they do not cost that much perf
                                    if (valid_pos_3f(server, X, Y, z))
                                    {
                                        map_set_air(&server->s_map.map, X, Y, z);
                                        proto10_world_cell(server, X, Y, z, P10_WORLD_CAUSE_PLAYER);
                                    }
                                }
                            }
                        }
                        vector3i_t pos;
                        pos.x = grenade->position.x;
                        pos.y = grenade->position.y;
                        pos.z = grenade->position.z;

                        vector3i_t* neigh = getGrenadeNeighbors(pos);

                        for (int index = 0; index < 54; ++index) {
                            if (valid_pos_v3i(server, neigh[index])) {
                                check_node(server, neigh[index]);
                            }
                        }
                    }
                }
                proto10_objects_grenade(server, player, grenade->position);
                proto10_entities_grenade_exploded(server, player, grenade);
                grenade->sent = 0;
                DL_DELETE(player->grenade, grenade);
                free(grenade);
                gamemode_terrain_changed(server, 0);
            }
        }
    }
}
