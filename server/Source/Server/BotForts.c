// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Cooperative fortifications for the bots: castle rings, bunkers and
 * watchtowers raised by a team where its objective is. The shapes are
 * informed by the prefabs and the schematic library of BattleSpades
 * (AGPL-3.0): its fort_wall, bunker, watchtower and objective_ring showed
 * what a team can build together, and its bounded projects showed how to
 * share the work. This file is an independent implementation for this
 * server: no code, no asset and no cell list is taken from it. Every cell
 * goes through the ordinary block action (receive_block_action), so the
 * server's own rate, reach, support and protected-cell rules apply.
 *
 * Why the cell ordering and the stands matter: a block needs a solid
 * neighbour and the builder must be within four blocks of it, so a cell is
 * only claimable when a bot can stand where it reaches it without being
 * inside the structure. The stand search is run again every think: the
 * upper cells (a tower's parapet, a bunker's roof) become reachable only
 * once the structure below them exists.
 */

#include <Server/BotForts.h>
#include <Server/BotsInternal.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/MapStore.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Util/Nanos.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FORT_BUILD_SECONDS 90
#define FORT_CLAIM_SECONDS 8

typedef struct
{
    int x, y, z;
} fort_cell_t;

typedef struct
{
    uint8_t     used;
    uint8_t     kind;
    uint8_t     team;
    uint8_t     complete;
    uint8_t     count;
    uint8_t     built; /* how many cells are solid, for the status line */
    uint16_t    id;
    int         anchor[3];
    int         fx, fy; /* toward the threat */
    int         rx, ry; /* across: (-fy, fx) */
    fort_cell_t cells[BOT_FORT_CELLS];
    uint8_t     builders[BOT_FORT_BUILDERS];
    int8_t      claim[BOT_FORT_BUILDERS];
    uint64_t    claim_at[BOT_FORT_BUILDERS];
    uint64_t    expires;
    uint64_t    next_log;
} bot_fort_t;

static bot_fort_t forts[BOT_FORTS];
static uint16_t   forts_next_id = 1;
static uint8_t    forts_enabled = 1;

static const char* _fort_kind_name(uint8_t kind)
{
    return kind == BOT_FORT_RING ? "ring" : kind == BOT_FORT_BUNKER ? "bunker" : "tower";
}

void bot_forts_init(server_t* server)
{
    (void) server;
    forts_enabled = 1;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "bots") : NULL;
        toml_datum_t  d = t != NULL ? toml_bool_in(t, "cooperative_forts") : (toml_datum_t) {0};
        if (d.ok) {
            forts_enabled = d.u.b ? 1 : 0;
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_BOT_FORTS");
    if (env != NULL && *env != '\0') {
        forts_enabled = atoi(env) != 0;
    }
}

void bot_forts_reset(void)
{
    memset(forts, 0, sizeof forts);
    forts_next_id = 1;
}

/* a column's useful ground: -1 for open water, the map's edge or a column
   with no room to build over it */
static int _fort_ground(server_t* server, int x, int y)
{
    map_store_t* map = &server->s_map.map;
    if (x < 2 || y < 2 || x >= (int) map->size_x - 2 || y >= (int) map->size_y - 2) {
        return -1;
    }
    int top = map_top(map, (uint16_t) x, (uint16_t) y);
    if (top < 4 || top >= (int) map->size_z - 2) {
        return -1;
    }
    return top;
}

static uint8_t _fort_add(bot_fort_t* f, int x, int y, int z);

/* The base a bunker or a tower stands on: the highest ground of its local
   rectangle (z grows downwards, so the smallest top). A slope of up to two
   cells is levelled with foundation cells under the lower columns; more
   than that is no site for a straight structure. A ring follows each of its
   own columns instead. */
static int _fort_base(server_t* server, const bot_fort_t* f, int u0, int u1, int v0, int v1)
{
    int base = INT_MAX, low = 0;
    for (int u = u0; u <= u1; u++) {
        for (int v = v0; v <= v1; v++) {
            int x = f->anchor[0] + u * f->rx + v * f->fx;
            int y = f->anchor[1] + u * f->ry + v * f->fy;
            int top = _fort_ground(server, x, y);
            if (top < 0) {
                return -1;
            }
            if (top < base) {
                base = top;
            }
            if (top > low) {
                low = top;
            }
        }
    }
    return low - base > 2 ? -1 : base;
}

