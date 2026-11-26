// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Roles: the mode's goal for a bot, its gun, the posts of the guard and the
    campers and where they stand.
*/

#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <Server/BotHeli.h>
#include <Server/Classes.h>

/* going the hidden way: the infiltrator always, a sneaky attacker while its
   way is a sneaking one. Neither goes looking for fights (an enemy within
   BOT_SNEAK_RANGE, or one that shoots it): the infiltrator died in 2Fort's
   middle, dodging in a fight for 20 s, before it reached its tunnel */
uint8_t _bot_sneaking(const bot_ai_t* ai)
{
    return ai->role == GM_BOT_INFILTRATE || (ai->sneaky && ai->path_stealth);
}

/* A plan from the planner thread becomes the bot's way, unless one of its
   digs opens a cell the mode protects (the planner's copy of the map does
   not know them): that cell is banned and the next plan goes around it */

/* the overwatch gives its spot up for good: the next think picks another */
void _bot_camp_ban(player_t* bot, bot_ai_t* ai, const char* why)
{
    ai->camp_ban[ai->camp_bans % 8][0] = ai->camp_x;
    ai->camp_ban[ai->camp_bans % 8][1] = ai->camp_y;
    ai->camp_ban[ai->camp_bans % 8][2] = ai->camp_z;
    ai->camp_bans++;
    ai->camp_set = 0;
    evt_log(bot, "bot_camp", "action=unreachable cell=%d,%d,%d why=%s tried=%.0f", ai->camp_x, ai->camp_y, ai->camp_z,
            why, ai->camp_try);
}

/* A post: a wall of its own in the colour of the ground, then the watch.
   Home is what the team protects (the goal's home_*: the CTF's own intel).
   - the guard (sniper 0): an L, two blocks high, at its back (the side the
     enemy comes from, wx/wy) and on one side; crouched behind it, always,
     eyes on home, the look slowly sweeping a little each way;
   - the overwatch: a wall three wide and two high at its back (away from
     home, wx/wy): home in front, nothing at its back. Crouched, its aim
     sweeping across home, resting at each end;
   - a camper (and a forward post): a castle wall toward the enemy (wx/wy),
     standing behind it with only the head over the top (low to reload).
   Both crouched postures used to stand up every few seconds, or build
   nothing at all (the overwatch was shot in the back) */
