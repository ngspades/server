// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Blocks and the spade: putting and digging cells through the same packet
    handlers as a player, the server's block rate, walls and pillars.
*/

#include <Server/GameApi.h>
#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <Server/Classes.h>

/* Can the bot put a block in this cell without landing it inside a player? */
static uint8_t _bot_block_is_free(server_t* server, uint32_t X, uint32_t Y, uint32_t Z)
{
    player_t *other, *tmp;
    HASH_ITER(hh, server->players, other, tmp)
    {
        if (other->state == STATE_DISCONNECTED || !other->alive) {
            continue;
        }
        /* a body in that cell: its column, and its height (feet 2.4 under
           the position, the head ~0.6 over it). Any player in the column
           refused every block of Babel's staircases, with bots on the steps
           above and below */
        if ((uint32_t) other->movement.position.x == X && (uint32_t) other->movement.position.y == Y &&
            (float) Z < other->movement.position.z + 2.4f && (float) Z + 1.0f > other->movement.position.z - 0.6f)
        {
            return 0;
        }
    }
    return 1;
}

/* Places a single block at (X, Y, Z) through the real block action path, so
   it is validated and charged to the bot exactly like a player's placement.
   Z is the server's height axis and grows downwards. Success is judged on
   the observable effect: one block spent and the cell now solid. */

uint8_t _bot_place_block(server_t* server, player_t* bot, uint32_t X, uint32_t Y, uint32_t Z)
{
    const char* why = NULL;
    if (gamemode_player_rules(server, bot) & GM_RULE_NO_BLOCK) {
        why = "mode";
    } else if (bot->blocks == 0) {
        why = "no-blocks";
    } else if (!bot->can_build || !server->global_ab) {
        why = "building-disabled";
    } else if (bot->sprinting) {
        why = "sprinting"; /* game_player_block_action refuses it */
    } else if (bot->item != TOOL_BLOCK) {
        why = "wrong-tool";
    } else if (!valid_pos_v3i(server, (vector3i_t) {(int) X, (int) Y, (int) Z})) {
        why = "out-of-map";
    } else if (map_solid(&server->s_map.map, (uint16_t) X, (uint16_t) Y, (uint16_t) Z)) {
        why = "cell-solid";
    } else if (!_bot_block_is_free(server, X, Y, Z)) {
        why = "player-there";
    } else if (!_bot_block_ready(server, bot, BLOCKACTION_BUILD)) {
        why = "rate"; /* the server's delay since the last build or dig */
    }
    if (why != NULL) {
        evt_log(bot, "block", "skipped cell=%d,%d,%d why=%s blocks=%u", (int) X, (int) Y, (int) Z, why, bot->blocks);
        return 0;
    }

    uint16_t blocks_before = bot->blocks;
    game_player_block_action(server, bot, BLOCKACTION_BUILD, X, Y, Z);

    uint8_t ok = bot->blocks < blocks_before && map_solid(&server->s_map.map, (uint16_t) X, (uint16_t) Y, (uint16_t) Z);
    if (!ok) {
        /* the server refused it: rate limit (BLOCK_DELAY), distance or no
           solid neighbour */
        evt_log(bot, "block", "refused cell=%d,%d,%d why=server blocks=%u", (int) X, (int) Y, (int) Z, bot->blocks);
    }
    return ok;
}

/* The floor cell the bot stands on: a standing player is 2.4 above it (the
   spawn and the intel checks use the same offset). In the water the feet
   reach 64; the water layer (63) is the floor then. */
int _bot_floor_cell(server_t* server, player_t* bot)
{
    int fz = (int) roundf(bot->movement.position.z + 2.4f);
    return fz > (int) server->s_map.map.size_z - 1 ? (int) server->s_map.map.size_z - 1 : fz;
}

/* A spade or block action through the real handler, so it is validated and
   charged like a player's. Returns 1 when the cell changed. */
/* The server's own block rate rule (BlockChecks.c): an action waits for the
   last build, dig and three block dig, each by the action's delay. A bot that
   tried early was refused and every refusal told the admins it "probably has
   speed destroy hack", so it checks first and simply tries again later */