/* fill the column from the base down to its own ground: the plinth a wall
   on a slope stands on */
static uint8_t _fort_foundations(server_t* server, bot_fort_t* f, int u, int v, int base)
{
    int x = f->anchor[0] + u * f->rx + v * f->fx;
    int y = f->anchor[1] + u * f->ry + v * f->fy;
    int top = _fort_ground(server, x, y);
    if (top < 0) {
        return 0;
    }
    for (int z = top - 1; z >= base; z--) {
        if (!_fort_add(f, x, y, z)) {
            return 0;
        }
    }
    return 1;
}

static uint8_t _fort_add(bot_fort_t* f, int x, int y, int z)
{
    if (f->count >= BOT_FORT_CELLS || z < 0) {
        return 0;
    }
    for (int k = 0; k < f->count; k++) {
        if (f->cells[k].x == x && f->cells[k].y == y && f->cells[k].z == z) {
            return 1;
        }
    }
    f->cells[f->count].x = x;
    f->cells[f->count].y = y;
    f->cells[f->count].z = z;
    f->count++;
    return 1;
}

static uint8_t _fort_add_local(bot_fort_t* f, int u, int v, int w, int base)
{
    int x = f->anchor[0] + u * f->rx + v * f->fx;
    int y = f->anchor[1] + u * f->ry + v * f->fy;
    return _fort_add(f, x, y, base - 1 - w);
}

/* A ring of radius four around the site, one cell high, two gates facing
   along the threat line, and a battlement post on each diagonal. The cells
   follow each column's own ground: a territory on a slope keeps its wall on
   the slope instead of floating. */
static uint8_t _fort_shape_ring(server_t* server, bot_fort_t* f)
{
    for (int w = 0; w <= 1; w++) {
        for (int du = -4; du <= 4; du++) {
            for (int dv = -4; dv <= 4; dv++) {
                int r2 = du * du + dv * dv;
                if (r2 < 15 || r2 > 20) {
                    continue; /* 3.8 < r <= 4.5: 24 columns, less a gate each side */
                }
                if (abs(du) <= 1 && (dv == 4 || dv == -4)) {
                    continue; /* the two gates, three cells wide */
                }
                uint8_t battlement = (uint8_t) (abs(du) == 3 && abs(dv) == 3);
                if (w == 1 && !battlement) {
                    continue;
                }
                int x = f->anchor[0] + du * f->rx + dv * f->fx;
                int y = f->anchor[1] + du * f->ry + dv * f->fy;
                int top = _fort_ground(server, x, y);
                if (top < 0 || abs(top - f->anchor[2]) > 4) {
                    continue; /* a column of water or a cliff: the ring keeps its gap */
                }
                if (!_fort_add(f, x, y, top - 1 - w)) {
                    return 0;
                }
            }
        }
    }
    return f->count >= 16;
}

/* Three by three: a one-high firing wall at the front, two-high sides, a
   roof over the back two rows and a two-high door at the back. Bots inside
   shoot over the front wall and are covered from above. */
static uint8_t _fort_shape_bunker(server_t* server, bot_fort_t* f)
{
    int base = _fort_base(server, f, -1, 1, -1, 1);
    if (base < 0) {
        return 0;
    }
    for (int u = -1; u <= 1; u++) {
        for (int v = -1; v <= 1; v++) {
            if (!_fort_foundations(server, f, u, v, base)) {
                return 0;
            }
        }
    }
    for (int u = -1; u <= 1; u++) {
        if (!_fort_add_local(f, u, 1, 0, base)) {
            return 0; /* the front wall */
        }
    }
    for (int side = -1; side <= 1; side += 2) {
        if (!_fort_add_local(f, side, 1, 1, base)) {
            return 0; /* the front corners, two high */
        }
        for (int v = 0; v >= -1; v--) {
            for (int w = 0; w <= 1; w++) {
                if (!_fort_add_local(f, side, v, w, base)) {
                    return 0; /* the sides */
                }
            }
        }
    }
    for (int v = 0; v >= -1; v--) {
        for (int u = -1; u <= 1; u++) {
            if (!_fort_add_local(f, u, v, 2, base)) {
                return 0; /* the roof (the back door keeps its headroom) */
            }
        }
    }
    return f->count > 0;
}