void _bot_defend_hold(server_t* server, player_t* bot, bot_ai_t* ai, float wx, float wy, uint64_t now,
                             uint8_t sniper)
{
    map_store_t* map       = &server->s_map.map;
    uint8_t   overwatch = sniper && ai->overwatch;
    int       fz        = _bot_floor_cell(server, bot);
    int       cx = (int) bot->movement.position.x, cy = (int) bot->movement.position.y;
    if (!ai->dwall_planned || abs(ai->dwall_floor - fz) > 1 || abs(ai->dpost_x - cx) > 2 || abs(ai->dpost_y - cy) > 2) {
        float dx = wx - bot->movement.position.x, dy = wy - bot->movement.position.y;
        int   ax = 0, ay = 0;
        if (fabsf(dx) >= fabsf(dy)) {
            ax = dx > 0 ? 1 : -1;
        } else {
            ay = dy > 0 ? 1 : -1;
        }
        if (overwatch) {
            ax = -ax; /* the wall goes at its back, away from home */
            ay = -ay;
        }
        int px = -ay, py = ax; /* across */
        /* each column with its height: the one the bot shoots through is a
           block high (a loophole). Two high everywhere, the eye of a bot
           standing was 0.25 over the top a block away and crouched under it:
           every shot more than ~10 degrees down (from a hill) hit the wall,
           17-23% of a post's shots were held (bot_shot_held why=wall) */
        int cols[16][3], n = 0;
        ai->dwall_n = 0;
        if (!sniper) {
            cols[n][0] = ax, cols[n][1] = ay, cols[n][2] = 1, n++; /* the L: back (the loophole), corner, side */
            cols[n][0] = ax + px, cols[n][1] = ay + py, cols[n][2] = 2, n++;
            cols[n][0] = px, cols[n][1] = py, cols[n][2] = 2, n++;
        } else if (overwatch) {
            cols[n][0] = ax - px, cols[n][1] = ay - py, cols[n][2] = 2, n++; /* three wide behind */
            cols[n][0] = ax, cols[n][1] = ay, cols[n][2] = 1, n++;
            cols[n][0] = ax + px, cols[n][1] = ay + py, cols[n][2] = 2, n++;
        } else if (ai->wall_style == 1) {
            /* a bunker: a front five wide, two high with three high
               merlons at its ends, the middle column a block high (the
               loophole: a standing eye was 0.25 over a two high wall a
               block away, and shots down a hill hit it), and a wing two
               deep at each side, two then three high. A pair's holders
               stand two apart (wall_side): each puts up the front and its
               own side's wing, the two fronts overlap into one bunker */
            static const int front[5] = {3, 2, 1, 2, 3};
            for (int k = -2; k <= 2; k++) {
                cols[n][0] = ax + k * px, cols[n][1] = ay + k * py, cols[n][2] = front[k + 2], n++;
            }
            for (int side = -1; side <= 1; side += 2) {
                if (ai->wall_side != 0 && side != ai->wall_side) {
                    continue;
                }
                cols[n][0] = 2 * side * px, cols[n][1] = 2 * side * py, cols[n][2] = 2, n++;
                cols[n][0] = -ax + 2 * side * px, cols[n][1] = -ay + 2 * side * py, cols[n][2] = 3, n++;
            }
        } else {
            /* a castle front, from the idea of BattleSpades' fort_wall: seven
               wide and one high through the middle (the bot shoots over it
               standing), a three high tower at each end and a two high wing
               behind each tower. Two columns side by side were no cover on an
               open hill. Merlons a block over the front stood at the eye's
               height and held 40 of the 70 shots its own wall stopped; the
               ends two high held half the rest, so the height stays at the
               ends and the firing lane stays one high. 15 cells, all within
               a bot's four block reach from its post. A post's two holders
               (the CTF's forward posts) stand two apart: their fronts make
               one eleven wide */
            for (int k = -3; k <= 3; k++) {
                cols[n][0] = ax + k * px, cols[n][1] = ay + k * py, cols[n][2] = abs(k) == 3 ? 3 : 1, n++;
            }
            for (int side = -1; side <= 1; side += 2) {
                cols[n][0] = 3 * side * px, cols[n][1] = 3 * side * py, cols[n][2] = 2, n++;
            }
        }
        for (int row = 0; row < 3; row++) { /* the bottom row first: a block needs one under it */
            for (int k = 0; k < n; k++) {
                if (row >= cols[k][2]) {
                    continue;
                }
                ai->dwall[ai->dwall_n][0] = cx + cols[k][0];
                ai->dwall[ai->dwall_n][1] = cy + cols[k][1];
                ai->dwall[ai->dwall_n][2] = fz - 1 - row;
                ai->dwall_n++;
            }
        }
        ai->dwall_planned = 1;
        ai->dwall_floor   = fz;
        ai->dpost_x       = cx;
        ai->dpost_y       = cy;
        ai->dwall_fails   = 0;
        evt_log(bot, sniper ? "bot_camp" : "bot_defend", "action=post cell=%d,%d,%d wall=%s toward=%d,%d", cx, cy, fz,
                !sniper ? "L" : overwatch ? "back" : ai->wall_style == 1 ? "bunker" : "castle", ax, ay);
    }
    for (int k = 0; k < ai->dwall_n && ai->dwall_fails < 6 && bot->blocks > 0; k++) {
        int x = ai->dwall[k][0], y = ai->dwall[k][1], z = ai->dwall[k][2];
        if (map_solid(map, x, y, z) || !_bot_placeable(server, x, y, z) ||
            !gamemode_cell_editable(server, x, y, z))
        {
            continue;
        }
        /* camouflage: the colour of the ground the wall stands on */
        int ground = z;
        while (ground < map->size_z - 1 && !map_solid(map, x, y, ground)) {
            ground++;
        }
        bot->tool_color.raw = map_color(map, x, y, ground);
        _bot_set_tool(server, bot, TOOL_BLOCK);
        _bot_face(bot, x + 0.5f, y + 0.5f, (float) z);
        uint8_t ok = _bot_place_block(server, bot, (uint32_t) x, (uint32_t) y, (uint32_t) z);
        ai->dwall_fails = ok ? 0 : ai->dwall_fails + 1;
        evt_log(bot, sniper ? "bot_camp" : "bot_defend", "action=wall cell=%d,%d,%d ok=%u color=%06x", x, y, z, ok,
                (unsigned) (bot->tool_color.raw & 0xFFFFFF));
        ai->next_think = now + BOT_BLOCK_WAIT;
        return;
    }
    _bot_set_tool(server, bot, TOOL_GUN);
    /* a planted shooter steps sideways after a few shots */
    if (now < ai->shift_until) {
        bot->move_right = ai->shift_dir > 0;
        bot->move_left  = ai->shift_dir < 0;
        bot->crouching  = 0;
        _bot_face(bot, wx, wy, bot->movement.position.z);
        ai->next_think = now + NANO_IN_MILLI * 100;
        return;
    }
    /* the guard beats: stands and leans out for a moment, then is low again.
       A statue behind the same wall is the first thing a player learns. */
    if (!sniper) {
        if (now >= ai->beat_next) {
            ai->beat_on = !ai->beat_on;
            ai->beat_next = now + (ai->beat_on ? NANO_IN_MILLI * 900 : (uint64_t) NANO_IN_SECOND * 6);
            evt_log(bot, "bot_defend", "action=beat on=%u", ai->beat_on);
        }
    }
    if (!sniper || overwatch) {
        /* low, eyes on our intel, the look sweeping a little each way and
           resting at each end, as a watch does */
        float lx = ai->home_set ? ai->home_x : wx, ly = ai->home_set ? ai->home_y : wy;
        float t   = (float) (now % (NANO_IN_SECOND * 3600ULL)) / (float) NANO_IN_SECOND + (float) bot->id;
        float s   = sinf(t * (overwatch ? 0.45f : 0.35f));
        float off = (fabsf(s) > 0.92f ? (s > 0 ? 1.0f : -1.0f) * 0.92f : s) * (overwatch ? 0.45f : 0.35f);
        float dx = lx - bot->movement.position.x, dy = ly - bot->movement.position.y;
        float a  = atan2f(dy, dx) + off;
        float d  = fmaxf(sqrtf(dx * dx + dy * dy), 1.0f);
        _bot_face(bot, bot->movement.position.x + cosf(a) * d, bot->movement.position.y + sinf(a) * d,
                  ai->home_set ? ai->home_z - 1.0f : bot->movement.position.z);
        bot->crouching = ai->beat_on ? 0 : 1;
        if (ai->beat_on && !overwatch) {
            /* the beat looks at the way the enemy comes, not at the intel */
            _bot_face(bot, wx, wy, bot->movement.position.z);
        }
        ai->next_think = now + NANO_IN_MILLI * 100;
        return;
    }
    /* the camper: standing behind its wall, only the head shows over it,
       low only to reload */
    _bot_face(bot, wx, wy, bot->movement.position.z);
    bot->crouching = bot->reloading;
    ai->next_think = now + NANO_IN_MILLI * 200;
}

