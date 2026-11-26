// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Bots.h>
#include <Server/Profile.h>
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/Gamemodes/Zombie.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Player.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Sky.h>
#include <Util/Notice.h>
#include <modes/modes.h>
#include <proto10/codec.h>
#include <Server/Server.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tomlc99/toml.h>

#define ZOMBIE_TEAM         TEAM_B
#define HUMAN_TEAM          TEAM_A
/* Call of Duty's Infected: everyone a survivor in the countdown,
   patient zero drawn at the outbreak, 120 s from then and 15 s more for
   every infection, capped at round_seconds + 5 * infection_bonus, the last
   survivor shown to the zombies. A server may change these in [zombie] or
   with the SP_ZOMBIE_* test overrides. */
typedef struct
{
    int   rounds;
    int   countdown;       /* seconds to scatter before the outbreak */
    int   round_seconds;   /* seconds from the outbreak */
    int   infection_bonus; /* seconds more for every infection */
    int   end_seconds;     /* intermission before the next round */
    int   first_infected;
    int   minimum_players;
    int   zombie_respawn_seconds; /* kept for the spawn work */
    int   radar_seconds;
    int   claw_damage;
    int   classes;           /* the survivors pick a class (the class screen) */
    int   night;             /* the match plays at night (the flashlights, the zombies' night vision) */
    float spawn_near_min;
    float spawn_near_max;
} zombie_rules_t;

static const zombie_rules_t zombie_rules_default = {
    .rounds                = 3,
    .countdown             = 10,
    .round_seconds         = 120,
    .infection_bonus       = 15,
    .end_seconds           = 5,
    .first_infected        = 0, /* 0: one for every ZOMBIE_FIRST_PER players */
    .minimum_players       = 2,
    .zombie_respawn_seconds = 3,
    .radar_seconds         = 5,
    .claw_damage           = 50,
    .classes               = 0,
    .night                 = 1, /* the mode the owner asked for: hunters with lamps, zombies seeing in the dark */
    .spawn_near_min        = 35.0f,
    .spawn_near_max        = 50.0f,
};

static zombie_rules_t zombie_rules;

/* the host's live choice for the classes (the console's /classes): -1 the
   mode's own rule (config.toml, the command line) */
static int zombie_classes_host = -1;

/* on its high ground a bot patrols: every PATROL_STEP seconds the group
   moves PATROL_STRIDE places on (each by rank, so no two share one; a
   stride across the rings walks it over the site, one place on was a
   step: still 68% of the time), and its look sweeps
   PATROL_SWEEP radians each side of "out" over PATROL_SWEEP_S. Standing on
   one place facing one way, players read them as AFK */
#define PATROL_STEP         4
#define PATROL_STRIDE       7
#define PATROL_SWEEP        1.2f
#define PATROL_SWEEP_S      9.0f
#define ZOMBIE_FIRST_PER    6    /* players a patient zero, first_infected 0 (_zombie_outbreak) */
#define HIGH_CHUNK          16   /* the client's chunk (chunk.h) */
/* the search box round the bot: 48 blocks each way. Inside its own 2 x 2
   chunks (32 x 32) a town's survivors found no roof they could walk up to
   (up 0 or 1 for 33 of 44 sites on 20thCenturyTown) and walled themselves
   in where they spawned */
#define HIGH_R              48
#define HIGH_DROP           6    /* the walk: one block up (autoclimb), at most this down */
#define HIGH_ROOM           6    /* floors at its level within 2 of it: room for the group */
#define HIGH_SPOTS          49   /* the places on it, nearest the middle first (7 x 7) */
#define HIGH_SITES          32   /* high grounds a round: the survivors spawn all over the map */
#define HIGH_FLAT           2    /* a site less than this above its search's start is flat ground */
#define FORT_R              3    /* a flat site's fort: a ring this far round its middle */
#define FORT_CELLS          (8 * FORT_R)
#define FORT_HIGH           3    /* blocks: a zombie needs its leap at the wall, slowed, in the guns (over 2 it walked in after a hop) */
#define JOIN_R              64.0f /* a bot on flat ground joins high ground or a group this near */
#define HIGH_SAME           4    /* two searches ending within this, at one level, are one site */
/* no height on foot: a roof ROOF_MIN to ROOF_MAX over a floor walked to,
   climbed with a pillar against its wall (a block a level: a human has 50),
   each bot its own of up to ROOF_PILLARS. 20thCenturyTown's houses have no
   stairs: 8000 cells walked from a spawn and nothing two blocks up */
#define ROOF_MIN            3
#define ROOF_MAX            10
#define ROOF_PILLARS        8
#define HELP_R              8.0f /* a zombie this near a groupmate is everyone's in the group */

typedef enum
{
    ZOMBIE_WAITING,
    ZOMBIE_COUNTDOWN_STATE,
    ZOMBIE_FIGHT,
    ZOMBIE_ENDED,
} zombie_state_t;

static const char* zombie_state_names[] = {"waiting", "countdown", "fight", "end"};

static struct
{
    zombie_state_t state;
    uint64_t       since;
    int            round;
    int            winner; /* TEAM_A, TEAM_B, -1 none yet */
    uint8_t        match_over;
    int            humans;     /* alive, last tick */
    uint64_t       deadline;   /* the fight's end, game nanos */
    int            infections; /* this round */
    uint64_t       survival_at; /* the next survival point */
    uint64_t       radar_at;   /* the last survivor's next ping */
    int            last_id;    /* the last survivor told so, -1 none */
    uint16_t       round_of[256]; /* the round each player was in at its start: a later one joined late */
    uint8_t        turn_here[256]; /* patient zero: its spawn is where it stood */
    vector3f_t     here[256];
    uint16_t       help_logged[256]; /* the zombie each bot last turned to, + 1 */
} zombie;

/* a human bot's high ground: the highest place a player can walk to from
   where it spawned (steps of one block up, drops of HIGH_DROP, no water)
   with room for a group, and the places on it the bots hold together, each
   its own, facing out. The first version had them ring the middle of their
   spawn with a fort on flat ground, where they read as dumb: up the tallest
   building they can reach instead. Not the highest of the whole map: the
   highest of the 2 x 2 chunks round the bot (the four whose shared corner
   is nearest it), so a survivor spawned anywhere holds the high ground of
   its own neighbourhood; the bots with the same four chunks share a site
   and end up together */
typedef struct
{
    int     set;
    int     ax, ay;     /* the four chunks' first cell */
    int     cx, cy, fz; /* the middle and its floor (the solid cell stood on) */
    int     n;
    int16_t x[HIGH_SPOTS], y[HIGH_SPOTS];
    int     up; /* levels over where its search began: under HIGH_FLAT it is flat */
    int     same; /* another site's index + 1 this search ended on: that one is used */
    /* a roof site: the columns beside its wall to build a pillar in, and the
       floor each stands on */
    int     pillars;
    int16_t px[ROOF_PILLARS], py[ROOF_PILLARS], pz[ROOF_PILLARS];
    /* flat: the fort round it, the columns and the ground each wall stands on */
    int     fort_n, fort_built;
    int16_t fx[FORT_CELLS], fy[FORT_CELLS], fg[FORT_CELLS];
} high_site_t;

static struct
{
    int         n;
    high_site_t site[HIGH_SITES];
    uint8_t     of[256]; /* each player's site + 1, 0 none (a new life, a new one) */
} high;

/* a floor at x, y, z: solid, three free cells over it (a player and a
   crouch's room), not the water layer */
static int _high_floor(map_store_t* map, int x, int y, int z)
{
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y || z < 0 || z >= map->size_z - 1 ||
        !map_solid(map, x, y, z)) {
        return 0;
    }
    for (int h = 1; h <= 3; h++) {
        if (z - h >= 0 && map_solid(map, x, y, z - h)) {
            return 0;
        }
    }
    return 1;
}

/* the top of a column, or the bottom of the map off it (the roof search
   looks round cells at the edge) */
static int _high_top(map_store_t* map, int x, int y)
{
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return map->size_z;
    }
    return map_top(map, (uint16_t) x, (uint16_t) y);
}

/* a place with floor at its level on all four sides: off the edge, where a
   walk between places does not drop off the roof */
static int _high_inner(map_store_t* map, int x, int y, int z)
{
    static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 4; k++) {
        if (!_high_floor(map, x + d[k][0], y + d[k][1], z)) {
            return 0;
        }
    }
    return 1;
}

/* a site over the zombies: high ground walked to, or a roof climbed */
static int _high_up(const high_site_t* st)
{
    return st->up >= HIGH_FLAT || st->pillars > 0;
}

