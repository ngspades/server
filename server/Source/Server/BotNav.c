// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    The way to a goal: the planned way (BotPlanner.c) followed step by step,
    the fallback that makes its own way, water, and turning the body toward
    where it goes.
*/

#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <modes/modes.h>
#include <Server/Classes.h>
#include <Server/BotProjects.h>

static uint8_t _bot_step_behind(const bot_ai_t* ai, const player_t* bot, int k);

/* Water is a trap for a bot: it slows movement, it can hurt when the gamemode
   enables it and blocks cannot be placed there. It is the bottom map layer,
   which physics treats as empty, so a column whose first solid block is that
   layer is open water (is_water_column). The wade flag is still checked as
   the runtime truth, e.g. when the bot falls into a pit down to the sea. */
uint8_t _bot_is_wet(const player_t* bot)
{
    return bot->wade;
}

uint8_t _bot_column_usable(server_t* server, uint16_t x, uint16_t y, int* floor_out)
{
    if (x < 3 || y < 3 || x >= server->s_map.map.size_x - 3 || y >= server->s_map.map.size_y - 3) {
        return 0; /* map edge: the floor scan runs off the map */
    }
    /* floor is the first solid block scanning downwards, and it must leave
       room above for the bot plus the two wall cells */
    for (int z = 0; z < (int) server->s_map.map.size_z; z++) {
        if (map_solid(&server->s_map.map, x, y, (uint16_t) z)) {
            if (z < 4) {
                return 0; /* too close to the ceiling */
            }
            if (z >= (int) server->s_map.map.size_z - 1) {
                return 0; /* the water plane */
            }
            for (int k = 1; k <= 3; k++) {
                if (map_solid(&server->s_map.map, x, y, (uint16_t) (z - k))) {
                    return 0; /* no room to stand and build */
                }
            }
            if (floor_out) {
                *floor_out = z;
            }
            return 1;
        }
    }
    return 0;
}

/* Nearest dry, standable column around the bot, ring by ring. */
uint8_t _bot_nearest_dry(server_t* server, player_t* bot, float* x_out, float* y_out)
{
    for (uint8_t radius = 3; radius <= 64; radius += 3) {
        uint8_t steps = 8 + radius / 2;
        for (uint8_t dir = 0; dir < steps; dir++) {
            float angle = (float) dir * 2.0f * (float) M_PI / (float) steps;
            float x     = bot->movement.position.x + cosf(angle) * (float) radius;
            float y     = bot->movement.position.y + sinf(angle) * (float) radius;
            if (x < 0.0f || y < 0.0f) {
                continue;
            }
            if (_bot_column_usable(server, (uint16_t) x, (uint16_t) y, NULL)) {
                *x_out = x;
                *y_out = y;
                return 1;
            }
        }
    }
    return 0;
}

void _bot_pick_objective(server_t* server, player_t* bot, bot_ai_t* ai)
{
    /* the mode says where the fight is: the enemy base in CTF, the enemy's
       spawn otherwise (the arena used to send bots to a stale base at 0,0) */
    vector3f_t base = {0, 0, 0};
    gamemode_patrol_anchor(server, bot, &base.x, &base.y);

    /* Bots used to walk straight into the sea: prefer a dry objective and only
       fall back to the base area when every attempt is water. */
    for (uint8_t attempt = 0; attempt < 16; attempt++) {
        float x = base.x + (float) ((int) (gen_rand(&server->rand) * 25.0) - 12);
        float y = base.y + (float) ((int) (gen_rand(&server->rand) * 25.0) - 12);

        if (x < 4.0f || y < 4.0f || x > (float) server->s_map.map.size_x - 4.0f
            || y > (float) server->s_map.map.size_y - 4.0f) {
            continue;
        }
        if (!_bot_column_usable(server, (uint16_t) x, (uint16_t) y, NULL)) {
            continue; /* water, map edge or no headroom */
        }
        ai->obj_x = x;
        ai->obj_y = y;
        evt_log(bot, "bot_objective", "src=enemy-base obj=%.1f,%.1f base=%.1f,%.1f", x, y, base.x, base.y);
        return;
    }

    /* Nothing near the enemy base worked: look around the bot itself so it
       always has a sane, walkable destination instead of a random corner. */
    if (_bot_nearest_dry(server, bot, &ai->obj_x, &ai->obj_y)) {
        evt_log(bot, "bot_objective", "src=nearest-dry obj=%.1f,%.1f base=%.1f,%.1f", ai->obj_x, ai->obj_y, base.x,
                base.y);
        return;
    }

    /* Last resort: stay where we are rather than walk into a wall. */
    ai->obj_x = bot->movement.position.x;
    ai->obj_y = bot->movement.position.y;
    evt_log(bot, "bot_objective", "src=stay obj=%.1f,%.1f base=%.1f,%.1f", ai->obj_x, ai->obj_y, base.x, base.y);
}

/* Walk keys toward (dx, dy) in the bot's own frame without turning: forward,
   back or sideways, like a player stepping back onto a ledge */
static void _bot_step_toward(player_t* bot, float dx, float dy)
{
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return;
    }
    float hx = bot->movement.forward_orientation.x, hy = bot->movement.forward_orientation.y;
    float sx = bot->movement.strafe_orientation.x, sy = bot->movement.strafe_orientation.y;
    float a = (dx * hx + dy * hy) / len, b = (dx * sx + dy * sy) / len;
    bot->move_forward   = a > 0.38f;
    bot->move_backwards = a < -0.38f;
    bot->move_right     = b > 0.38f;
    bot->move_left      = b < -0.38f;
    bot->sprinting      = 0;
}

/* Simple wall probe straight ahead: 2 = blocked, 0 = walkable. */
static uint8_t _bot_wall_ahead(server_t* server, player_t* bot, float angle)
{
    float px = bot->movement.position.x + cosf(angle) * 2.0f;
    float py = bot->movement.position.y + sinf(angle) * 2.0f;
    float pz = bot->movement.position.z;

    if (px < 2 || px >= server->s_map.map.size_x - 2 || py < 2 || py >= server->s_map.map.size_y - 2) {
        return 2;
    }
    int z = (int) pz;
    if (z < 0 || z + 2 >= server->s_map.map.size_z) {
        return 0;
    }
    if (map_solid(&server->s_map.map, (uint16_t) px, (uint16_t) py, (uint16_t) z) &&
        map_solid(&server->s_map.map, (uint16_t) px, (uint16_t) py, (uint16_t) (z + 1)))
    {
        return 2;
    }
    return 0;
}

/* Is the ground a few steps along this heading dry? Probing at several
   distances catches a shoreline before the bot is already wading. */
static uint8_t _bot_dry_ahead(server_t* server, player_t* bot, float angle)
{
    for (float d = 2.0f; d <= 6.0f; d += 2.0f) {
        float px = bot->movement.position.x + cosf(angle) * d;
        float py = bot->movement.position.y + sinf(angle) * d;
        if (px < 0.0f || py < 0.0f) {
            return 0;
        }
        if (is_water_column(server, (uint16_t) px, (uint16_t) py)) {
            return 0;
        }
    }
    return 1;
}

/* Heading towards (x, y) that stays on land: the straight line when it is dry
   and open, otherwise the smallest detour (never turning back). When every
   forward heading is water the bot has to wade, e.g. to cross to another
   island, so the straight line is kept: land is preferred, not mandatory. */
static float _bot_steer(server_t* server, player_t* bot, bot_ai_t* ai, float x, float y)
{
    static const float offsets[] = {0.0f, 0.4f, 0.8f, 1.2f, (float) M_PI_2};
    float              want      = atan2f(y - bot->movement.position.y, x - bot->movement.position.x);
    float              side      = ai->strafe_dir ? 1.0f : -1.0f;

    for (uint8_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        for (int s = 0; s < (i == 0 ? 1 : 2); s++) {
            float angle = want + offsets[i] * (s == 0 ? side : -side);
            if (_bot_dry_ahead(server, bot, angle) && _bot_wall_ahead(server, bot, angle) != 2) {
                return angle;
            }
        }
    }
    if (_bot_wall_ahead(server, bot, want) == 2) {
        return want + side * (float) M_PI_2; /* blocked and no dry way: slide along */
    }
    return want;
}

/* One step of getting through, the way a player does it, toward heading
   angle (snapped to the nearest axis: the cell in front, never a corner):
   - a wall: the spade's three block dig opens a passage, at our level, or
     one block up when the goal is higher (steps climbing out of a pit);
   - a one block step: walk up to it and jump;
   - water or a drop of 3+ in front: a bridge block at our level;
   - open space while the goal is higher: a step block (on a support block
     when there is nothing under it).
   Blocks cannot go at 62 or 63 (the water level): where the way ahead needs
   one there, a bot that must go up digs a staircase into the nearest wall
   instead, and otherwise stays put rather than walk into the water.
   goal_floor 0: no height to reach. */