uint8_t _bot_block_ready(server_t* server, const player_t* bot, uint8_t action)
{
    uint64_t now   = get_nanos();
    uint64_t delay = action == BLOCKACTION_BUILD        ? BLOCK_DELAY
                     : action == BLOCKACTION_DESTROY_ONE ? SPADE_DELAY
                                                         : classes_dig_delay(server, (player_t*) bot);
    delay += NANO_IN_MILLI * 10;
    return now - bot->timers.since_last_block_plac >= delay && now - bot->timers.since_last_block_dest >= delay &&
           now - bot->timers.since_last_3block_dest >= delay;
}

/* Who put a block: a cell, the team and when, for BOT_BUILT_KEEP. A bot
   spaded the stairs and pillars its mates were climbing (a jam's dig, a
   harvest for blocks, a wall between it and an enemy), and the mate fell or
   started over. A slot is overwritten by the next block put there, so a
   cell the enemy dug and filled again is the enemy's. 16384 slots, the
   older entry losing a collision: an old block a bot may dig again. */
#define BOT_BUILT_SLOTS 16384
/* a block is the team's while fresh, or while its builder is by it (on its
   stair, up its pillar): an old wall on the way is dug as before (kept 180 s,
   2Fort's attackers no longer dug through their posts' walls and picked up
   the intel 8.9 times a match to 5.9) */
#define BOT_BUILT_KEEP  ((uint64_t) 30 * NANO_IN_SECOND)
#define BOT_BUILT_NEAR  6.0f
static struct
{
    uint32_t key;  /* x | y << 11 | z << 22, 0 an empty slot */
    uint8_t  team;
    uint8_t  builder;
    uint32_t gen;  /* the map's load (map_store_t.version's high half) */
    uint64_t at;
} bot_built[BOT_BUILT_SLOTS];

static uint32_t _bot_built_key(int x, int y, int z)
{
    return ((uint32_t) x & 0x7FF) | ((uint32_t) y & 0x7FF) << 11 | ((uint32_t) z & 0x3F) << 22 | 1u << 31;
}

void bots_on_build(server_t* server, player_t* builder, int x, int y, int z)
{
    if (builder->team != TEAM_A && builder->team != TEAM_B) {
        return;
    }
    uint32_t key = _bot_built_key(x, y, z);
    uint32_t i   = (key * 2654435761u) % BOT_BUILT_SLOTS;
    bot_built[i].key  = key;
    bot_built[i].team = builder->team;
    bot_built[i].builder = builder->id;
    bot_built[i].gen  = (uint32_t) (server->s_map.map.version >> 32);
    bot_built[i].at   = get_nanos();
}

uint8_t bots_team_built(server_t* server, uint8_t team, int x, int y, int z)
{
    if (!map_solid(&server->s_map.map, x, y, z)) {
        return 0;
    }
    uint32_t key = _bot_built_key(x, y, z);
    uint32_t i   = (key * 2654435761u) % BOT_BUILT_SLOTS;
    if (bot_built[i].key != key || bot_built[i].team != team ||
        bot_built[i].gen != (uint32_t) (server->s_map.map.version >> 32))
    {
        return 0;
    }
    if (get_nanos() - bot_built[i].at < BOT_BUILT_KEEP) {
        return 1;
    }
    player_t* b  = NULL;
    uint8_t   id = bot_built[i].builder;
    HASH_FIND(hh, server->players, &id, sizeof(id), b);
    return b != NULL && b->alive && b->team == team && fabsf(b->movement.position.x - (float) x) < BOT_BUILT_NEAR &&
           fabsf(b->movement.position.y - (float) y) < BOT_BUILT_NEAR &&
           fabsf(b->movement.position.z - (float) z) < BOT_BUILT_NEAR;
}

/* a spade or a dig of the bot's would take a block its own team put (the
   cells a three block dig opens: z - 1 to z + 1). A bot coming down its own
   pillar on purpose (climb 2) and Babel's stair breakers (work 2, the
   enemy's steps) are not asked */