static void _zombie_high_site(server_t* server, int sx, int sy, high_site_t* out)
{
    map_store_t* map = &server->s_map.map;
    int       w = 2 * HIGH_R + 1, zs = map->size_z;
    /* one byte per cell of the searched box, a breadth first walk; once per
       site, a few ms */
    uint8_t* seen  = calloc((size_t) w * w * zs, 1);
    int32_t* queue = malloc(sizeof(int32_t) * (size_t) w * w * 8);
    if (seen == NULL || queue == NULL) {
        free(seen);
        free(queue);
        out->set = 0;
        return;
    }
#define HIGH_IDX(x, y, z) ((((y) - sy + HIGH_R) * w + ((x) - sx + HIGH_R)) * zs + (z))
    int head = 0, tail = 0, cap = w * w * 8;
    int z0 = map_top(map, (uint16_t) sx, (uint16_t) sy);
    if (z0 < map->size_z && _high_floor(map, sx, sy, z0)) {
        seen[HIGH_IDX(sx, sy, z0)] = 1;
        queue[tail++]              = HIGH_IDX(sx, sy, z0);
    }
    float            best = -1e9f;
    int              bx = sx, by = sy, bz = z0, nodes = 0;
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    while (head < tail) {
        int32_t id = queue[head++];
        int     z  = id % zs;
        int     x  = (id / zs) % w + sx - HIGH_R;
        int     y  = (id / zs) / w + sy - HIGH_R;
        nodes++;
        /* room for the group at this level */
        int room = 0;
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < map->size_x && ny < map->size_y && _high_floor(map, nx, ny, z)) {
                    room++;
                }
            }
        }
        if (room >= HIGH_ROOM) {
            /* height first; closer breaks the ties (a long walk is a long
               time in the open) */
            float d     = sqrtf((float) ((x - sx) * (x - sx) + (y - sy) * (y - sy)));
            float score = (float) (zs - z) - d * 0.08f;
            if (score > best) {
                best = score;
                bx   = x;
                by   = y;
                bz   = z;
            }
        }
        for (int k = 0; k < 4; k++) {
            int nx = x + dirs[k][0], ny = y + dirs[k][1];
            if (nx < 0 || ny < 0 || nx >= map->size_x || ny >= map->size_y || abs(nx - sx) > HIGH_R ||
                abs(ny - sy) > HIGH_R)
            {
                continue;
            }
            for (int nz = z - 1; nz <= z + HIGH_DROP && nz < zs - 1; nz++) {
                /* a step up needs the head's room over this floor too */
                if (nz < z && z - 4 >= 0 && map_solid(map, x, y, z - 4)) {
                    continue;
                }
                if (!_high_floor(map, nx, ny, nz)) {
                    continue;
                }
                if (!seen[HIGH_IDX(nx, ny, nz)] && tail < cap) {
                    seen[HIGH_IDX(nx, ny, nz)] = 1;
                    queue[tail++]              = HIGH_IDX(nx, ny, nz);
                }
                break; /* the first floor down this column: the one walked onto */
            }
        }
    }
    out->pillars = 0;
    if (z0 - bz < HIGH_FLAT) {
        /* no way up on foot: the best roof beside a floor walked to, by its
           height and its distance (the same weights) */
        float rbest = -1e9f;
        int   rx = 0, ry = 0, rz = 0;
        for (int y = sy - HIGH_R; y <= sy + HIGH_R; y++) {
            for (int x = sx - HIGH_R; x <= sx + HIGH_R; x++) {
                if (x < 1 || y < 1 || x >= map->size_x - 1 || y >= map->size_y - 1) {
                    continue;
                }
                for (int z = 0; z < zs - 1; z++) {
                    if (!seen[HIGH_IDX(x, y, z)]) {
                        continue;
                    }
                    for (int k = 0; k < 4; k++) {
                        int nx = x + dirs[k][0], ny = y + dirs[k][1];
                        int top = _high_top(map, nx, ny);
                        int rise = z - top;
                        /* over the pillar's floor, and over where the search began */
                        if (rise < ROOF_MIN || rise > ROOF_MAX || z0 - top < HIGH_FLAT || !_high_floor(map, nx, ny, top)) {
                            continue;
                        }
                        int room = 0;
                        for (int dy = -2; dy <= 2; dy++) {
                            for (int dx = -2; dx <= 2; dx++) {
                                room += _high_floor(map, nx + dx, ny + dy, top) &&
                                        _high_top(map, nx + dx, ny + dy) == top;
                            }
                        }
                        float d     = sqrtf((float) ((x - sx) * (x - sx) + (y - sy) * (y - sy)));
                        float score = (float) (zs - top) - d * 0.08f;
                        if (room >= HIGH_ROOM && score > rbest) {
                            rbest = score;
                            rx    = nx;
                            ry    = ny;
                            rz    = top;
                        }
                    }
                }
            }
        }
        if (rbest > -1e9f) {
            bx = rx;
            by = ry;
            bz = rz;
            /* the pillars: floors walked to beside the roof's edge near it */
            for (int r = 0; r <= 3 && out->pillars < ROOF_PILLARS; r++) {
                for (int dy = -r; dy <= r && out->pillars < ROOF_PILLARS; dy++) {
                    for (int dx = -r; dx <= r && out->pillars < ROOF_PILLARS; dx++) {
                        int ex = rx + dx, ey = ry + dy;
                        if ((abs(dx) != r && abs(dy) != r) ||
                            _high_top(map, ex, ey) != rz ||
                            !_high_floor(map, ex, ey, rz)) {
                            continue;
                        }
                        for (int k = 0; k < 4 && out->pillars < ROOF_PILLARS; k++) {
                            int gx = ex + dirs[k][0], gy = ey + dirs[k][1];
                            if (abs(gx - sx) > HIGH_R || abs(gy - sy) > HIGH_R || gx < 0 || gy < 0 || gx >= map->size_x ||
                                gy >= map->size_y) {
                                continue;
                            }
                            for (int z = rz + ROOF_MIN; z <= rz + ROOF_MAX && z < zs - 1; z++) {
                                if (!seen[HIGH_IDX(gx, gy, z)]) {
                                    continue;
                                }
                                int dup = 0;
                                for (int q = 0; q < out->pillars; q++) {
                                    dup |= out->px[q] == gx && out->py[q] == gy;
                                }
                                if (!dup) {
                                    out->px[out->pillars] = (int16_t) gx;
                                    out->py[out->pillars] = (int16_t) gy;
                                    out->pz[out->pillars] = (int16_t) z;
                                    out->pillars++;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    out->cx = bx;
    out->cy = by;
    out->fz = bz;
    out->n  = 0;
    /* the places on it: reached floors at its level (a roof's: floors of
       its surface), nearest first */
    for (int r = 0; r <= 3; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int x = bx + dx, y = by + dy;
                if ((abs(dx) != r && abs(dy) != r) || out->n >= HIGH_SPOTS || abs(x - sx) > HIGH_R ||
                    abs(y - sy) > HIGH_R || x < 0 || y < 0 || x >= map->size_x || y >= map->size_y)
                {
                    continue;
                }
                if (out->pillars > 0 ? _high_top(map, x, y) == bz &&
                                           _high_floor(map, x, y, bz)
                                     : seen[HIGH_IDX(x, y, bz)]) {
                    out->x[out->n] = (int16_t) x;
                    out->y[out->n] = (int16_t) y;
                    out->n++;
                }
            }
        }
    }
#undef HIGH_IDX
    free(seen);
    free(queue);
    out->set = out->n > 0;
    out->up  = z0 - bz;
    /* flat ground has nothing to stand on above the zombies: the group walls
       itself in (the fort of the first version, now only where there is no
       height) */
    out->fort_n = out->fort_built = 0;
    if (out->set && out->up < HIGH_FLAT && out->pillars == 0) {
        for (int dy = -FORT_R; dy <= FORT_R; dy++) {
            for (int dx = -FORT_R; dx <= FORT_R; dx++) {
                int x = bx + dx, y = by + dy;
                if ((abs(dx) != FORT_R && abs(dy) != FORT_R) || x < 0 || y < 0 || x >= map->size_x ||
                    y >= map->size_y)
                {
                    continue;
                }
                int g = map_top(map, (uint16_t) x, (uint16_t) y);
                /* level with the middle, and no wall on water or a cliff */
                if (g >= map->size_z - 2 || abs(g - bz) > 2) {
                    continue;
                }
                out->fx[out->fort_n] = (int16_t) x;
                out->fy[out->fort_n] = (int16_t) y;
                out->fg[out->fort_n] = (int16_t) g;
                out->fort_n++;
            }
        }
    }
    evt_log(NULL, "zombie_high", "action=site at=%d,%d,%d up=%d from=%d,%d,%d dist=%.0f spots=%d walked=%d pillars=%d", bx,
            by, bz, z0 - bz, sx, sy, z0, sqrtf((float) ((bx - sx) * (bx - sx) + (by - sy) * (by - sy))), out->n, nodes,
            out->pillars);
}

/* human bots: up to the high ground, each to its own place there, holding it
   facing out from the middle. Zombie bots (and humans when there is none)
   hunt as in any mode */
/* the bot's site: the one it has while its floor stands, else the one of its
   four chunks another bot found, else searched from where it stands */
static high_site_t* _zombie_site_for(server_t* server, player_t* bot)
{
    map_store_t* map = &server->s_map.map;
    int       k   = high.of[bot->id];
    if (k > 0 && _high_floor(map, high.site[k - 1].cx, high.site[k - 1].cy, high.site[k - 1].fz)) {
        return &high.site[k - 1];
    }
    high.of[bot->id] = 0;
    int bx = (int) bot->movement.position.x, by = (int) bot->movement.position.y;
    /* the chunk corner nearest the bot, and the four chunks round it */
    int ax = ((bx + HIGH_CHUNK / 2) / HIGH_CHUNK - 1) * HIGH_CHUNK;
    int ay = ((by + HIGH_CHUNK / 2) / HIGH_CHUNK - 1) * HIGH_CHUNK;
    /* a search that found no room is kept too, or every think redid it */
    int i = 0;
    while (i < high.n && (high.site[i].ax != ax || high.site[i].ay != ay)) {
        i++;
    }
    if (i == high.n && high.n == HIGH_SITES) {
        return NULL;
    }
    high_site_t* st = &high.site[i];
    if (i == high.n || (st->set && !_high_floor(map, st->cx, st->cy, st->fz))) {
        /* new, or its floor was dug away: searched (again) */
        _zombie_high_site(server, bx, by, st);
        st->ax   = ax;
        st->ay   = ay;
        st->same = 0;
        /* the same roof found from another neighbourhood: one group there,
           not two side by side each holding half of it */
        for (int j = 0; st->set && j < high.n; j++) {
            high_site_t* o = &high.site[j];
            if (j != i && o->set && o->same == 0 && o->fz == st->fz && abs(o->cx - st->cx) <= HIGH_SAME &&
                abs(o->cy - st->cy) <= HIGH_SAME) {
                st->same = j + 1;
                evt_log(bot, "zombie_high", "action=same site=%d as=%d at=%d,%d,%d", i, j, st->cx, st->cy, st->fz);
                break;
            }
        }
        high.n += i == high.n;
    }
    if (st->set && st->same > 0) {
        i  = st->same - 1;
        st = &high.site[i];
    }
    if (st->set) {
        high.of[bot->id] = (uint8_t) (i + 1);
    }
    return high.of[bot->id] ? &high.site[high.of[bot->id] - 1] : NULL;
}

/* living human players on site i + 1, the bot left out */
static int _zombie_site_humans(server_t* server, player_t* bot, int of)
{
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        n += p != bot && p->team == HUMAN_TEAM && p->alive && high.of[p->id] == of;
    }
    return n;
}

/* a bot on flat ground goes up the nearest high site within JOIN_R that
   any search found (a roof its own neighbourhood lacks), with or without
   mates: on a town's flat streets the bots walled themselves in where they
   spawned, by the stairs of a building one search over. With no high site
   near, alone, it goes to the nearest site where other humans are (walled
   in): alone on Zombie_Island's plain
   every survivor was one zombie's meal, and the zombies won 17 rounds of
   20. With no limit the whole team crossed the island to the first site
   (up to 382 blocks): groups of the neighbourhood instead, one crowd is
   what the scattered spawn is for */
static high_site_t* _zombie_group_for(server_t* server, player_t* bot, high_site_t* site)
{
    if (_high_up(site)) {
        return site;
    }
    float best = JOIN_R * JOIN_R;
    int   to   = 0;
    for (int i = 0; i < high.n; i++) {
        high_site_t* st = &high.site[i];
        if (!st->set || st->same > 0 || !_high_up(st) || i + 1 == high.of[bot->id] ||
            !_high_floor(&server->s_map.map, st->cx, st->cy, st->fz)) {
            continue;
        }
        float dx = st->cx + 0.5f - bot->movement.position.x, dy = st->cy + 0.5f - bot->movement.position.y;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            to   = i + 1;
        }
    }
    if (to == 0 && _zombie_site_humans(server, bot, high.of[bot->id]) > 0) {
        return site;
    }
    for (int i = 0; to == 0 && i < high.n; i++) {
        high_site_t* st = &high.site[i];
        /* a site whose middle is no floor any more (a neighbour's fort wall
           on it) gave the bot back its own the next think: two bots joined
           one 230 times each in a match */
        if (!st->set || st->same > 0 || i + 1 == high.of[bot->id] || _zombie_site_humans(server, bot, i + 1) == 0 ||
            !_high_floor(&server->s_map.map, st->cx, st->cy, st->fz)) {
            continue;
        }
        float dx = st->cx + 0.5f - bot->movement.position.x, dy = st->cy + 0.5f - bot->movement.position.y;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            to   = i + 1;
        }
    }
    if (to == 0) {
        return site;
    }
    evt_log(bot, "zombie_high", "action=join site=%d at=%d,%d dist=%.0f up=%d", to - 1, high.site[to - 1].cx,
            high.site[to - 1].cy, sqrtf(best), high.site[to - 1].up);
    high.of[bot->id] = (uint8_t) to;
    return &high.site[to - 1];
}

/* the missing wall cell of the site's fort nearest the bot, bottom first:
   1 and its column and z */
static int _zombie_fort_missing(server_t* server, player_t* bot, high_site_t* st, int* index, int* z)
{
    map_store_t* map  = &server->s_map.map;
    float     best = 1e9f;
    st->fort_built = 0;
    for (int k = 0; k < st->fort_n; k++) {
        for (int h = 1; h <= FORT_HIGH; h++) {
            int cz = st->fg[k] - h;
            if (map_solid(map, st->fx[k], st->fy[k], cz)) {
                st->fort_built++;
                continue;
            }
            if (!gamemode_cell_editable(server, st->fx[k], st->fy[k], cz)) {
                break;
            }
            float dx = st->fx[k] + 0.5f - bot->movement.position.x, dy = st->fy[k] + 0.5f - bot->movement.position.y;
            float d  = dx * dx + dy * dy;
            if (d < best) {
                best   = d;
                *index = k;
                *z     = cz;
            }
            break; /* the lower block first */
        }
    }
    return best < 1e9f;
}

/* the zombie nearest a groupmate within HELP_R of it, if any: the whole
   group turns to it */
static player_t* _zombie_threat(server_t* server, player_t* bot, float* mate_d)
{
    player_t *best = NULL, *p, *tmp, *z, *tmp2;
    float     bd   = HELP_R * HELP_R;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team != HUMAN_TEAM || !p->alive || high.of[p->id] != high.of[bot->id]) {
            continue;
        }
        HASH_ITER(hh, server->players, z, tmp2)
        {
            if (z->team != ZOMBIE_TEAM || !z->alive || z->state != STATE_READY) {
                continue;
            }
            float dx = z->movement.position.x - p->movement.position.x;
            float dy = z->movement.position.y - p->movement.position.y;
            float dz = z->movement.position.z - p->movement.position.z;
            float d  = dx * dx + dy * dy + dz * dz;
            if (d < bd) {
                bd    = d;
                best  = z;
                *mate_d = sqrtf(d);
            }
        }
    }
    return best;
}