/* Two by two and three high, a one-block stair up one side and a parapet on
   the threat side: seventeen cells plus the plinth of a slope. Three by
   three and three high was too slow on a hill that is fought over (a team
   lost and retook it before the parapet went up); this one is a proper
   lookout a step over the ring and still finishes. The stair gives the bots
   a way onto the top. */
static uint8_t _fort_shape_tower(server_t* server, bot_fort_t* f)
{
    int base = _fort_base(server, f, -1, 0, 0, 1);
    if (base < 0) {
        return 0;
    }
    for (int v = 0; v <= 1; v++) {
        for (int u = -1; u <= 0; u++) {
            if (!_fort_foundations(server, f, u, v, base)) {
                return 0;
            }
        }
    }
    for (int w = 0; w <= 2; w++) {
        for (int v = 0; v <= 1; v++) {
            for (int u = -1; u <= 0; u++) {
                if (!_fort_add_local(f, u, v, w, base)) {
                    return 0;
                }
            }
        }
    }
    for (int u = -1; u <= 0; u++) {
        if (!_fort_add_local(f, u, 1, 3, base)) {
            return 0; /* the parapet on the threat side */
        }
    }
    /* three steps, one block up each, ending level with the top. A side
       whose three columns are level with the base is needed, or the first
       step stands on a slope and cannot be climbed from the ground; the
       back is tried first, then the sides, never the parapet's front. */
    static const int stairs[3][3][2] = {
        {{1, -1}, {0, -1}, {-1, -1}}, /* back: up the side away from the threat */
        {{1, 2}, {1, 1}, {1, 0}},     /* right: up the outside, ending on the top */
        {{-2, 2}, {-2, 1}, {-2, 0}},  /* left */
    };
    for (int k = 0; k < 3; k++) {
        int x0 = f->anchor[0] + stairs[k][0][0] * f->rx + stairs[k][0][1] * f->fx;
        int y0 = f->anchor[1] + stairs[k][0][0] * f->ry + stairs[k][0][1] * f->fy;
        int x1 = f->anchor[0] + stairs[k][1][0] * f->rx + stairs[k][1][1] * f->fx;
        int y1 = f->anchor[1] + stairs[k][1][0] * f->ry + stairs[k][1][1] * f->fy;
        int x2 = f->anchor[0] + stairs[k][2][0] * f->rx + stairs[k][2][1] * f->fx;
        int y2 = f->anchor[1] + stairs[k][2][0] * f->ry + stairs[k][2][1] * f->fy;
        if (_fort_ground(server, x0, y0) != base || _fort_ground(server, x1, y1) != base ||
            _fort_ground(server, x2, y2) != base)
        {
            continue;
        }
        if (!_fort_add_local(f, stairs[k][0][0], stairs[k][0][1], 0, base) ||
            !_fort_add_local(f, stairs[k][1][0], stairs[k][1][1], 1, base) ||
            !_fort_add_local(f, stairs[k][2][0], stairs[k][2][1], 2, base))
        {
            return 0;
        }
        return f->count > 0;
    }
    return 0;
}

static int _fort_cell_at(const bot_fort_t* f, int x, int y, int z)
{
    for (int k = 0; k < f->count; k++) {
        if (f->cells[k].x == x && f->cells[k].y == y && f->cells[k].z == z) {
            return k;
        }
    }
    return -1;
}

/* Is (x, y, z) a cell the structure will fill? A stand may not be inside
   one: the roof or the next wall block would grow through the bot. */
static uint8_t _fort_planned(const bot_fort_t* f, int x, int y, int z)
{
    return _fort_cell_at(f, x, y, z) >= 0;
}

/* Where a bot stands to reach the cell: a column within three of it, a
   solid floor at most four levels under it, two free body cells, no planned
   cell in the body, and the cell within the block action's reach (four
   blocks from the hip). Null when there is none yet. */
