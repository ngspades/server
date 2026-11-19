#include <Util/Checks/PositionChecks.h>
#include <Server/Structs/ServerStruct.h>
#include <assert.h>
#include <math.h>

/* A cell is on the map when it is not negative, under the two horizontal
   extents (x and y) and under the depth (z); lowest_z says how far above the
   world a body may still stand, a roof walk being the usual reason. A macro,
   not a static helper: an inline function with external linkage (the header
   declares these) may not use a name of this file's own. */
#define MAP_CELL_INSIDE(server, x, y, z, lowest_z) \
    ((x) >= 0 && (x) < (server)->s_map.map.size_x && (y) >= 0 && (y) < (server)->s_map.map.size_y && \
     (z) >= (lowest_z) && (z) < (server)->s_map.map.size_z)

inline uint32_t distance_in_3d(vector3f_t vector1, vector3f_t vector2)
{
    float distance = sqrt(fabs(pow(vector1.x - vector2.x, 2)) + fabs(pow(vector1.y - vector2.y, 2)) +
                          fabs(pow(vector1.z - vector2.z, 2)));

    /* a client that sends a bad float would poison every check downstream */
    assert(isnan(distance) == 0);
    return distance;
}

inline uint32_t distance_in_2d(vector3f_t vector1, vector3f_t vector2)
{
    float distance = sqrt(fabs(pow(vector1.x - vector2.x, 2)) + fabs(pow(vector1.y - vector2.y, 2)));

    assert(isnan(distance) == 0);
    return distance;
}

inline uint8_t collision_3d(vector3f_t vector1, vector3f_t vector2, uint8_t distance)
{
    return !(fabs(vector1.x - vector2.x) < distance && fabs(vector1.y - vector2.y) < distance &&
             fabs(vector1.z - vector2.z) < distance);
}

inline uint8_t valid_pos_v3i(server_t* server, vector3i_t pos)
{
    return MAP_CELL_INSIDE(server, pos.x, pos.y, pos.z, 0);
}

inline uint8_t valid_pos_v3f(server_t* server, vector3f_t pos)
{
    return MAP_CELL_INSIDE(server, pos.x, pos.y, pos.z, 0);
}

inline uint8_t valid_pos_v3f_below_z(server_t* server, vector3f_t pos)
{
    return MAP_CELL_INSIDE(server, pos.x, pos.y, pos.z, -4);
}

inline uint8_t valid_pos_3i(server_t* server, int x, int y, int z)
{
    return MAP_CELL_INSIDE(server, x, y, z, 0);
}

inline uint8_t valid_pos_3f(server_t* server, float x, float y, float z)
{
    return MAP_CELL_INSIDE(server, x, y, z, 0);
}

uint8_t valid_player_pos(server_t* server, player_t* player, float X, float Y, float Z)
{
    /* A player walking on a roof stands at z -2, and a jump over it takes
       that to -4: below that is outside the map. The top of the world is
       allowed (z one past the depth), where every cell reads as air. */
    if (!(X >= 0 && X < server->s_map.map.size_x && Y >= 0 && Y < server->s_map.map.size_y && Z >= -4 &&
          Z <= server->s_map.map.size_z))
    {
        return 0;
    }

    /* A crouching player has their feet (solid_zp2) inside a solid block, and
       water acts as solid block: their middle is inside the water while their
       feet are not inside anything. */
    uint8_t solid_z   = Z < 0 ? 0 : map_solid(&server->s_map.map, X, Y, Z);
    uint8_t solid_zp1 = Z + 1 < 0 ? 0 : map_solid(&server->s_map.map, X, Y, Z + 1);
    uint8_t solid_zp2 = Z + 2 < 0 ? 0 : map_solid(&server->s_map.map, X, Y, Z + 2);
    if (solid_z) {
        return 0;
    }
    if (solid_zp2 && Z != server->s_map.map.size_z - 3 && !player->crouching) {
        return 0;
    }
    if (solid_zp1 && !(Z == server->s_map.map.size_z - 2 && player->crouching)) {
        return 0;
    }
    return 1;
}