static int _zombie_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    high_site_t* site = NULL;
    if (bot->team == HUMAN_TEAM && bot->alive &&
        (zombie.state == ZOMBIE_COUNTDOWN_STATE || zombie.state == ZOMBIE_FIGHT)) {
        site = _zombie_site_for(server, bot);
    }
    if (site == NULL) {
        return gamemode_hunt_nearest(server, bot, out);
    }
    site = _zombie_group_for(server, bot, site);
    memset(out, 0, sizeof(*out));
    map_store_t* map = &server->s_map.map;
    float     mdx = site->cx + 0.5f - bot->movement.position.x, mdy = site->cy + 0.5f - bot->movement.position.y;
    uint8_t   in  = mdx * mdx + mdy * mdy <= (FORT_R + 1.5f) * (FORT_R + 1.5f);
    int       fk, fz;
    /* a roof: under its level, its own pillar column by its rank (the next
       free one when that is built), walked to, then the pillar up to the
       roof's level; over it, the places as on any high ground */
    if (site->pillars > 0 && bot->movement.position.z + 2.4f > (float) site->fz + 0.5f && bot->blocks > 0) {
        int rank = 0;
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            rank += p != bot && p->is_bot && p->team == HUMAN_TEAM && p->alive && p->id < bot->id &&
                    high.of[p->id] == high.of[bot->id];
        }
        int pk = rank % site->pillars;
        int bcx = (int) bot->movement.position.x, bcy = (int) bot->movement.position.y;
        for (int q = 0; q < site->pillars; q++) {
            int c = (rank + q) % site->pillars;
            /* built to the top by another: no floor for a second body; the
               one it stands in is its own */
            if ((site->px[c] == bcx && site->py[c] == bcy) ||
                map_top(map, (uint16_t) site->px[c], (uint16_t) site->py[c]) > site->fz) {
                pk = c;
                break;
            }
        }
        uint8_t here = bcx == site->px[pk] && bcy == site->py[pk];
        gm_goal_name(out, here ? "roof-climb" : "to-roof", 0);
        out->x         = site->px[pk] + 0.5f;
        out->y         = site->py[pk] + 0.5f;
        out->z         = here ? (float) site->fz : (float) site->pz[pk];
        out->exact     = 1;
        out->climb     = here;
        out->walk_only = 1;
        out->role      = GM_BOT_RUSH;
        return 1;
    }
    /* a flat site: once there, walls round it, from inside (the column next
       to the wall toward the middle) */
    if (site->fort_n > 0 && in && bot->blocks > 0 && _zombie_fort_missing(server, bot, site, &fk, &fz)) {
        int sx = site->fx[fk] - (site->fx[fk] > site->cx) + (site->fx[fk] < site->cx);
        int sy = site->fy[fk] - (site->fy[fk] > site->cy) + (site->fy[fk] < site->cy);
        gm_goal_name(out, "fort-build", 0);
        out->x      = sx + 0.5f;
        out->y      = sy + 0.5f;
        out->z      = (float) map_top(map, (uint16_t) sx, (uint16_t) sy);
        out->exact  = 1;
        out->work   = 1;
        out->work_x = site->fx[fk];
        out->work_y = site->fy[fk];
        out->work_z = fz;
        out->walk_only = 1;
        return 1;
    }
    /* its place by its rank among the living human bots of its site: the
       places fill from the middle and nobody shares one */
    int       rank = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != bot && p->is_bot && p->team == HUMAN_TEAM && p->alive && p->id < bot->id &&
            high.of[p->id] == high.of[bot->id]) {
            rank++;
        }
    }
    /* inside the fort on a flat site: its ring is the places FORT_R out */
    int places = site->n;
    if (site->fort_n > 0) {
        places = 0;
        while (places < site->n && abs(site->x[places] - site->cx) < FORT_R &&
               abs(site->y[places] - site->cy) < FORT_R) {
            places++;
        }
        places = places > 0 ? places : site->n;
    }
    /* the time on this state (the round's countdown or fight), never the
       clock's absolute value: a seeded run must replay the same */
    float on_state = (float) (get_nanos() - zombie.since) / (float) NANO_IN_SECOND;
    int   shift    = (int) (on_state / PATROL_STEP);
    int   k        = (rank + shift * PATROL_STRIDE) % places;
    /* patrolling (moved on from its first place) it keeps off the edges: the
       walks between places dropped bots off the roofs (Ghost_Town, 4 matches:
       6 falls to 14) */
    for (int q = 0; shift > 0 && q < places && !_high_inner(map, site->x[k], site->y[k], site->fz); q++) {
        k = (k + 1) % places;
    }
    gm_goal_name(out, site->fort_n > 0 ? "fort-hold" : "high-ground", 0);
    out->x       = site->x[k] + 0.5f;
    out->y       = site->y[k] + 0.5f;
    out->z       = (float) site->fz;
    out->hold    = 0.8f;
    /* on foot, except into a walled site from outside: over a wall two high
       there is no step, the spade opens it and the builders mend it */
    out->walk_only = !(site->fort_n > 0 && !in);
    /* on the way it keeps climbing and shoots on the move (the rusher's run):
       the climb takes ~100 steps in the fight, and stopping at every zombie
       kept the group at the foot (on the top: 0 of 11 most of a round) */
    float tx = site->x[k] + 0.5f - bot->movement.position.x, ty = site->y[k] + 0.5f - bot->movement.position.y;
    if (tx * tx + ty * ty > 3.0f * 3.0f || fabsf(bot->movement.position.z - (float) site->fz) > 4.0f) {
        out->role = GM_BOT_RUSH;
    }
    /* out from the middle; the middle itself looks along a side */
    float ox = (float) (site->x[k] - site->cx), oy = (float) (site->y[k] - site->cy);
    if (ox == 0.0f && oy == 0.0f) {
        oy = 1.0f;
    }
    /* the sweep, each bot in its own phase */
    float sweep = PATROL_SWEEP * sinf(on_state * 6.2831853f / PATROL_SWEEP_S + (float) bot->id);
    float rx = ox * cosf(sweep) - oy * sinf(sweep), ry = ox * sinf(sweep) + oy * cosf(sweep);
    out->watch_x = site->x[k] + rx * 8.0f + 0.5f;
    out->watch_y = site->y[k] + ry * 8.0f + 0.5f;
    /* a zombie at a groupmate: everyone on its place turns to it, so their
       sight (a cone round the view) picks it and they shoot it off the mate
       instead of watching their own side */
    float     md;
    player_t* threat = out->role == GM_BOT_RUSH ? NULL : _zombie_threat(server, bot, &md);
    if (threat != NULL) {
        out->watch_x = threat->movement.position.x;
        out->watch_y = threat->movement.position.y;
        if (zombie.help_logged[bot->id] != threat->id + 1) {
            zombie.help_logged[bot->id] = (uint16_t) (threat->id + 1);
            evt_log(bot, "zombie_help", "zombie=%u mate_dist=%.1f", threat->id, md);
        }
    } else {
        zombie.help_logged[bot->id] = 0;
    }
    return 1;
}

