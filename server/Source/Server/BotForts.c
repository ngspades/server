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
#define FORT_CLAIM_HIGH_SECONDS 20 /* a claim whose stand is up the structure: the walk up its stair */
#define FORT_ROLL_SECONDS  20 /* a bot's yes or no to a structure holds this long */
#define FORT_WATCH_SECONDS 45 /* a watch on a finished structure, then the bot goes back to fighting */
#define FORT_SHIFT_CELLS   24 /* a builder's share of one structure: then it fights and another helps */
#define FORT_CELL_TRIES    3  /* claims of a cell that ran out with the builder beside it: the cell is dropped */
#define FORT_BAN_SECONDS   60 /* a site the plan refused is not tried again for this long */
#define FORT_BANS          32

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
    int         base; /* the level a straight structure stands on (its ground cell's z) */
    int         fx, fy; /* toward the threat */
    int         rx, ry; /* across: (-fy, fx) */
    fort_cell_t cells[BOT_FORT_CELLS];
    uint8_t     builders[BOT_FORT_BUILDERS];
    uint8_t     placed[BOT_FORT_BUILDERS]; /* cells each builder put: its share, and its cooldown */
    uint8_t     tries[BOT_FORT_CELLS];     /* claims of each cell that ran out with the builder near it */
    int8_t      claim[BOT_FORT_BUILDERS];
    uint64_t    claim_at[BOT_FORT_BUILDERS];
    uint8_t     claim_high[BOT_FORT_BUILDERS]; /* its stand is on the structure, over the base */
    uint64_t    expires;
    uint64_t    next_log;
    uint8_t     has_watch;
    int         watch[3]; /* the objective a finished lookout's posts look at */
    uint8_t     posts[BOT_FORT_POSTS];
    uint64_t    post_until[BOT_FORT_POSTS];
    uint8_t     post_reached[BOT_FORT_POSTS]; /* the watch stood on its post (logged once) */
} bot_fort_t;

typedef struct
{
    int      x, y;
    uint8_t  kind;
    uint64_t until;
} fort_ban_t;

static bot_fort_t forts[BOT_FORTS];
static uint16_t   forts_next_id = 1;
static uint8_t    forts_enabled = 1;
static fort_ban_t fort_bans[FORT_BANS];

/* Bots must not build all the time (a team that answered every request
   stood round its walls while the enemy took the objective): a request is a
   chance a bot takes or not, a helper the same, and a bot that did its part
   fights a while before it is asked again. */
static int fort_build_chance = 35; /* percent: start a structure asked for */
static int fort_help_chance  = 70; /* percent: join a team's unfinished one, or watch from a finished one */
static int fort_cooldown_s   = 60; /* seconds of fighting after a share of building or a watch */

static uint64_t fort_cooldown_until[BOT_MAX_PLAYERS];
enum { FORT_ROLL_START = 0, FORT_ROLL_HELP, FORT_ROLL_WATCH, FORT_ROLLS };
static const char* const fort_roll_name[FORT_ROLLS] = {"start", "help", "watch"};
static uint8_t  fort_roll_yes[FORT_ROLLS][BOT_MAX_PLAYERS];
static uint64_t fort_roll_until[FORT_ROLLS][BOT_MAX_PLAYERS];

static const char* _fort_kind_name(uint8_t kind)
{
    switch (kind) {
        case BOT_FORT_RING:
            return "ring";
        case BOT_FORT_BUNKER:
            return "bunker";
        case BOT_FORT_TOWER:
            return "tower";
        case BOT_FORT_WATCHTOWER:
            return "watchtower";
        default:
            return "barricade";
    }
}