static uint8_t _bot_digs_team_block(server_t* server, player_t* bot, uint8_t action, int x, int y, int z)
{
    if (action == BLOCKACTION_BUILD) {
        return 0;
    }
    bot_ai_t* ai = _bot_ai(bot->id);
    if (ai->goal_climb == 2 || ai->work == 2) {
        return 0;
    }
    int lo = action == BLOCKACTION_DESTROY_THREE ? z - 1 : z, hi = action == BLOCKACTION_DESTROY_THREE ? z + 1 : z;
    for (int k = lo; k <= hi; k++) {
        if (bots_team_built(server, bot->team, x, y, k)) {
            return 1;
        }
    }
    return 0;
}

uint8_t _bot_block_action(server_t* server, player_t* bot, uint8_t action, int X, int Y, int Z)
{
    /* the last word on a mate's block: whatever asked for the dig */
    if (_bot_digs_team_block(server, bot, action, X, Y, Z)) {
        evt_log(bot, "bot_path", "action=dig_refused why=team_built cell=%d,%d,%d", X, Y, Z);
        return 0;
    }
    /* a mode that carries no blocks (the arena): the pillar and the support
       block come through here too */
    if (action == BLOCKACTION_BUILD && (gamemode_player_rules(server, bot) & GM_RULE_NO_BLOCK)) {
        return 0;
    }
    if (!_bot_block_ready(server, bot, action)) {
        return 0;
    }
    uint8_t  before = map_solid(&server->s_map.map, (uint16_t) X, (uint16_t) Y, (uint16_t) Z);
    _bot_trigger(bot); /* the swing (or the build) everyone sees and hears */
    game_player_block_action(server, bot, action, (uint32_t) X, (uint32_t) Y, (uint32_t) Z);
    return map_solid(&server->s_map.map, (uint16_t) X, (uint16_t) Y, (uint16_t) Z) != before;
}