static void _zombie_set_state(zombie_state_t state)
{
    zombie.state = state;
    zombie.since = get_nanos();
}

static int _zombie_in_team(const player_t* p)
{
    return (p->team == TEAM_A || p->team == TEAM_B) &&
           (p->state == STATE_READY || p->state == STATE_WAITING_FOR_RESPAWN || p->state == STATE_SPAWNING);
}

/* a team change with no death (the player is dead already, or the round
   is starting): the bridge tells clients from player->team */
static void _zombie_set_team(server_t* server, player_t* p, uint8_t team)
{
    if (p->team == team) {
        return;
    }
    if (server->protocol.num_team_users[p->team] > 0) {
        server->protocol.num_team_users[p->team]--;
    }
    p->team = team;
    server->protocol.num_team_users[team]++;
}

/* a number in [zombie]; an integer and a float are both accepted */
static int _zombie_cfg_int(toml_table_t* table, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(table, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(table, key);
    return d.ok ? (int) d.u.d : fallback;
}

static float _zombie_cfg_float(toml_table_t* table, const char* key, float fallback)
{
    toml_datum_t d = toml_double_in(table, key);
    if (d.ok) {
        return (float) d.u.d;
    }
    d = toml_int_in(table, key);
    return d.ok ? (float) d.u.i : fallback;
}

/* a switch in [zombie]: true or false */
static int _zombie_cfg_bool(toml_table_t* table, const char* key, int fallback)
{
    toml_datum_t d = toml_bool_in(table, key);
    if (d.ok) {
        return d.u.b ? 1 : 0;
    }
    return fallback;
}

static int _zombie_env_int(const char* name, int fallback)
{
    const char* value = getenv(name);
    return value != NULL && *value != '\0' ? atoi(value) : fallback;
}

/* a switch in the environment: 1, true, yes or on; 0, false, no or off */
static int _zombie_env_bool(const char* name, int fallback)
{
    const char* value = getenv(name);
    if (value == NULL || *value == '\0') {
        return fallback;
    }
    if (!strcasecmp(value, "true") || !strcasecmp(value, "yes") || !strcasecmp(value, "on")) {
        return 1;
    }
    if (!strcasecmp(value, "false") || !strcasecmp(value, "no") || !strcasecmp(value, "off")) {
        return 0;
    }
    return atoi(value) != 0 ? 1 : 0;
}

/* [zombie] in Resources/config.toml, then the SP_ZOMBIE_* test overrides.
   The table is optional: the defaults are the shipped mode. */
static void _zombie_read_rules(void)
{
    zombie_rules = zombie_rules_default;

    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* table = conf != NULL ? toml_table_in(conf, "zombie") : NULL;
        if (table != NULL) {
            zombie_rules.rounds                = _zombie_cfg_int(table, "rounds", zombie_rules.rounds);
            zombie_rules.countdown             = _zombie_cfg_int(table, "countdown", zombie_rules.countdown);
            zombie_rules.round_seconds         = _zombie_cfg_int(table, "round_seconds", zombie_rules.round_seconds);
            zombie_rules.infection_bonus       = _zombie_cfg_int(table, "infection_bonus", zombie_rules.infection_bonus);
            zombie_rules.end_seconds           = _zombie_cfg_int(table, "end_seconds", zombie_rules.end_seconds);
            zombie_rules.first_infected        = _zombie_cfg_int(table, "first_infected", zombie_rules.first_infected);
            zombie_rules.minimum_players       = _zombie_cfg_int(table, "minimum_players", zombie_rules.minimum_players);
            zombie_rules.zombie_respawn_seconds =
                _zombie_cfg_int(table, "zombie_respawn_seconds", zombie_rules.zombie_respawn_seconds);
            zombie_rules.radar_seconds   = _zombie_cfg_int(table, "radar_seconds", zombie_rules.radar_seconds);
            zombie_rules.claw_damage     = _zombie_cfg_int(table, "claw_damage", zombie_rules.claw_damage);
            zombie_rules.classes         = _zombie_cfg_bool(table, "classes", zombie_rules.classes);
            zombie_rules.night           = _zombie_cfg_bool(table, "night", zombie_rules.night);
            zombie_rules.spawn_near_min  = _zombie_cfg_float(table, "spawn_near_min", zombie_rules.spawn_near_min);
            zombie_rules.spawn_near_max  = _zombie_cfg_float(table, "spawn_near_max", zombie_rules.spawn_near_max);
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }

    zombie_rules.rounds          = _zombie_env_int("SP_ZOMBIE_ROUNDS", zombie_rules.rounds);
    zombie_rules.countdown       = _zombie_env_int("SP_ZOMBIE_COUNTDOWN", zombie_rules.countdown);
    zombie_rules.round_seconds   = _zombie_env_int("SP_ZOMBIE_ROUND_SECONDS", zombie_rules.round_seconds);
    zombie_rules.infection_bonus = _zombie_env_int("SP_ZOMBIE_INFECTION_BONUS", zombie_rules.infection_bonus);
    zombie_rules.end_seconds     = _zombie_env_int("SP_ZOMBIE_END_SECONDS", zombie_rules.end_seconds);
    zombie_rules.first_infected  = _zombie_env_int("SP_ZOMBIE_FIRST_INFECTED", zombie_rules.first_infected);
    zombie_rules.minimum_players = _zombie_env_int("SP_ZOMBIE_MIN_PLAYERS", zombie_rules.minimum_players);
    zombie_rules.radar_seconds   = _zombie_env_int("SP_ZOMBIE_RADAR", zombie_rules.radar_seconds);
    zombie_rules.claw_damage     = _zombie_env_int("SP_ZOMBIE_CLAW", zombie_rules.claw_damage);
    zombie_rules.classes         = _zombie_env_bool("SP_ZOMBIE_CLASSES", zombie_rules.classes);
    zombie_rules.night           = _zombie_env_bool("SP_ZOMBIE_NIGHT", zombie_rules.night);

    if (zombie_rules.rounds < 1) {
        zombie_rules.rounds = 1;
    }
    if (zombie_rules.rounds > 255) {
        zombie_rules.rounds = 255;
    }
    if (zombie_rules.countdown < 0) {
        zombie_rules.countdown = 0;
    }
    if (zombie_rules.round_seconds < 1) {
        zombie_rules.round_seconds = 1;
    }
    if (zombie_rules.infection_bonus < 0) {
        zombie_rules.infection_bonus = 0;
    }
    if (zombie_rules.end_seconds < 0) {
        zombie_rules.end_seconds = 0;
    }
    if (zombie_rules.first_infected < 0) {
        zombie_rules.first_infected = 0;
    }
    if (zombie_rules.minimum_players < 1) {
        zombie_rules.minimum_players = 1;
    }
    if (zombie_rules.radar_seconds < 1) {
        zombie_rules.radar_seconds = 1;
    }
    if (zombie_rules.claw_damage < 1) {
        zombie_rules.claw_damage = 1;
    }
    if (zombie_rules.spawn_near_min < 1.0f) {
        zombie_rules.spawn_near_min = 1.0f;
    }
    if (zombie_rules.spawn_near_max < zombie_rules.spawn_near_min) {
        zombie_rules.spawn_near_max = zombie_rules.spawn_near_min;
    }
}

static int _zombie_sky_start(server_t* server)
{
    if (!zombie_rules.night) {
        return 0; /* the host asked for the server's own [sky] */
    }
    /* midnight, the clock stopped: the survivors hunt in the dark with
       their lamps and the zombies see by night vision, and a long round
       never turns to day. The weather stays what config.toml says. An
       admin's /sky still changes it; the next map start is night again. */
    sky_set_rate(server, 0.0f, "zombie");
    sky_set_time(server, 0.0f, "zombie");
    return 1;
}

static void _zombie_init(server_t* server)
{
    _zombie_read_rules();
    /* the host's patient zeros (--first-infected, the setup screen) over
       config.toml's */
    if (server->first_infected_flag != 0) {
        zombie_rules.first_infected = server->first_infected_flag == 255 ? 0 : server->first_infected_flag;
        evt_log(NULL, "zombie", "action=first_infected n=%d by=host", zombie_rules.first_infected);
    }
    /* the host's choice for the classes: the console first (/classes), then
       the command line (--classes 0|1), then config.toml's [zombie] classes */
    int classes = zombie_rules.classes;
    if (server->classes_flag == 1) {
        classes = 1;
    } else if (server->classes_flag == 2) {
        classes = 0;
    }
    if (zombie_classes_host >= 0) {
        classes = zombie_classes_host;
    }
    zombie_rules.classes = classes;
    /* the survivors pick a class (the class screen): the mode's own list has
       no classes, the host's choice turns them on for this map. The classes
       own the perks then (like ngCTF): every class earns its own streak perk
       and the Rocketeer wears the jetpack, so the mode's jetpack (two kills)
       would be a second progression on top of it. */
    if (zombie_rules.classes) {
        server->features_on |= GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS;
    }
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    /* the teams are what they are in this mode, whatever config.toml calls
       them: clients draw these names and the zombies' green */
    snprintf(server->protocol.name_team[HUMAN_TEAM], sizeof(server->protocol.name_team[0]), "Humans");
    snprintf(server->protocol.name_team[ZOMBIE_TEAM], sizeof(server->protocol.name_team[0]), "Zombies");
    server->protocol.color_team[ZOMBIE_TEAM].r = 70;
    server->protocol.color_team[ZOMBIE_TEAM].g = 150;
    server->protocol.color_team[ZOMBIE_TEAM].b = 50;
    zombie.round      = 0;
    zombie.winner     = -1;
    memset(&high, 0, sizeof(high)); /* a new map: new sites */
    zombie.match_over = 0;
    server->protocol.gamemode.score_limit = (uint8_t) zombie_rules.rounds;
    _zombie_set_state(ZOMBIE_WAITING);
    char first[32];
    if (zombie_rules.first_infected > 0) {
        snprintf(first, sizeof first, "%d", zombie_rules.first_infected);
    } else {
        snprintf(first, sizeof first, "one per %d players", ZOMBIE_FIRST_PER);
    }
    LOG_INFO("Zombies: %d s countdown, %d s round, +%d s an infection, %u rounds to win, %s first infected%s%s",
             zombie_rules.countdown, zombie_rules.round_seconds, zombie_rules.infection_bonus,
             (unsigned) server->protocol.gamemode.score_limit, first,
             zombie_rules.classes ? ", the survivors pick a class" : "",
             zombie_rules.night ? ", at night" : "");
    evt_log(NULL, "zombie", "action=rules rounds=%d countdown=%d round=%d bonus=%d first=%d min_players=%d radar=%d claw=%d classes=%d night=%d",
            zombie_rules.rounds, zombie_rules.countdown, zombie_rules.round_seconds, zombie_rules.infection_bonus,
            zombie_rules.first_infected, zombie_rules.minimum_players, zombie_rules.radar_seconds,
            zombie_rules.claw_damage, zombie_rules.classes, zombie_rules.night);
}

/* a new round: everyone a survivor, respawned, with the countdown to scatter
   in; patient zero is drawn at the outbreak (_zombie_outbreak) */
static void _zombie_start_round(server_t* server)
{
    int       n = 0;
    player_t *p, *tmp;
    zombie.round++;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_zombie_in_team(p)) {
            _zombie_set_team(server, p, HUMAN_TEAM);
            p->state                = STATE_SPAWNING;
            p->alive                = 0;
            zombie.round_of[p->id] = (uint16_t) zombie.round;
            n++;
        }
    }
    int zombies       = 0;
    zombie.infections = 0;
    zombie.last_id    = -1;
    zombie.survival_at = get_nanos() + NANO_IN_SECOND;
    _zombie_set_state(ZOMBIE_COUNTDOWN_STATE);
    /* the survivors spawn somewhere else every round: new sites */
    memset(&high, 0, sizeof(high));
    evt_log(NULL, "zombie", "action=countdown round=%d players=%d zombies=%d score=%u-%u", zombie.round, n, zombies,
            server->protocol.gamemode.score[0], server->protocol.gamemode.score[1]);
}