static int _fort_env_int(const char* name, int value, int lo, int hi)
{
    const char* env = getenv(name);
    if (env != NULL && *env != '\0') {
        value = atoi(env);
    }
    return value < lo ? lo : value > hi ? hi : value;
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
        d = t != NULL ? toml_int_in(t, "build_chance") : (toml_datum_t) {0};
        if (d.ok) {
            fort_build_chance = (int) d.u.i;
        }
        d = t != NULL ? toml_int_in(t, "help_chance") : (toml_datum_t) {0};
        if (d.ok) {
            fort_help_chance = (int) d.u.i;
        }
        d = t != NULL ? toml_int_in(t, "build_cooldown") : (toml_datum_t) {0};
        if (d.ok) {
            fort_cooldown_s = (int) d.u.i;
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_BOT_FORTS");
    if (env != NULL && *env != '\0') {
        forts_enabled = atoi(env) != 0;
    }
    fort_build_chance = _fort_env_int("SP_BOT_BUILD_CHANCE", fort_build_chance, 0, 100);
    fort_help_chance  = _fort_env_int("SP_BOT_HELP_CHANCE", fort_help_chance, 0, 100);
    fort_cooldown_s   = _fort_env_int("SP_BOT_BUILD_COOLDOWN", fort_cooldown_s, 0, 3600);
    evt_log(NULL, "bot_fort", "action=rules enabled=%u build_chance=%d help_chance=%d cooldown=%d", forts_enabled,
            fort_build_chance, fort_help_chance, fort_cooldown_s);
}

void bot_forts_reset(void)
{
    memset(forts, 0, sizeof forts);
    memset(fort_bans, 0, sizeof fort_bans);
    memset(fort_cooldown_until, 0, sizeof fort_cooldown_until);
    memset(fort_roll_until, 0, sizeof fort_roll_until);
    forts_next_id = 1;
}

/* the bot did its part (a share of a structure, a watch): it fights a while */
static void _fort_rest(player_t* p, uint8_t id, const char* why)
{
    if (fort_cooldown_s <= 0) {
        return;
    }
    fort_cooldown_until[id] = get_nanos() + (uint64_t) fort_cooldown_s * NANO_IN_SECOND;
    evt_log(p, "bot_fort", "action=rest player=%u seconds=%d why=%s", id, fort_cooldown_s, why);
}

/* Will this bot build (or help, or watch)? One roll a bot and purpose,
   kept FORT_ROLL_SECONDS whatever structure asks: the builder slots are
   filled every tick, and a roll a tick (or a roll for each of two sites in
   reach) would be a yes within a second. */
static uint8_t _fort_willing(server_t* server, player_t* p, int purpose, int chance)
{
    uint64_t now = get_nanos();
    if (now < fort_roll_until[purpose][p->id]) {
        return fort_roll_yes[purpose][p->id];
    }
    uint8_t yes                       = chance >= 100 || (chance > 0 && gen_rand(&server->rand) * 100.0f < (float) chance);
    fort_roll_yes[purpose][p->id]   = yes;
    fort_roll_until[purpose][p->id] = now + (uint64_t) FORT_ROLL_SECONDS * NANO_IN_SECOND;
    evt_log(p, "bot_fort", "action=decide why=%s chance=%d yes=%u", fort_roll_name[purpose], chance, yes);
    return yes;
}

/* kind 0xFF: a ban of any kind (the hilltop search serves several) */
static uint8_t _fort_banned(uint8_t kind, int x, int y)
{
    uint64_t now = get_nanos();
    for (int k = 0; k < FORT_BANS; k++) {
        if (fort_bans[k].until > now && (kind == 0xFF || fort_bans[k].kind == kind) && abs(fort_bans[k].x - x) <= 2 &&
            abs(fort_bans[k].y - y) <= 2)
        {
            return 1;
        }
    }
    return 0;
}

/* a site the structure's plan cannot take (water, a protected cell, an
   overlap, nowhere to stand): the searches skip it a minute, or the same
   hilltop was offered and refused every few seconds */
static void _fort_ban(uint8_t kind, int x, int y)
{
    int      pick = 0;
    uint64_t now  = get_nanos();
    for (int k = 0; k < FORT_BANS; k++) {
        if (fort_bans[k].until <= now) {
            pick = k;
            break;
        }
        if (fort_bans[k].until < fort_bans[pick].until) {
            pick = k;
        }
    }
    fort_bans[pick].x     = x;
    fort_bans[pick].y     = y;
    fort_bans[pick].kind  = kind;
    fort_bans[pick].until = now + (uint64_t) FORT_BAN_SECONDS * NANO_IN_SECOND;
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
    f->base = base;
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
    f->base = base;
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

/* A lookout over an objective: the tower's two by two core six high (a bot
   on it stands six blocks over the ground, its eye eight: over the roofs
   and the walls round a base or a hill), a parapet on the threat side that
   makes it seven, and a stair of single steps that winds round the core's
   open sides from the front corner to the back, so it needs no level run of
   columns: each step leans on the core, and the one on the corner, which
   touches nothing, stands on a support column. Six and not seven: the
   server takes a block within five of the builder, so every core cell goes
   up from the ground; a seventh level needed a builder on the half built
   stair, and of seventeen towers seven were given up with only that level
   missing. The three high tower was a step over a ring; it saw nothing a bot
   on the ground did not. */
static uint8_t _fort_shape_watchtower(server_t* server, bot_fort_t* f)
{
    int base = _fort_base(server, f, -1, 1, -1, 2);
    if (base < 0) {
        return 0;
    }
    f->base = base;
    /* the core and the stair's columns */
    static const int cols[9][2] = {{-1, 0}, {0, 0}, {-1, 1}, {0, 1}, {1, 2}, {1, 1}, {1, 0}, {1, -1}, {0, -1}};
    for (int k = 0; k < 9; k++) {
        if (!_fort_foundations(server, f, cols[k][0], cols[k][1], base)) {
            return 0;
        }
    }
    for (int w = 0; w <= 5; w++) {
        for (int v = 0; v <= 1; v++) {
            for (int u = -1; u <= 0; u++) {
                if (!_fort_add_local(f, u, v, w, base)) {
                    return 0;
                }
            }
        }
    }
    for (int u = -1; u <= 0; u++) {
        if (!_fort_add_local(f, u, 1, 6, base)) {
            return 0; /* the parapet on the threat side: the top's back half stays free to stand on */
        }
    }
    /* step k one up from step k - 1; the last (w 4) is a jump under the
       core's top (w 5), at its back, away from the parapet */
    static const int steps[5][2] = {{1, 2}, {1, 1}, {1, 0}, {1, -1}, {0, -1}};
    for (int k = 0; k < 5; k++) {
        if (!_fort_add_local(f, steps[k][0], steps[k][1], k, base)) {
            return 0;
        }
    }
    for (int w = 0; w < 3; w++) {
        if (!_fort_add_local(f, 1, -1, w, base)) {
            return 0; /* under the corner step */
        }
    }
    return f->count > 0;
}

/* A barricade of two floors, seven wide: the ground floor's bots shoot over
   a two high wall (the deck in front of them, then the front wall), and a
   firing deck one level up behind merlons, reached by a step at each end,
   lets a second row shoot over the first. Cheeks at both ends cover the deck
   from the side. A wall one row deep with nobody over it was cover for two
   bots; this holds four to six on one front. */
static uint8_t _fort_shape_barricade(server_t* server, bot_fort_t* f)
{
    int base = _fort_base(server, f, -3, 3, 0, 2);
    if (base < 0) {
        return 0;
    }
    f->base = base;
    for (int u = -3; u <= 3; u++) {
        for (int v = 1; v <= 2; v++) {
            if (!_fort_foundations(server, f, u, v, base)) {
                return 0;
            }
        }
    }
    for (int side = -2; side <= 2; side += 4) {
        if (!_fort_foundations(server, f, side, 0, base)) {
            return 0;
        }
    }
    for (int u = -2; u <= 2; u++) {
        for (int w = 0; w <= 1; w++) {
            if (!_fort_add_local(f, u, 1, w, base) || !_fort_add_local(f, u, 2, w, base)) {
                return 0; /* the deck and the ground floor's wall */
            }
        }
    }
    for (int u = -2; u <= 2; u += 2) {
        for (int w = 2; w <= 3; w++) {
            if (!_fort_add_local(f, u, 2, w, base)) {
                return 0; /* the deck's merlons; the gaps between them are its loopholes */
            }
        }
    }
    for (int side = -3; side <= 3; side += 6) {
        for (int v = 1; v <= 2; v++) {
            for (int w = 0; w <= 2; w++) {
                if (!_fort_add_local(f, side, v, w, base)) {
                    return 0; /* the cheeks */
                }
            }
        }
    }
    for (int side = -2; side <= 2; side += 4) {
        if (!_fort_add_local(f, side, 0, 0, base)) {
            return 0; /* a step up to the deck at each end */
        }
    }
    return f->count > 0;
}

/* how far over its ground a bot on the finished structure stands (its floor
   cell is base - rise): the lookouts' eye height for the sight test */
static int _fort_rise(uint8_t kind)
{
    return kind == BOT_FORT_WATCHTOWER ? 6 : kind == BOT_FORT_BARRICADE ? 2 : kind == BOT_FORT_TOWER ? 3 : 0;
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
            /* down to six under the cell: a watchtower's upper core levels
               are in reach from the ground (the 3.8 check below says which),
               so only its parapet needs a builder on the stair */
            for (int zf = c->z + 1; zf <= c->z + 6; zf++) {
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
                } else if (f->kind == BOT_FORT_WATCHTOWER || f->kind == BOT_FORT_BARRICADE) {
                    inside = 0; /* open shapes: a stand on the stair or the deck is how the top goes up */
                } else {
                    float lu = (px - ax) * f->rx + (py - ay) * f->ry;
                    float lv = (px - ax) * f->fx + (py - ay) * f->fy;
                    inside   = fabsf(lu) <= 1.0f && fabsf(lv) <= 1.0f;
                }
                if (inside) {
                    score += 20.0f;
                }
                /* the structure is cover from the threat's side only: a
                   builder in front of it (toward the enemy) puts the wall
                   up and then stands in the open it was meant to shield it
                   from (about half the barricades' and bunkers' blocks were
                   placed from there). Stand behind the cell whenever some
                   column within reach is; a front stand is the last resort
                   (a ring is round: its inside/outside rule above is
                   the whole story) */
                if (f->kind != BOT_FORT_RING) {
                    float ahead = ddx * (float) f->fx + ddy * (float) f->fy;
                    /* ddx, ddy are stand minus cell: positive is the threat's side */
                    if (ahead > 0.5f) {
                        score += 12.0f + 4.0f * ahead;
                    }
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
        for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
            if (f->builders[b] != 0xFF && f->placed[b] > 0) {
                _fort_rest(NULL, f->builders[b], "discard");
            }
        }
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

static int _fort_post_of(uint8_t id)
{
    for (int k = 0; k < BOT_FORTS; k++) {
        if (!forts[k].used) {
            continue;
        }
        for (int b = 0; b < BOT_FORT_POSTS; b++) {
            if (forts[k].posts[b] == id) {
                return k;
            }
        }
    }
    return -1;
}

/* a bot a fort may take: same team, playing, alive, with blocks, not
   already on a structure, a watch or a bridge, not resting after its last
   share, and never the objective's carrier (it runs home) */
static uint8_t _fort_free_bot(server_t* server, player_t* p, uint8_t team)
{
    const bot_ai_t* ai = _bot_ai(p->id);
    return p->team == team && p->state == STATE_READY && p->alive && ai != NULL && !ai->dummy && p->blocks > 0 &&
           _fort_slot_of(p->id) < 0 && _fort_post_of(p->id) < 0 && !bot_project_busy(p->id) &&
           get_nanos() >= fort_cooldown_until[p->id] && !_bot_carries(server, p);
}

/* fill the structure's empty builder slots with the nearest free bots: a
   respawned mate takes the place of one that left, so the work goes on. A
   free bot helps only on its help roll (the others go on with their goal),
   and from farther than a request takes builders: a team's site going up is
   worth a walk */
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
        evt_log(NULL, "bot_fort", "action=release id=%u slot=%d player=%u placed=%u", f->id, b, f->builders[b],
                f->placed[b]);
        f->builders[b] = 0xFF;
        f->claim[b]    = -1;
        f->placed[b]   = 0;
    }
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        if (f->builders[b] != 0xFF) {
            continue;
        }
        player_t *p, *tmp;
        float     best = BOT_FORT_HELP * BOT_FORT_HELP;
        player_t* pick = NULL;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (!p->is_bot || !_fort_free_bot(server, p, f->team)) {
                continue;
            }
            float dx = p->movement.position.x - ((float) f->anchor[0] + 0.5f);
            float dy = p->movement.position.y - ((float) f->anchor[1] + 0.5f);
            float d  = dx * dx + dy * dy;
            if (d < best && _fort_willing(server, p, FORT_ROLL_HELP, fort_help_chance)) {
                best = d;
                pick = p;
            }
        }
        if (pick == NULL) {
            break;
        }
        f->builders[b] = pick->id;
        f->claim[b]    = -1;
        f->placed[b]   = 0;
        evt_log(pick, "bot_fort", "action=assign id=%u kind=%s slot=%d at=%d,%d why=help dist=%.1f", f->id,
                _fort_kind_name(f->kind), b, f->anchor[0], f->anchor[1], sqrtf(best));
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

/* The columns a bot walks to from (cx, cy) within radius: dry ground joined
   by steps of at most one block, the objective's own floor outward. The
   searches take a site only there: "the highest ground" alone was a roof
   (Busan_Port's sheds, seven over the hill) that no plan reached, and its
   builders walked partial plans until the structure expired, nothing put. */
#define FORT_REACH_R 48
static uint8_t fort_reach[2 * FORT_REACH_R + 1][2 * FORT_REACH_R + 1];
static int     fort_reach_cx, fort_reach_cy;

static int _fort_walk_reach(server_t* server, int cx, int cy, int radius)
{
    static int queue[(2 * FORT_REACH_R + 1) * (2 * FORT_REACH_R + 1)];
    if (radius > FORT_REACH_R) {
        radius = FORT_REACH_R;
    }
    memset(fort_reach, 0, sizeof fort_reach);
    fort_reach_cx = cx;
    fort_reach_cy = cy;
    if (_fort_ground(server, cx, cy) < 0) {
        return 0;
    }
    int head = 0, tail = 0, n = 0;
    fort_reach[FORT_REACH_R][FORT_REACH_R] = 1;
    queue[tail++]                          = FORT_REACH_R * (2 * FORT_REACH_R + 1) + FORT_REACH_R;
    while (head < tail) {
        int q = queue[head++];
        int u = q % (2 * FORT_REACH_R + 1) - FORT_REACH_R, v = q / (2 * FORT_REACH_R + 1) - FORT_REACH_R;
        int top = _fort_ground(server, cx + u, cy + v);
        n++;
        static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int d = 0; d < 4; d++) {
            int nu = u + dirs[d][0], nv = v + dirs[d][1];
            if (nu * nu + nv * nv > radius * radius || fort_reach[nv + FORT_REACH_R][nu + FORT_REACH_R]) {
                continue;
            }
            int t2 = _fort_ground(server, cx + nu, cy + nv);
            if (t2 < 0 || abs(t2 - top) > 1) {
                continue;
            }
            fort_reach[nv + FORT_REACH_R][nu + FORT_REACH_R] = 1;
            queue[tail++] = (nv + FORT_REACH_R) * (2 * FORT_REACH_R + 1) + (nu + FORT_REACH_R);
        }
    }
    return n;
}

