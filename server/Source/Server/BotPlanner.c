// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <modes/modes.h>
#include <Server/BotPlanner.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Server/MapStore.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The planner's copy of the map: a 64 bit word per column            */
/* ------------------------------------------------------------------ */

static uint64_t* cols;
static int       size_x, size_y;
static int       refresh_at; /* the next column planner_refresh copies */
/* a new map's copy is built a slice per tick (planner_refresh): built at
   once it took 20-36 ms of a tick on a 512 map */
#define PLANNER_BUILD_SLICE 16384
static int      build_at = -1; /* the next column to build, -1 when built */
static uint64_t build_began;

/* taken for reading by a search, for writing when the copy is rebuilt */
static pthread_rwlock_t map_lock = PTHREAD_RWLOCK_INITIALIZER;

/* a column's solid cells at once (map_column): a call per cell took 58 ms
   for a 512 map, on the tick when the map changes */
static uint64_t _column_of(map_store_t* map, int x, int y)
{
    return map_column(map, x, y);
}

static uint64_t _col(int x, int y)
{
    return __atomic_load_n(&cols[(size_t) y * size_x + x], __ATOMIC_RELAXED);
}

/* outside the map: solid (nobody walks off it); above the top: air; the
   bottom layer (63, the water) is always solid in the data */
static int _solid(int x, int y, int z)
{
    if (z < 0) {
        return 0;
    }
    if (z >= 64 || x < 0 || y < 0 || x >= size_x || y >= size_y) {
        return 1;
    }
    return (int) ((_col(x, y) >> z) & 1);
}

/* the smallest solid z of the column: its top surface */
static int _top(int x, int y)
{
    uint64_t w = _col(x, y);
    return w ? __builtin_ctzll(w) : 63;
}