static uint8_t _fort_stand(server_t* server, const bot_fort_t* f, const fort_cell_t* c, int* ox, int* oy, int* ofloor)
{
    map_store_t* map  = &server->s_map.map;
    float        best = 1e9f;
    int          bx = 0, by = 0, bf = 0;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            if (dx * dx + dy * dy > 9) {
                continue;
            }
            int sx = c->x + dx, sy = c->y + dy;
            for (int zf = c->z + 1; zf <= c->z + 4; zf++) {
                if (zf < 0 || zf >= (int) map->size_z - 2) {
                    continue;
                }
                if (!map_solid(map, (uint16_t) sx, (uint16_t) sy, (uint16_t) zf)) {
                    continue;
                }
                int b1 = zf - 1, b2 = zf - 2;
                if (b1 < 0 || b2 < 0 || map_solid(map, (uint16_t) sx, (uint16_t) sy, (uint16_t) b1) ||
                    map_solid(map, (uint16_t) sx, (uint16_t) sy, (uint16_t) b2) || _fort_planned(f, sx, sy, b1) ||
                    _fort_planned(f, sx, sy, b2))
                {
                    continue;
                }
                float ddx = (float) (sx + 0.5f) - ((float) c->x + 0.5f);
                float ddy = (float) (sy + 0.5f) - ((float) c->y + 0.5f);
                float ddz = ((float) zf - 2.4f) - ((float) c->z + 0.5f);
                float d2  = ddx * ddx + ddy * ddy + ddz * ddz;
                if (d2 > 3.8f * 3.8f) {
                    continue;
                }
                /* the floor under the block is the natural one: prefer it,
                   and prefer a stand outside the structure: a stand in a
                   ring or a bunker has to be walked into through a gate,
                   and builders read stuck at the wall's foot */
                float score = d2 + (float) (zf - (c->z + 1)) * 2.0f;
                float ax = (float) f->anchor[0] + 0.5f, ay = (float) f->anchor[1] + 0.5f;
                float px = (float) sx + 0.5f, py = (float) sy + 0.5f;
                int   inside;
                if (f->kind == BOT_FORT_RING) {
                    inside = (px - ax) * (px - ax) + (py - ay) * (py - ay) < 3.5f * 3.5f;
                } else {
                    float lu = (px - ax) * f->rx + (py - ay) * f->ry;
                    float lv = (px - ax) * f->fx + (py - ay) * f->fy;
                    inside   = fabsf(lu) <= 1.0f && fabsf(lv) <= 1.0f;
                }
                if (inside) {
                    score += 20.0f;
                }
                if (score < best) {
                    best   = score;
                    bx     = sx;
                    by     = sy;
                    bf     = zf;
                }
            }
        }
    }
    if (best >= 1e9f) {
        return 0;
    }
    *ox     = bx;
    *oy     = by;
    *ofloor = bf;
    return 1;
}

/* the live fort of a kind at a site, whatever team raised it (cover is
   shared) */
static int _fort_at(uint8_t kind, int x, int y, int radius)
{
    for (int k = 0; k < BOT_FORTS; k++) {
        bot_fort_t* f = &forts[k];
        if (!f->used || f->kind != kind) {
            continue;
        }
        int dx = f->anchor[0] - x, dy = f->anchor[1] - y;
        if (dx * dx + dy * dy <= radius * radius) {
            return k;
        }
    }
    return -1;
}

uint8_t bot_fort_exists(uint8_t kind, int x, int y, int radius)
{
    return _fort_at(kind, x, y, radius) >= 0;
}

uint8_t bot_fort_complete(uint8_t kind, int x, int y, int radius)
{
    int k = _fort_at(kind, x, y, radius);
    return k >= 0 && forts[k].complete;
}

uint8_t bot_fort_top(uint8_t kind, int x, int y, int radius, float* out_x, float* out_y, int* out_floor)
{
    int k = _fort_at(kind, x, y, radius);
    if (k < 0 || !forts[k].complete) {
        return 0;
    }
    bot_fort_t* f  = &forts[k];
    int         bz = INT_MAX;
    for (int i = 0; i < f->count; i++) {
        if (f->cells[i].x == f->anchor[0] && f->cells[i].y == f->anchor[1] && f->cells[i].z < bz) {
            bz = f->cells[i].z;
        }
    }
    if (bz == INT_MAX) {
        return 0;
    }
    *out_x     = (float) f->anchor[0] + 0.5f;
    *out_y     = (float) f->anchor[1] + 0.5f;
    *out_floor = bz;
    return 1;
}