uint8_t _bot_clear_path(server_t* server, player_t* bot, bot_ai_t* ai, float angle, int goal_floor)
{
    if (ai->pillar_floor != 0 && _bot_pillar_up(server, bot, ai, goal_floor) == 1) {
        return BOT_PATH_ACTED; /* a pillar jump in progress */
    }
    if (bot->airborne) {
        return _bot_is_wet(bot) ? BOT_PATH_WALK : BOT_PATH_WAIT;
    }
    /* a bridge project this bot owns or partners: place its next cell */
    if (bot_project_step(server, bot, goal_floor)) {
        return BOT_PATH_ACTED;
    }
    map_store_t* map = &server->s_map.map;
    int       dx = 0, dy = 0;
    if (fabsf(cosf(angle)) >= fabsf(sinf(angle))) {
        dx = cosf(angle) > 0.0f ? 1 : -1;
    } else {
        dy = sinf(angle) > 0.0f ? 1 : -1;
    }
    int cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    int ax = cx + dx, ay = cy + dy;
    int fz = _bot_floor_cell(server, bot);
    if (ax < 2 || ay < 2 || ax >= map->size_x - 2 || ay >= map->size_y - 2 || fz < 5) {
        return BOT_PATH_WALK;
    }
    uint8_t s1       = map_solid(map, ax, ay, fz - 1);
    uint8_t s2       = map_solid(map, ax, ay, fz - 2);
    uint8_t s3       = map_solid(map, ax, ay, fz - 3);
    uint8_t rising   = goal_floor > 0 && goal_floor < fz - 1;
    int     lowest   = map->size_z - 3; /* the deepest cell a block may go in */

    if (s1 && !s2 && !s3) {
        /* one block up: a jump climbs it, but the physics' autoclimb only
           looks at the cell in front once the body touches it. A bot in the
           water jumped from 0.05 short of the wall and never got out: walk
           up to it first, then jump */
        float gap = dx > 0   ? (float) ax - (bot->movement.position.x + 0.45f)
                    : dx < 0 ? bot->movement.position.x - 0.45f - (float) (ax + 1)
                    : dy > 0 ? (float) ay - (bot->movement.position.y + 0.45f)
                             : bot->movement.position.y - 0.45f - (float) (ay + 1);
        if (gap < 0.1f) {
            bot->jumping = 1;
        }
        return BOT_PATH_WALK;
    }
    if (s1 || s2 || s3) {
        /* a wall: open it at our feet, or one up when the goal is higher */
        return _bot_dig(server, bot, ai, ax, ay, (rising && s1) ? fz - 3 : fz - 2, rising, goal_floor);
    }

    /* ground in front: walk on. Getting closer comes before getting higher:
       raising a pillar on flat ground and walking off it made bots climb
       and fall in a loop (198 pillars in a match, no intel) */
    uint8_t floor_ahead = map_solid(map, ax, ay, fz);
    if (floor_ahead) {
        return BOT_PATH_WALK;
    }
    int depth = 0;
    while (fz + depth < map->size_z - 1 && !map_solid(map, ax, ay, fz + depth)) {
        depth++;
    }
    uint8_t water_ahead = is_water_column(server, ax, ay);
    if (!rising && depth < 3 && !water_ahead) {
        return BOT_PATH_WALK; /* a small drop: just walk down */
    }
    /* water or a gap in front and the goal well above: go up right here
       (a shore at the bottom of a pit: nothing to bridge to) */
    if (goal_floor > 0 && goal_floor < fz - 2 && !_bot_is_wet(bot) && _bot_pillar_up(server, bot, ai, goal_floor) == 1) {
        return BOT_PATH_ACTED;
    }
    /* a bridge at our level */
    int z = fz;
    if (z <= lowest && bot->blocks > 0 && !_bot_placeable(server, ax, ay, z) && _bot_placeable(server, cx, cy, z)) {
        /* nothing for it to rest on: the bot stands on the edge of the next
           column, and the cell under its centre is empty. Fill that first,
           it rests on the block under the bot's feet */
        _bot_set_tool(server, bot, TOOL_BLOCK);
        _bot_face(bot, cx + 0.5f, cy + 0.5f, (float) z);
        uint8_t ok = _bot_place_block(server, bot, (uint32_t) cx, (uint32_t) cy, (uint32_t) z) ||
                     _bot_block_action(server, bot, BLOCKACTION_BUILD, cx, cy, z);
        evt_log(bot, "bot_path", "action=support cell=%d,%d,%d ok=%u blocks=%u", cx, cy, z, ok, bot->blocks);
        if (ok) {
            return BOT_PATH_ACTED;
        }
    }
    if (z <= lowest && bot->blocks > 0) {
        if (!floor_ahead && bot_project_create_bridge(server, bot, ax, ay, z, dx, dy)) {
            return BOT_PATH_ACTED; /* two bots now share the crossing */
        }
        _bot_set_tool(server, bot, TOOL_BLOCK);
        _bot_face(bot, ax + 0.5f, ay + 0.5f, (float) z);
        uint8_t ok = _bot_place_block(server, bot, (uint32_t) ax, (uint32_t) ay, (uint32_t) z);
        evt_log(bot, "bot_path", "action=%s cell=%d,%d,%d ok=%u blocks=%u goal_floor=%d",
                floor_ahead ? "step" : "bridge", ax, ay, z, ok, bot->blocks, goal_floor);
        if (ok) {
            return BOT_PATH_ACTED;
        }
    }
    /* nothing to build on this way (the water level, no blocks): going up,
       dig a staircase into a wall beside us, the side nearest the goal */
    if (rising) {
        static const int sides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        int   best = -1;
        float best_turn = 10.0f;
        for (int k = 0; k < 4; k++) {
            int wx = cx + sides[k][0], wy = cy + sides[k][1];
            if (!map_solid(map, wx, wy, fz - 1) || !map_solid(map, wx, wy, fz - 2)) {
                continue; /* not a wall */
            }
            float turn = fabsf(remainderf(atan2f((float) sides[k][1], (float) sides[k][0]) - angle, 2.0f * (float) M_PI));
            if (turn < best_turn) {
                best_turn = turn;
                best      = k;
            }
        }
        if (best >= 0) {
            return _bot_dig(server, bot, ai, cx + sides[best][0], cy + sides[best][1], fz - 3, rising, goal_floor);
        }
    }
    if (bot->blocks < 10 && z <= lowest && _bot_harvest(server, bot)) {
        return BOT_PATH_ACTED; /* blocks for the bridge or the steps */
    }
    evt_log(bot, "bot_path", "action=stop cell=%d,%d,%d why=%s", ax, ay, z,
            !bot->blocks ? "no_blocks" : z > lowest ? "water_level" : "unplaceable");
    return BOT_PATH_STOP;
}

/* The way ahead is closed and nothing can be built or dug there: try from
   somewhere else, sideways for a moment (it stood still forever before) */
void _bot_start_detour(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    if (ai->detour_until > now) {
        return;
    }
    ai->detour_turn  = (gen_rand(&server->rand) < 0.5 ? 1.0f : -1.0f) * (float) M_PI_2;
    ai->detour_until = now + NANO_IN_MILLI * 1500;
    evt_log(bot, "bot_path", "action=detour turn=%+.0f", ai->detour_turn * 180.0f / (float) M_PI);
}

/* A water cell a player can swim through: the water layer under it and
   three free cells above it (a tunnel under the map counts). */
static uint8_t _bot_swimmable(server_t* server, int x, int y)
{
    map_store_t* map = &server->s_map.map;
    int       wz  = map->size_z - 1;
    if (x < 2 || y < 2 || x >= map->size_x - 2 || y >= map->size_y - 2) {
        return 0;
    }
    return !map_solid(map, x, y, wz - 1) && !map_solid(map, x, y, wz - 2) &&
           !map_solid(map, x, y, wz - 3);
}

/* A shore the physics lets a swimmer climb: land one block above the water
   (cell 62) with room to stand on it, next to a water cell with room to jump
   (a fourth free cell: a low roof stops the jump short of the step). Steps
   cannot be built at 62 or 63 nor dug below 62, so from the water this is
   the only way out; 2Fort's water tunnels have such shores at their ends. */
static uint8_t _bot_climbable_shore(server_t* server, int wx, int wy, int lx, int ly)
{
    map_store_t* map = &server->s_map.map;
    int       wz  = map->size_z - 1;
    if (lx < 2 || ly < 2 || lx >= map->size_x - 2 || ly >= map->size_y - 2) {
        return 0;
    }
    return map_solid(map, lx, ly, wz - 1) && !map_solid(map, lx, ly, wz - 2) &&
           !map_solid(map, lx, ly, wz - 3) && !map_solid(map, lx, ly, wz - 4) &&
           !map_solid(map, wx, wy, wz - 4);
}

/* Breadth first through the water around the bot to the nearest climbable
   shore. Fills ai->route with the water cells to swim through and obj_x/y
   with the land cell; 0 when there is none within BOT_ROUTE_RADIUS. */