/* an objective whose walkable ground round it is this small is a roof or a
   ledge of its own (a territory on a building, Normandie's hill on a block
   of houses): the team does not get there on foot, and a structure there
   was walked at with partial plans until it expired. A third of the disc,
   at most 300 columns */
static uint8_t _fort_reach_enough(int walking, int radius)
{
    int need = (int) (3.14159f * (float) radius * (float) radius / 3.0f);
    return walking >= (need < 300 ? need : 300);
}

static uint8_t _fort_reached(int x, int y)
{
    int u = x - fort_reach_cx, v = y - fort_reach_cy;
    return u >= -FORT_REACH_R && u <= FORT_REACH_R && v >= -FORT_REACH_R && v <= FORT_REACH_R &&
           fort_reach[v + FORT_REACH_R][u + FORT_REACH_R];
}

/* any structure within radius of (x, y), whatever its kind or team */
static uint8_t _fort_near_any(int x, int y, int radius)
{
    for (int k = 0; k < BOT_FORTS; k++) {
        if (!forts[k].used) {
            continue;
        }
        int dx = forts[k].anchor[0] - x, dy = forts[k].anchor[1] - y;
        if (dx * dx + dy * dy <= radius * radius) {
            return 1;
        }
    }
    return 0;
}

uint8_t bot_fort_hilltop(server_t* server, int cx, int cy, int radius, int min_rise, int* out_x, int* out_y, int* out_z)
{
    map_store_t* map = &server->s_map.map;
    if (cx < 3 || cy < 3 || cx >= map->size_x - 3 || cy >= map->size_y - 3) {
        return 0;
    }
    int   z0   = map_top(map, (uint16_t) cx, (uint16_t) cy);
    float best = 1e9f;
    int   bx = -1, by = -1, bz = 0;
    if (!_fort_reach_enough(_fort_walk_reach(server, cx, cy, radius), radius)) {
        return 0;
    }
    for (int y = cy - radius; y <= cy + radius; y += 2) {
        for (int x = cx - radius; x <= cx + radius; x += 2) {
            if (x < 3 || y < 3 || x >= map->size_x - 3 || y >= map->size_y - 3) {
                continue;
            }
            int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 > radius * radius) {
                continue;
            }
            int top = map_top(map, (uint16_t) x, (uint16_t) y);
            /* z grows downwards: higher ground is a smaller z; dry only */
            if (top < 4 || top >= map->size_z - 2 || top > z0 - min_rise) {
                continue;
            }
            int level = 1;
            for (int v = -1; v <= 1 && level; v++) {
                for (int u = -1; u <= 1; u++) {
                    if (abs(map_top(map, (uint16_t) (x + u), (uint16_t) (y + v)) - top) > 1) {
                        level = 0;
                        break;
                    }
                }
            }
            if (!level || !_fort_reached(x, y) || _fort_near_any(x, y, 8) || _fort_banned(0xFF, x, y)) {
                continue;
            }
            float score = (float) top * 2.0f + sqrtf((float) d2) * 0.15f;
            if (score < best) {
                best = score;
                bx   = x;
                by   = y;
                bz   = top;
            }
        }
    }
    if (bx < 0) {
        return 0;
    }
    *out_x = bx;
    *out_y = by;
    *out_z = bz;
    return 1;
}