void planner_cell(server_t* server, int x, int y, int z)
{
    if (cols == NULL || x < 0 || y < 0 || z < 0 || x >= size_x || y >= size_y || z >= 64) {
        return;
    }
    uint64_t  bit = 1ULL << z;
    uint64_t* w   = &cols[(size_t) y * size_x + x];
    if (map_solid(&server->s_map.map, (uint16_t) x, (uint16_t) y, (uint16_t) z)) {
        __atomic_fetch_or(w, bit, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(w, ~bit, __ATOMIC_RELAXED);
    }
}

static void _planner_built(void);

void planner_refresh(server_t* server, int n)
{
    if (cols == NULL) {
        return;
    }
    size_t total = (size_t) size_x * size_y;
    if (build_at >= 0) {
        size_t end = (size_t) build_at + PLANNER_BUILD_SLICE;
        if (end > total) {
            end = total;
        }
        for (size_t i = (size_t) build_at; i < end; i++) {
            __atomic_store_n(&cols[i], _column_of(&server->s_map.map, (int) (i % size_x), (int) (i / size_x)),
                             __ATOMIC_RELAXED);
        }
        build_at = end >= total ? -1 : (int) end;
        if (build_at < 0) {
            _planner_built();
        }
        return;
    }
    for (int k = 0; k < n; k++) {
        size_t i = (size_t) refresh_at;
        __atomic_store_n(&cols[i], _column_of(&server->s_map.map, (int) (i % size_x), (int) (i / size_x)),
                         __ATOMIC_RELAXED);
        refresh_at = (int) ((i + 1) % total);
    }
}

/* ------------------------------------------------------------------ */
/* The search (only ever run on the planner thread)                   */
/* ------------------------------------------------------------------ */

#define NAV_TABLE      (1 << 17) /* nodes a search may touch */
#define NAV_EXPAND_MAX 40000     /* nodes it may expand (2Fort base to base: ~10000) */
#define NAV_DROP_MAX   6         /* deeper drops hurt (Physics.c fall damage) */

static struct
{
    uint64_t key[NAV_TABLE]; /* 0 empty, else 1 + x | y << 16 | z << 32 */
    /* the search that wrote the slot: one of an older search reads as
       empty, so a search does not clear the 1 MB key table first */
    uint32_t gen[NAV_TABLE];
    float    g[NAV_TABLE];
    int32_t  parent[NAV_TABLE];
    uint8_t  act[NAV_TABLE];
    uint8_t  closed[NAV_TABLE];
    int32_t  heap[NAV_TABLE];
    float    heap_f[NAV_TABLE];
    int      heap_n;
    int      used;
    int      ban[3];
    uint8_t  stealth;
    uint8_t  cover; /* plan_request_t.cover */
    uint8_t  nodig; /* plan_request_t.nodig */
    uint8_t  deadly_water;
    uint8_t  leap;
    uint8_t  miner; /* plan_request_t.miner */
} nav;

static uint64_t _nav_key(int x, int y, int z)
{
    return 1ULL + ((uint64_t) x | ((uint64_t) y << 16) | ((uint64_t) z << 32));
}

static void _nav_unkey(uint64_t key, int* x, int* y, int* z)
{
    key -= 1;
    *x = (int) (key & 0xFFFF);
    *y = (int) ((key >> 16) & 0xFFFF);
    *z = (int) (key >> 32);
}

/* the slot of (x, y, z), created when absent; -1 when the table is full */
static uint32_t nav_gen; /* this search's number (nav.gen) */

static int32_t _nav_slot(int x, int y, int z)
{
    uint64_t key = _nav_key(x, y, z);
    uint32_t h   = (uint32_t) ((key * 0x9E3779B97F4A7C15ULL) >> 47) & (NAV_TABLE - 1);
    while (nav.gen[h] == nav_gen && nav.key[h] != 0) {
        if (nav.key[h] == key) {
            return (int32_t) h;
        }
        h = (h + 1) & (NAV_TABLE - 1);
    }
    if (nav.used >= NAV_TABLE * 3 / 4) {
        return -1;
    }
    nav.used++;
    nav.gen[h]    = nav_gen;
    nav.key[h]    = key;
    nav.g[h]      = 1e30f;
    nav.closed[h] = 0;
    return (int32_t) h;
}

static void _nav_push(int32_t slot, float f)
{
    int i = nav.heap_n++;
    while (i > 0) {
        int up = (i - 1) / 2;
        if (nav.heap_f[up] <= f) {
            break;
        }
        nav.heap[i]   = nav.heap[up];
        nav.heap_f[i] = nav.heap_f[up];
        i             = up;
    }
    nav.heap[i]   = slot;
    nav.heap_f[i] = f;
}

static int32_t _nav_pop(void)
{
    int32_t top  = nav.heap[0];
    int32_t last = nav.heap[--nav.heap_n];
    float   lf   = nav.heap_f[nav.heap_n];
    int     i    = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= nav.heap_n) {
            break;
        }
        if (c + 1 < nav.heap_n && nav.heap_f[c + 1] < nav.heap_f[c]) {
            c++;
        }
        if (nav.heap_f[c] >= lf) {
            break;
        }
        nav.heap[i]   = nav.heap[c];
        nav.heap_f[i] = nav.heap_f[c];
        i             = c;
    }
    nav.heap[i]   = last;
    nav.heap_f[i] = lf;
    return top;
}

/* room for a standing player over floor f: three free cells */
static int _room(int x, int y, int f)
{
    return !_solid(x, y, f - 1) && !_solid(x, y, f - 2) && !_solid(x, y, f - 3);
}

/* the server the map belongs to, for the cells nobody may edit */
static server_t* planner_server;

/* a cell a block may go in or the spade may open: not protected by the
   map's .txt or the mode. Without it the plans of Raindance's bases (two
   protected sectors) were refused one cell at a time, a new plan a second.
   Called from this thread: those rules are set when the map loads, and the
   only one that changes during a match (Arena's gates) is a flag; the main
   thread checks the finished plan again (_bot_take_plan) */
static int _editable(int x, int y, int z)
{
    return planner_server == NULL || gamemode_cell_editable(planner_server, x, y, z);
}

/* the cells over floor f the spade must open, -1 when it cannot (the dig is
   refused at 62 and below, and on cells nobody may edit) */
static int _dig_cost(int x, int y, int f)
{
    int n = 0;
    for (int z = f - 3; z <= f - 1; z++) {
        if (!_solid(x, y, z)) {
            continue;
        }
        if (z < 1 || z >= 62 || !_editable(x, y, z)) {
            return -1;
        }
        n++;
    }
    return n;
}

/* walking along a ledge or the water's edge: a body drifting round a
   corner fell off it (into 2Fort's channel, every few seconds in a match) */
static float _edge(int x, int y, int f)
{
    static const int sides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 4; k++) {
        int sx = x + sides[k][0], sy = y + sides[k][1];
        if (!_solid(sx, sy, f) && !_solid(sx, sy, f + 1) && _room(sx, sy, f)) {
            return 1.5f;
        }
    }
    return 0.0f;
}