/* is_block_placable's rule: free, above the water level, a solid neighbour */
uint8_t _bot_placeable(server_t* server, int x, int y, int z)
{
    static const int n[6][3] = {{0, 0, -1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    map_store_t*        map     = &server->s_map.map;
    if (z < 0 || z > map->size_z - 3 || map_solid(map, x, y, z)) {
        return 0;
    }
    for (int k = 0; k < 6; k++) {
        if (map_solid(map, x + n[k][0], y + n[k][1], z + n[k][2])) {
            return 1;
        }
    }
    return 0;
}

/* Out of blocks: dig single blocks next to the bot with the spade, each one
   gives a block back (the three block dig gives nothing). The cells at the
   feet and the body around it, never the floor under it. 1 when it dug. */
uint8_t _bot_harvest(server_t* server, player_t* bot)
{
    /* a mode where nothing may be dug (the arena) gives no blocks back */
    if (gamemode_player_rules(server, bot) & GM_RULE_NO_DESTROY) {
        return 0;
    }
    static const int sides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    map_store_t*        map         = &server->s_map.map;
    int              cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    int              fz = _bot_floor_cell(server, bot);
    for (int level = 1; level <= 2; level++) {
        for (int k = 0; k < 4; k++) {
            int x = cx + sides[k][0], y = cy + sides[k][1], z = fz - level;
            if (z >= map->size_z - 2 || !map_solid(map, x, y, z) || !gamemode_cell_editable(server, x, y, z) ||
                bots_team_built(server, bot->team, x, y, z))
            {
                continue;
            }
            _bot_set_tool(server, bot, TOOL_SPADE);
            _bot_face(bot, x + 0.5f, y + 0.5f, (float) z);
            uint8_t ok = _bot_block_action(server, bot, BLOCKACTION_DESTROY_ONE, x, y, z);
            evt_log(bot, "bot_path", "action=harvest cell=%d,%d,%d ok=%u blocks=%u", x, y, z, ok, bot->blocks);
            return ok;
        }
    }
    return 0;
}

/* Path results: walk on, an action was taken (wait for the block delay), or
   this way is closed (do not walk into it: the water below a shore) */

/* Going straight up the way players do: jump, and at the top of the jump put
   a block where the feet were; land on it one block higher. A roof over the
   head is dug first. Blocks go at 61 at the deepest, so this starts on land,
   never in the water. Returns BOT_PATH_ACTED while it works (the caller
   comes back soon: the top of a jump lasts a fraction of a second). */

/* the three block spade dig that opens the cell (ax, ay) one block up from
   the bot's floor fz: the solid cell under the opening becomes a step */
uint8_t _bot_dig(server_t* server, player_t* bot, bot_ai_t* ai, int ax, int ay, int centre, uint8_t rising,
                        int goal_floor)
{
    /* a goal that keeps the terrain (gm_bot_goal_t.keep_terrain) digs
       nothing, not even out of a jam: go round. Babel's staircase builders,
       stuck under their spot, spaded the steps they had just put: 304 blocks
       put in a match and 11 of them standing */
    if (ai->goal_keep) {
        evt_log(bot, "bot_path", "action=dig_refused why=keep_terrain cell=%d,%d,%d goal_floor=%d", ax, ay, centre,
                goal_floor);
        return BOT_PATH_STOP;
    }
    /* a mode that keeps its map whole (the arena): the drill bores too */
    if (gamemode_player_rules(server, bot) & GM_RULE_NO_DESTROY) {
        evt_log(bot, "bot_path", "action=dig_refused why=mode cell=%d,%d,%d goal_floor=%d", ax, ay, centre, goal_floor);
        return BOT_PATH_STOP;
    }
    /* a cell the map or the mode protects is never dug: go round it (a bot
       at the foot of Raindance's intel tower spaded its wall 31 times,
       refused every time, instead of taking the stair) */
    for (int dz = -1; dz <= 1; dz++) {
        if (map_solid(&server->s_map.map, ax, ay, centre + dz) &&
            !gamemode_cell_editable(server, ax, ay, centre + dz))
        {
            evt_log(bot, "bot_path", "action=dig_refused why=protected cell=%d,%d,%d goal_floor=%d", ax, ay,
                    centre + dz, goal_floor);
            return BOT_PATH_STOP;
        }
    }
    /* a mate's stair, pillar or wall: go round it, as round a protected cell */
    if (_bot_digs_team_block(server, bot, BLOCKACTION_DESTROY_THREE, ax, ay, centre)) {
        evt_log(bot, "bot_path", "action=dig_refused why=team_built cell=%d,%d,%d goal_floor=%d", ax, ay, centre,
                goal_floor);
        return BOT_PATH_STOP;
    }
    /* the Miner's drill bores the whole way ahead at once (12 deep), the
       spade's way when it is out of charges or the way goes up. Only into
       the ground: a tunnel it is in (solid ground over its head, no air in
       it) or the face of a hill ahead taller than the bot (solid from its
       top down to the dig). Any cover over the head counted before, a roof
       or a tree's crown too, and Miners holding a town's hill drilled the
       ground their team stood on. Never out of its own base (the server
       refuses it there), nor with a mate close by: the bit bores their
       ground too */
    map_store_t* map  = &server->s_map.map;
    int          bx   = (int) bot->movement.position.x, by = (int) bot->movement.position.y;
    int          head = (int) bot->movement.position.z - 1;
    int          btop = map_top(map, bx, by);
    uint8_t      under = btop < head - 1;
    for (int z = btop; under && z < head; z++) {
        under = map_solid(map, bx, by, z);
    }
    int     atop = map_top(map, ax, ay);
    uint8_t face = atop < centre - 3;
    for (int z = atop; face && z <= centre + 1; z++) {
        face = map_solid(map, ax, ay, z);
    }
    uint8_t drill = (under || face) &&
                    !classes_drill_base(server, bot->team, bot->movement.position.x, bot->movement.position.y);
    if (drill) {
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (drill && p != bot && p->team == bot->team && p->alive && p->state == STATE_READY &&
                fabsf(p->movement.position.x - bot->movement.position.x) < BOT_DRILL_MATE_NEAR &&
                fabsf(p->movement.position.y - bot->movement.position.y) < BOT_DRILL_MATE_NEAR)
            {
                drill = 0;
                if (bot->drill_charges > 0) {
                    evt_log(bot, "bot_path", "action=drill_held cell=%d,%d,%d why=mate", ax, ay, centre);
                }
            }
        }
    }
    if (drill && bot->drill_charges > 0 && !rising &&
        get_nanos() - bot->drill_at > (uint64_t) (DRILL_INTERVAL * NANO_IN_SECOND))
    {
        _bot_set_tool(server, bot, TOOL_GRENADE);
        float      ex = ax + 0.5f - bot->movement.eye_pos.x, ey = ay + 0.5f - bot->movement.eye_pos.y;
        float      el = sqrtf(ex * ex + ey * ey);
        vector3f_t d  = {el > 0.01f ? ex / el : 0.0f, el > 0.01f ? ey / el : 0.0f, 0.0f};
        _bot_face(bot, ax + 0.5f, ay + 0.5f, bot->movement.eye_pos.z);
        uint8_t ok = (uint8_t) classes_drill(server, bot, d, "bot");
        evt_log(bot, "bot_path", "action=drill cell=%d,%d,%d ok=%u goal_floor=%d", ax, ay, centre, ok, goal_floor);
        if (ok) {
            ai->next_think = get_nanos() + BOT_DIG_WAIT;
            return BOT_PATH_ACTED;
        }
    }
    _bot_set_tool(server, bot, TOOL_SPADE);
    _bot_face(bot, ax + 0.5f, ay + 0.5f, (float) centre);
    /* how much ground is over the opening (its top cell centre - 1), read
       before the dig: 0 is the column's own top (open sky), 1 one block of
       roof. A Miner's tunnel should be deep (BotPlanner.c, MINER_ROOF);
       players saw its digs carve trenches into the surface */
    int     roof_before = (centre - 1) - map_top(&server->s_map.map, ax, ay);
    uint8_t solid_before = (uint8_t) (map_solid(&server->s_map.map, ax, ay, centre - 1) +
                                      map_solid(&server->s_map.map, ax, ay, centre) +
                                      map_solid(&server->s_map.map, ax, ay, centre + 1));
    _bot_block_action(server, bot, BLOCKACTION_DESTROY_THREE, ax, ay, centre);
    uint8_t ok = (uint8_t) (map_solid(&server->s_map.map, ax, ay, centre - 1) +
                            map_solid(&server->s_map.map, ax, ay, centre) +
                            map_solid(&server->s_map.map, ax, ay, centre + 1)) < solid_before;
    evt_log(bot, "bot_path", "action=dig cell=%d,%d,%d ok=%u rising=%u goal_floor=%d", ax, ay, centre, ok, rising,
            goal_floor);
    if (ok && classes_dig(server, bot) > 1) {
        _bot_tunnel_dug(bot, ai, get_nanos());
        int roof = roof_before < 0 ? 0 : roof_before;
        evt_log(bot, "bot_miner_dig", "cell=%d,%d,%d depth=%d surface=%u stealth=%u", ax, ay, centre, roof,
                roof <= 1, ai->path_stealth);
    }
    /* refused, it was the three block dig's delay (the class's, a second
       without classes): wait it out (a refusal used to send the bot on a
       detour) */
    ai->next_think = get_nanos() + classes_dig_delay(server, bot) + (BOT_DIG_WAIT - THREEBLOCK_DELAY);
    return BOT_PATH_ACTED;
}

uint8_t _bot_pillar_up(server_t* server, player_t* bot, bot_ai_t* ai, int goal_floor)
{
    map_store_t* map = &server->s_map.map;
    int       cx  = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    uint64_t  now = get_nanos();
    if (bot->airborne) {
        if (ai->pillar_floor == 0) {
            return 0x7F; /* in the air for another reason: let it land */
        }
        float feet = bot->movement.position.z + 2.4f;
        int   cell = ai->pillar_floor - 1;
        if (feet < (float) cell - 0.05f) {
            /* above the cell the feet left: fill it (the column jumped from:
               a jump with some speed left drifted over the water once) */
            uint8_t ok = !map_solid(map, ai->pillar_x, ai->pillar_y, cell) &&
                         _bot_block_action(server, bot, BLOCKACTION_BUILD, ai->pillar_x, ai->pillar_y, cell);
            evt_log(bot, "bot_path", "action=pillar cell=%d,%d,%d ok=%u blocks=%u goal_floor=%d", ai->pillar_x,
                    ai->pillar_y, cell, ok, bot->blocks, goal_floor);
            ai->pillar_floor = 0;
            ai->next_think   = now + BOT_BLOCK_WAIT;
            return 1;
        }
        if (bot->movement.velocity.z > 0.0f) {
            ai->pillar_floor = 0; /* falling again: missed the top */
        }
        ai->next_think = now + NANO_IN_MILLI * 30;
        return 1;
    }
    int fz = _bot_floor_cell(server, bot);
    if (fz - 1 > map->size_z - 3) {
        return 0; /* in the water */
    }
    if (bot->blocks == 0) {
        return _bot_harvest(server, bot); /* nothing to build with: get some */
    }
    /* the block goes under the bot's centre, so the centre must stand on
       something: on the edge of a shore it hung over the water and every
       block was refused as floating. Step onto the cell it stands on */
    if (!map_solid(map, cx, cy, fz)) {
        /* a short step toward the column holding it up (its target is fixed
           here: re-aimed at every step, it walked right across the shore) */
        for (int k = 0; k < 4; k++) {
            int hx = (int) (bot->movement.position.x + ((k & 1) ? 0.45f : -0.45f));
            int hy = (int) (bot->movement.position.y + ((k & 2) ? 0.45f : -0.45f));
            if (map_solid(map, hx, hy, fz)) {
                _bot_face(bot, hx + 0.5f, hy + 0.5f, bot->movement.position.z);
                bot->move_forward = 1;
                break;
            }
        }
        ai->next_think = now + NANO_IN_MILLI * 60;
        return 1;
    }
    /* room to rise one block: the cell above the head, in every column the
       body overlaps (it is 0.9 wide: standing near the edge of a cell, the
       head hit the roof of the next column on every jump) */
    for (int k = 0; k < 4; k++) {
        int hx = (int) (bot->movement.position.x + ((k & 1) ? 0.45f : -0.45f));
        int hy = (int) (bot->movement.position.y + ((k & 2) ? 0.45f : -0.45f));
        /* above the top of the world is open sky, not a roof (Babel's
           pillars stopped at floor 3 digging at z = -1) */
        if (fz - 4 >= 0 && map_solid(map, hx, hy, fz - 4)) {
            _bot_set_tool(server, bot, TOOL_SPADE);
            _bot_face(bot, hx + 0.5f, hy + 0.5f, (float) (fz - 5));
            _bot_block_action(server, bot, BLOCKACTION_DESTROY_THREE, hx, hy, fz - 5);
            uint8_t ok = !map_solid(map, hx, hy, fz - 4);
            evt_log(bot, "bot_path", "action=dig_up cell=%d,%d,%d ok=%u", hx, hy, fz - 5, ok);
            ai->next_think = now + BOT_BLOCK_WAIT;
            return 1;
        }
    }
    _bot_set_tool(server, bot, TOOL_BLOCK);
    _bot_face(bot, cx + 0.5f, cy + 0.5f, (float) fz); /* looking down at the feet */
    if (fabsf(bot->movement.velocity.x) + fabsf(bot->movement.velocity.y) > 0.02f) {
        ai->next_think = now + NANO_IN_MILLI * 60; /* let the centring step settle first */
        return 1;
    }
    bot->jumping     = 1;
    ai->pillar_floor = fz;
    ai->pillar_x     = cx;
    ai->pillar_y     = cy;
    ai->next_think   = now + NANO_IN_MILLI * 30;
    return 1;
}

/* Chooses where the wall goes: the cell the enemy's line of sight crosses out
   of the bot's own cell, never the bot's own cell (the block would land inside
   it), resting on that column's floor. Returns 0 when there is no sane spot. */
uint8_t _bot_plan_wall(server_t* server, player_t* bot, player_t* threat, bot_ai_t* ai)
{
    float dx  = threat->movement.position.x - bot->movement.position.x;
    float dy  = threat->movement.position.y - bot->movement.position.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) {
        return 0;
    }
    dx /= len;
    dy /= len;

    /* The first cell the line of sight enters, by the cell boundary it
       crosses first (a grid walk). Stepping one block along the direction
       instead crossed both boundaries at once and put 31% of the walls in the
       cell diagonally beside the bot, on the second cell of the line: the
       enemy still saw the bot's own cell, beside the wall. A 4-neighbour cell
       shares the bot's cell on the other axis, so the wall hides the whole
       column the bot stands in. */
    float px = bot->movement.position.x, py = bot->movement.position.y;
    float fx = px - floorf(px), fy = py - floorf(py);
    int   bx = (int) floorf(px), by = (int) floorf(py);
    float tx = dx > 0.0f ? (1.0f - fx) / dx : (dx < 0.0f ? fx / -dx : INFINITY);
    float ty = dy > 0.0f ? (1.0f - fy) / dy : (dy < 0.0f ? fy / -dy : INFINITY);
    int   X  = bx;
    int   Y  = by;
    if (tx <= ty) {
        X += dx > 0.0f ? 1 : -1;
    } else {
        Y += dy > 0.0f ? 1 : -1;
    }
    if (X < 1 || Y < 1 || X >= server->s_map.map.size_x - 1 || Y >= server->s_map.map.size_y - 1) {
        evt_log(bot, "bot_wall", "abort=map-edge column=%d,%d", X, Y);
        return 0;
    }

    /* Same convention as set_player_respawn_point: z grows downwards, the
       floor is the first solid block scanning down from the bot, and the wall
       cells are the two right above it. */
    int floor_z = -1;
    for (int z = (int) bot->movement.position.z; z < (int) server->s_map.map.size_z; z++) {
        if (z >= 0 && map_solid(&server->s_map.map, (uint16_t) X, (uint16_t) Y, (uint16_t) z)) {
            floor_z = z;
            break;
        }
    }
    if (floor_z < 3 || floor_z >= (int) server->s_map.map.size_z - 1) {
        evt_log(bot, "bot_wall", "abort=%s column=%d,%d floor=%d", floor_z < 3 ? "no-floor" : "water", X, Y, floor_z);
        return 0;
    }
    /* The wall rises from the wall column's floor, so it must reach the
       bot's own body: a step down in front (the wall's floor lower) leaves
       the two blocks under its crouched chest and it hides behind nothing
       (one step up still covers it). */
    int own = _bot_floor_cell(server, bot);
    if (floor_z > own || floor_z < own - 1) {
        evt_log(bot, "bot_wall", "abort=level column=%d,%d floor=%d own=%d", X, Y, floor_z, own);
        return 0;
    }

    ai->wall_x       = X;
    ai->wall_y       = Y;
    ai->wall_floor   = floor_z;
    /* It hides behind the wall from its own cell's centre, which the wall
       shares on the other axis: the dodge is measured from there, so it
       cannot drift out of the wall's shadow without moving a cell. */
    ai->cover_x      = (float) bx + 0.5f;
    ai->cover_y      = (float) by + 0.5f;
    ai->cover_blocks = 0;
    ai->build_fails  = 0;
    ai->has_cover    = 1;
    evt_log(bot, "bot_wall", "plan column=%d,%d floor=%d cells=%d,%d stand=%.1f,%.1f bot=%.1f,%.1f threat=%.1f,%.1f",
            X, Y, floor_z, floor_z - 1, floor_z - 2, ai->cover_x, ai->cover_y, bot->movement.position.x,
            bot->movement.position.y, threat->movement.position.x, threat->movement.position.y);
    return 1;
}