/* The structure's plan at a site: its cells (facing along one axis), and
   whether the ground takes it: no cell in the water, no protected cell, no
   overlap with another structure, and something to put now from a stand a
   bot reaches. NULL when it does; else the reason, for the refusal's log
   (cell: the cell it is about). Cells already solid count as built. */
static const char* _fort_plan(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy,
                              bot_fort_t* out, fort_cell_t* cell)
{
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
    memset(out, 0, sizeof *out);
    out->anchor[0] = x;
    out->anchor[1] = y;
    out->anchor[2] = z;
    out->base      = z;
    out->fx        = fx;
    out->fy        = fy;
    out->rx        = -fy;
    out->ry        = fx;
    out->kind      = kind;
    out->team      = team;
    uint8_t shape;
    switch (kind) {
        case BOT_FORT_RING:
            shape = _fort_shape_ring(server, out);
            break;
        case BOT_FORT_BUNKER:
            shape = _fort_shape_bunker(server, out);
            break;
        case BOT_FORT_TOWER:
            shape = _fort_shape_tower(server, out);
            break;
        case BOT_FORT_WATCHTOWER:
            shape = _fort_shape_watchtower(server, out);
            break;
        default:
            shape = _fort_shape_barricade(server, out);
            break;
    }
    if (!shape || out->count < 8) {
        return "shape";
    }
    int built = 0, claimable = 0;
    for (int i = 0; i < out->count; i++) {
        fort_cell_t* c = &out->cells[i];
        *cell          = *c;
        if (c->z < 0 || c->z >= (int) server->s_map.map.size_z - 2) {
            return "water";
        }
        if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z)) {
            built++;
            continue;
        }
        if (!gamemode_cell_editable(server, c->x, c->y, c->z)) {
            return "protected";
        }
        for (int k = 0; k < BOT_FORTS; k++) {
            if (forts[k].used && _fort_cell_at(&forts[k], c->x, c->y, c->z) >= 0) {
                return "overlap";
            }
        }
        int sx, sy, sf;
        if (_bot_placeable(server, c->x, c->y, c->z) && _fort_stand(server, out, c, &sx, &sy, &sf)) {
            claimable++;
        }
    }
    out->built = (uint8_t) built;
    if (built < out->count && claimable == 0) {
        return "unreachable";
    }
    return NULL;
}

