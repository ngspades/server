#include <Server/Server.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>
#include <stdlib.h>

/* Planes of the major axis are 512 apart, so one cell of the walk is one
   crossing of a major axis plane. A minor axis with no distance never
   crosses one and would divide by zero: it gets a plane delay no walk
   reaches. */
#define TMAX_ALT_VALUE  (0x3FFFFFFF / 1024)
#define MAX_LINE_LENGTH 50

static int line_plane_gap(int major, int minor)
{
    return minor != 0 ? major * 512 / minor : TMAX_ALT_VALUE;
}

/* Steps the axis whose next plane comes first and reports a cell past the
   map's positive edge, which ends the line. A negative cell is not tested:
   the walk only ever stopped at the top/right edge. */
static int line_advance(const map_store_t* map, vector3i_t* pos, const vector3i_t* step, vector3i_t* tmax,
                        const vector3i_t* delta)
{
    if (tmax->z <= tmax->x && tmax->z <= tmax->y) {
        pos->z += step->z;
        if (pos->z >= map->size_z) {
            return 1;
        }
        tmax->z += delta->z;
        return 0;
    }
    if (tmax->x < tmax->y) {
        pos->x += step->x;
        if (pos->x >= map->size_x) {
            return 1;
        }
        tmax->x += delta->x;
        return 0;
    }
    pos->y += step->y;
    if (pos->y >= map->size_y) {
        return 1;
    }
    tmax->y += delta->y;
    return 0;
}

/**
 * @brief Calculate block line
 *
 * @param v1 Start position
 * @param v2 End position
 * @param result Array of blocks positions
 * @return Number of block positions
 */
int line_get_blocks(const vector3i_t* v1, const vector3i_t* v2, vector3i_t* result)
{
    const map_store_t* map = &get_server()->s_map.map;

    vector3i_t pos  = *v1;
    vector3i_t dist = {v2->x - v1->x, v2->y - v1->y, v2->z - v1->z};
    vector3i_t step = {dist.x < 0 ? -1 : 1, dist.y < 0 ? -1 : 1, dist.z < 0 ? -1 : 1};
    vector3i_t a    = {abs(dist.x), abs(dist.y), abs(dist.z)};

    /* The dominant axis sets the pace; the other two get the number of
       planes they cross while it walks its own 512. */
    vector3i_t tmax;
    if (a.x >= a.y && a.x >= a.z) {
        tmax.x = 512;
        tmax.y = line_plane_gap(a.x, a.y);
        tmax.z = line_plane_gap(a.x, a.z);
    } else if (a.y >= a.z) {
        tmax.x = line_plane_gap(a.y, a.x);
        tmax.y = 512;
        tmax.z = line_plane_gap(a.y, a.z);
    } else {
        tmax.x = line_plane_gap(a.z, a.x);
        tmax.y = line_plane_gap(a.z, a.y);
        tmax.z = 512;
    }

    vector3i_t delta = {tmax.x * 2, tmax.y * 2, tmax.z * 2};

    int count = 0;
    for (;;) {
        result[count++] = pos;

        if (count >= MAX_LINE_LENGTH || (pos.x == v2->x && pos.y == v2->y && pos.z == v2->z)) {
            break;
        }
        if (line_advance(map, &pos, &step, &tmax, &delta)) {
            break;
        }
    }

    return count;
}
