#include <Server/Gamemodes/Gamemode.h>
#include <Server/Proto10/World.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Log.h>
#include <Util/Alloc.h>

vector3i_t* get_neighbours(vector3i_t pos)
{
    static vector3i_t neighArray[6];

    static const vector3i_t offsets[6] = {{0, 0, -1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}};

    for (int i = 0; i < 6; i++) {
        neighArray[i].x = pos.x + offsets[i].x;
        neighArray[i].y = pos.y + offsets[i].y;
        neighArray[i].z = pos.z + offsets[i].z;
    }

    return neighArray;
}

/* a structure larger than this is held: a map built to float (Heaven_amp_Hell's
   sky) was searched cell by cell at every grenade, millions of cells,
   and the tick stood still for seconds. Nothing a player builds is this big */
#define NODE_CHECK_MAX 100000

/* the search's cells to look at (a stack), the cells it reached in the order
   it reached them (the collapse goes in that order, as it did when they
   were a hash's insertion order), and a bit a cell for "reached": a hash
   node a cell, malloc'ed and freed at every check, cost more than the
   search itself */
static vector3i_t* stack;
static int         stack_cap;
static vector3i_t* reached;
static int         reached_cap;
static uint8_t*    seen;
static size_t      seen_bytes;

static int _grow(vector3i_t** buf, int* cap, int need)
{
    if (need <= *cap) {
        return 1;
    }
    int         cap2 = *cap ? *cap : 4096;
    while (cap2 < need) {
        cap2 *= 2;
    }
    vector3i_t* buf2 = (vector3i_t*) realloc(*buf, sizeof(vector3i_t) * (size_t) cap2);
    if (!buf2) {
        return 0;
    }
    *buf = buf2;
    *cap = cap2;
    return 1;
}

uint8_t check_node(server_t* server, vector3i_t position)
{
    map_store_t* map = &server->s_map.map;
    if (valid_pos_v3i(server, position) && map_solid(map, position.x, position.y, position.z) == 0) {
        return 1;
    }
    size_t cells = (size_t) map->size_x * map->size_y * map->size_z;
    if (seen_bytes != (cells + 7) / 8) {
        free(seen);
        seen_bytes = (cells + 7) / 8;
        seen       = (uint8_t*) calloc(seen_bytes, 1);
        if (!seen) {
            seen_bytes = 0;
            return 1;
        }
    }
    int top = 0, n = 0;
    if (!_grow(&stack, &stack_cap, 1)) {
        return 1;
    }
    stack[top++] = position;
    uint8_t held = 0;

    while (top > 0) {
        vector3i_t c = stack[--top];
        /* the ground holds it, and so does a cell nobody may break (the
           mode's or the map's protection): Babel's slab floats in the sky by
           design, and a block taken from a pillar or a step against it
           brought the whole slab down, the intel's platform with it */
        if (n >= NODE_CHECK_MAX) {
            LOG_INFO("Collapse check stopped at %d cells: the structure is held", NODE_CHECK_MAX);
            held = 1;
            break;
        }
        if (c.z >= map->size_z - 2 || !gamemode_cell_editable(server, c.x, c.y, c.z)) {
            held = 1;
            break;
        }
        size_t id = ((size_t) c.x * map->size_y + (size_t) c.y) * map->size_z + (size_t) c.z;
        if (seen[id >> 3] & (1u << (id & 7))) {
            continue;
        }
        seen[id >> 3] |= (uint8_t) (1u << (id & 7));
        if (!_grow(&reached, &reached_cap, n + 1) || !_grow(&stack, &stack_cap, top + 6)) {
            held = 1;
            break;
        }
        reached[n++] = c;
        static const int off[6][3] = {{0, 0, -1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}};
        for (int k = 0; k < 6; k++) {
            int x = c.x + off[k][0], y = c.y + off[k][1], z = c.z + off[k][2];
            if (x >= 0 && x < map->size_x && y >= 0 && y < map->size_y && z >= 0 && z < map->size_z &&
                map_solid(map, x, y, z))
            {
                stack[top++] = (vector3i_t){x, y, z};
            }
        }
    }

    for (int i = 0; i < n; i++) {
        vector3i_t c  = reached[i];
        size_t     id = ((size_t) c.x * map->size_y + (size_t) c.y) * map->size_z + (size_t) c.z;
        seen[id >> 3] &= (uint8_t) ~(1u << (id & 7));
        if (!held && valid_pos_v3i(server, c)) {
            map_set_air(map, c.x, c.y, c.z);
            /* 0.75 clients compute the collapse themselves, 1.0 clients are told */
            proto10_world_cell(server, c.x, c.y, c.z, P10_WORLD_CAUSE_COLLAPSE);
        }
    }
    return held;
}