static void _zombie_end_round(server_t* server, int winner, const char* why)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    zombie.winner       = winner;
    gm->score[winner]++;
    zombie.match_over = gm->score[winner] >= gm->score_limit;
    _zombie_set_state(ZOMBIE_ENDED);
    evt_log(NULL, "zombie", "action=round_end round=%d winner=%s why=%s score=%u-%u match_over=%u",
            zombie.round, winner == HUMAN_TEAM ? "humans" : "zombies", why, gm->score[0], gm->score[1],
            zombie.match_over);
}

/* the outbreak, and again whenever no zombie is left in the fight (patient
   zero left): living survivors drawn at random turn where they stand,
   first_infected of them, or (0, the default) one for every
   ZOMBIE_FIRST_PER players, at least one. One alone did nothing once the
   survivors held their high ground together: 12 bots, Zombie_Island, the
   zombies won 0.5 rounds a match, 1.75 with two (the humans 2.25 and 0.75) */
static void _zombie_outbreak(server_t* server, const char* why)
{
    player_t* in[256];
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_zombie_in_team(p) && p->team == HUMAN_TEAM && p->alive) {
            in[n++] = p;
        }
    }
    int count = zombie_rules.first_infected > 0 ? zombie_rules.first_infected : n / ZOMBIE_FIRST_PER;
    count     = count < 1 ? 1 : count;
    if (count > n - 1) {
        count = n - 1;
    }
    for (int z = 0; z < count && n - z >= 2; z++) {
        int pick = z + (int) (gen_rand(&server->rand) * (float) (n - z));
        pick     = pick < n ? pick : n - 1;
        player_t* t = in[pick];
        in[pick]    = in[z];
        in[z]       = t;
        _zombie_set_team(server, t, ZOMBIE_TEAM);
        /* turned where it stands: respawned on the spot as a zombie */
        zombie.here[t->id]      = t->movement.position;
        zombie.turn_here[t->id] = 1;
        t->state                = STATE_SPAWNING;
        t->alive                = 0;
        zombie.round_of[t->id] = (uint16_t) zombie.round;
        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "OUTBREAK! %s is infected", t->name);
        evt_log(t, "zombie", "action=patient_zero why=%s survivors=%d", why, n - z - 1);
    }
}