/* sneaking: nothing solid over the three cells of the body, the sky is
   open and anyone on a roof or in a tower sees it */
/* Open ground: nothing at chest height in the 8 columns round it and no roof
   over the head. A player goes along walls and only runs across the open
   when there is no other way: every bot takes NAV_OPEN a step more there
   (a covered way up to ~1.8 times as long wins); the infiltrator's open sky
   costs far more */
#define NAV_OPEN 0.8f

static float _exposed(int x, int y, int f)
{
    if (nav.stealth) {
        return _top(x, y) >= f - 3 ? 5.0f : 0.0f;
    }
    if (!nav.cover || _top(x, y) < f - 3) {
        return 0.0f;
    }
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx || dy) && _solid(x + dx, y + dy, f - 2)) {
                return 0.0f;
            }
        }
    }
    return NAV_OPEN;
}

/* the spade's cost: sneaking, a tunnel is the point (a three cell dig 3,
   against 6 for a step under the open sky) */
static float _dig(int n)
{
    /* the Miner's dig: one swing clears what the spade needs three for, and
       sneaking a tunnel is as cheap as walking under a roof */
    if (nav.miner) {
        return nav.stealth ? 1.0f + 0.2f * (float) n : 2.5f + 1.0f * (float) n;
    }
    return nav.stealth ? 1.5f + 0.5f * (float) n : 4.0f + 2.5f * (float) n;
}

/* A Miner's tunnel goes under the ground, not through its skin: a dig
   whose opening (top cell head) has less than MINER_ROOF cells of ground
   over it costs MINER_SHALLOW more for each one missing, so its plans go
   down a ramp, along a few cells under the surface (under hills and
   mountains as they come) and up near the goal, and walk where no dig is
   needed. Cheap digs everywhere carved trenches into the surface: 61% of
   the Miners' digs on Raindance and 69% on Busan_Port had open sky or one
   block over them, which players read as griefing the map */
#define MINER_ROOF    3
#define MINER_SHALLOW 2.5f

static float _dig_at(int x, int y, int f, int n)
{
    float cost = _dig(n);
    if (nav.miner) {
        int roof = (f - 3) - _top(x, y); /* cells of ground over the opening */
        if (roof < MINER_ROOF) {
            cost += MINER_SHALLOW * (float) (MINER_ROOF - (roof < 0 ? 0 : roof));
        }
    }
    return cost;
}

/* Each level up counts 4 (a natural step costs 2, a pillar block 7), each
   level down 0.5. With 0.5 both ways a goal 40 levels up (Babel's intel on
   the platform at the top of the world) looked close, the search spent its
   whole budget on the ground and every plan ended under the platform */
#define NAV_BUILD_NEAR 16 /* columns from the goal blocks may be built up */
#define NAV_H_UP   4.0f
#define NAV_H_DOWN 0.5f

static float _h(int x, int y, int z, int gx, int gy, int gz)
{
    float dx = (float) abs(x - gx), dy = (float) abs(y - gy);
    float h  = dx > dy ? dx + 0.414f * dy : dy + 0.414f * dx;
    if (gz <= 0) {
        return h;
    }
    /* z grows downwards: z > gz is a goal above */
    return h + (z > gz ? NAV_H_UP * (float) (z - gz) : NAV_H_DOWN * (float) (gz - z));
}