uint8_t _bot_water_route(server_t* server, player_t* bot, bot_ai_t* ai)
{
    enum { SIDE = 2 * BOT_ROUTE_RADIUS + 1 };
    static int16_t parent[SIDE * SIDE];
    static int16_t queue[SIDE * SIDE];
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int ox = (int) bot->movement.position.x - BOT_ROUTE_RADIUS;
    int oy = (int) bot->movement.position.y - BOT_ROUTE_RADIUS;
    for (int k = 0; k < SIDE * SIDE; k++) {
        parent[k] = -1;
    }
    int head = 0, tail = 0, start = BOT_ROUTE_RADIUS * SIDE + BOT_ROUTE_RADIUS;
    parent[start] = (int16_t) start;
    queue[tail++] = (int16_t) start;
    while (head < tail) {
        int cur = queue[head++];
        int cx = cur % SIDE + ox, cy = cur / SIDE + oy;
        for (int d = 0; d < 4; d++) {
            int nx = cx + dirs[d][0], ny = cy + dirs[d][1];
            if (_bot_climbable_shore(server, cx, cy, nx, ny)) {
                /* walk the parents back from cur: the route, reversed */
                int16_t cells[BOT_ROUTE_MAX][2];
                int     n = 0;
                for (int c = cur; n < BOT_ROUTE_MAX; c = parent[c]) {
                    cells[n][0] = (int16_t) (c % SIDE + ox);
                    cells[n][1] = (int16_t) (c / SIDE + oy);
                    n++;
                    if (c == start) {
                        break;
                    }
                }
                for (int k = 0; k < n; k++) {
                    ai->route[k][0] = cells[n - 1 - k][0];
                    ai->route[k][1] = cells[n - 1 - k][1];
                }
                ai->route_len    = (uint16_t) n;
                ai->route_i      = 0;
                ai->obj_x        = nx + 0.5f;
                ai->obj_y        = ny + 0.5f;
                ai->escape_floor = server->s_map.map.size_z - 2;
                return 1;
            }
            int lx = nx - ox, ly = ny - oy;
            if (lx < 0 || ly < 0 || lx >= SIDE || ly >= SIDE || parent[ly * SIDE + lx] >= 0 ||
                !_bot_swimmable(server, nx, ny))
            {
                continue;
            }
            parent[ly * SIDE + lx] = (int16_t) cur;
            queue[tail++]          = (int16_t) (ly * SIDE + lx);
        }
    }
    return 0;
}

/* Walks towards (x, y) on land as far as possible. */
void _bot_walk_to(server_t* server, player_t* bot, bot_ai_t* ai, float x, float y)
{
    float angle = _bot_steer(server, bot, ai, x, y);
    float direct = atan2f(y - bot->movement.position.y, x - bot->movement.position.x);
    float off    = fabsf(remainderf(angle - direct, 2.0f * (float) M_PI));
    ai->move_src = off > 0.3f ? "steer" : "walk";
    _bot_face(bot, bot->movement.position.x + cosf(angle), bot->movement.position.y + sinf(angle),
              bot->movement.position.z);
    bot->move_forward = 1;
}

/* Strafe along the world direction (wx, wy) while facing wherever the bot is
   already looking: move_right follows +strafe_orientation in physics. */
void _bot_strafe_world(player_t* bot, float wx, float wy)
{
    float dot = wx * bot->movement.strafe_orientation.x + wy * bot->movement.strafe_orientation.y;
    if (dot > 0.0f) {
        bot->move_right = 1;
    } else {
        bot->move_left = 1;
    }
}

/* ground at the bot's own floor, one step of (nx, ny) away: the step off
   another body is only taken where it can stand (beyond the map edge it is
   no ground either, and libmapvxl reads past its blocks there) */
static uint8_t _bot_space_ground(server_t* server, player_t* bot, float nx, float ny, int fz)
{
    int x = (int) (bot->movement.position.x + nx * 0.8f);
    int y = (int) (bot->movement.position.y + ny * 0.8f);
    if (x < 0 || y < 0 || x >= (int) server->s_map.map.size_x || y >= (int) server->s_map.map.size_y) {
        return 0;
    }
    return _nav_solid(&server->s_map.map, x, y, fz);
}

/* Players have no bodies to each other (in Ace of Spades they walk through
   one another), so nothing in the physics keeps two bots that want the same
   spot out of each other's cell: in the arena a whole team spawned on one
   cell, and both teams' bots ended a round standing inside each other at
   the one place they all headed for. A bot with another player inside
   BOT_PERSONAL_SPACE turns its step away from it, and a bot standing still
   steps off. Only onto ground at its own floor: a bot on a pillar or at the
   edge of a drop stays where it is. */
void _bot_keep_space(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    /* SP_BOT_SPACE=0 turns it off for a measurement */
    static int space_off = -1;
    if (space_off < 0) {
        space_off = getenv("SP_BOT_SPACE") != NULL && !strcmp(getenv("SP_BOT_SPACE"), "0");
    }
    if (space_off) {
        return;
    }
    /* in the air, in the water, climbing or keeping the terrain: the step
       the think chose is the only safe one */
    if (bot->airborne || ai->escaping || ai->goal_climb || ai->pillar_floor != 0 || ai->goal_keep) {
        return;
    }
    /* a bot holding a post (the guard, the overwatch, a camper, a forward
       post) stays on it: the cover was picked for that cell */
    if (ai->camper || ai->role == GM_BOT_DEFEND || (ai->goal_intent & GM_INTENT_POST)) {
        return;
    }
    player_t *p, *tmp, *close = NULL;
    float     best = BOT_PERSONAL_SPACE * BOT_PERSONAL_SPACE;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == bot || p->state != STATE_READY || !p->alive) {
            continue;
        }
        float dz = bot->movement.position.z - p->movement.position.z;
        if (dz > 1.6f || dz < -1.6f) {
            continue; /* a bot above or below on a staircase is no crowd */
        }
        float dx = bot->movement.position.x - p->movement.position.x;
        float dy = bot->movement.position.y - p->movement.position.y;
        float d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best  = d2;
            close = p;
        }
    }
    if (close == NULL) {
        return;
    }
    float ax = bot->movement.position.x - close->movement.position.x;
    float ay = bot->movement.position.y - close->movement.position.y;
    float al = sqrtf(ax * ax + ay * ay);
    if (al < 0.05f) {
        /* right on top of it: aside, to the bot's own right */
        ax = bot->movement.strafe_orientation.x;
        ay = bot->movement.strafe_orientation.y;
        al = sqrtf(ax * ax + ay * ay);
        if (al < 0.05f) {
            return;
        }
    }
    ax /= al;
    ay /= al;
    int fz = _bot_floor_cell(server, bot);

    /* walking into it (both after the same enemy, a queue on the way): the
       step is given up and it goes to the step off below, or the two march
       on into each other's cell */
    if (sqrtf(best) < BOT_PERSONAL_STOP) {
        ai->move_wx = 0.0f;
        ai->move_wy = 0.0f;
    }
    if (ai->move_wx != 0.0f || ai->move_wy != 0.0f) {
        /* on its way: the same step, turned away from the body */
        float nx = ai->move_wx + ax * 1.2f, ny = ai->move_wy + ay * 1.2f;
        float nl = sqrtf(nx * nx + ny * ny);
        if (nl < 0.01f) {
            return;
        }
        nx /= nl;
        ny /= nl;
        if (!_bot_space_ground(server, bot, nx, ny, fz)) {
            return; /* the way round it leaves the ground it stands on */
        }
        ai->move_wx = nx;
        ai->move_wy = ny;
        if (now >= ai->space_logged) {
            ai->space_logged = now + _bot_secs(5.0f);
            evt_log(bot, "bot_space", "action=slide other=%u dist=%.2f", close->id, sqrtf(best));
        }
        return;
    }

    /* standing on it: a short step off, not every think (two bots would
       shuffle back and forth) */
    if (now < ai->space_step_until) {
        ai->move_wx = ai->space_dx;
        ai->move_wy = ai->space_dy;
        return;
    }
    if (now < ai->space_next) {
        return;
    }
    if (!_bot_space_ground(server, bot, ax, ay, fz)) {
        return;
    }
    ai->space_dx         = ax;
    ai->space_dy         = ay;
    ai->space_step_until = now + NANO_IN_MILLI * 300;
    ai->space_next       = now + _bot_secs(0.6f);
    ai->move_wx          = ax;
    ai->move_wy          = ay;
    evt_log(bot, "bot_space", "action=step other=%u dist=%.2f", close->id, sqrtf(best));
}

/* The game mode's objective, as a destination (CTF: take the intel, carry it
   home, hunt the carrier; gamemode_bot_goal). Pickup and capture happen
   exactly like for a human, so the bot only has to stand on the right cell.
   Returns 0 when the mode has no objective for it right now. */
/* ------------------------------------------------------------------ */
/* The way to the goal: a search over the cells a player can stand in  */
/* ------------------------------------------------------------------ */

/* The search itself runs on the planner's thread (BotPlanner.c); these
   check a step on the real map while the bot walks its plan */
static const char* nav_names[] = {"walk", "drop", "up", "bridge", "stair", "dig", "pillar"};

uint8_t _nav_solid(map_store_t* map, int x, int y, int z)
{
    if (z < 0) {
        return 0;
    }
    /* outside the map: no cell. A bot that walked out of the world (the
       boundary is open) would read past the map and crash the server */
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    return z >= map->size_z || map_solid(map, x, y, z);
}

/* room for a standing player over floor f: three free cells */
uint8_t _nav_room(map_store_t* map, int x, int y, int f)
{
    return !_nav_solid(map, x, y, f - 1) && !_nav_solid(map, x, y, f - 2) && !_nav_solid(map, x, y, f - 3);
}

/* floor f in column x, y is next to a drop of three cells or more (not
   counting the sea, a fall into it does not hurt) */
uint8_t _nav_edge(map_store_t* map, int x, int y, int f)
{
    if (f >= map->size_z - 4) {
        return 0;
    }
    for (int ny = -1; ny <= 1; ny++) {
        for (int nx = -1; nx <= 1; nx++) {
            if ((nx || ny) && !_nav_solid(map, x + nx, y + ny, f) && !_nav_solid(map, x + nx, y + ny, f + 1) &&
                !_nav_solid(map, x + nx, y + ny, f + 2))
            {
                return 1;
            }
        }
    }
    return 0;
}

/* the cells over floor f the spade must open, -1 when it cannot: the dig
   is refused at 62 and below, and on cells the mode protects */