/* the last survivor: told, restocked, and pinged to every zombie's map */
static void _zombie_last_survivor(server_t* server, player_t* last)
{
    uint64_t now = get_nanos();
    if (zombie.last_id != last->id) {
        zombie.last_id  = last->id;
        zombie.radar_at = now;
        broadcast_server_notice(server, 0, "%s is the last survivor", last->name);
        player_restock(server, last);
        evt_log(last, "zombie", "action=last_survivor");
    }
    if (now < zombie.radar_at) {
        return;
    }
    zombie.radar_at = now + (uint64_t) zombie_rules.radar_seconds * NANO_IN_SECOND;
    char        id[8], pos[3][16];
    const char* parts[6] = {"ping", id, "danger", pos[0], pos[1], pos[2]};
    snprintf(id, sizeof id, "%u", (unsigned) last->id);
    snprintf(pos[0], sizeof pos[0], "%.1f", last->movement.position.x);
    snprintf(pos[1], sizeof pos[1], "%.1f", last->movement.position.y);
    snprintf(pos[2], sizeof pos[2], "%.1f", last->movement.position.z + 1.0f);
    player_t *p, *tmp;
    int       sent = 0;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == ZOMBIE_TEAM && _zombie_in_team(p)) {
            proto10_bridge_generic(server, p, 6, parts);
            sent++;
        }
    }
    evt_log(last, "zombie", "action=radar sent=%d", sent);
}

/* too few players to play: no round score, everyone a survivor again */
static void _zombie_abort(server_t* server, const char* why)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_zombie_in_team(p)) {
            _zombie_set_team(server, p, HUMAN_TEAM);
            p->state                = STATE_SPAWNING;
            p->alive                = 0;
            zombie.round_of[p->id]  = (uint16_t) zombie.round;
        }
    }
    zombie.last_id = -1;
    zombie.winner  = -1;
    _zombie_set_state(ZOMBIE_WAITING);
    evt_log(NULL, "zombie", "action=abort why=%s", why);
}

static void _zombie_tick(server_t* server)
{
    int       present = 0, humans = 0, alive[2] = {0, 0};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_zombie_in_team(p)) {
            continue;
        }
        present++;
        humans += !p->is_bot;
        if (p->state == STATE_SPAWNING || (p->state == STATE_READY && p->alive)) {
            alive[p->team]++;
        }
    }
    /* A round must not run before the first human is in it: the listen
       server starts its bots first, and a countdown that began without the
       player made them join during the fight, where the mode puts every
       joiner on the zombie team. Bots alone still play in a benchmark
       (SP_BOTS_PLAY). Not bots_playing(): a spectating host counts there,
       and the round began while the player still watched, so picking the
       survivors afterwards put it on the zombies. */
    int humans_or_benchmark = humans > 0 || bots_play_alone();
    zombie.humans = alive[HUMAN_TEAM];
    float elapsed = (float) (get_nanos() - zombie.since) / (float) NANO_IN_SECOND;
    switch (zombie.state) {
        case ZOMBIE_WAITING:
            if (present >= zombie_rules.minimum_players && humans_or_benchmark) {
                _zombie_start_round(server);
            }
            break;
        case ZOMBIE_COUNTDOWN_STATE:
            if (present < zombie_rules.minimum_players || !humans_or_benchmark) {
                _zombie_set_state(ZOMBIE_WAITING);
            } else if (elapsed >= (float) zombie_rules.countdown) {
                _zombie_set_state(ZOMBIE_FIGHT);
                zombie.deadline = get_nanos() + (uint64_t) zombie_rules.round_seconds * NANO_IN_SECOND;
                _zombie_outbreak(server, "outbreak");
                evt_log(NULL, "zombie", "action=fight round=%d humans=%d", zombie.round, alive[HUMAN_TEAM]);
            }
            break;
        case ZOMBIE_FIGHT: {
            if (present < zombie_rules.minimum_players || !humans_or_benchmark) {
                _zombie_abort(server, "players");
                break;
            }
            /* every 10 s: how many living humans hold a high ground (within
               4 of a site's middle, at its height), and on how many sites */
            static uint64_t gather_at;
            uint64_t        now_g = get_nanos();
            if (high.n > 0 && now_g >= gather_at) {
                gather_at = now_g + (uint64_t) 10 * NANO_IN_SECOND;
                /* each human counted once: the sites of neighbouring chunks
                   overlap */
                int     on = 0, held = 0;
                uint8_t used[HIGH_SITES] = {0};
                HASH_ITER(hh, server->players, p, tmp)
                {
                    if (p->team != HUMAN_TEAM || p->state != STATE_READY || !p->alive) {
                        continue;
                    }
                    for (int i = 0; i < high.n; i++) {
                        high_site_t* st = &high.site[i];
                        if (st->set && fabsf(p->movement.position.x - (st->cx + 0.5f)) <= 4.5f &&
                            fabsf(p->movement.position.y - (st->cy + 0.5f)) <= 4.5f &&
                            fabsf(p->movement.position.z - (float) st->fz) <= 4.0f)
                        {
                            on++;
                            held += !used[i];
                            used[i] = 1;
                            break;
                        }
                    }
                }
                int forts = 0, walls = 0;
                for (int i = 0; i < high.n; i++) {
                    forts += high.site[i].fort_n > 0 && high.site[i].fort_built > 0;
                    walls += high.site[i].fort_built;
                }
                evt_log(NULL, "zombie_high", "action=gather on=%d humans=%d sites=%d held=%d forts=%d walls=%d", on,
                        alive[HUMAN_TEAM], high.n, held, forts, walls);
            }
            /* survival score: one point a second to every living survivor,
               carried in PlayerUpdate.score (the scoreboard shows it) */
            uint64_t now_fight = get_nanos();
            if (now_fight >= zombie.survival_at) {
                zombie.survival_at = now_fight + NANO_IN_SECOND;
                HASH_ITER(hh, server->players, p, tmp)
                {
                    if (p->team == HUMAN_TEAM && p->state == STATE_READY && p->alive && p->score < UINT32_MAX) {
                        p->score++;
                    }
                }
            }
            /* a human who died in the fight turns where it fell: the kill
               action puts it in WAITING_FOR_RESPAWN after player_killed, so
               the next tick moves it on to the spawn at the death cell */
            HASH_ITER(hh, server->players, p, tmp)
            {
                if (p->team == ZOMBIE_TEAM && zombie.turn_here[p->id] && p->state == STATE_WAITING_FOR_RESPAWN) {
                    p->state        = STATE_SPAWNING;
                    p->respawn_time = 0;
                    evt_log(p, "zombie", "action=instant_turn");
                }
            }
            /* a player who joined after the round began is infected */
            int zombies_in = 0;
            HASH_ITER(hh, server->players, p, tmp)
            {
                if (!_zombie_in_team(p)) {
                    continue;
                }
                if (p->team == HUMAN_TEAM && zombie.round_of[p->id] != zombie.round) {
                    _zombie_set_team(server, p, ZOMBIE_TEAM);
                    zombie.round_of[p->id] = (uint16_t) zombie.round;
                    evt_log(p, "zombie", "action=late_join");
                }
                zombies_in += p->team == ZOMBIE_TEAM;
            }
            if (alive[HUMAN_TEAM] == 0) {
                _zombie_end_round(server, ZOMBIE_TEAM, "all_infected");
            } else if (get_nanos() >= zombie.deadline) {
                _zombie_end_round(server, HUMAN_TEAM, "survived");
            } else if (zombies_in == 0) {
                _zombie_outbreak(server, "no_zombie_left");
            } else if (alive[HUMAN_TEAM] == 1) {
                HASH_ITER(hh, server->players, p, tmp)
                {
                    if (_zombie_in_team(p) && p->team == HUMAN_TEAM && p->alive && p->state == STATE_READY) {
                        _zombie_last_survivor(server, p);
                    }
                }
            }
            break;
        }
        case ZOMBIE_ENDED:
            if (elapsed < (float) zombie_rules.end_seconds) {
                break;
            }
            if (zombie.match_over) {
                evt_log(NULL, "zombie", "action=match_end score=%u-%u", server->protocol.gamemode.score[0],
                        server->protocol.gamemode.score[1]);
                gamemode_match_won(server);
            } else if (present >= zombie_rules.minimum_players) {
                _zombie_start_round(server);
            } else {
                _zombie_set_state(ZOMBIE_WAITING);
            }
            break;
    }
}