static uint8_t _fort_create(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy,
                            const int* watch)
{
    if (!forts_enabled || (team != TEAM_A && team != TEAM_B) || kind > BOT_FORT_BARRICADE) {
        return 0;
    }
    /* one structure of each kind a site, whoever raised it */
    int same = kind == BOT_FORT_RING ? 8 : kind == BOT_FORT_WATCHTOWER || kind == BOT_FORT_BARRICADE ? 10 : 6;
    if (_fort_at(kind, x, y, same) >= 0 || _fort_banned(kind, x, y)) {
        return 0;
    }
    int slot = _fort_free_slot();
    if (slot < 0) {
        return 0;
    }
    static bot_fort_t tmp; /* ~1.3 KB: not on the tick's stack */
    fort_cell_t       cell = {0, 0, 0};
    const char*       why  = _fort_plan(server, team, kind, x, y, z, fx, fy, &tmp, &cell);
    if (why != NULL) {
        evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=%s cell=%d,%d,%d", _fort_kind_name(kind), x, y,
                why, cell.x, cell.y, cell.z);
        _fort_ban(kind, x, y);
        return 0;
    }
    int built = tmp.built;
    /* builders, nearest first, each on its own roll: a request is an
       opportunity, not an order */
    uint8_t ids[BOT_FORT_BUILDERS];
    float   dist[BOT_FORT_BUILDERS];
    int     n = 0, asked = 0;
    if (built < tmp.count) {
        player_t *p, *tmp_p;
        HASH_ITER(hh, server->players, p, tmp_p)
        {
            if (!p->is_bot || n >= BOT_FORT_BUILDERS || !_fort_free_bot(server, p, team)) {
                continue;
            }
            float dx = p->movement.position.x - ((float) x + 0.5f);
            float dy = p->movement.position.y - ((float) y + 0.5f);
            float d  = dx * dx + dy * dy;
            if (d > BOT_FORT_JOIN * BOT_FORT_JOIN) {
                continue;
            }
            asked++;
            if (!_fort_willing(server, p, FORT_ROLL_START, fort_build_chance)) {
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
            evt_log(NULL, "bot_fort", "action=refused kind=%s at=%d,%d why=%s asked=%d", _fort_kind_name(kind), x, y,
                    asked > 0 ? "declined" : "no_builders", asked);
            return 0;
        }
    }
    bot_fort_t* f = &forts[slot];
    if (f->used) {
        evt_log(NULL, "bot_fort", "action=evict id=%u kind=%s at=%d,%d", f->id, _fort_kind_name(f->kind),
                f->anchor[0], f->anchor[1]);
    }
    *f          = tmp;
    f->used     = 1;
    f->id       = forts_next_id++;
    f->expires  = get_nanos() + (uint64_t) FORT_BUILD_SECONDS * NANO_IN_SECOND;
    f->next_log = 0;
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        f->builders[b] = b < n ? ids[b] : 0xFF;
        f->claim[b]    = -1;
        f->placed[b]   = 0;
    }
    for (int b = 0; b < BOT_FORT_POSTS; b++) {
        f->posts[b] = 0xFF;
    }
    if (watch != NULL) {
        f->has_watch = 1;
        f->watch[0]  = watch[0];
        f->watch[1]  = watch[1];
        f->watch[2]  = watch[2];
    }
    if (built == f->count) {
        f->complete = 1;
        f->expires  = get_nanos();
    }
    evt_log(NULL, "bot_fort",
            "action=create id=%u kind=%s team=%u at=%d,%d,%d face=%d,%d cells=%u built=%d builders=%d asked=%d watch=%d,%d",
            f->id, _fort_kind_name(kind), team, x, y, z, f->fx, f->fy, f->count, built, n, asked,
            watch != NULL ? watch[0] : -1, watch != NULL ? watch[1] : -1);
    for (int b = 0; b < n; b++) {
        player_t* who = NULL;
        HASH_FIND(hh, server->players, &ids[b], sizeof(ids[b]), who);
        evt_log(who, "bot_fort", "action=assign id=%u kind=%s slot=%d at=%d,%d why=start", f->id, _fort_kind_name(kind),
                b, x, y);
    }
    return 1;
}

uint8_t bot_fort_request(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy)
{
    return _fort_create(server, team, kind, x, y, z, fx, fy, NULL);
}

uint8_t bot_fort_request_watch(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy,
                               int wx, int wy, int wz)
{
    int watch[3] = {wx, wy, wz};
    return _fort_create(server, team, kind, x, y, z, fx, fy, watch);
}

/* the post a bot on the finished structure looks from: the watchtower's top
   (its anchor column, the back half beside the parapet), the barricade's
   deck behind the gaps either side of its middle merlon */
static uint8_t _fort_post_cell(const bot_fort_t* f, int b, int* x, int* y, int* floor)
{
    int u, v, rise = _fort_rise(f->kind);
    if (f->kind == BOT_FORT_WATCHTOWER) {
        if (b != 0) {
            return 0;
        }
        u = 0;
        v = 0;
    } else if (f->kind == BOT_FORT_BARRICADE) {
        u = b == 0 ? -1 : 1;
        v = 1;
    } else {
        return 0;
    }
    *x     = f->anchor[0] + u * f->rx + v * f->fx;
    *y     = f->anchor[1] + u * f->ry + v * f->fy;
    *floor = f->base - rise;
    return 1;
}