void bot_fort_discard(uint8_t team, int x, int y, int radius)
{
    for (int k = 0; k < BOT_FORTS; k++) {
        bot_fort_t* f = &forts[k];
        if (!f->used || f->team != team) {
            continue;
        }
        int dx = f->anchor[0] - x, dy = f->anchor[1] - y;
        if (dx * dx + dy * dy > radius * radius) {
            continue;
        }
        evt_log(NULL, "bot_fort", "action=discard id=%u kind=%s at=%d,%d built=%u/%u", f->id,
                _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], f->built, f->count);
        memset(f, 0, sizeof *f);
    }
}

/* --- builders --- */

static int _fort_slot_of(uint8_t id)
{
    for (int k = 0; k < BOT_FORTS; k++) {
        if (!forts[k].used) {
            continue;
        }
        for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
            if (forts[k].builders[b] == id) {
                return k;
            }
        }
    }
    return -1;
}

static int _fort_builder_of(const bot_fort_t* f, uint8_t id)
{
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        if (f->builders[b] == id) {
            return b;
        }
    }
    return -1;
}

/* a bot a fort may take: same team, playing, alive, with blocks, not
   already on a structure or a bridge */
static uint8_t _fort_free_bot(const player_t* p, uint8_t team, uint8_t id)
{
    const bot_ai_t* ai = _bot_ai(id);
    return p->team == team && p->state == STATE_READY && p->alive && ai != NULL && !ai->dummy && p->blocks > 0 &&
           _fort_slot_of(id) < 0 && !bot_project_busy(id);
}

/* fill the structure's empty builder slots with the nearest free bots: a
   respawned mate takes the place of one that left, so the work goes on */
static void _fort_fill_builders(server_t* server, bot_fort_t* f)
{
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        if (f->builders[b] == 0xFF) {
            continue;
        }
        player_t* p = NULL;
        HASH_FIND(hh, server->players, &f->builders[b], sizeof(f->builders[b]), p);
        if (p != NULL && p->state != STATE_DISCONNECTED && p->team == f->team && p->blocks > 0) {
            continue; /* dead for a moment: it respawns and comes back */
        }
        evt_log(NULL, "bot_fort", "action=release id=%u slot=%d player=%u", f->id, b, f->builders[b]);
        f->builders[b] = 0xFF;
        f->claim[b]    = -1;
    }
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        if (f->builders[b] != 0xFF) {
            continue;
        }
        player_t *p, *tmp;
        float     best = BOT_FORT_JOIN * BOT_FORT_JOIN;
        uint8_t   pick = 0xFF;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (!_fort_free_bot(p, f->team, p->id)) {
                continue;
            }
            float dx = p->movement.position.x - ((float) f->anchor[0] + 0.5f);
            float dy = p->movement.position.y - ((float) f->anchor[1] + 0.5f);
            float d  = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                pick = p->id;
            }
        }
        if (pick == 0xFF) {
            break;
        }
        f->builders[b] = pick;
        f->claim[b]    = -1;
        player_t* p2   = NULL;
        HASH_FIND(hh, server->players, &pick, sizeof(pick), p2);
        evt_log(p2, "bot_fort", "action=assign id=%u kind=%s slot=%d at=%d,%d", f->id, _fort_kind_name(f->kind), b,
                f->anchor[0], f->anchor[1]);
    }
}

uint8_t bot_fort_builder(const player_t* bot)
{
    if (!forts_enabled || bot == NULL) {
        return 0;
    }
    int k = _fort_slot_of(bot->id);
    return k >= 0 && !forts[k].complete && bot->blocks > 0;
}

/* --- asking for a structure --- */

static int _fort_free_slot(void)
{
    for (int k = 0; k < BOT_FORTS; k++) {
        if (!forts[k].used) {
            return k;
        }
    }
    /* all taken: the oldest complete structure gives its place up (its cells
       stay standing; a mode that still wants it asks again) */
    int oldest = -1;
    for (int k = 0; k < BOT_FORTS; k++) {
        if (forts[k].complete && (oldest < 0 || forts[k].expires < forts[oldest].expires)) {
            oldest = k;
        }
    }
    return oldest;
}