static void _offer(int x, int y, int z, int32_t from, uint8_t act, float cost, int gx, int gy, int gz)
{
    if ((act == NAV_DIG && nav.nodig) || (nav.deadly_water && z >= 63)) {
        return;
    }
    if (x == nav.ban[0] && y == nav.ban[1] && z == nav.ban[2]) {
        return;
    }
    int32_t s = _nav_slot(x, y, z);
    if (s < 0 || nav.closed[s]) {
        return;
    }
    float g = nav.g[from] + cost;
    if (g >= nav.g[s]) {
        return;
    }
    nav.g[s]      = g;
    nav.parent[s] = from;
    nav.act[s]    = act;
    /* weighted a little: a slightly longer way found much faster. Sneaking
       costs far more than the distance, so its search is greedier, or it
       expanded its whole budget every time (36 ms a plan) */
    /* the estimate weighed up with the open ground's cost (NAV_OPEN): with
       it left at 1.3, Raindance's open plains made 4 times the nodes and 11%
       of the plans partial */
    /* Sneaking weighs only the distance across: the levels to climb count
       once. Weighed 3 times too, every level dug down into the ground looked
       12 dearer (Raindance's intel is up a tower), the search never went
       under the ground and its sneaking plans dug nothing (dig=0, partial
       at 20000 nodes): a body is out of sight 4 levels down */
    float est = _h(x, y, z, gx, gy, gz);
    if (nav.stealth) {
        float across = _h(x, y, z, gx, gy, 0);
        /* a Miner's by 4: the ramp down to its tunnel (MINER_ROOF) is dear,
           and at 3 the search spread over the ground before taking it
           (Busan_Port's Miner plans partial 63%, 38% at 4) */
        est          = (nav.miner ? 4.0f : 3.0f) * across + (est - across);
    } else {
        est *= nav.cover ? 1.7f : 1.3f;
    }
    _nav_push(s, g + est);
}

/* A node is a column and the floor cell a player stands on there (z grows
   downwards; the water layer is the floor of a swimmer). Steps between
   neighbours cost what doing them costs a player: walking, swimming, a jump
   up a step, a drop, and the steps that change the map (a bridge block, a
   stair block, a block under its own feet, a spade dig). Walking a little
   further beats a block, a block beats a dig, and the water costs most:
   a bot in it is easy prey and gets out only at a climbable shore */