/* a human who dies in the fight comes back a zombie */
static void _zombie_player_killed(server_t* server, player_t* killer, player_t* victim)
{
    if (zombie.state != ZOMBIE_FIGHT || victim->team != HUMAN_TEAM) {
        return;
    }
    _zombie_set_team(server, victim, ZOMBIE_TEAM);
    /* it turns where it fell: the respawn is the body's cell, not the
       zombie spawn ring (the kill is applied a moment after this hook) */
    zombie.here[victim->id]      = victim->movement.position;
    zombie.turn_here[victim->id] = 1;
    /* every infection buys the survivors time (MW 2019), but the round
       never grows without bound: a last survivor should not be chased for
       minutes on end. Cap the whole fight to round_seconds plus a handful of
       extensions, so a crowded match still ends in a few minutes. */
    zombie.infections++;
    zombie.deadline += (uint64_t) zombie_rules.infection_bonus * NANO_IN_SECOND;
    uint64_t max_deadline = zombie.since +
                            (uint64_t) (zombie_rules.round_seconds + 5 * zombie_rules.infection_bonus) * NANO_IN_SECOND;
    if (zombie.deadline > max_deadline) {
        zombie.deadline = max_deadline;
    }
    evt_log(victim, "zombie", "action=infect by=%u humans_left=%d infections=%d", killer != NULL ? killer->id : 255,
            zombie.humans > 0 ? zombie.humans - 1 : 0, zombie.infections);
}

static unsigned _zombie_player_rules(server_t* server, player_t* player)
{
    (void) server;
    return player->team == ZOMBIE_TEAM
           ? GM_RULE_NO_GUN | GM_RULE_NO_GRENADE | GM_RULE_NO_BLOCK | GM_RULE_HIGH_JUMP | GM_RULE_NO_FALL |
                 GM_RULE_FAST | GM_RULE_NO_PERKS | GM_RULE_NO_LIGHT | GM_RULE_NIGHT_VISION
           : 0;
}

/* a zombie's claw does the configured damage and slows the zombie, longer
   for each hit in a row (its client does the same on the hit's confirmation); a
   zombie has GAME_ZOMBIE_HEALTH: every hit on one is scaled down (rounded
   up, so a pellet still hurts), headshots too, which no longer kill it at
   once */
static long _zombie_hit_damage(server_t* server, player_t* attacker, player_t* victim, long damage, uint8_t kill_reason)
{
    long result = damage;
    if (attacker != NULL && attacker->team == ZOMBIE_TEAM && victim->team == HUMAN_TEAM && kill_reason == 2) {
        uint64_t now   = get_nanos();
        uint64_t from  = attacker->zombie_slow_until > now ? attacker->zombie_slow_until : now;
        uint64_t until = from + (uint64_t) (GAME_ZOMBIE_CLAW_SLOW_SECONDS * NANO_IN_SECOND);
        uint64_t cap   = now + (uint64_t) (GAME_ZOMBIE_CLAW_SLOW_MAX * NANO_IN_SECOND);
        attacker->zombie_slow_until = until < cap ? until : cap;
        evt_log(attacker, "zombie", "action=claw victim=%u damage=%d slowed_ms=%llu", victim->id, zombie_rules.claw_damage,
                (unsigned long long) ((attacker->zombie_slow_until - now) / 1000000));
        result = zombie_rules.claw_damage;
    } else if (victim->team == ZOMBIE_TEAM && damage > 0) {
        /* a hit slows it down for a moment (its client does the same when
           the Damage arrives, so its prediction agrees) */
        if (attacker != NULL && attacker != victim) {
            victim->zombie_slow_until = get_nanos() + (uint64_t) (GAME_ZOMBIE_SLOW_SECONDS * NANO_IN_SECOND);
        }
        result = (damage * 100 + GAME_ZOMBIE_HEALTH - 1) / GAME_ZOMBIE_HEALTH;
    }
    /* a survivor's class scales the hit it takes (a zombie has no class:
       its own health scaling above is the one) */
    return classes_hit_damage(server, attacker, victim, result, kill_reason);
}

/* the survivors pick a class when they enter the game (the class screen);
   a zombie gets none: it stays a zombie */
static void _zombie_client_in_game(server_t* server, player_t* player)
{
    classes_client_in_game(server, player);
}

/* the class screen's requests ("class 2", the drill, the second gun) */
static int _zombie_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    return classes_command(server, player, command, arg);
}

/* the host's choice for the classes (the console's /classes): the survivors
   pick a class at the class screen, the zombies stay zombies */
static int _zombie_set_classes(server_t* server, int on)
{
    zombie_classes_host = on ? 1 : 0;
    zombie_rules.classes = zombie_classes_host;
    if (zombie_rules.classes) {
        server->features_on |= GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS;
    } else {
        server->features_on &= ~(GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS);
    }
    /* the clients in the game: the class screen appears (or goes) at once */
    classes_tell_all(server, zombie_rules.classes);
    LOG_INFO("Zombies: the classes are %s", zombie_rules.classes ? "on" : "off");
    evt_log(NULL, "zombie", "action=classes on=%d", zombie_rules.classes);
    return 1;
}

/* a zombie spawns with its hands out (the spade's slot); the spawn point is
   the team's, as usual */
/* dry ground within r blocks of x, y: 1 and the position set */
static int _zombie_ground_near(server_t* server, player_t* player, float x, float y, float rmin, float rmax)
{
    map_store_t* map = &server->s_map.map;
    for (int attempt = 0; attempt < 16; attempt++) {
        float a  = gen_rand(&server->rand) * 6.2831853f;
        float r  = rmin + gen_rand(&server->rand) * (rmax - rmin);
        int   cx = (int) (x + cosf(a) * r), cy = (int) (y + sinf(a) * r);
        if (cx < 0 || cy < 0 || cx >= map->size_x || cy >= map->size_y) {
            continue;
        }
        int top = map_top(map, (uint16_t) cx, (uint16_t) cy);
        if (top >= map->size_z - 2 || top < 4) {
            continue;
        }
        player->movement.position.x = cx + 0.5f;
        player->movement.position.y = cy + 0.5f;
        player->movement.position.z = (float) (top - 1) - 2.4f;
        return 1;
    }
    return 0;
}

/* dry ground anywhere in the play area (inside a boundary_damage box): 1 and
   the position set */