/* The camper's spot: high ground on its own side (from our spawn toward the
   enemy's patrol anchor, 20 to 50% of the way, up to 25 cells off that
   line), dry, with room to stand, scored by height and by how much of the
   way toward the enemy it sees from eye level. Picked once per life */
/* inside the map's play area (its author's boundary), with a margin */
static int _bot_in_play_area(server_t* server, int x, int y)
{
    int x0, y0, x1, y1;
    return !map_meta_play_area(server, &x0, &y0, &x1, &y1) || (x >= x0 + 2 && y >= y0 + 2 && x <= x1 - 2 && y <= y1 - 2);
}

void _bot_camp_spot(server_t* server, player_t* bot, bot_ai_t* ai)
{
    /* the overwatch: the highest spot 10 to 45 cells from home (what the
       team protects) that sees it from a crouched eye, flat round it, and
       not one its plan could not reach; among spots as high (one cell lower
       at most) the nearer 25 cells, behind home, with a view of the enemy's way and low cover
       beside it. Height counted half a point a cell before, as much as a
       cell nearer 25: the overwatch sat on low ground under a tower that saw
       the whole base. Every column of the ring is looked at (200 random
       draws missed the tops), the sight rays only for the highest ones */
    if (ai->overwatch && ai->home_set) {
        map_store_t*  map = &server->s_map.map;
        static int cand[6400][3];
        float      best = -1e9f;
        /* the way the enemy comes: from our intel toward its base */
        float ex, ey;
        gamemode_patrol_anchor(server, bot, &ex, &ey);
        float wl = sqrtf((ex - ai->home_x) * (ex - ai->home_x) + (ey - ai->home_y) * (ey - ai->home_y));
        float wux = wl > 1.0f ? (ex - ai->home_x) / wl : 0.0f, wuy = wl > 1.0f ? (ey - ai->home_y) / wl : 0.0f;
        /* a flat town has no solid ground that sees home: a second pass
           takes a roof. It gave up after the first, found=0 every think,
           camp_hx was never set and the home read as moved for ever
           (3900 searches a match) */
        int passes = ai->camp_ground ? 2 : 1;
        for (int pass = 0; pass < passes && !ai->camp_set; pass++) {
            int n = 0;
            best  = -1e9f;
            for (int oy = -45; oy <= 45 && n < 6400; oy++) {
                for (int ox = -45; ox <= 45 && n < 6400; ox++) {
                    int r2 = ox * ox + oy * oy;
                    int x = (int) ai->home_x + ox, y = (int) ai->home_y + oy;
                    if (r2 < 10 * 10 || r2 > 45 * 45 || x < 2 || y < 2 || x >= map->size_x - 2 ||
                        y >= map->size_y - 2 || !_bot_in_play_area(server, x, y))
                    {
                        continue;
                    }
                    int z = map_top(map, (uint16_t) x, (uint16_t) y);
                    if (z >= map->size_z - 2 || z < 4) {
                        continue;
                    }
                    if (ai->camp_ground && pass == 0) {
                        /* solid from its top to the bottom: no air under it */
                        uint64_t column = map_column(map, x, y);
                        int      depth  = map->size_z < 64 ? map->size_z : 64;
                        uint64_t below  = (depth >= 64 ? ~(uint64_t) 0 : (((uint64_t) 1 << depth) - 1)) &
                                         ~(((uint64_t) 1 << z) - 1);
                        if ((column & below) != below) {
                            continue;
                        }
                    }
                    /* the whole roof round a banned spot (its height, 12 cells
                       round): banned a few cells at a time, the bot went along
                       2Fort's long roof one ban after the other */
                    uint8_t banned = 0;
                    for (int k = 0; k < ai->camp_bans && k < 8; k++) {
                        int bd = abs(ai->camp_ban[k][0] - x) > abs(ai->camp_ban[k][1] - y)
                                     ? abs(ai->camp_ban[k][0] - x)
                                     : abs(ai->camp_ban[k][1] - y);
                        banned |= bd <= 3 || (bd <= 12 && abs(ai->camp_ban[k][2] - z) <= 1);
                    }
                    /* flat ground round it: the tip of a tower or a wall it falls
                       off while it builds and turns (it kept falling off one) */
                    int flat = 0;
                    for (int ny = -1; ny <= 1 && !banned; ny++) {
                        for (int nx = -1; nx <= 1; nx++) {
                            int nz = map_top(map, (uint16_t) (x + nx), (uint16_t) (y + ny));
                            flat += (nx || ny) && abs(nz - z) <= 1;
                        }
                    }
                    if (banned || flat < 6) {
                        continue;
                    }
                    cand[n][0] = x;
                    cand[n][1] = y;
                    cand[n][2] = z;
                    n++;
                }
            }
            /* from the top down: the first height with a spot that sees home,
               and the one under it */
            int top = -1;
            for (int z = 4; z < map->size_z - 2 && (top < 0 || z <= top + 1); z++) {
                for (int k = 0; k < n; k++) {
                    if (cand[k][2] != z) {
                        continue;
                    }
                    int x = cand[k][0], y = cand[k][1];
                    /* seen crouched: the overwatch keeps low */
                    if (!physics_can_see(server, x + 0.5f, y + 0.5f, (float) z - 1.6f, ai->home_x, ai->home_y,
                                         ai->home_z - 1.0f))
                    {
                        continue;
                    }
                    if (top < 0) {
                        top = z;
                    }
                    float ox = x + 0.5f - ai->home_x, oy = y + 0.5f - ai->home_y;
                    float r  = sqrtf(ox * ox + oy * oy);
                    /* behind the intel as the enemy comes: looking at it, it also
                       looks over the ground they cross (on Raindance's first pick
                       they came at its back), and that ground in sight */
                    float behind = -(ox * wux + oy * wuy) / r;
                    int   sight  = 0;
                    for (float d = 10.0f; d <= 40.0f; d += 10.0f) {
                        float tx = ai->home_x + wux * d, ty = ai->home_y + wuy * d;
                        if (tx < 0 || ty < 0 || tx >= map->size_x || ty >= map->size_y) {
                            break;
                        }
                        int tz = map_top(map, (uint16_t) tx, (uint16_t) ty);
                        sight +=
                            physics_can_see(server, x + 0.5f, y + 0.5f, (float) z - 1.6f, tx, ty, (float) tz - 1.5f);
                    }
                    /* something low beside it to crouch behind (a ledge, a crate):
                       over it, the crouched eye still sees home */
                    int cover = 0;
                    for (int ny = -1; ny <= 1; ny++) {
                        for (int nx = -1; nx <= 1; nx++) {
                            cover += (nx || ny) && map_solid(map, x + nx, y + ny, z - 1);
                        }
                    }
                    float score = (z == top ? 20.0f : 0.0f) - fabsf(r - 25.0f) * 0.3f + behind * 6.0f +
                                  (float) sight * 3.0f + (float) (cover < 4 ? cover : 4) * 2.0f;
                    if (score > best) {
                        best         = score;
                        ai->camp_x   = x;
                        ai->camp_y   = y;
                        ai->camp_z   = z;
                        ai->camp_set = 1;
                    }
                }
            }
        }
        if (ai->camp_set) {
            ai->camp_hx = ai->home_x;
            ai->camp_hy = ai->home_y;
            evt_log(bot, "bot_camp", "action=spot overwatch=1 cell=%d,%d,%d home=%.0f,%.0f,%.0f score=%.0f",
                    ai->camp_x, ai->camp_y, ai->camp_z, ai->home_x, ai->home_y, ai->home_z, best);
            return;
        }
        /* no spot even with roofs: home did not move, so the next think
           does not search again */
        ai->camp_hx = ai->home_x;
        ai->camp_hy = ai->home_y;
        evt_log(bot, "bot_camp", "action=spot overwatch=1 found=0");
    }
    map_store_t* map   = &server->s_map.map;
    quad3d_t* own   = &server->protocol.spawns[bot->team == TEAM_A ? 0 : 1];
    float     ox    = (own->from.x + own->to.x) * 0.5f, oy = (own->from.y + own->to.y) * 0.5f;
    float     ex, ey;
    gamemode_patrol_anchor(server, bot, &ex, &ey);
    float dx = ex - ox, dy = ey - oy, len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        return;
    }
    float ux = dx / len, uy = dy / len;
    float best = -1.0f;
    for (int k = 0; k < 160; k++) {
        float t = 0.2f + 0.3f * gen_rand(&server->rand), side = (gen_rand(&server->rand) * 2.0f - 1.0f) * 25.0f;
        int   x = (int) (ox + dx * t - uy * side), y = (int) (oy + dy * t + ux * side);
        if (x < 2 || y < 2 || x >= map->size_x - 2 || y >= map->size_y - 2 || !_bot_in_play_area(server, x, y)) {
            continue;
        }
        int z = map_top(map, (uint16_t) x, (uint16_t) y);
        if (z >= map->size_z - 2 || z < 4) {
            continue; /* water, or the top of the world */
        }
        float eye   = (float) z - 2.5f;
        int   sight = 0;
        for (float d = 20.0f; d <= 60.0f; d += 10.0f) {
            float tx = x + 0.5f + ux * d, ty = y + 0.5f + uy * d;
            /* a spot near the map's edge looks off it: nothing to see there
               (read as a column, it crashed the server: h4, Four_way) */
            if (tx < 0 || ty < 0 || tx >= map->size_x || ty >= map->size_y) {
                continue;
            }
            int tz = map_top(map, (uint16_t) tx, (uint16_t) ty);
            sight += physics_can_see(server, x + 0.5f, y + 0.5f, eye, tx, ty, (float) tz - 1.5f) ? 1 : 0;
        }
        float score = (float) (map->size_z - z) + 6.0f * (float) sight;
        if (score > best) {
            best        = score;
            ai->camp_x  = x;
            ai->camp_y  = y;
            ai->camp_z  = z;
            ai->camp_set = 1;
        }
    }
    if (ai->camp_set) {
        evt_log(bot, "bot_camp", "action=spot cell=%d,%d,%d score=%.0f", ai->camp_x, ai->camp_y, ai->camp_z, best);
    }
}