static void _expand_plan(int32_t cur, int blocks, int gx, int gy, int gz)
{
    static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    int              x, y, f;
    _nav_unkey(nav.key[cur], &x, &y, &f);
    int     wz     = 63;
    int     lowest = 61; /* the deepest cell a block may go in */
    uint8_t wet    = f >= wz;
    uint8_t head   = !_solid(x, y, f - 4); /* room to jump */
    /* a Miner's cube dig clears the roof over its head as it digs a step up */
    uint8_t miner_up = nav.miner && !head && !wet && f - 4 >= 1 && _editable(x, y, f - 4);
    /* blocks up (a pillar, a stair block) only near the goal, as a player
       builds up to what it wants to reach: offered on every column of the
       way, they made the search three dimensional and it never got to
       Raindance's spiral stairs 100 cells off (partial plans, pillars and
       bridges in the open, out of blocks) */
    uint8_t near_build = gz <= 0 || (abs(x - gx) <= NAV_BUILD_NEAR && abs(y - gy) <= NAV_BUILD_NEAR);

    for (int d = 0; d < 8; d++) {
        int nx = x + dirs[d][0], ny = y + dirs[d][1];
        if (nx < 2 || ny < 2 || nx >= size_x - 2 || ny >= size_y - 2) {
            continue;
        }
        if (d >= 4) {
            /* diagonals only walk, and never cut a corner */
            if (_room(nx, ny, f) && _solid(nx, ny, f) && _room(x + dirs[d][0], y, f) && _room(x, y + dirs[d][1], f)) {
                _offer(nx, ny, f, cur, NAV_WALK, wet ? 4.2f : 1.414f + _edge(nx, ny, f) + _exposed(nx, ny, f), gx, gy,
                       gz);
            }
            continue;
        }
        /* sneaking: a step dug down into the ground, the way into a tunnel */
        if (nav.stealth && !wet && f + 1 <= lowest + 1 && _solid(nx, ny, f + 1)) {
            int n = _dig_cost(nx, ny, f + 1);
            if (n > 0) {
                _offer(nx, ny, f + 1, cur, NAV_DIG, _dig_at(nx, ny, f + 1, n) + _exposed(nx, ny, f + 1), gx, gy, gz);
            }
        }
        if (_room(nx, ny, f)) {
            if (_solid(nx, ny, f)) {
                _offer(nx, ny, f, cur, NAV_WALK, f >= wz ? 3.0f : 1.0f + _edge(nx, ny, f) + _exposed(nx, ny, f), gx,
                       gy, gz);
                /* a step up built on flat ground */
                if (!wet && head && near_build && blocks > 0 && f - 1 <= lowest && !_solid(nx, ny, f - 4) &&
                    _editable(nx, ny, f - 1)) {
                    _offer(nx, ny, f - 1, cur, NAV_STAIR, nav.stealth ? 10.0f : 6.0f, gx, gy, gz);
                }
                continue;
            }
            int z = f + 1;
            while (z < wz && z - f <= NAV_DROP_MAX && !_solid(nx, ny, z)) {
                z++;
            }
            if (z - f <= NAV_DROP_MAX) {
                /* into the water only when nothing better exists. A one
                   cell hole takes a body (0.9 wide) only when it is right
                   over its centre: the bot rocked over one for a minute */
                uint8_t wide = !_solid(nx + dirs[d][0], ny + dirs[d][1], f);
                _offer(nx, ny, z, cur, NAV_DROP,
                       1.0f + 0.3f * (float) (z - f) + (z >= wz ? 25.0f : 0.0f) + (wide ? 0.0f : 4.0f), gx, gy, gz);
            }
            if (!wet && blocks > 0 && f <= lowest && _editable(nx, ny, f)) {
                _offer(nx, ny, f, cur, NAV_BRIDGE, nav.stealth ? 9.0f : 5.0f, gx, gy, gz);
            }
            continue;
        }
        if (head && _solid(nx, ny, f - 1) && _room(nx, ny, f - 1)) {
            /* also out of the water: the shore at 62 (_bot_climbable_shore) */
            _offer(nx, ny, f - 1, cur, NAV_UP, 2.0f, gx, gy, gz);
            continue;
        }
        /* a Miner under a roof (its own tunnel is three cells high: no room
           to jump) goes up a step all the same: the dig in front of it is a
           3 x 3 x 3 cube that takes the roof over its head too. Without it
           a Miner in its tunnel could plan no way up, and one that fell into
           a pit or its own tunnel stood there with every plan partial */
        if (miner_up && _solid(nx, ny, f - 1) && _room(nx, ny, f - 1)) {
            _offer(nx, ny, f - 1, cur, NAV_DIG, 1.0f + _dig_at(nx, ny, f - 1, 1), gx, gy, gz);
            continue;
        }
        if (wet) {
            continue; /* nothing can be dug from the water */
        }
        /* the zombie's high jump: onto a ledge 2 to 4 up, or into a notch
           the spade cuts in the wall there, straight up from this cell (its
           column free over the jump). Notch over notch climbs a wall no
           step reaches (a house, the survivors on its roof). Dear: the jump
           comes back every GAME_ZOMBIE_LEAP_COOLDOWN, the bot waits for it */
        for (int k = 2; k <= nav.leap && f - k >= 1; k++) {
            if (_solid(x, y, f - k - 3) || !_solid(nx, ny, f - k)) {
                break;
            }
            /* up to GAME_ZOMBIE_TAP_JUMP_UP a normal jump: no wait */
            float up = k <= GAME_ZOMBIE_TAP_JUMP_UP ? 2.5f : 4.0f * (float) k;
            if (_room(nx, ny, f - k)) {
                _offer(nx, ny, f - k, cur, NAV_UP, up, gx, gy, gz);
                break;
            }
            int nk = _dig_cost(nx, ny, f - k);
            if (nk >= 0) {
                _offer(nx, ny, f - k, cur, NAV_DIG, up + _dig_at(nx, ny, f - k, nk), gx, gy, gz);
            }
        }
        int n = _dig_cost(nx, ny, f);
        if (n >= 0 && _solid(nx, ny, f)) {
            _offer(nx, ny, f, cur, NAV_DIG, _dig_at(nx, ny, f, n) + _exposed(nx, ny, f), gx, gy, gz);
        }
        /* a step dug into the wall, when going up */
        n = _dig_cost(nx, ny, f - 1);
        if ((head || miner_up) && n >= 0 && _solid(nx, ny, f - 1)) {
            _offer(nx, ny, f - 1, cur, NAV_DIG, 1.0f + _dig_at(nx, ny, f - 1, n) + _exposed(nx, ny, f - 1), gx, gy, gz);
        }
    }
    if (!wet && head && near_build && blocks > 0 && f - 1 <= lowest && _editable(x, y, f - 1)) {
        _offer(x, y, f - 1, cur, NAV_PILLAR, nav.stealth ? 11.0f : 7.0f, gx, gy, gz);
    }
}

/* With no full way within the budget, the way to the closest cell it
   reached, if that is closer than the start */