static int _zombie_ground_anywhere(server_t* server, player_t* player)
{
    map_store_t* map = &server->s_map.map;
    int       x0, y0, x1, y1;
    map_meta_play_area(server, &x0, &y0, &x1, &y1);
    for (int attempt = 0; attempt < 64; attempt++) {
        int cx = x0 + (int) (gen_rand(&server->rand) * (float) (x1 - x0 + 1));
        int cy = y0 + (int) (gen_rand(&server->rand) * (float) (y1 - y0 + 1));
        if (cx < x0 || cy < y0 || cx > x1 || cy > y1) {
            continue;
        }
        int top = map_top(map, (uint16_t) cx, (uint16_t) cy);
        if (top >= map->size_z - 2 || top < 4) {
            continue;
        }
        player->movement.position.x = cx + 0.5f;
        player->movement.position.y = cy + 0.5f;
        player->movement.position.z = (float) (top - 1) - 2.4f;
        evt_log(player, "zombie", "action=scatter at=%d,%d,%d tries=%d", cx, cy, top, attempt + 1);
        return 1;
    }
    return 0;
}

static int _zombie_spawn_point(server_t* server, player_t* player)
{
    if (player->team != ZOMBIE_TEAM) {
        /* survivors spawn anywhere on the map: round their spawn they all
           started in one crowd, where patient zero turned inside a survivor
           it could not aim at and infected the rest in seconds. Next to
           their spawn only when no dry cell was drawn */
        high.of[player->id] = 0;
        if (_zombie_ground_anywhere(server, player)) {
            return 1;
        }
        quad3d_t* s = &server->protocol.spawns[HUMAN_TEAM];
        return _zombie_ground_near(server, player, (s->from.x + s->to.x) * 0.5f, (s->from.y + s->to.y) * 0.5f, 1.0f,
                                   10.0f);
    }
    player->item = TOOL_SPADE;
    if (zombie.turn_here[player->id]) {
        zombie.turn_here[player->id] = 0;
        player->movement.position    = zombie.here[player->id];
        return 1;
    }
    if (zombie.state != ZOMBIE_FIGHT) {
        return 0;
    }
    /* in the fight a zombie comes back between spawn_near_min and
       spawn_near_max blocks from a living human drawn at random, on dry ground: from its own spawn on
       de_dust it ran ~220 blocks of open ground under the humans' guns (70
       zombie deaths, 5 infections, the humans won every round), and the
       survivors are scattered over the whole map now */
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        n += p->team == HUMAN_TEAM && p->alive && p->state == STATE_READY;
    }
    if (n == 0) {
        return 0;
    }
    int pick = (int) (gen_rand(&server->rand) * (float) n);
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == HUMAN_TEAM && p->alive && p->state == STATE_READY && pick-- <= 0) {
            return _zombie_ground_near(server, player, p->movement.position.x, p->movement.position.y,
                                       zombie_rules.spawn_near_min, zombie_rules.spawn_near_max);
        }
    }
    return 0;
}

static int _zombie_seconds_left(void)
{
    uint64_t now = get_nanos();
    if (zombie.state == ZOMBIE_FIGHT) {
        return zombie.deadline > now ? (int) ((zombie.deadline - now) / NANO_IN_SECOND) : 0;
    }
    int left = zombie.state == ZOMBIE_COUNTDOWN_STATE ? zombie_rules.countdown - (int) ((now - zombie.since) / NANO_IN_SECOND) : 0;
    return left > 0 ? left : 0;
}

static void _zombie_state_keys(server_t* server, gm_keys_t* out)
{
    (void) server;
    gm_keys_put(out, P10_GM_ARENA_STATE, "%s", zombie_state_names[zombie.state]);
    gm_keys_put(out, P10_GM_ARENA_ROUND, "%d", zombie.round);
    gm_keys_put(out, P10_GM_ARENA_TIME, "%d", _zombie_seconds_left());
    gm_keys_put(out, P10_GM_ARENA_WINNER, "%s",
                zombie.winner == TEAM_A ? "team1" : zombie.winner == TEAM_B ? "team2" : "");
    gm_keys_put(out, P10_GM_ZOMBIE_TEAM, "%d", ZOMBIE_TEAM == TEAM_A ? 1 : 2);
    gm_keys_put(out, P10_GM_ZOMBIE_HUMANS, "%d", zombie.humans);
}

/* the zombie mode plays the maps made for it, those with "zombie" in their
   name (aos.party's Beaulieu_Abbey_Zombie, Zombie_Island), the ones its
   catalog entry picked for it (common/modes/modes.h: towns whose houses the
   survivors hold) and any map whose toml is tagged for it ([map] modes, the
   maps repository's tag: a player's own map). Any other map is dropped from
   its rotation */
static int _zombie_map_fits(const char* map_name)
{
    for (const char* c = map_name; *c; c++) {
        if (strncasecmp(c, "zombie", 6) == 0) {
            return 1;
        }
    }
    const struct game_mode_info* mode = game_mode_find("zombie");
    for (int k = 0; k < game_mode_map_count(mode); k++) {
        if (strcmp(mode->maps[k], map_name) == 0) {
            return 1;
        }
    }
    return gamemode_map_tagged(map_name, "zombie");
}

/* a spectator joins the zombies in the fight and the survivors outside it;
   a player in a team may leave to the spectators but not cross over. This
   closes the "change team and back" hole; the client's menu row is hidden
   later, the server refuses the request now. */
static int _zombie_join_team(server_t* server, player_t* player, uint8_t requested, uint8_t* out)
{
    (void) server;
    (void) player;
    if (requested == TEAM_SPECTATOR) {
        *out = TEAM_SPECTATOR;
    } else {
        *out = zombie.state == ZOMBIE_FIGHT ? ZOMBIE_TEAM : HUMAN_TEAM;
    }
    return 1;
}

static int _zombie_team_change(server_t* server, player_t* player, uint8_t requested, uint8_t* out)
{
    if (player->team == TEAM_SPECTATOR) {
        return _zombie_join_team(server, player, requested, out);
    }
    if (requested == TEAM_SPECTATOR) {
        *out = TEAM_SPECTATOR;
        return 1;
    }
    *out = player->team;
    return 1;
}

/* a human survivor is the player the escape watch may mark when sealed in */
static int _zombie_escape_watch_player(server_t* server, player_t* player)
{
    (void) server;
    return player->team == HUMAN_TEAM;
}

/* the humans' perk: the jetpack after two kills (zombies earn none,
   GM_RULE_NO_PERKS); no sentry, no MG */
static const gm_perk_rule_t zombie_perks[P10_PERK_COUNT] = {
[P10_PERK_JETPACK] = {.enabled = 1, .points = 0, .kill = 0},
};

/* crates for the survivors (with or without the classes): a zombie
   killed by a human drops one a fifth of the time, 12 to 24 blocks away
   (the killer has to go and get it, out of its hold), at most one every
   20 s and 3 lying about: help, not a supply line (none at the hot
   spots: the zombies' victims die round the humans' holds) */
/* windows.h makes near and far empty macros (16 bit pointers): the
   designators below came out as ". = 12.0f" in a Windows unity batch */
#ifdef _WIN32
#undef near
#undef far
#endif
static const gm_crate_rule_t zombie_crates = {
.chance = 0.2f, .crowd = 1, .team = HUMAN_TEAM, .near_dist = 12.0f, .far_dist = 24.0f, .cooldown = 20, .max_out = 3};

/* a zombie faces the nearest survivor (it comes back near one); a survivor
   scattered over the map faces the nearest zombie, or the old way when
   there is none yet */
static int _zombie_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    float     best = 1e30f;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == player || p->team == player->team || (p->team != TEAM_A && p->team != TEAM_B) || !p->alive ||
            p->state != STATE_READY)
        {
            continue;
        }
        float dx = p->movement.position.x - player->movement.position.x;
        float dy = p->movement.position.y - player->movement.position.y;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            *x   = p->movement.position.x;
            *y   = p->movement.position.y;
        }
    }
    return best < 1e30f;
}

const gamemode_ops_t zombie_ops = {
.id            = GAME_MODE_ZOMBIE,
.name          = "zombie",
.wire_name     = "zombie",
/* the survivors' squads (names and map); a zombie leaves its squad */
.squad_teams   = 1u << HUMAN_TEAM,
.perks         = 1,
.perk_rules    = zombie_perks,
.crate_rule    = &zombie_crates,
.init          = _zombie_init,
.sky_start     = _zombie_sky_start,
.tick          = _zombie_tick,
.state_keys    = _zombie_state_keys,
.spawn_point   = _zombie_spawn_point,
.spawn_facing  = _zombie_spawn_facing,
.join_team     = _zombie_join_team,
.team_change   = _zombie_team_change,
.escape_watch_player = _zombie_escape_watch_player,
.player_rules  = _zombie_player_rules,
.hit_damage    = _zombie_hit_damage,
.player_killed = _zombie_player_killed,
.map_fits      = _zombie_map_fits,
.bot_goal      = _zombie_bot_goal,
    .client_in_game = _zombie_client_in_game,
    .player_command = _zombie_command,
    .set_classes   = _zombie_set_classes,
};