/* Wall cell for block n (0 = feet, 1 = chest). */
static uint8_t _bot_wall_cell_solid(server_t* server, bot_ai_t* ai, uint8_t n)
{
    return map_solid(&server->s_map.map, (uint16_t) ai->wall_x, (uint16_t) ai->wall_y,
                           (uint16_t) (ai->wall_floor - 1 - n));
}

uint8_t _bot_wall_standing(server_t* server, bot_ai_t* ai)
{
    return ai->has_cover && _bot_wall_cell_solid(server, ai, 0) && _bot_wall_cell_solid(server, ai, 1);
}

/* One wall block per call: the server rate limits placement (BLOCK_DELAY), so
   the caller waits between the two. A cell that is already solid counts, the
   terrain did the work. Returns 1 when the block is in place. */
uint8_t _bot_build_step(server_t* server, player_t* bot, bot_ai_t* ai)
{
    uint8_t n = ai->cover_blocks;
    if (_bot_wall_cell_solid(server, ai, n)) {
        ai->cover_blocks++;
        return 1;
    }
    _bot_set_tool(server, bot, TOOL_BLOCK);
    if (_bot_place_block(server, bot, (uint32_t) ai->wall_x, (uint32_t) ai->wall_y, (uint32_t) (ai->wall_floor - 1 - n))) {
        ai->cover_blocks++;
        return 1;
    }
    return 0;
}