uint8_t bot_fort_request(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy)
{
    if (!forts_enabled || (team != TEAM_A && team != TEAM_B) || kind > BOT_FORT_TOWER) {
        return 0;
    }
    /* one structure of each kind a site, whoever raised it */
    if (_fort_at(kind, x, y, kind == BOT_FORT_RING ? 8 : 6) >= 0) {
        return 0;
    }
    if (abs(fx) >= abs(fy)) {
        fx = fx > 0 ? 1 : fx < 0 ? -1 : 0;
        fy = 0;
    } else {
        fy = fy > 0 ? 1 : fy < 0 ? -1 : 0;
        fx = 0;
    }
    if (fx == 0 && fy == 0) {
        fy = 1;
    }
    int slot = _fort_free_slot();
    if (slot < 0) {
        return 0;
    }
    bot_fort_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.anchor[0] = x;
    tmp.anchor[1] = y;
    tmp.anchor[2] = z;
    tmp.fx        = fx;
    tmp.fy        = fy;
    tmp.rx        = -fy;
    tmp.ry        = fx;
    tmp.kind      = kind;
    tmp.team      = team;
    uint8_t shape = kind == BOT_FORT_RING     ? _fort_shape_ring(server, &tmp)
                    : kind == BOT_FORT_BUNKER ? _fort_shape_bunker(server, &tmp)
                                              : _fort_shape_tower(server, &tmp);
    if (!shape || tmp.count < 8) {
        evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=shape", _fort_kind_name(kind), x, y);
        return 0;
    }
    /* the ground has to take it: no cell in the water, no protected cell, no
       overlap with another structure. Cells already solid count as built */
    int    built = 0, claimable = 0;
    for (int i = 0; i < tmp.count; i++) {
        fort_cell_t* c = &tmp.cells[i];
        if (c->z < 0 || c->z >= (int) server->s_map.map.size_z - 2) {
            evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=water", _fort_kind_name(kind), x, y);
            return 0;
        }
        if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z)) {
            built++;
            continue;
        }
        if (!gamemode_cell_editable(server, c->x, c->y, c->z)) {
            evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=protected cell=%d,%d,%d",
                    _fort_kind_name(kind), x, y, c->x, c->y, c->z);
            return 0;
        }
        for (int k = 0; k < BOT_FORTS; k++) {
            if (forts[k].used && _fort_cell_at(&forts[k], c->x, c->y, c->z) >= 0) {
                evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=overlap cell=%d,%d,%d",
                        _fort_kind_name(kind), x, y, c->x, c->y, c->z);
                return 0;
            }
        }
        int sx, sy, sf;
        if (_bot_placeable(server, c->x, c->y, c->z) && _fort_stand(server, &tmp, c, &sx, &sy, &sf)) {
            claimable++;
        }
    }
    if (built == tmp.count) {
        /* already there (a failed structure, or one another team raised):
           create it complete, so the mode can use its top and it is mended
           if it is shot down */
    } else if (claimable == 0) {
        evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=unreachable", _fort_kind_name(kind), x, y);
        return 0;
    }
    /* builders, nearest first */
    uint8_t ids[BOT_FORT_BUILDERS];
    float   dist[BOT_FORT_BUILDERS];
    int     n = 0;
    player_t *p, *tmp_p;
    HASH_ITER(hh, server->players, p, tmp_p)
    {
        if (!_fort_free_bot(p, team, p->id) || n >= BOT_FORT_BUILDERS) {
            continue;
        }
        float dx = p->movement.position.x - ((float) x + 0.5f);
        float dy = p->movement.position.y - ((float) y + 0.5f);
        float d  = dx * dx + dy * dy;
        if (d > BOT_FORT_JOIN * BOT_FORT_JOIN) {
            continue;
        }
        int at = n;
        while (at > 0 && dist[at - 1] > d) {
            dist[at] = dist[at - 1];
            ids[at]  = ids[at - 1];
            at--;
        }
        dist[at] = d;
        ids[at]  = p->id;
        n++;
    }
    if (n == 0) {
        evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=no_builders", _fort_kind_name(kind), x, y);
        return 0;
    }
    bot_fort_t* f = &forts[slot];
    memset(f, 0, sizeof *f);
    *f            = tmp;
    f->used       = 1;
    f->id         = forts_next_id++;
    f->expires    = get_nanos() + (uint64_t) FORT_BUILD_SECONDS * NANO_IN_SECOND;
    f->next_log   = 0;
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        f->builders[b] = b < n && built < f->count ? ids[b] : 0xFF;
        f->claim[b]    = -1;
    }
    if (built == f->count) {
        f->complete = 1;
        f->expires  = get_nanos();
    }
    evt_log(NULL, "bot_fort", "action=create id=%u kind=%s team=%u at=%d,%d,%d face=%d,%d cells=%u built=%d builders=%d",
            f->id, _fort_kind_name(kind), team, x, y, z, fx, fy, f->count, built, n);
    for (int b = 0; b < n; b++) {
        player_t* who = NULL;
        HASH_FIND(hh, server->players, &ids[b], sizeof(ids[b]), who);
        evt_log(who, "bot_fort", "action=assign id=%u kind=%s slot=%d at=%d,%d", f->id, _fort_kind_name(kind), b, x, y);
    }
    return 1;
}

