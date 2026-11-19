// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EscapeWatch.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

enum
{
    ESCAPE_NONE = 0,
    ESCAPE_OUT_OF_BOUNDS,
    ESCAPE_BELOW_FLOOR,
    ESCAPE_SKY,
    ESCAPE_EMBEDDED,
    ESCAPE_ENTOMBED,
};

static struct
{
    uint8_t on;
    int     sky_seconds;
    int     embedded_seconds;
    int     entomb_seconds;
    uint64_t next_check;
} watch = {1, 5, 3, 5, 0};

static int _escape_cfg_int(toml_table_t* t, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(t, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(t, key);
    return d.ok ? (int) d.u.d : fallback;
}

void escape_watch_init(server_t* server)
{
    (void) server;
    watch.on = 1;
    watch.sky_seconds = 5;
    watch.embedded_seconds = 3;
    watch.entomb_seconds = 5;
    watch.next_check = 0;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = conf != NULL ? toml_table_in(conf, "conduct") : NULL;
        if (t != NULL) {
            toml_datum_t on = toml_bool_in(t, "escape_watch");
            if (on.ok) {
                watch.on = on.u.b ? 1 : 0;
            }
            watch.sky_seconds      = _escape_cfg_int(t, "escape_watch_sky_seconds", watch.sky_seconds);
            watch.embedded_seconds = _escape_cfg_int(t, "escape_watch_embedded_seconds", watch.embedded_seconds);
            watch.entomb_seconds   = _escape_cfg_int(t, "escape_watch_entomb_seconds", watch.entomb_seconds);
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }
    const char* env = getenv("SP_ESCAPE_WATCH");
    if (env != NULL && *env != '\0') {
        watch.on = atoi(env) != 0;
    }
    if (watch.sky_seconds < 0) {
        watch.sky_seconds = 0;
    }
    if (watch.embedded_seconds < 0) {
        watch.embedded_seconds = 0;
    }
    if (watch.entomb_seconds < 0) {
        watch.entomb_seconds = 0;
    }
    LOG_INFO("Escape watch: %s (sky %d s, embedded %d s, entombed %d s)", watch.on ? "on" : "off",
             watch.sky_seconds, watch.embedded_seconds, watch.entomb_seconds);
}

static int _escape_solid(map_store_t* map, int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z) {
        return 1;
    }
    /* the bottom layer is water the physics lets a body through: it is not a
       wall for the sealed-pocket test */
    if (z >= map->size_z - 1) {
        return 0;
    }
    return map_solid(map, x, y, z);
}

/* a small sealed air pocket: breadth first through at most 256 air cells;
   reaching the sky, a side or the bottom is open air */