uint8_t bot_fort_overlook(server_t* server, uint8_t team, uint8_t kind, int ox, int oy, int oz, int rmin, int rmax,
                          int fx, int fy, int* out_x, int* out_y, int* out_z)
{
    if (!forts_enabled || (kind != BOT_FORT_WATCHTOWER && kind != BOT_FORT_BARRICADE)) {
        return 0;
    }
    enum { CANDIDATES = 8 };
    float cand_s[CANDIDATES];
    int   cand_x[CANDIDATES], cand_y[CANDIDATES], cand_z[CANDIDATES];
    int   nc = 0, seen = 0, sighted = 0;
    float mid = 0.5f * (float) (rmin + rmax);
    /* a body standing on the objective: its chest (z grows downwards) */
    float tx = (float) ox + 0.5f, ty = (float) oy + 0.5f, tz = (float) oz - 1.5f;
    int   rise = _fort_rise(kind);
    /* the barricade's eye is over its deck, a row toward the threat */
    int dfx = abs(fx) >= abs(fy) ? (fx > 0 ? 1 : fx < 0 ? -1 : 0) : 0;
    int dfy = abs(fx) >= abs(fy) ? 0 : (fy > 0 ? 1 : -1);
    if (dfx == 0 && dfy == 0) {
        dfy = 1;
    }
    int eu      = kind == BOT_FORT_BARRICADE ? 1 : 0;
    int walking = _fort_walk_reach(server, ox, oy, rmax);
    if (!_fort_reach_enough(walking, rmax)) {
        evt_log(NULL, "bot_fort", "action=overlook_none kind=%s team=%u objective=%d,%d,%d walking=%d why=island",
                _fort_kind_name(kind), team, ox, oy, oz, walking);
        return 0;
    }
    for (int y = oy - rmax; y <= oy + rmax; y += 2) {
        for (int x = ox - rmax; x <= ox + rmax; x += 2) {
            int d2 = (x - ox) * (x - ox) + (y - oy) * (y - oy);
            if (d2 < rmin * rmin || d2 > rmax * rmax) {
                continue;
            }
            int top = _fort_ground(server, x, y);
            if (top < 0 || !_fort_reached(x, y) || _fort_near_any(x, y, 8) || _fort_banned(kind, x, y)) {
                continue;
            }
            int level = 1;
            for (int v = -1; v <= 1 && level; v++) {
                for (int u = -1; u <= 1; u++) {
                    int t2 = _fort_ground(server, x + u, y + v);
                    if (t2 < 0 || abs(t2 - top) > 2) {
                        level = 0;
                        break;
                    }
                }
            }
            if (!level) {
                continue;
            }
            seen++;
            float ex = (float) (x + eu * dfx) + 0.5f, ey = (float) (y + eu * dfy) + 0.5f;
            float ez = (float) (top - rise) - 2.2f;
            if (!_bot_line_clear(server, ex, ey, ez, tx, ty, tz)) {
                continue;
            }
            sighted++;
            /* the highest ground first, then the middle of the range */
            float score = (float) top * 1.5f + fabsf(sqrtf((float) d2) - mid) * 0.25f;
            int   at    = nc < CANDIDATES ? nc++ : CANDIDATES;
            if (at == CANDIDATES) {
                if (score >= cand_s[CANDIDATES - 1]) {
                    continue;
                }
                at = CANDIDATES - 1;
            }
            while (at > 0 && cand_s[at - 1] > score) {
                cand_s[at] = cand_s[at - 1];
                cand_x[at] = cand_x[at - 1];
                cand_y[at] = cand_y[at - 1];
                cand_z[at] = cand_z[at - 1];
                at--;
            }
            cand_s[at] = score;
            cand_x[at] = x;
            cand_y[at] = y;
            cand_z[at] = top;
        }
    }
    static bot_fort_t tmp;
    for (int k = 0; k < nc; k++) {
        fort_cell_t cell;
        const char* why = _fort_plan(server, team, kind, cand_x[k], cand_y[k], cand_z[k], fx, fy, &tmp, &cell);
        if (why != NULL) {
            _fort_ban(kind, cand_x[k], cand_y[k]);
            continue;
        }
        *out_x = cand_x[k];
        *out_y = cand_y[k];
        *out_z = cand_z[k];
        evt_log(NULL, "bot_fort",
                "action=overlook kind=%s team=%u at=%d,%d,%d objective=%d,%d,%d walking=%d seen=%d sighted=%d try=%d",
                _fort_kind_name(kind), team, *out_x, *out_y, *out_z, ox, oy, oz, walking, seen, sighted, k);
        return 1;
    }
    evt_log(NULL, "bot_fort", "action=overlook_none kind=%s team=%u objective=%d,%d,%d walking=%d seen=%d sighted=%d",
            _fort_kind_name(kind), team, ox, oy, oz, walking, seen, sighted);
    return 0;
}

/* --- the tick: what is built, who builds it, when it is given up --- */

/* every builder leaves the structure; one that put a cell rests */
static void _fort_release_all(server_t* server, bot_fort_t* f, const char* why)
{
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        if (f->builders[b] != 0xFF && f->placed[b] > 0) {
            player_t* p = NULL;
            HASH_FIND(hh, server->players, &f->builders[b], sizeof(f->builders[b]), p);
            _fort_rest(p, f->builders[b], why);
        }
        f->builders[b] = 0xFF;
        f->claim[b]    = -1;
        f->placed[b]   = 0;
    }
}

/* A finished lookout (a watchtower, a barricade, asked with the objective it
   is for) is held: its posts go to the nearest willing bots of its team, who
   stand there looking at the objective and fight from the height, then rest
   (go back to their own goal) after FORT_WATCH_SECONDS. A watch ends early
   when the bot dies or leaves; the post is given again. */