static void _search(const plan_request_t* rq, plan_result_t* out)
{
    uint64_t started = get_real_nanos();
    memset(out, 0, sizeof(*out));
    /* any floor from the top of the world down (above it is open sky:
       Babel's platform is floor 1; floors under 4 were refused, and a
       carrier on it never got a way down) */
    if (rq->sx < 2 || rq->sy < 2 || rq->sx >= size_x - 2 || rq->sy >= size_y - 2 || rq->sf < 1) {
        return;
    }
    if (++nav_gen == 0) {
        /* wrapped after 4 billion searches: start the numbers over */
        memset(nav.gen, 0, sizeof(nav.gen));
        nav_gen = 1;
    }
    nav.used    = 0;
    nav.heap_n  = 0;
    nav.stealth = rq->stealth;
    nav.cover   = rq->cover;
    nav.nodig   = rq->nodig;
    nav.deadly_water = rq->deadly_water;
    nav.leap         = rq->leap;
    nav.miner        = rq->miner;
    memcpy(nav.ban, rq->ban, sizeof(nav.ban));
    int32_t start     = _nav_slot(rq->sx, rq->sy, rq->sf);
    nav.g[start]      = 0.0f;
    nav.parent[start] = -1;
    nav.act[start]    = NAV_WALK;
    _nav_push(start, 0.0f);

    int32_t found = -1, best = start;
    float   best_h   = _h(rq->sx, rq->sy, rq->sf, rq->gx, rq->gy, rq->gz);
    int     budget   = rq->stealth ? NAV_EXPAND_MAX / 2 : NAV_EXPAND_MAX;
    int     expanded = 0;
    while (nav.heap_n > 0 && expanded < budget) {
        int32_t cur = _nav_pop();
        if (nav.closed[cur]) {
            continue;
        }
        nav.closed[cur] = 1;
        expanded++;
        int x, y, f;
        _nav_unkey(nav.key[cur], &x, &y, &f);
        int reach = rq->exact ? 0 : 1;
        if (abs(x - rq->gx) <= reach && abs(y - rq->gy) <= reach && (rq->gz <= 0 || abs(f - rq->gz) <= 2)) {
            found = cur;
            break;
        }
        float h = _h(x, y, f, rq->gx, rq->gy, rq->gz);
        if (h < best_h) {
            best_h = h;
            best   = cur;
        }
        _expand_plan(cur, rq->blocks, rq->gx, rq->gy, rq->gz);
    }
    out->expanded = expanded;
    int32_t end   = found >= 0 ? found : best;
    out->ms       = (float) (get_real_nanos() - started) / (float) NANO_IN_MILLI;
    if (end == start) {
        return;
    }
    /* the steps from the end back to the start, then reversed */
    int n = 0;
    for (int32_t s = end; s != start && s >= 0; s = nav.parent[s]) {
        n++;
    }
    int skip = n > PLAN_PATH_MAX ? n - PLAN_PATH_MAX : 0; /* a long way: its first part */
    int k    = n - skip;
    out->len = (uint16_t) k;
    for (int32_t s = end; s != start && s >= 0; s = nav.parent[s]) {
        if (skip > 0) {
            skip--;
            continue;
        }
        int x, y, z;
        _nav_unkey(nav.key[s], &x, &y, &z);
        k--;
        out->cell[k][0] = (int16_t) x;
        out->cell[k][1] = (int16_t) y;
        out->cell[k][2] = (int16_t) z;
        out->act[k]     = nav.act[s];
        out->counts[nav.act[s]]++;
        uint8_t was = nav.cover;
        nav.cover   = 1;
        out->open += nav.stealth ? 0 : _exposed(x, y, z) > 0.0f;
        nav.cover = was;
    }
    out->full = found >= 0;
    out->cost = nav.g[end];
}

/* ------------------------------------------------------------------ */
/* Requests and results, one slot per player id                       */
/* ------------------------------------------------------------------ */

static struct
{
    plan_request_t request;
    uint32_t       ticket;  /* of the latest request */
    uint8_t        pending; /* waiting for the thread */
    plan_result_t  result;
    uint32_t       done;    /* the ticket result is for */
    uint64_t       ready_at; /* SP_FAST: handed over from this game time on */
} slots[256];

/* SP_FAST: an answer is handed over this many ticks after the question, the
   thread's usual wait */
#define PLANNER_FAST_DELAY 2

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_wake = PTHREAD_COND_INITIALIZER;
static int             started_thread;
static int             ready;
static int             next_slot; /* round robin, so no bot waits behind another */