static int _escape_entombed(map_store_t* map, int sx, int sy, int sz)
{
    int32_t qx[256], qy[256], qz[256];
    int     head = 0, tail = 0;
    if (_escape_solid(map, sx, sy, sz)) {
        sz--;
    }
    if (_escape_solid(map, sx, sy, sz)) {
        return 0;
    }
    qx[tail] = sx;
    qy[tail] = sy;
    qz[tail] = sz;
    tail++;
    static const int dx[6] = {1, -1, 0, 0, 0, 0};
    static const int dy[6] = {0, 0, 1, -1, 0, 0};
    static const int dz[6] = {0, 0, 0, 0, 1, -1};
    while (head < tail) {
        int x = qx[head], y = qy[head], z = qz[head];
        head++;
        if (z <= 0 || x <= 0 || y <= 0 || x >= map->size_x - 1 || y >= map->size_y - 1 || z >= map->size_z - 1) {
            return 0;
        }
        for (int k = 0; k < 6; k++) {
            int nx = x + dx[k], ny = y + dy[k], nz = z + dz[k];
            if (nx < 0 || ny < 0 || nz < 0 || nx >= map->size_x || ny >= map->size_y || nz >= map->size_z) {
                return 0;
            }
            if (map_solid(map, nx, ny, nz)) {
                continue;
            }
            int seen = 0;
            for (int i = 0; i < tail; i++) {
                if (qx[i] == nx && qy[i] == ny && qz[i] == nz) {
                    seen = 1;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            if (tail >= 256) {
                return 0; /* too large to be a one-room hideout */
            }
            qx[tail] = nx;
            qy[tail] = ny;
            qz[tail] = nz;
            tail++;
        }
    }
    return 1;
}

static uint8_t _escape_kind(server_t* server, player_t* p)
{
    map_store_t* map = &server->s_map.map;
    int       x   = (int) p->movement.position.x;
    int       y   = (int) p->movement.position.y;
    int       z   = (int) p->movement.position.z;
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return ESCAPE_OUT_OF_BOUNDS;
    }
    if (p->movement.position.z > (float) map->size_z + 1.0f) {
        return ESCAPE_BELOW_FLOOR;
    }
    if (p->movement.position.z < 1.0f) {
        return ESCAPE_SKY;
    }
    if (_escape_solid(map, x, y, z - 1) && _escape_solid(map, x, y, z) && _escape_solid(map, x, y, z + 1)) {
        return ESCAPE_EMBEDDED;
    }
    if (gamemode_escape_watch_player(server, p) && _escape_entombed(map, x, y, z)) {
        return ESCAPE_ENTOMBED;
    }
    return ESCAPE_NONE;
}

static int _escape_threshold(uint8_t kind)
{
    switch (kind) {
        case ESCAPE_SKY: return watch.sky_seconds;
        case ESCAPE_EMBEDDED: return watch.embedded_seconds;
        case ESCAPE_ENTOMBED: return watch.entomb_seconds;
        default: return 0;
    }
}

static const char* _escape_name(uint8_t kind)
{
    switch (kind) {
        case ESCAPE_OUT_OF_BOUNDS: return "out_of_bounds";
        case ESCAPE_BELOW_FLOOR: return "below_floor";
        case ESCAPE_SKY: return "sky";
        case ESCAPE_EMBEDDED: return "embedded";
        case ESCAPE_ENTOMBED: return "entombed";
        default: return "none";
    }
}

static void _escape_mark(server_t* server, player_t* victim)
{
    uint8_t enemy = victim->team == TEAM_A ? TEAM_B : TEAM_A;
    char    id[8], x[16], y[16], z[16];
    snprintf(id, sizeof id, "%u", (unsigned) victim->id);
    snprintf(x, sizeof x, "%.1f", victim->movement.position.x);
    snprintf(y, sizeof y, "%.1f", victim->movement.position.y);
    snprintf(z, sizeof z, "%.1f", victim->movement.position.z + 1.0f);
    const char* parts[6] = {"ping", id, "danger", x, y, z};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == enemy && p->protocol == PROTOCOL_10) {
            proto10_bridge_generic(server, p, 6, parts);
        }
    }
}

void escape_watch_tick(server_t* server)
{
    if (!watch.on) {
        return;
    }
    uint64_t now = get_nanos();
    if (now < watch.next_check) {
        return;
    }
    watch.next_check = now + NANO_IN_SECOND;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!is_past_join_screen(p) || p->is_bot || p->state != STATE_READY || !p->alive) {
            if (p->escape_kind != ESCAPE_NONE) {
                p->escape_kind   = ESCAPE_NONE;
                p->escape_since  = 0;
                p->escape_marked = 0;
                evt_log(p, "escape_watch", "action=clear why=not_playing");
            }
            continue;
        }
        uint8_t kind = _escape_kind(server, p);
        if (kind == ESCAPE_NONE) {
            if (p->escape_kind != ESCAPE_NONE) {
                evt_log(p, "escape_watch", "action=clear why=%s", _escape_name(p->escape_kind));
                p->escape_kind   = ESCAPE_NONE;
                p->escape_since  = 0;
                p->escape_marked = 0;
            }
            continue;
        }
        if (p->escape_kind != kind) {
            p->escape_kind   = kind;
            p->escape_since  = now;
            p->escape_marked = 0;
        }
        if (!p->escape_marked && (now - p->escape_since) / NANO_IN_SECOND >= (uint64_t) _escape_threshold(kind)) {
            p->escape_marked = 1;
            evt_log(p, "escape_watch", "action=flag why=%s at=%d,%d,%d", _escape_name(kind), (int) p->movement.position.x,
                    (int) p->movement.position.y, (int) p->movement.position.z);
        }
        if (p->escape_marked) {
            _escape_mark(server, p);
        }
    }
}