static void _fort_fill_posts(server_t* server, bot_fort_t* f, uint64_t now)
{
    /* (bot_fort_manning, BotForts.h: whether the watch stands on its post) */
    if (!f->has_watch || (f->kind != BOT_FORT_WATCHTOWER && f->kind != BOT_FORT_BARRICADE)) {
        return;
    }
    int posts = f->kind == BOT_FORT_WATCHTOWER ? 1 : BOT_FORT_POSTS;
    for (int b = 0; b < posts; b++) {
        if (f->posts[b] == 0xFF) {
            continue;
        }
        player_t* p = NULL;
        HASH_FIND(hh, server->players, &f->posts[b], sizeof(f->posts[b]), p);
        uint8_t gone = p == NULL || p->state != STATE_READY || !p->alive || p->team != f->team;
        if (!gone && !f->post_reached[b] && bot_fort_manning(server, p)) {
            f->post_reached[b] = 1;
            evt_log(p, "bot_fort", "action=on_post id=%u kind=%s post=%d after=%.1f", f->id, _fort_kind_name(f->kind), b,
                    (double) (FORT_WATCH_SECONDS - (double) (f->post_until[b] - now) / NANO_IN_SECOND));
        }
        if (gone || now >= f->post_until[b]) {
            evt_log(p, "bot_fort", "action=unwatch id=%u kind=%s post=%d why=%s reached=%u", f->id,
                    _fort_kind_name(f->kind), b, gone ? "gone" : "time", f->post_reached[b]);
            if (!gone) {
                _fort_rest(p, f->posts[b], "watch");
            }
            f->posts[b] = 0xFF;
        }
    }
    for (int b = 0; b < posts; b++) {
        if (f->posts[b] != 0xFF) {
            continue;
        }
        int px, py, pf;
        if (!_fort_post_cell(f, b, &px, &py, &pf)) {
            continue;
        }
        player_t *p, *tmp;
        player_t* pick = NULL;
        float     best = BOT_FORT_HELP * BOT_FORT_HELP;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (!p->is_bot || !_fort_free_bot(server, p, f->team)) {
                continue;
            }
            float dx = p->movement.position.x - ((float) px + 0.5f);
            float dy = p->movement.position.y - ((float) py + 0.5f);
            float d  = dx * dx + dy * dy;
            if (d < best && _fort_willing(server, p, FORT_ROLL_WATCH, fort_help_chance)) {
                best = d;
                pick = p;
            }
        }
        if (pick == NULL) {
            return;
        }
        f->posts[b]        = pick->id;
        f->post_reached[b] = 0;
        f->post_until[b]   = now + (uint64_t) FORT_WATCH_SECONDS * NANO_IN_SECOND;
        evt_log(pick, "bot_fort", "action=watch id=%u kind=%s post=%d cell=%d,%d,%d objective=%d,%d dist=%.1f", f->id,
                _fort_kind_name(f->kind), b, px, py, pf, f->watch[0], f->watch[1], sqrtf(best));
    }
}

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
               if the fort is shot open. Those who put a cell rest first: they
               go back to fighting */
            _fort_release_all(server, f, "complete");
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
            if (now >= f->expires && built * 10 >= f->count * 9) {
                /* nine tenths up and nobody put a cell for the whole build
                   time: what stands is the structure (a watchtower given up
                   at 44 of 45 for one step dug away took its watch with it).
                   The missing cells leave the plan; the rest is mended */
                int kept = 0;
                for (int i = 0; i < f->count; i++) {
                    fort_cell_t* c = &f->cells[i];
                    if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z)) {
                        f->cells[kept] = *c;
                        f->tries[kept] = 0;
                        kept++;
                    }
                }
                evt_log(NULL, "bot_fort", "action=settle id=%u kind=%s at=%d,%d built=%d/%u", f->id,
                        _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], built, f->count);
                f->count    = (uint8_t) kept;
                f->complete = 1;
                f->expires  = now;
                _fort_release_all(server, f, "settle");
                continue;
            }
            if (now >= f->expires) {
                /* the first cells left, in the structure's frame (u across,
                   v toward the threat, w over its base): which part the
                   builders could not reach */
                char missing[96] = "";
                int  len = 0, shown = 0;
                for (int i = 0; i < f->count && shown < 4; i++) {
                    fort_cell_t* c = &f->cells[i];
                    if (map_solid(&server->s_map.map, (uint16_t) c->x, (uint16_t) c->y, (uint16_t) c->z)) {
                        continue;
                    }
                    int dx = c->x - f->anchor[0], dy = c->y - f->anchor[1];
                    len += snprintf(missing + len, sizeof missing - (size_t) len, "%s%d/%d/%d", shown ? ";" : "",
                                    dx * f->rx + dy * f->ry, dx * f->fx + dy * f->fy, f->base - 1 - c->z);
                    shown++;
                }
                evt_log(NULL, "bot_fort", "action=expire id=%u kind=%s at=%d,%d built=%d/%u missing=%s", f->id,
                        _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], built, f->count, missing);
                _fort_release_all(server, f, "expire");
                memset(f, 0, sizeof *f);
                continue;
            }
        } else {
            _fort_fill_posts(server, f, now);
        }
        if (now >= f->next_log) {
            f->next_log = now + 5ull * NANO_IN_SECOND;
            evt_log(NULL, "bot_fort",
                    "action=status id=%u kind=%s at=%d,%d built=%d/%u complete=%u builders=%u,%u,%u,%u posts=%u,%u",
                    f->id, _fort_kind_name(f->kind), f->anchor[0], f->anchor[1], built, f->count, f->complete,
                    f->builders[0], f->builders[1], f->builders[2], f->builders[3], f->posts[0], f->posts[1]);
        }
    }
}

/* --- the bot: its stand and the one cell it puts --- */

/* one more failed claim of cell i: at FORT_CELL_TRIES it leaves the plan
   (the last cell takes its place, the claims follow it). Never the
   watchtower's top in its anchor column, where its watch stands. 1 when it
   was dropped. */