static int _nav_dig_cost(server_t* server, int x, int y, int f)
{
    map_store_t* map = &server->s_map.map;
    int       n   = 0;
    for (int z = f - 3; z <= f - 1; z++) {
        if (!_nav_solid(map, x, y, z)) {
            continue;
        }
        if (z < 1 || z >= map->size_z - 2 || !gamemode_cell_editable(server, x, y, z)) {
            return -1;
        }
        n++;
    }
    return n;
}

static uint8_t _bot_take_plan(server_t* server, player_t* bot, bot_ai_t* ai, const plan_result_t* r, uint64_t now)
{
    float wait = (float) (now - ai->plan_asked) / (float) NANO_IN_MILLI;
    if (r->len == 0) {
        evt_log(bot, "bot_nav", "action=plan result=none expanded=%d goal=%d,%d,%d wait=%.0f", r->expanded,
                ai->plan_goal[0], ai->plan_goal[1], ai->plan_goal[2], wait);
        return 0;
    }
    for (int k = 0; k < r->len; k++) {
        /* the planner's map copy does not know the cells the map or the
           mode protects: a block there is refused, and a bot stood on the
           edge of Raindance's tower trying to bridge into one until it fell */
        uint8_t refused =
            (r->act[k] == NAV_DIG && _nav_dig_cost(server, r->cell[k][0], r->cell[k][1], r->cell[k][2]) < 0) ||
            ((r->act[k] == NAV_BRIDGE || r->act[k] == NAV_STAIR) &&
             !gamemode_cell_editable(server, r->cell[k][0], r->cell[k][1], r->cell[k][2]));
        if (refused) {
            ai->nav_ban[0]    = r->cell[k][0];
            ai->nav_ban[1]    = r->cell[k][1];
            ai->nav_ban[2]    = r->cell[k][2];
            ai->nav_ban_until = now + _bot_secs(20.0f);
            evt_log(bot, "bot_nav", "action=plan_refused why=protected cell=%d,%d,%d", r->cell[k][0], r->cell[k][1],
                    r->cell[k][2]);
            return 0;
        }
    }
    /* a sneaking way that does not get there (half the budget, open sky
       costly): the plain way for a while, getting there comes first (the
       infiltrator stood at the foot of Raindance's spiral stairs) */
    /* Unless the partial way gets well closer (its end at most 60% of the
       way the bot is from the goal): then it is a tunnel's first stretch,
       and the next plan, from its end, goes on hidden. Every partial way
       turned sneaking off, and on Raindance (its intel 30 blocks past the
       end of a 60 cell tunnel) the infiltrator was 0% of the time under a
       roof */
    /* a Miner's partial tunnel is its tunnel's next stretch whatever its
       length: it digs on from the end, it never goes back to the surface
       for it */
    if (ai->plan_stealth && !r->full && classes_dig(server, bot) <= 1) {
        float gdx = ai->plan_goal[0] + 0.5f - bot->movement.position.x;
        float gdy = ai->plan_goal[1] + 0.5f - bot->movement.position.y;
        float edx = (float) (ai->plan_goal[0] - r->cell[r->len - 1][0]);
        float edy = (float) (ai->plan_goal[1] - r->cell[r->len - 1][1]);
        if (edx * edx + edy * edy > 0.36f * (gdx * gdx + gdy * gdy)) {
            ai->stealth_off = now + _bot_secs(15.0f);
        }
    }
    memcpy(ai->path, r->cell, sizeof(r->cell[0]) * r->len);
    memcpy(ai->path_act, r->act, r->len);
    ai->path_len     = r->len;
    ai->path_i       = 0;
    /* the way starts where the bot asked from (~200 ms ago); walking on
       meanwhile it is a few cells along: start at the nearest of the first
       steps, or every new plan read as lost at step 0. On its own floor
       only: at the foot of Raindance's spiral stair the nearest step was
       right overhead, eight floors up, every plan read as lost, and the bot
       fell back to digging the tower the map protects */
    {
        float best = 1e9f;
        int   fz   = _bot_floor_cell(server, bot);
        for (int k = 0; k < r->len && k < 12; k++) {
            if (abs(r->cell[k][2] - fz) > 1) {
                continue;
            }
            float dx = r->cell[k][0] + 0.5f - bot->movement.position.x;
            float dy = r->cell[k][1] + 0.5f - bot->movement.position.y;
            if (dx * dx + dy * dy < best) {
                best       = dx * dx + dy * dy;
                ai->path_i = (uint16_t) k;
            }
        }
        while (ai->path_i + 1 < ai->path_len && _bot_step_behind(ai, bot, ai->path_i)) {
            ai->path_i++;
        }
    }
    ai->path_gx      = ai->plan_gx;
    ai->path_gy      = ai->plan_gy;
    ai->path_at      = now;
    ai->path_fails   = 0;
    ai->path_step_at = now;
    ai->path_stealth = ai->plan_stealth;
    /* the overwatch's spot out of its reach (a roof the planner finds no way
       onto without blocks): another one, and never that one again. Only
       near it: from far off a plan is partial for its length (the
       planner's 40000 nodes), and king of the hill's Marksmen, spawned
       300 blocks from the hill, banned spot after spot, 400 a match */
    float cdx = (float) ai->camp_x - bot->movement.position.x, cdy = (float) ai->camp_y - bot->movement.position.y;
    if (!r->full && ai->overwatch && ai->camp_set && ai->plan_goal[0] == ai->camp_x &&
        ai->plan_goal[1] == ai->camp_y && cdx * cdx + cdy * cdy < BOT_CAMP_JUDGE * BOT_CAMP_JUDGE)
    {
        _bot_camp_ban(bot, ai, "plan");
    }
    /* the walking steps under ground already there (a roof over the head:
       a tunnel dug before, by it or a mate): a Miner should take up the
       tunnels it finds rather than dig a new one beside them */
    int tunnel = 0;
    for (int k = 0; k < r->len; k++) {
        if (r->act[k] == NAV_WALK && map_top(&server->s_map.map, r->cell[k][0], r->cell[k][1]) < r->cell[k][2] - 4) {
            tunnel++;
        }
    }
    evt_log(bot, "bot_nav",
            "action=plan stealth=%u result=%s steps=%u expanded=%d ms=%.1f wait=%.0f cost=%.0f to=%d,%d,%d "
            "goal=%d,%d,%d walk=%d drop=%d up=%d bridge=%d stair=%d dig=%d pillar=%d open=%d why=%s miner=%u "
            "tunnel=%d",
            ai->plan_stealth, r->full ? "full" : "partial", r->len, r->expanded, r->ms, wait, r->cost,
            r->cell[r->len - 1][0], r->cell[r->len - 1][1], r->cell[r->len - 1][2], ai->plan_goal[0],
            ai->plan_goal[1], ai->plan_goal[2], r->counts[NAV_WALK], r->counts[NAV_DROP], r->counts[NAV_UP],
            r->counts[NAV_BRIDGE], r->counts[NAV_STAIR], r->counts[NAV_DIG], r->counts[NAV_PILLAR], r->open,
            ai->plan_why ? ai->plan_why : "?", classes_dig(server, bot) > 1, tunnel);
    return 1;
}

#define BOT_LOOKAHEAD 8 /* steps of the way the bot may aim straight at */
#define BOT_REJOIN_FAR 6.0f /* off its way, it rejoins a step this near (beyond 3.5 in a walkable line) */

/* step k of the way is behind the bot as the way goes (toward step k + 1):
   going back to it is a U-turn. A new plan starts where the bot asked from,
   and a bot running past a cell turned round for it (bot_turnjump: 120-180
   degree turns) */
static uint8_t _bot_step_behind(const bot_ai_t* ai, const player_t* bot, int k)
{
    /* the next step walks, steps up or drops: heading for it is right (a
       dig, a block or a pillar needs the bot in its cell first) */
    uint8_t next = k + 1 < ai->path_len ? ai->path_act[k + 1] : NAV_KINDS;
    if (ai->path_act[k] != NAV_WALK || (next != NAV_WALK && next != NAV_UP && next != NAV_DROP)) {
        return 0;
    }
    float tx = ai->path[k][0] + 0.5f - bot->movement.position.x;
    float ty = ai->path[k][1] + 0.5f - bot->movement.position.y;
    float wx = (float) (ai->path[k + 1][0] - ai->path[k][0]), wy = (float) (ai->path[k + 1][1] - ai->path[k][1]);
    return tx * wx + ty * wy < 0.0f && tx * tx + ty * ty < 3.0f * 3.0f;
}

/* a body 0.7 wide walks from (x0, y0) to (x1, y1) on floor f: firm ground
   and room above all along, at its centre and both its sides */
static uint8_t _bot_line_walkable(map_store_t* map, float x0, float y0, float x1, float y1, int f)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) {
        return 1;
    }
    float px = -dy / len * 0.35f, py = dx / len * 0.35f;
    for (float t = 0.0f; t <= len; t += 0.25f) {
        float bx = x0 + dx * t / len, by = y0 + dy * t / len;
        for (int side = -1; side <= 1; side++) {
            int cx = (int) (bx + px * side), cy = (int) (by + py * side);
            if (!_nav_solid(map, cx, cy, f) || !_nav_room(map, cx, cy, f) || _nav_edge(map, cx, cy, f)) {
                return 0;
            }
        }
    }
    return 1;
}