/* --- the tick: what is built, who builds it, when it is given up --- */

void bot_forts_tick(server_t* server)
{
    uint64_t now = get_nanos();
    for (int k = 0; k < BOT_FORTS; k++) {
        bot_fort_t* f = &forts[k];
        if (!f->used) {
            continue;
        }
        int built = 0;
        for (int i = 0; i < f->count; i++) {
            if (map_solid(&server->s_map.map, (uint16_t) f->cells[i].x, (uint16_t) f->cells[i].y,
                          (uint16_t) f->cells[i].z))
            {
                built++;
            }
        }
        f->built = (uint8_t) built;
        if (built == f->count && !f->complete) {
            f->complete = 1;
            f->expires  = now;
            /* the builders are free for the next structure (the ring after a
               tower, the bunker after a ring); a repair fills the slots again
               if the fort is shot open */
            for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
                f->builders[b] = 0xFF;
                f->claim[b]    = -1;
            }
            evt_log(NULL, "bot_fort", "action=complete id=%u kind=%s team=%u at=%d,%d cells=%u", f->id,
                    _fort_kind_name(f->kind), f->team, f->anchor[0], f->anchor[1], f->count);
        } else if (built < f->count && f->complete) {
            /* shot open: the team mends it while its builders are still near */
            f->complete = 0;
            f->expires  = now + (uint64_t) FORT_BUILD_SECONDS * NANO_IN_SECOND;
            evt_log(NULL, "bot_fort", "action=damaged id=%u kind=%s at=%d,%d built=%d/%u", f->id,
                    _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], built, f->count);
        }
        if (!f->complete) {
            _fort_fill_builders(server, f);
            if (now >= f->expires) {
                evt_log(NULL, "bot_fort", "action=expire id=%u kind=%s at=%d,%d built=%d/%u", f->id,
                        _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], built, f->count);
                memset(f, 0, sizeof *f);
                continue;
            }
        }
        if (now >= f->next_log) {
            f->next_log = now + 5ull * NANO_IN_SECOND;
            evt_log(NULL, "bot_fort", "action=status id=%u kind=%s at=%d,%d built=%d/%u complete=%u builders=%u,%u,%u,%u",
                    f->id, _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], built, f->count, f->complete,
                    f->builders[0], f->builders[1], f->builders[2], f->builders[3]);
        }
    }
}

/* --- the bot: its stand and the one cell it puts --- */