static uint8_t _fort_cell_tried(server_t* server, bot_fort_t* f, int i)
{
    (void) server;
    if (i < 0 || i >= f->count || ++f->tries[i] < FORT_CELL_TRIES) {
        return 0;
    }
    fort_cell_t* c = &f->cells[i];
    if (f->kind == BOT_FORT_WATCHTOWER && c->z <= f->base - _fort_rise(f->kind)) {
        return 0; /* the top and its parapet: they wait for the stair, they are the point of it */
    }
    int dx = c->x - f->anchor[0], dy = c->y - f->anchor[1];
    evt_log(NULL, "bot_fort", "action=drop_cell id=%u kind=%s cell=%d,%d,%d local=%d/%d/%d why=tries", f->id,
            _fort_kind_name(f->kind), c->x, c->y, c->z, dx * f->rx + dy * f->ry, dx * f->fx + dy * f->fy,
            f->base - 1 - c->z);
    int last = f->count - 1;
    for (int b = 0; b < BOT_FORT_BUILDERS; b++) {
        if (f->claim[b] == i) {
            f->claim[b] = -1;
        } else if (f->claim[b] == last) {
            f->claim[b] = (int8_t) i;
        }
    }
    f->cells[i] = f->cells[last];
    f->tries[i] = f->tries[last];
    f->count--;
    return 1;
}

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
            !gamemode_cell_editable(server, c->x, c->y, c->z))
        {
            f->claim[b] = -1;
            idx         = -1;
        } else if (now - f->claim_at[b] >
                   (uint64_t) (f->claim_high[b] ? FORT_CLAIM_HIGH_SECONDS : FORT_CLAIM_SECONDS) * NANO_IN_SECOND) {
            /* the claim ran out. With the builder beside the cell it is a
               cell nobody manages (its stand taken, out of the server's
               reach from every stand): a structure held open by one such
               cell waited the 90 s and was given up at 44 of 45 */
            float dx = (float) c->x + 0.5f - bot->movement.position.x;
            float dy = (float) c->y + 0.5f - bot->movement.position.y;
            f->claim[b] = -1;
            idx         = -1;
            if (dx * dx + dy * dy < 25.0f) {
                _fort_cell_tried(server, f, (int) (c - f->cells));
            }
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
    f->claim_high[b] = (uint8_t) (sf < f->base - 1);
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
    /* the server takes a block closer than five (Block.c truncates the
       distance to the cell's corner and refuses over 4): the stand is chosen
       at 3.8 from the cell's middle, and 3.8 from its corner here left a
       builder on a good stand for a tower's fifth level waiting there until
       its claim ran out */
    if (dx * dx + dy * dy + dz * dz > 4.5f * 4.5f) {
        return 0; /* still walking */
    }
    /* do not put it from the threat's side when the stand chosen is behind
       the cell: the reach (4.5) is met long before the stand when the bot
       comes from the enemy, and it then built the wall and stood in front
       of it. Walk round to the stand first. A claim that never gets there
       runs out like any other (FORT_CLAIM_SECONDS). */
    if (f->kind != BOT_FORT_RING) {
        float ahead = (bot->movement.position.x - ((float) c->x + 0.5f)) * (float) f->fx +
                      (bot->movement.position.y - ((float) c->y + 0.5f)) * (float) f->fy;
        int   sx, sy, sf;
        if (ahead > 0.7f && _fort_stand(server, f, c, &sx, &sy, &sf)) {
            float sa = ((float) sx - (float) c->x) * (float) f->fx + ((float) sy - (float) c->y) * (float) f->fy;
            if (sa <= 0.5f) {
                return 0;
            }
        }
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
        if (f->placed[b] < 255) {
            f->placed[b]++;
        }
        /* which side of the cell the builder stood on, along the threat:
           front = on the enemy's side, where the wall does not cover it */
        float ahead = (bot->movement.position.x - ((float) c->x + 0.5f)) * (float) f->fx +
                      (bot->movement.position.y - ((float) c->y + 0.5f)) * (float) f->fy;
        evt_log(bot, "bot_fort", "action=place id=%u kind=%s cell=%d,%d,%d blocks=%u exposed=%d ahead=%.1f", f->id,
                _fort_kind_name(f->kind), c->x, c->y, c->z, bot->blocks, ahead > 0.7f, ahead);
        /* its share is done: it goes back to the fight and a mate helps (a
           seventy cell watchtower kept one builder off the objective for
           minutes); alone on it, it finishes, or the site is given up near
           its end */
        uint8_t mates = 0;
        for (int o = 0; o < BOT_FORT_BUILDERS; o++) {
            mates += o != b && f->builders[o] != 0xFF;
        }
        if (f->placed[b] >= FORT_SHIFT_CELLS && mates > 0) {
            evt_log(bot, "bot_fort", "action=release id=%u slot=%d player=%u placed=%u why=share", f->id, b, bot->id,
                    f->placed[b]);
            _fort_rest(bot, bot->id, "share");
            f->builders[b] = 0xFF;
            f->placed[b]   = 0;
        }
    }
    return 1;
}

uint8_t bot_fort_cell_kept(uint8_t team, int x, int y, int z)
{
    if (!forts_enabled) {
        return 0;
    }
    for (int k = 0; k < BOT_FORTS; k++) {
        if (forts[k].used && forts[k].team == team && _fort_cell_at(&forts[k], x, y, z) >= 0) {
            return 1;
        }
    }
    return 0;
}

uint8_t bot_fort_post_goal(server_t* server, player_t* bot, float* x, float* y, int* floor, float* wx, float* wy)
{
    (void) server;
    if (!forts_enabled || bot == NULL) {
        return 0;
    }
    int k = _fort_post_of(bot->id);
    if (k < 0) {
        return 0;
    }
    bot_fort_t* f = &forts[k];
    for (int b = 0; b < BOT_FORT_POSTS; b++) {
        int px, py, pf;
        if (f->posts[b] != bot->id || !_fort_post_cell(f, b, &px, &py, &pf)) {
            continue;
        }
        *x     = (float) px + 0.5f;
        *y     = (float) py + 0.5f;
        *floor = pf;
        *wx    = (float) f->watch[0] + 0.5f;
        *wy    = (float) f->watch[1] + 0.5f;
        return 1;
    }
    return 0;
}

uint8_t bot_fort_manning(server_t* server, const player_t* bot)
{
    float x, y, wx, wy;
    int   floor;
    if (bot == NULL || bot->airborne || !bot_fort_post_goal(server, (player_t*) bot, &x, &y, &floor, &wx, &wy)) {
        return 0;
    }
    float dx = x - bot->movement.position.x, dy = y - bot->movement.position.y;
    return dx * dx + dy * dy <= 1.0f && abs(_bot_floor_cell(server, (player_t*) bot) - floor) <= 1;
}