/* the way ends on an edge within 8 cells (the top of a pillar one cell
   wide): walk, the creep takes the last 3. Running there, Babel's carriers
   went 3 cells past their pillar's top and fell off */
static int _bot_path_end_narrow(map_store_t* map, player_t* bot, bot_ai_t* ai)
{
    if (ai->path_len == 0) {
        return 0;
    }
    int   ex = ai->path[ai->path_len - 1][0], ey = ai->path[ai->path_len - 1][1], ez = ai->path[ai->path_len - 1][2];
    float dx = ex + 0.5f - bot->movement.position.x, dy = ey + 0.5f - bot->movement.position.y;
    return dx * dx + dy * dy < 8.0f * 8.0f && _nav_edge(map, ex, ey, ez);
}

/* One think of walking the planned way: does the next step's work (a block,
   a dig, a pillar jump) and walks into it. 0 when there is no way to follow
   (done, or the bot left it: plan again). */
static uint8_t _bot_follow(server_t* server, player_t* bot, bot_ai_t* ai, uint8_t rush)
{
    map_store_t* map = &server->s_map.map;
    uint64_t  now = get_nanos();
    if (ai->pillar_floor != 0 && _bot_pillar_up(server, bot, ai, 0) == 1) {
        return 1; /* a pillar jump in progress */
    }
    int cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    int fz = _bot_floor_cell(server, bot);
    while (ai->path_i < ai->path_len) {
        int16_t* p    = ai->path[ai->path_i];
        uint8_t  act  = ai->path_act[ai->path_i];
        uint8_t  up   = act == NAV_UP || act == NAV_STAIR || act == NAV_PILLAR || (act == NAV_DIG && p[2] < fz);
        if (p[0] != cx || p[1] != cy || fz > p[2] + 1 || fz < p[2] - 1 || (up && (fz > p[2] || bot->airborne))) {
            break;
        }
        ai->path_i++;
        ai->path_fails   = 0;
        ai->path_step_at = now;
    }
    /* walking straight at a point further on (below), the bot cuts the
       corners of the grid and passes between the way's cells: it is as far
       along as the nearest walking step ahead on its floor, if that is within
       1.2 cells (exact cells only, the way's index stayed behind it and every
       few seconds it read as lost) */
    {
        float best = 1.2f * 1.2f;
        int   at   = -1;
        for (int k = ai->path_i; k < ai->path_len && k <= ai->path_i + BOT_LOOKAHEAD; k++) {
            if (ai->path_act[k] != NAV_WALK || ai->path[k][2] != fz) {
                break;
            }
            float dx = ai->path[k][0] + 0.5f - bot->movement.position.x;
            float dy = ai->path[k][1] + 0.5f - bot->movement.position.y;
            if (dx * dx + dy * dy <= best) {
                best = dx * dx + dy * dy;
                at   = k;
            }
        }
        if (at > ai->path_i) {
            ai->path_i       = (uint16_t) at;
            ai->path_fails   = 0;
            ai->path_step_at = now;
        }
        while (ai->path_i + 1 < ai->path_len && _bot_step_behind(ai, bot, ai->path_i)) {
            ai->path_i++;
            ai->path_step_at = now;
        }
    }
    if (ai->path_i >= ai->path_len) {
        ai->path_len = 0;
        return 0;
    }
    int16_t* p   = ai->path[ai->path_i];
    uint8_t  act = ai->path_act[ai->path_i];
    int      tx = p[0], ty = p[1], tf = p[2];
    float    ddx = tx + 0.5f - bot->movement.position.x, ddy = ty + 0.5f - bot->movement.position.y;
    /* off the way: pushed, fallen, or the map changed under it (3.5 cells:
       walking straight between the way's cells and running while a new plan
       is worked out, 2.5 read as lost at every new plan) */
    if (ddx * ddx + ddy * ddy > 3.5f * 3.5f || (!bot->airborne && fz > tf + 2 && act != NAV_DROP)) {
        /* the way is often still beside it (a fight's strafing, a run past
           a bend, the steps walked while the plan was worked out): it
           rejoins the nearest step on its floor within 3.5 cells, ahead of
           it or a little behind, and goes on. Each lost threw the plan
           away for a new one: 244 in a 300 s 40 bot match on Busan_Port,
           and planless meanwhile */
        /* up to 6 cells when the straight line there can be walked: a
           third of the lost on Busan_Port were 3.6 to 5 cells from a step
           of their own floor (~150 a 300 s 40 bot match) */
        float best = BOT_REJOIN_FAR * BOT_REJOIN_FAR;
        int   at   = -1;
        int   from = ai->path_i > 8 ? ai->path_i - 8 : 0;
        for (int k = from; k < ai->path_len && !bot->airborne; k++) {
            if (abs(ai->path[k][2] - fz) > 1) {
                continue;
            }
            float dx = ai->path[k][0] + 0.5f - bot->movement.position.x;
            float dy = ai->path[k][1] + 0.5f - bot->movement.position.y;
            /* ahead counts a little nearer: rejoining behind walks back */
            float d = dx * dx + dy * dy + (k < ai->path_i ? 1.0f : 0.0f);
            if (d < best && d > 3.5f * 3.5f &&
                !_bot_line_walkable(map, bot->movement.position.x, bot->movement.position.y, ai->path[k][0] + 0.5f,
                                    ai->path[k][1] + 0.5f, ai->path[k][2])) {
                continue;
            }
            if (d < best) {
                best = d;
                at   = k;
            }
        }
        if (at >= 0) {
            evt_log(bot, "bot_nav", "action=rejoin step=%u/%u from=%u at=%d,%d,%d", (unsigned) at, ai->path_len,
                    ai->path_i, cx, cy, fz);
            ai->path_i       = (uint16_t) at;
            ai->path_fails   = 0;
            ai->path_step_at = now;
            p                = ai->path[ai->path_i];
            act              = ai->path_act[ai->path_i];
            tx               = p[0];
            ty               = p[1];
            tf               = p[2];
            ddx              = tx + 0.5f - bot->movement.position.x;
            ddy              = ty + 0.5f - bot->movement.position.y;
        } else {
            evt_log(bot, "bot_nav", "action=lost step=%u/%u at=%d,%d,%d want=%d,%d,%d", ai->path_i, ai->path_len, cx,
                    cy, fz, tx, ty, tf);
            ai->path_len = 0;
            return 0;
        }
    }
    /* refused again and again, or no closer for a long while (a digging
       step takes a few seconds): plan around that cell for a while */
    if (ai->path_fails > 4 || now - ai->path_step_at > _bot_secs(act == NAV_WALK || act == NAV_DROP ? 5.0f : 12.0f)) {
        evt_log(bot, "bot_nav", "action=blocked step=%s cell=%d,%d,%d fails=%u", nav_names[act], tx, ty, tf,
                ai->path_fails);
        ai->nav_ban[0]    = (int16_t) tx;
        ai->nav_ban[1]    = (int16_t) ty;
        ai->nav_ban[2]    = (int16_t) tf;
        ai->nav_ban_until = now + _bot_secs(20.0f);
        ai->path_len      = 0;
        return 0;
    }
    if (bot->airborne) {
        /* keep the heading and push toward the step: turned to the next cell
           in the air, a bot that flew past it turned right round (bot_uturn
           air=1), and pushing straight on after a step behind it the guard
           hung under Raindance's spiral stair */
        float fdx = tx + 0.5f - bot->movement.position.x, fdy = ty + 0.5f - bot->movement.position.y;
        ai->move_src = "path";
        if (act != NAV_PILLAR && fdx * fdx + fdy * fdy > 0.4f * 0.4f) {
            _bot_step_toward(bot, fdx, fdy);
        } else {
            bot->move_forward = !_bot_is_wet(bot) && act == NAV_PILLAR ? 0 : 1;
        }
        ai->next_think = now + NANO_IN_MILLI * 50;
        return 1;
    }

    /* the step's work, while it is not done */
    if (act == NAV_PILLAR) {
        _bot_pillar_up(server, bot, ai, tf);
        return 1;
    }
    if ((act == NAV_BRIDGE || act == NAV_STAIR) && !map_solid(map, tx, ty, tf)) {
        if (bot->blocks == 0) {
            if (!_bot_harvest(server, bot)) {
                ai->path_fails = 99;
            }
            ai->next_think = now + BOT_BLOCK_WAIT;
            return 1;
        }
        _bot_set_tool(server, bot, TOOL_BLOCK);
        _bot_face(bot, tx + 0.5f, ty + 0.5f, (float) tf);
        uint8_t ok = _bot_place_block(server, bot, (uint32_t) tx, (uint32_t) ty, (uint32_t) tf);
        evt_log(bot, "bot_path", "action=%s cell=%d,%d,%d ok=%u blocks=%u", nav_names[act], tx, ty, tf, ok,
                bot->blocks);
        ai->path_fails = ok ? 0 : ai->path_fails + 1;
        ai->next_think = now + BOT_BLOCK_WAIT;
        return 1;
    }
    /* something in the way (a dig step, or a block someone put there); for
       a Miner going up a step under its own roof, the roof over its head
       (the planner's miner_up: the cube dig in front takes it too) */
    uint8_t roofed = tf < fz && classes_dig(server, bot) >= 3 && map_solid(map, cx, cy, fz - 4);
    if (!_nav_room(map, tx, ty, tf) || roofed) {
        if (_nav_dig_cost(server, tx, ty, tf) < 0) {
            ai->path_fails = 99;
            return 1;
        }
        ai->path_fails++;
        return _bot_dig(server, bot, ai, tx, ty, tf - 2, tf < fz, tf) != 0;
    }
    if (act != NAV_DROP && !map_solid(map, tx, ty, tf)) {
        evt_log(bot, "bot_nav", "action=lost why=floor_gone cell=%d,%d,%d", tx, ty, tf);
        ai->path_len = 0;
        return 0;
    }

    /* Straight at the furthest point of the way it can walk to in a line:
       facing each next cell's centre, a diagonal way was a zig-zag of 45
       degree turns (bot_spin src=path, a median 239 degrees a second on
       Raindance) */
    float lx = tx + 0.5f, ly = ty + 0.5f;
    if (act == NAV_WALK && tf == fz) {
        for (int k = ai->path_i + 1; k < ai->path_len && k <= ai->path_i + BOT_LOOKAHEAD; k++) {
            if (ai->path_act[k] != NAV_WALK || ai->path[k][2] != fz) {
                break;
            }
            if (!_bot_line_walkable(map, bot->movement.position.x, bot->movement.position.y, ai->path[k][0] + 0.5f,
                                    ai->path[k][1] + 0.5f, fz))
            {
                break;
            }
            lx = ai->path[k][0] + 0.5f;
            ly = ai->path[k][1] + 0.5f;
        }
    }
    /* close to the point aimed at, a step aside swings the heading to it
       wildly and the bot circles it (bot_turnjump: 30-60 degree turns on one
       step): aim at the next step then */
    float ldx = lx - bot->movement.position.x, ldy = ly - bot->movement.position.y;
    if (ldx * ldx + ldy * ldy < 0.7f * 0.7f && ai->path_i + 1 < ai->path_len && ai->path[ai->path_i + 1][2] == tf) {
        lx = ai->path[ai->path_i + 1][0] + 0.5f;
        ly = ai->path[ai->path_i + 1][1] + 0.5f;
    }
    /* Within 1.5 cells of that point (a step up, a drop, the lookahead
       stopped by an edge) the bot looks the way the way goes on and its keys,
       forward, back or aside, take it onto the cell. Facing the cell's
       centre, a step past or beside it turned the bot right round, and on
       the cell round again to the next one (bot_uturn src=path: ~45 in 150 s
       on Raindance, around steps up after a jump). A drop is still faced:
       a body falls into a one cell hole only from over its centre, and the
       eight way keys left bots beside it (bot_nav action=blocked step=drop) */
    float   adx = lx - bot->movement.position.x, ady = ly - bot->movement.position.y;
    float   ad  = sqrtf(adx * adx + ady * ady);
    uint8_t close_by = ad < 1.5f && act != NAV_DROP;
    /* a step up ahead is faced and pushed into straight: the keys go eight
       ways only, and a diagonal push ran the guard into the corner of
       Raindance's spiral stair (bot_nav action=blocked step=up) */
    if (close_by && tf < fz &&
        adx * bot->movement.forward_orientation.x + ady * bot->movement.forward_orientation.y > 0.17f * ad)
    {
        close_by = 0;
    }
    if (close_by) {
        int   k2 = ai->path_i + 2 < ai->path_len ? ai->path_i + 2 : ai->path_len - 1;
        float wx = (float) (ai->path[k2][0] - tx), wy = (float) (ai->path[k2][1] - ty);
        if (wx == 0.0f && wy == 0.0f && ai->path_i > 0) {
            wx = (float) (tx - ai->path[ai->path_i - 1][0]);
            wy = (float) (ty - ai->path[ai->path_i - 1][1]);
        }
        if (wx != 0.0f || wy != 0.0f) {
            _bot_face(bot, bot->movement.position.x + wx, bot->movement.position.y + wy, bot->movement.position.z);
        }
        if (adx * adx + ady * ady > 0.2f * 0.2f) {
            _bot_step_toward(bot, adx, ady);
        } else {
            bot->move_forward = 1;
        }
    } else {
        _bot_face(bot, lx, ly, bot->movement.position.z);
        bot->move_forward = 1;
    }
    uint8_t straight = bot->move_forward && !bot->move_backwards && !bot->move_left && !bot->move_right;
    ai->aim_px   = lx;
    ai->aim_py   = ly;
    ai->move_src = "path";
    if (act == NAV_DROP) {
        ai->drop_ok = now + _bot_secs(1.0f);
    }
    if (act == NAV_DROP && ddx * ddx + ddy * ddy < 0.3f * 0.3f) {
        /* over the hole: let it glide to its centre and fall in (walking
           on, it overshot back and forth) */
        bot->move_forward = 0;
        ai->next_think    = now + NANO_IN_MILLI * 40;
        return 1;
    }
    if (tf < fz) {
        /* up a step: jump once the body touches it (the physics' autoclimb
           only looks at the cell in front then; see _bot_clear_path) */
        int   dx  = tx - cx, dy = ty - cy;
        float gap = dx > 0   ? (float) tx - (bot->movement.position.x + 0.45f)
                    : dx < 0 ? bot->movement.position.x - 0.45f - (float) (tx + 1)
                    : dy > 0 ? (float) ty - (bot->movement.position.y + 0.45f)
                             : bot->movement.position.y - 0.45f - (float) (ty + 1);
        /* a zombie's tap takes GAME_ZOMBIE_TAP_JUMP_UP; higher waits for the leap */
        int leap_up = fz - tf > GAME_ZOMBIE_TAP_JUMP_UP;
        if (leap_up && get_nanos() < _bot_leap_back(bot)) {
            /* a leap up (rq.leap): a normal jump falls short, it waits at
               the wall for the high one, the step's clock stopped */
            bot->move_forward = 0;
            bot->move_left = bot->move_right = bot->move_backwards = 0;
            bot->sprinting    = 0;
            ai->path_step_at  = now;
        } else if (gap < 0.1f) {
            bot->jumping = 1;
            if (leap_up) {
                _bot_leap(bot, (float) (fz - tf));
            }
            if (leap_up && !bot->airborne) {
                /* each leap counts as a try: the wait keeps the step's clock
                   still, and a leap that never lands (one zombie leapt at
                   one ledge 211 times in a match) gets the step banned */
                ai->path_fails++;
                evt_log(bot, "bot_zombie", "action=leap_step up=%d cell=%d,%d,%d try=%u", fz - tf, tx, ty, tf,
                        ai->path_fails);
            }
        }
    } else if (straight && (rush || !ai->path_stealth) && act == NAV_WALK && !_bot_path_end_narrow(map, bot, ai) &&
               (ai->path_i + 1 >= ai->path_len ||
                (ai->path_act[ai->path_i + 1] == NAV_WALK && ai->path[ai->path_i + 1][2] == tf)))
    {
        /* the next step is flat too (sprinting turns the autoclimb off): run,
           like a player holding shift; only the sneaking walk. Three flat
           steps ahead, the first rule, left them walking 84% of the time */
        bot->sprinting = 1;
    }
    ai->next_think = now + NANO_IN_MILLI * 100;
    if ((act != NAV_DROP || tf - fz <= 1) && (_nav_edge(map, tx, ty, tf) || _nav_edge(map, cx, cy, fz))) {
        /* along an edge, up a tower, onto a bridge block: slowly (sneaking
           is half speed) and in shorter steps, or a think of walking carried
           it off the side (Babel's bridges over the moat: 77 deaths in 480 s) */
        bot->sprinting = 0;
        bot->sneaking  = 1;
        ai->next_think = now + NANO_IN_MILLI * 50;
    }
    return 1;
}