/* the gun of the bot's role, given while it is dead or not spawned yet, as
   a player picks a gun on the respawn screen (changing it alive is a death
   in 0.75): the guard a shotgun, the infiltrator an SMG, the rest a rifle */
void _bot_role_weapon(server_t* server, player_t* bot, bot_ai_t* ai)
{
    gm_bot_goal_t goal;
    memset(&goal, 0, sizeof(goal));
    uint8_t role = gamemode_bot_goal(server, bot, &goal) ? goal.role : GM_BOT_ATTACK;
    /* attackers and rushers: a rifle or an SMG, drawn once per bot */
    if (!ai->pref_set) {
        ai->pref_set    = 1;
        ai->pref_weapon = gen_rand(&server->rand) < 0.5f ? WEAPON_SMG : WEAPON_RIFLE;
        /* the attackers spread over the four classes */
        ai->pref_class = (uint8_t) ((int) (gen_rand(&server->rand) * GAME_CLASSES_COUNT) % GAME_CLASSES_COUNT);
    }
    uint8_t want = role == GM_BOT_DEFEND || role == GM_BOT_COVER ? WEAPON_SHOTGUN
                   : role == GM_BOT_INFILTRATE                  ? WEAPON_SMG
                   : goal.seated && goal.camp                   ? WEAPON_RIFLE
                                                                : ai->pref_weapon;
    /* campers are the mode's seats (CTF draws them, the overwatch among
       them); a mode without seats has none. The rank of the rifles made
       Babel's staircase builders campers: the camp spot, a view of the
       intel from under the slab across Nine-Eye's moat, replaced their
       staircase and they walked into the water (25 of 32 deaths in a 600 s
       match) */
    uint8_t camper    = goal.seated && goal.camp;
    uint8_t overwatch = goal.seated && goal.camp == 2;
    if (camper != ai->camper || overwatch != ai->overwatch) {
        ai->camper      = camper;
        ai->overwatch   = overwatch;
        ai->camp_set    = 0;
        ai->role_logged = 0; /* log the change */
    }
    /* a third of the team's attackers go the hidden way near the goal (in
       their squad: bot_holds_post): the 1st, 4th, 7th... attacker by id, so a team with one
       attacker has a sneaky one. By id % 4 == 2 it was nobody with 8 bots
       (that id was always the infiltrator) and players never saw a bot go
       through a tunnel but the infiltrator */
    uint8_t sneaky = 0;
    if (role == GM_BOT_ATTACK && !camper) {
        int       rank = 0;
        player_t *o, *tmp;
        HASH_ITER(hh, server->players, o, tmp)
        {
            bot_ai_t* oa = o->is_bot ? _bot_ai(o->id) : NULL;
            if (oa != NULL && o != bot && o->team == bot->team && o->id < bot->id && oa->role == GM_BOT_ATTACK &&
                !oa->camper)
            {
                rank++;
            }
        }
        sneaky = rank % 3 == 0;
    }
    /* a bot that may not dig does not go the hidden way: the tunnels it
       plans are refused cell by cell (the arena), and it crosses the whole
       map sneaking, slower than the fight it is walking into */
    if (gamemode_player_rules(server, bot) & GM_RULE_NO_DESTROY) {
        sneaky = 0;
    }
    /* where the classes play (ngCTF) the class is the way a bot plays and
       there are no roles: the Miner tunnels by its class
       (miner_way), nobody else sneaks */
    if (classes_on(server)) {
        sneaky = 0;
    }
    if (sneaky != ai->sneaky) {
        ai->sneaky      = sneaky;
        ai->role_logged = 0;
    }
    /* in a classes mode the bot picks a class like a player does (the
       "class" command's path) and the class gives the gun. There are no
       roles there: the classes are dealt round the team's
       bots by id, so every class plays from 4 bots on (drawn at random, 8
       bots could have no Miner), team B starting two classes on; a class
       switched off goes to the next one on */
    /* a bot the mode keeps without a gun (a zombie) gets no class */
    if (classes_on(server) && classes_of(server, bot) != NULL) {
        int       rank = 0;
        player_t *o, *tmp;
        HASH_ITER(hh, server->players, o, tmp)
        {
            if (o->is_bot && o != bot && o->team == bot->team && o->id < bot->id) {
                rank++;
            }
        }
        int k = (rank + (bot->team == TEAM_B ? 2 : 0)) % GAME_CLASSES_COUNT;
        for (int n = 0; n < GAME_CLASSES_COUNT && !game_class_get(k)->enabled; n++) {
            k = (k + 1) % GAME_CLASSES_COUNT;
        }
        if (bot->klass != k) {
            classes_pick(server, bot, k, "bot");
        }
        want = classes_weapon(bot->klass);
    }
    /* logged when the role or the gun changes (a rusher keeps the rifle) */
    if (bot->weapon != want || ai->role != role || !ai->role_logged) {
        ai->role_logged = 1;
        ai->role        = role;
        if (bot->weapon != want) {
            bot->weapon = want;
            set_default_player_ammo(bot);
        }
        evt_log(bot, "bot_role", "role=%s%s weapon=%s",
                ai->overwatch ? "overwatch/" : ai->camper ? "camper/" : ai->sneaky ? "sneaky/" : "",
                role == GM_BOT_DEFEND       ? "defend"
                : role == GM_BOT_INFILTRATE ? "infiltrate"
                : role == GM_BOT_COVER      ? "cover"
                : role == GM_BOT_RUSH       ? "rush"
                                            : "attack",
                want == WEAPON_SHOTGUN ? "shotgun" : want == WEAPON_SMG ? "smg"
                : want == GAME_WEAPON_ROCKET ? "rocket" : "rifle");
    }
}