uint8_t bot_fort_goal(server_t* server, player_t* bot, float* ox, float* oy, int* ofloor)
{
    if (!forts_enabled || bot == NULL) {
        return 0;
    }
    int k = _fort_slot_of(bot->id);
    if (k < 0) {
        return 0;
    }
    bot_fort_t* f = &forts[k];
    if (f->complete || bot->blocks == 0) {
        return 0;
    }
    int b = _fort_builder_of(f, bot->id);
    if (b < 0) {
        return 0;
    }
    uint64_t now = get_nanos();
    int      idx = f->claim[b];
    if (idx >= 0) {
        fort_cell_t* c = &f->cells[idx];
        if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z) ||
            now - f->claim_at[b] > (uint64_t) FORT_CLAIM_SECONDS * NANO_IN_SECOND ||
            !gamemode_cell_editable(server, c->x, c->y, c->z))
        {
            f->claim[b] = -1;
            idx         = -1;
        }
    }
    if (idx >= 0) {
        int sx, sy, sf;
        if (_fort_stand(server, f, &f->cells[idx], &sx, &sy, &sf)) {
            *ox     = (float) sx + 0.5f;
            *oy     = (float) sy + 0.5f;
            *ofloor = sf;
            return 1;
        }
        f->claim[b] = -1;
    }
    int   best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < f->count; i++) {
        fort_cell_t* c = &f->cells[i];
        if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z) ||
            !gamemode_cell_editable(server, c->x, c->y, c->z) || !_bot_placeable(server, c->x, c->y, c->z))
        {
            continue;
        }
        uint8_t claimed = 0;
        for (int o = 0; o < BOT_FORT_BUILDERS; o++) {
            if (o != b && f->claim[o] == i) {
                claimed = 1;
            }
        }
        if (claimed) {
            continue;
        }
        int sx, sy, sf;
        if (!_fort_stand(server, f, c, &sx, &sy, &sf)) {
            continue;
        }
        float dx = (float) c->x + 0.5f - bot->movement.position.x;
        float dy = (float) c->y + 0.5f - bot->movement.position.y;
        float d  = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    if (best < 0) {
        return 0;
    }
    f->claim[b]    = (int8_t) best;
    f->claim_at[b] = now;
    int sx, sy, sf;
    if (!_fort_stand(server, f, &f->cells[best], &sx, &sy, &sf)) {
        f->claim[b] = -1;
        return 0;
    }
    *ox     = (float) sx + 0.5f;
    *oy     = (float) sy + 0.5f;
    *ofloor = sf;
    return 1;
}

uint8_t bot_fort_step(server_t* server, player_t* bot)
{
    if (!forts_enabled || bot == NULL) {
        return 0;
    }
    int k = _fort_slot_of(bot->id);
    if (k < 0) {
        return 0;
    }
    bot_fort_t* f = &forts[k];
    if (f->complete || bot->blocks == 0) {
        return 0;
    }
    int b = _fort_builder_of(f, bot->id);
    if (b < 0 || f->claim[b] < 0) {
        return 0;
    }
    fort_cell_t* c = &f->cells[f->claim[b]];
    if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z)) {
        f->claim[b] = -1;
        return 1;
    }
    /* close enough to put it, from wherever it stopped: the stand is a
       suggestion, the server's reach is what counts (a builder that stopped
       at the stand's edge never placed and read as stuck there) */
    if (bot->airborne) {
        return 0;
    }
    if ((int) bot->movement.position.x == c->x && (int) bot->movement.position.y == c->y) {
        return 0; /* in the block's own column: walk out first */
    }
    float dx = (float) c->x - bot->movement.position.x;
    float dy = (float) c->y - bot->movement.position.y;
    float dz = (float) c->z - bot->movement.position.z;
    if (dx * dx + dy * dy + dz * dz > 3.8f * 3.8f) {
        return 0; /* still walking */
    }
    _bot_clear_inputs(bot);
    /* the team's colour, not the ground's: a fortification is the team's and
       its people have to find it. The quick walls a post raises stay the
       ground's colour, as camouflage */
    bot->tool_color = server->protocol.color_team[bot->team];
    _bot_set_tool(server, bot, TOOL_BLOCK);
    _bot_face(bot, (float) c->x + 0.5f, (float) c->y + 0.5f, (float) c->z + 0.5f);
    uint8_t ok = _bot_place_block(server, bot, (uint32_t) c->x, (uint32_t) c->y, (uint32_t) c->z);
    if (ok) {
        f->claim[b] = -1;
        f->expires  = get_nanos() + (uint64_t) FORT_BUILD_SECONDS * NANO_IN_SECOND;
        evt_log(bot, "bot_fort", "action=place id=%u kind=%s cell=%d,%d,%d blocks=%u", f->id, _fort_kind_name(f->kind),
                c->x, c->y, c->z, bot->blocks);
    }
    return 1;
}