static void* _planner_thread(void* unused)
{
    (void) unused;
    static plan_result_t result;
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        int id = -1;
        while (id < 0) {
            for (int k = 0; k < 256; k++) {
                int s = (next_slot + k) & 255;
                if (slots[s].pending) {
                    id = s;
                    break;
                }
            }
            if (id < 0) {
                pthread_cond_wait(&queue_wake, &queue_lock);
            }
        }
        next_slot             = (id + 1) & 255;
        plan_request_t rq     = slots[id].request;
        uint32_t       ticket = slots[id].ticket;
        slots[id].pending     = 0;
        pthread_mutex_unlock(&queue_lock);

        pthread_rwlock_rdlock(&map_lock);
        _search(&rq, &result);
        pthread_rwlock_unlock(&map_lock);

        pthread_mutex_lock(&queue_lock);
        if (slots[id].ticket == ticket) { /* not replaced meanwhile */
            slots[id].result = result;
            slots[id].done   = ticket;
        }
        pthread_mutex_unlock(&queue_lock);
    }
    return NULL;
}

void planner_map_loaded(server_t* server)
{
    map_store_t* map   = &server->s_map.map;
    uint64_t  begin = get_real_nanos();
    pthread_rwlock_wrlock(&map_lock);
    planner_server = server;
    free(cols);
    cols  = NULL;
    ready = 0;
    if (map->size_z != 64) {
        LOG_WARNING("Bot planner: maps %u cells deep are not supported (64), bots walk without plans", map->size_z);
    } else {
        size_x     = map->size_x;
        size_y     = map->size_y;
        refresh_at = 0;
        /* zeroed (all air) until its slices are built: no plan is asked for
           before ready */
        cols = (uint64_t*) calloc((size_t) size_x * size_y, sizeof(uint64_t));
        if (cols == NULL) {
            LOG_ERROR("Bot planner: out of memory for a %dx%d map", size_x, size_y);
        } else {
            build_at    = 0;
            build_began = begin;
        }
    }
    pthread_rwlock_unlock(&map_lock);
    if (cols == NULL) {
        LOG_INFO("Bot planner: off");
    }
}

/* the last slice is in: plans may be asked for */
static void _planner_built(void)
{
    ready = 1;
    if (!started_thread) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, _planner_thread, NULL) != 0) {
            LOG_ERROR("Bot planner: could not start its thread, bots walk without plans");
            ready = 0;
            return;
        }
        pthread_detach(thread);
        started_thread = 1;
    }
    LOG_INFO("Bot planner: ready (map copy built over %.0f ms, %d columns a tick)",
             (double) (get_real_nanos() - build_began) / (double) NANO_IN_MILLI, PLANNER_BUILD_SLICE);
}

int planner_ready(void)
{
    return ready;
}

uint32_t planner_submit(uint8_t id, const plan_request_t* request)
{
    pthread_mutex_lock(&queue_lock);
    uint32_t ticket = ++slots[id].ticket;
    if (ticket == 0) {
        ticket = ++slots[id].ticket; /* 0 means none to the caller */
    }
    slots[id].request = *request;
    if (get_fast_mode()) {
        /* SP_FAST: searched here, on the tick, and handed over exactly
           PLANNER_FAST_DELAY ticks later. On the thread both its answer's
           tick and the map copy it read (the tick edits it meanwhile)
           depended on the scheduler, and the same seed played another match */
        static plan_result_t result;
        pthread_rwlock_rdlock(&map_lock);
        _search(request, &result);
        pthread_rwlock_unlock(&map_lock);
        slots[id].result   = result;
        slots[id].done     = ticket;
        slots[id].ready_at = get_nanos() + PLANNER_FAST_DELAY * (uint64_t) (NANO_60TPS);
        pthread_mutex_unlock(&queue_lock);
        return ticket;
    }
    slots[id].pending = 1;
    pthread_cond_signal(&queue_wake);
    pthread_mutex_unlock(&queue_lock);
    return ticket;
}

int planner_poll(uint8_t id, uint32_t ticket, plan_result_t* out)
{
    pthread_mutex_lock(&queue_lock);
    int done = slots[id].done == ticket && (!get_fast_mode() || get_nanos() >= slots[id].ready_at);
    if (done) {
        *out           = slots[id].result;
        slots[id].done = 0;
    }
    pthread_mutex_unlock(&queue_lock);
    return done;
}