/* Walks the planned way to the goal, planning it when there is none, the
   goal moved, or it is getting old (the map changes all the time). 0: no
   way (the caller falls back to walking straight and clearing). */
uint8_t _bot_route_to(server_t* server, player_t* bot, bot_ai_t* ai, float gx, float gy, int gz, uint8_t exact,
                             uint8_t rush)
{
    uint64_t now   = get_nanos();
    float    mdx   = gx - ai->path_gx, mdy = gy - ai->path_gy;
    /* back from something else (a fight): it is somewhere else now, and the
       time spent fighting read as a step it could not get through */
    if (ai->path_len > 0 && now - ai->path_used_at > _bot_secs(1.0f)) {
        ai->path_len     = 0;
        ai->path_resumed = 1;
    }
    ai->path_used_at = now;
    /* the infiltrator sneaks, except home with the intel: then it is speed */
    /* only near the objective: across Raindance's open ground the sneaking
       plans (open sky costs, half the budget) covered ~40 cells each and the
       infiltrator, at half speed, was still mid-map after 240 s */
    float    sdx = gx - bot->movement.position.x, sdy = gy - bot->movement.position.y;
    /* a third of the attackers go the hidden way too (tunnels, under roofs,
       along walls: ai->sneaky, _bot_role_weapon). Everyone sneaks from 1.7 x
       BOT_SNEAK_NEAR (~120 blocks): at 70 the infiltrator made 4 to 6 sneaking
       plans a match on Raindance and players saw nobody use a tunnel */
    float    sneak_near = BOT_SNEAK_NEAR * 1.7f;
    /* a Miner tunnels from farther off: it digs a way three times as fast */
    if (classes_dig(server, bot) > 1) {
        sneak_near = BOT_SNEAK_NEAR * BOT_MINER_SNEAK;
    }
    /* a sneaky attacker walks with its squad (the leader's lane, the
       followers after it) and goes the hidden way only on its own goal
       past the lane: a follower sneaking after a leader on the surface
       tunnelled beside it */
    uint8_t  in_squad = (ai->goal_intent & GM_INTENT_SQUAD) != 0;
    /* every attacking Miner goes the hidden way near its goal, not only the
       sneaky one: the others walked on the surface past the spade that digs
       them a tunnel three times as fast. Only the last BOT_SNEAK_NEAR: from
       the Miner's own range (~120) Busan_Port's Miners dug the whole way
       and killed 8 a match instead of 13 */
    /* once tunnelling it keeps to it out to BOT_MINER_TUNNEL_OUT: crossing
       one line both ways flipped its plan between the tunnel and the
       surface, each new way leaving the last. A tunnel begun (tun_active,
       BotBlocks.c) counts with no way in hand too: after a fight the way is
       dropped (path_resumed), and the plan from there went back to the
       surface past BOT_MINER_TUNNEL_IN */
    uint8_t  miner = classes_dig(server, bot) > 1;
    float    miner_r = (ai->tun_active || (ai->path_stealth && ai->path_len > 0)) && !ai->closing_in
                           ? BOT_MINER_TUNNEL_OUT
                           : BOT_MINER_TUNNEL_IN;
    /* an enemy about (_bot_shotgun): from farther, under the ground */
    if (now < ai->miner_threat) {
        miner_r = BOT_MINER_THREAT_R;
    }
    uint8_t  miner_way = miner && (ai->role == GM_BOT_ATTACK || ai->role == GM_BOT_RUSH) &&
                        !ai->camper && !_bot_carries(server, bot) && !ai->sneaky &&
                        sdx * sdx + sdy * sdy < miner_r * miner_r;
    uint8_t  stealth = (ai->role == GM_BOT_INFILTRATE || ai->role == GM_BOT_COVER ||
                        ((ai->sneaky || miner_way) && !in_squad)) &&
                      !rush && !ai->goal_walk &&
                      sdx * sdx + sdy * sdy < sneak_near * sneak_near && now >= ai->stealth_off;
    /* a Miner closing in on an enemy (_bot_shotgun) goes the hidden way;
       only that way: on close_in_until (a second after it) the next plan to
       its goal sneaked too, a partial tunnel to an intel 300 blocks off
       that the plan after it, on the surface again, left */
    if (ai->routing_close_in && !ai->goal_walk) {
        stealth = 1;
    }
    /* a Miner's chase after an enemy (_bot_shotgun) borrows the way: the
       way to its goal is kept, and taken up again when the chase ends if
       the goal is the same: _bot_follow rejoins its nearest step, or reads
       it lost and plans */
    if (ai->routing_close_in && !ai->closing_in) {
        ai->closing_in = 1;
        ai->kept_len   = 0;
        if (ai->path_len > 0) {
            memcpy(ai->kept_path, ai->path, sizeof(ai->path[0]) * ai->path_len);
            memcpy(ai->kept_act, ai->path_act, ai->path_len);
            ai->kept_len     = ai->path_len;
            ai->kept_i       = ai->path_i;
            ai->kept_gx      = ai->path_gx;
            ai->kept_gy      = ai->path_gy;
            ai->kept_stealth = ai->path_stealth;
            ai->kept_at      = ai->path_at;
        }
        ai->path_len = 0;
    } else if (!ai->routing_close_in && ai->closing_in) {
        ai->closing_in = 0;
        ai->path_len   = 0;
        float kdx = ai->kept_gx - gx, kdy = ai->kept_gy - gy;
        if (ai->kept_len > 0 && kdx * kdx + kdy * kdy < 9.0f && now - ai->kept_at < _bot_secs(60.0f)) {
            memcpy(ai->path, ai->kept_path, sizeof(ai->path[0]) * ai->kept_len);
            memcpy(ai->path_act, ai->kept_act, ai->kept_len);
            ai->path_len     = ai->kept_len;
            ai->path_i       = ai->kept_i;
            ai->path_gx      = ai->kept_gx;
            ai->path_gy      = ai->kept_gy;
            ai->path_stealth = ai->kept_stealth;
            ai->path_at      = ai->kept_at;
            ai->path_resumed = 0;
            ai->path_step_at = now;
            ai->path_fails   = 0;
            /* a plan asked meanwhile was for the chase */
            ai->plan_ticket  = 0;
            evt_log(bot, "bot_nav", "action=resume_kept len=%u step=%u", ai->path_len, ai->path_i);
        }
        ai->kept_len = 0;
    }
    /* a goal that moved (a leader, a carrier) needs a new way only when the
       way's end is no longer near it, or it moved far: a new plan every 3
       cells was 25 plans a bot a minute, followers up to ~45, each one able
       to bend the bot's way */
    float    edx = 0.0f, edy = 0.0f;
    if (ai->path_len > 0) {
        edx = ai->path[ai->path_len - 1][0] + 0.5f - gx;
        edy = ai->path[ai->path_len - 1][1] + 0.5f - gy;
    }
    /* how near is near enough grows with the distance: a leader 30 blocks
       ahead that ran 6 changes nothing of the way behind it (moved was still
       71% of the plans with a fixed 6) */
    float    gdx = gx - bot->movement.position.x, gdy = gy - bot->movement.position.y;
    float    far_away = sqrtf(gdx * gdx + gdy * gdy) * 0.4f;
    float    near_enough = exact ? 1.0f : fmaxf(6.0f, far_away);
    float    leash = fmaxf(12.0f, far_away * 1.5f);
    uint8_t  moved = (mdx * mdx + mdy * mdy > 3.0f * 3.0f && edx * edx + edy * edy > near_enough * near_enough) ||
                    mdx * mdx + mdy * mdy > leash * leash;
    /* and at most every 4 s: a squad follower close behind its running
       leader still asked ~30 times a minute (bot_nav why=moved goal=follow) */
    moved = moved && now - ai->path_at > _bot_secs(4.0f);
    /* a Miner's tunnel is not planned again for its age as soon: each new
       way may bend off the one it is digging */
    float    old_s   = miner && ai->path_stealth ? 40.0f : 15.0f;
    uint8_t  stale   = ai->path_len == 0 || moved || now - ai->path_at > _bot_secs(old_s) ||
                    stealth != ai->path_stealth;
    /* the search runs on the planner thread (BotPlanner.c); meanwhile the
       bot keeps following the way it has, if any */
    if (stale && ai->plan_ticket == 0 && !bot->airborne && ai->pillar_floor == 0 && ai->path_retry <= now &&
        planner_ready())
    {
        plan_request_t rq;
        rq.sx      = (int) bot->movement.position.x;
        rq.sy      = (int) bot->movement.position.y;
        rq.sf      = _bot_floor_cell(server, bot);
        rq.gx      = (int) gx;
        rq.gy      = (int) gy;
        rq.gz      = gz;
        rq.exact   = exact;
        rq.stealth = stealth;
        /* the carrier runs the short way home; SP_BOT_COVER=0 turns the
           taste off for a measurement */
        static int cover_off = -1;
        if (cover_off < 0) {
            cover_off = getenv("SP_BOT_COVER") != NULL && !strcmp(getenv("SP_BOT_COVER"), "0");
        }
        rq.cover = !rush && !cover_off;
        /* no bridge, stair or pillar step on the way: blocks are for cover
           only (players saw bots dot the ground with stair blocks near the
           objective, 14 in 150 s on 2Fort); the spade still digs a way and
           Babel's climb has its own pillar (goal_climb) */
        rq.blocks  = 0;
        /* the guard about its base walks round it: its plan dug through
           the walls it mends (one cell filled and dug 60 times a match) */
        rq.nodig   = ai->goal_walk ||
                     (gamemode_player_rules(server, bot) & GM_RULE_NO_DESTROY) ||
                     (ai->role == GM_BOT_DEFEND && (ai->goal_intent & GM_INTENT_HOME));
        rq.deadly_water = _bot_water_kills(server);
        rq.leap         = (gamemode_player_rules(server, bot) & GM_RULE_HIGH_JUMP) ? 4 : 0;
        rq.miner        = classes_dig(server, bot) > 1;
        rq.ban[0]  = -1;
        if (ai->nav_ban_until > now) {
            rq.ban[0] = ai->nav_ban[0];
            rq.ban[1] = ai->nav_ban[1];
            rq.ban[2] = ai->nav_ban[2];
        }
        ai->plan_ticket  = planner_submit(bot->id, &rq);
        ai->plan_asked   = now;
        ai->plan_why     = ai->path_resumed           ? "resume"
                           : ai->path_len == 0        ? "none"
                           : moved                    ? "moved"
                           : stealth != ai->path_stealth ? "stealth"
                                                      : "old";
        ai->path_resumed = 0;
        ai->plan_goal[0] = rq.gx;
        ai->plan_goal[1] = rq.gy;
        ai->plan_goal[2] = rq.gz;
        ai->plan_gx      = gx;
        ai->plan_gy      = gy;
        ai->plan_stealth = stealth;
        /* at most a plan a second per bot, even when the way keeps closing */
        ai->path_retry = now + _bot_secs(1.0f);
    }
    static plan_result_t result;
    if (ai->plan_ticket != 0 && planner_poll(bot->id, ai->plan_ticket, &result)) {
        ai->plan_ticket = 0;
        /* asked long ago (it fought meanwhile and did not come to collect
           it: a plan was taken 52 s late once): from where it was then */
        if (now - ai->plan_asked > _bot_secs(3.0f)) {
            ai->path_retry = now;
        } else if (!_bot_take_plan(server, bot, ai, &result, now)) {
            ai->path_len = 0;
            if (result.len == 0) {
                ai->path_retry = now + _bot_secs(2.0f);
            }
        }
    } else if (ai->plan_ticket != 0 && now - ai->plan_asked > _bot_secs(5.0f)) {
        ai->plan_ticket = 0; /* lost (replaced, or the map changed): ask again */
    }
    if (ai->path_len == 0) {
        return 0;
    }
    return _bot_follow(server, bot, ai, rush);
}