uint8_t _bot_mode_goal(server_t* server, player_t* bot, bot_ai_t* ai, float* gx, float* gy, uint8_t* exact,
                              uint8_t* rush, int* gfloor, uint8_t* assault, float* wx, float* wy)
{
    gm_bot_goal_t goal;
    memset(&goal, 0, sizeof(goal));
    int         has  = gamemode_bot_goal(server, bot, &goal);
    /* on its way to a helicopter it decided to fly (BotHeli.c): its door is
       the goal until it sits in it */
    if (bot_heli_goal(server, bot, &goal)) {
        has = 1;
    }
    const char* name = has && goal.name ? goal.name : "none";
    *exact           = has ? goal.exact : 0;
    *rush            = has ? goal.rush : 0;
    *assault         = has ? goal.assault : 0;
    *wx              = goal.watch_x;
    *wy              = goal.watch_y;
    ai->role         = has ? goal.role : GM_BOT_ATTACK;
    ai->goal_hold    = has ? goal.hold : 0.0f;
    ai->goal_climb   = has ? goal.climb : 0;
    ai->goal_walk    = has ? goal.walk_only : 0;
    ai->goal_keep    = has ? goal.keep_terrain : 0;
    ai->goal_plunge  = has ? goal.plunge : 0;
    ai->goal_intent  = has ? goal.intent : 0;
    ai->camp_ground  = has ? goal.camp_ground : 0;
    ai->goal_perch   = has ? goal.perch : 0.0f;
    if ((has ? goal.wall_style : 0) != ai->wall_style || (has ? goal.wall_side : 0) != ai->wall_side) {
        ai->wall_style    = has ? goal.wall_style : 0;
        ai->wall_side     = has ? goal.wall_side : 0;
        ai->dwall_planned = 0; /* another wall: plan it again */
    }
    if (has && goal.has_home) {
        ai->home_set = 1;
        ai->home_x   = goal.home_x;
        ai->home_y   = goal.home_y;
        ai->home_z   = goal.home_z;
    }
    ai->work      = has ? goal.work : 0;
    ai->work_x    = goal.work_x;
    ai->work_y    = goal.work_y;
    ai->work_z    = goal.work_z;
    ai->work_color = has ? goal.work_color : 0;
    ai->guard_set = has && goal.has_guard;
    ai->guard_x   = goal.guard_x;
    ai->guard_y   = goal.guard_y;
    *gfloor = 0;
    if (has) {
        *gx     = goal.x;
        *gy     = goal.y;
        *gfloor = (int) goal.z;
    }
    /* a fresh bot has no goal yet: "none" is not news */
    if (strcmp(name, ai->mode_goal != NULL ? ai->mode_goal : "none") != 0) {
        evt_log(bot, "bot_goal", "goal=%s dest=%.1f,%.1f carries=%u", name, *gx, *gy, _bot_carries(server, bot));
        ai->mode_goal = name;
        /* the overwatch leaves its wall and its cover at once for the
           carrier (it finished a wall and dodged behind it for 8 s) */
        if (ai->camper && (ai->goal_intent & GM_INTENT_CHASE) && ai->phase != BOT_PHASE_PATROL) {
            _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
        }
        /* a new goal: progress toward it starts over */
        ai->goal_best     = 1e9f;
        ai->goal_progress = get_nanos();
        ai->clear_until   = 0;
    }
    return (uint8_t) has;
}