/* A Miner's tunnel to its goal, measured: the episode starts at its first
   dig on its way there (sneaking, not chasing an enemy), and ends at the
   goal (within 4), at a death, when the goal moves, when the way to it is
   on the surface again, or after 10 s with no progress. Players saw Miners
   dig a few cells, stop, and walk back out along their own tunnel:
   bot_miner action=tunnel_end why= counts each way it ends */
void _bot_tunnel_dug(player_t* bot, bot_ai_t* ai, uint64_t now)
{
    if (!ai->path_stealth || ai->closing_in) {
        return;
    }
    float dx = ai->path_gx - bot->movement.position.x, dy = ai->path_gy - bot->movement.position.y;
    float d  = sqrtf(dx * dx + dy * dy);
    if (!ai->tun_active) {
        ai->tun_active      = 1;
        ai->tun_gx          = ai->path_gx;
        ai->tun_gy          = ai->path_gy;
        ai->tun_from        = d;
        ai->tun_best        = d;
        ai->tun_digs        = 0;
        ai->tun_at          = now;
        ai->tun_progress_at = now;
    }
    ai->tun_digs++;
}

void _bot_tunnel_end(player_t* bot, bot_ai_t* ai, const char* why, uint64_t now)
{
    if (!ai->tun_active) {
        return;
    }
    ai->tun_active = 0;
    float gdx = ai->path_gx - ai->tun_gx, gdy = ai->path_gy - ai->tun_gy;
    evt_log(bot, "bot_miner", "action=tunnel_end why=%s digs=%u from=%.0f best=%.0f s=%.1f goal=%s moved=%.0f", why,
            ai->tun_digs, ai->tun_from, ai->tun_best, (double) (now - ai->tun_at) / (double) NANO_IN_SECOND,
            ai->mode_goal ? ai->mode_goal : "none", sqrtf(gdx * gdx + gdy * gdy));
}

void _bot_tunnel_watch(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    (void) server;
    if (!ai->tun_active) {
        return;
    }
    float dx = ai->tun_gx - bot->movement.position.x, dy = ai->tun_gy - bot->movement.position.y;
    float d  = sqrtf(dx * dx + dy * dy);
    if (d < ai->tun_best - 1.0f) {
        ai->tun_best        = d;
        ai->tun_progress_at = now;
    }
    float gdx = ai->path_gx - ai->tun_gx, gdy = ai->path_gy - ai->tun_gy;
    if (d <= 4.0f) {
        _bot_tunnel_end(bot, ai, "arrived", now);
    } else if (!ai->closing_in && ai->path_len > 0 && gdx * gdx + gdy * gdy > 36.0f) {
        _bot_tunnel_end(bot, ai, "goal", now);
    } else if (!ai->closing_in && ai->path_len > 0 && !ai->path_stealth) {
        _bot_tunnel_end(bot, ai, "surface", now);
    } else if (now - ai->tun_progress_at > _bot_secs(10.0f)) {
        _bot_tunnel_end(bot, ai, "stalled", now);
    }
}