/* One tick of turning: the real look goes toward the wanted one at a
   player's speed (fast onto an enemy, faster the better the bot), and the
   step the think chose keeps its direction in the world whatever the look
   (the keys are relative to it). Bots turned in one tick, so the view
   jumped onto an enemy for a shot and back */
void _bot_turn(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    if (!ai->look_ok) {
        return;
    }
    float dt = ai->turn_at ? (float) (now - ai->turn_at) / (float) NANO_IN_SECOND : 0.0f;
    ai->turn_at = now;
    dt          = dt > 0.05f ? 0.05f : dt;
    float rate  = ai->turn_combat ? BOT_TURN_AIM_MIN + (BOT_TURN_AIM_MAX - BOT_TURN_AIM_MIN) * ai->skill : BOT_TURN_WALK;
    float step  = rate * (float) M_PI / 180.0f * dt;

    vector3f_t c = ai->look_cur, w = ai->look_want;
    float      cyaw = atan2f(c.y, c.x), cpitch = asinf(fmaxf(-1.0f, fminf(1.0f, c.z)));
    float      wyaw = atan2f(w.y, w.x), wpitch = asinf(fmaxf(-1.0f, fminf(1.0f, w.z)));
    float      dyaw = wyaw - cyaw;
    while (dyaw > (float) M_PI) {
        dyaw -= 2.0f * (float) M_PI;
    }
    while (dyaw < -(float) M_PI) {
        dyaw += 2.0f * (float) M_PI;
    }
    float dpitch = wpitch - cpitch, dist = sqrtf(dyaw * dyaw + dpitch * dpitch);
    if (dist > step && dist > 0.0001f) {
        dyaw *= step / dist;
        dpitch *= step / dist;
    }
    cyaw += dyaw;
    cpitch += dpitch;
    vector3f_t ori = {cosf(cyaw) * cosf(cpitch), sinf(cyaw) * cosf(cpitch), sinf(cpitch)};
    ai->look_cur   = ori;
    /* turning on the way with nothing to turn for (no fight, no hit, no
       sound, no target): players saw bots swing left and right as they ran */
    if ((ai->move_wx != 0.0f || ai->move_wy != 0.0f) && !ai->turn_combat && ai->phase == BOT_PHASE_PATROL &&
        !ai->has_target && ai->alert_until <= now && ai->steps_until <= now)
    {
        ai->spin_deg += fabsf(dyaw) * 57.2958f;
        ai->spin_t += dt;
        /* the body turned round (150 degrees one way within 1.5 s): what
           players see as a bot spinning on its way. bot_turnjump counts the
           wanted heading, and a turn called off before the body followed
           read there as one too */
        ai->uturn_rad += dyaw;
        ai->uturn_t += dt;
        if (fabsf(ai->uturn_rad) >= 150.0f / 57.2958f) {
            evt_log(bot, "bot_uturn", "deg=%.0f secs=%.2f src=%s path=%u/%u air=%u pos=%.2f,%.2f,%.2f",
                    ai->uturn_rad * 57.2958f, ai->uturn_t, ai->move_src ? ai->move_src : "other", ai->path_i,
                    ai->path_len, bot->airborne, bot->movement.position.x, bot->movement.position.y,
                    bot->movement.position.z);
            ai->uturn_rad = ai->uturn_t = 0.0f;
        } else if (ai->uturn_t > 1.5f) {
            ai->uturn_rad = ai->uturn_t = 0.0f;
        }
        if (ai->spin_t >= 2.0f) {
            if (ai->spin_deg / ai->spin_t > 60.0f) {
                /* how much the way itself bends over the next 8 steps (a
                   spiral stair turns for real) */
                float bend = 0.0f;
                if (ai->path_len > ai->path_i + 8) {
                    int16_t* a  = ai->path[ai->path_i];
                    int16_t* b  = ai->path[ai->path_i + 4];
                    int16_t* c  = ai->path[ai->path_i + 8];
                    float    h1 = atan2f((float) (b[1] - a[1]), (float) (b[0] - a[0]));
                    float    h2 = atan2f((float) (c[1] - b[1]), (float) (c[0] - b[0]));
                    bend        = fabsf(remainderf(h2 - h1, 2.0f * (float) M_PI)) * 57.3f;
                }
                evt_log(bot, "bot_spin", "rate=%.0f src=%s goal=%s path=%u/%u bend=%.0f clear=%u pos=%.0f,%.0f,%.0f",
                        ai->spin_deg / ai->spin_t, ai->move_src ? ai->move_src : "other",
                        ai->mode_goal ? ai->mode_goal : "none", ai->path_i, ai->path_len, bend, ai->clear_until > now,
                        bot->movement.position.x, bot->movement.position.y, bot->movement.position.z);
            }
            ai->spin_deg = ai->spin_t = 0.0f;
        }
    } else {
        ai->uturn_rad = ai->uturn_t = 0.0f;
    }
    physics_reorient_player(bot, &ori);

    if (ai->move_wx != 0.0f || ai->move_wy != 0.0f) {
        float fx = ori.x, fy = ori.y, fl = sqrtf(fx * fx + fy * fy);
        float sx = bot->movement.strafe_orientation.x, sy = bot->movement.strafe_orientation.y;
        float sl = sqrtf(sx * sx + sy * sy);
        float a  = fl > 0.01f ? (ai->move_wx * fx + ai->move_wy * fy) / fl : 0.0f;
        float b  = sl > 0.01f ? (ai->move_wx * sx + ai->move_wy * sy) / sl : 0.0f;
        bot->move_forward   = a > 0.38f;
        bot->move_backwards = a < -0.38f;
        bot->move_right     = b > 0.38f;
        bot->move_left      = b < -0.38f;
        /* The rail: in a fight, no step off a drop of three cells or more.
           A fight's charge, zig-zag or side-step took a bot off Raindance's
           spiral stairs to its death. Only in a fight: on its way it froze
           bots centring on a Babel pillar and walking the planned way along
           the moat */
        map_store_t* map = &server->s_map.map;
        int       fz  = _bot_floor_cell(server, bot);
        int       px  = (int) (bot->movement.position.x + ai->move_wx * 0.7f);
        int       py  = (int) (bot->movement.position.y + ai->move_wy * 0.7f);
        uint8_t fighting = ai->turn_combat || ai->phase != BOT_PHASE_PATROL;
        /* and the intel's carrier walking with no plan yet (it is on its way
           from the planner): one that had just taken the intel at the top of
           Raindance's tower walked straight for home and off its edge, 20
           levels down. Every bot with no plan stopped at edges cost pickups
           (Raindance, 4 seeds at 240 s: 5 to 0) */
        uint8_t planless = _bot_carries(server, bot) && ai->path_len == 0 && !ai->goal_climb && ai->pillar_floor == 0;
        /* water that kills (Nine-Eye's moat, 100 a second): nobody walks
           into it, fight or not, plan or not (the plan has no step there);
           29 of 73 deaths on Babel were walks off the moat's edge */
        if (_bot_water_kills(server) && !bot->airborne && !_bot_is_wet(bot) && !ai->escaping &&
            _bot_falls_in_water(map, px, py, fz))
        {
            bot->move_forward = bot->move_backwards = bot->move_right = bot->move_left = 0;
            if (now >= ai->rail_logged) {
                ai->rail_logged = now + _bot_secs(5.0f);
                evt_log(bot, "bot_rail", "action=stop cell=%d,%d,%d why=water goal=%s", px, py, fz,
                        ai->mode_goal ? ai->mode_goal : "none");
            }
        }
        if ((fighting || planless) && !bot->airborne && now >= ai->drop_ok && ai->clear_until <= now &&
            !ai->escaping &&
            fz < map->size_z - 4 && !_nav_solid(map, px, py, fz) && !_nav_solid(map, px, py, fz + 1) &&
            !_nav_solid(map, px, py, fz + 2) && !_nav_solid(map, px, py, fz - 1))
        {
            bot->move_forward = bot->move_backwards = bot->move_right = bot->move_left = 0;
            if (now >= ai->rail_logged) {
                ai->rail_logged = now + _bot_secs(5.0f);
                evt_log(bot, "bot_rail", "action=stop cell=%d,%d,%d phase=%s goal=%s", px, py, fz,
                        _bot_phase_name(ai->phase), ai->mode_goal ? ai->mode_goal : "none");
            }
        }
    }
}

uint8_t _bot_water_kills(server_t* server)
{
    return server->protocol.gamemode.water_damage_enabled && server->protocol.gamemode.water_damage >= 50;
}

/* a step onto (x, y) from floor fz lands in the water: no floor at fz or
   below but the water layer (under Babel's slab the column's top is the
   slab, so the column's top block says nothing) */
uint8_t _bot_falls_in_water(map_store_t* map, int x, int y, int fz)
{
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    for (int z = fz - 1; z < map->size_z - 1; z++) {
        if (z >= 0 && map_solid(map, x, y, z)) {
            return 0;
        }
    }
    return 1;
}
