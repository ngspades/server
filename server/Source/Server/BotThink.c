// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    A bot's decision, a few times a second (_bot_think), and what follows it
    every tick (_bot_after_think).
*/

#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <Server/Plugins.h>
#include <Server/BotProjects.h>
#include <Server/BotForts.h>

/* what a think works with, shared by its parts (_think_*) */
typedef struct
{
    uint64_t   now;
    player_t*  target;
    uint8_t    visible;
    uint8_t    body_low;
    float      gx;
    float      gy;
    uint8_t    exact;
    uint8_t    rush;
    uint8_t    assault;
    int        gfloor;
    float      wx;
    float      wy;
    uint8_t    has_goal;
    uint8_t    guard;
    uint8_t    camping;
    uint8_t    chasing;
    uint8_t    briefing;
    float      hdist;
    float      mate_dx;
    float      mate_dy;
    float      mate_d;
    uint8_t    guarding;
    int        floor_now;
    float      tx;
    float      ty;
    float      tz;
    uint8_t    ow_hunt;
    uint8_t    pushing;
    float      push_dx;
    float      push_dy;
    uint8_t    lane_move;
    float      reach;
} bot_think_t;

/* the enemy to fight: the one kept, the one that shot it, a new one in sight */
static void _think_target(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    t->target  = NULL;
    t->visible = 0;
    if (ai->has_target) {
        player_t* kept;
        HASH_FIND(hh, server->players, &ai->target_id, sizeof(ai->target_id), kept);
        if (kept != NULL && kept->alive && kept->team != bot->team) {
            if (_bot_can_see(server, bot, kept)) {
                t->visible       = 1;
                ai->last_seen = t->now;
                t->target        = kept;
            } else if (t->now - ai->last_seen < _bot_secs(BOT_TARGET_MEMORY) ||
                       /* a Miner after it underground keeps it longer (_bot_shotgun) */
                       (t->now < ai->close_in_until && t->now - ai->last_seen < _bot_secs(BOT_MINER_STALK))) {
                t->target = kept;
            }
        }
        if (t->target == NULL) {
            ai->has_target = 0;
        }
    }
    /* shot at: whoever shot is the one to answer, wherever it stands, once
       the bot had time to react (skill), if it can be seen */
    if (ai->alert_until > t->now && t->now >= ai->alert_react_at && (t->target == NULL || t->target->id != ai->alert_from)) {
        player_t* shooter;
        HASH_FIND(hh, server->players, &ai->alert_from, sizeof(ai->alert_from), shooter);
        if (shooter != NULL && shooter->alive && shooter->team != bot->team && _bot_can_see(server, bot, shooter)) {
            t->target         = shooter;
            t->visible        = 1;
            ai->has_target = 1;
            ai->target_id  = shooter->id;
            ai->last_seen  = t->now;
            float tdx = shooter->movement.position.x - bot->movement.position.x;
            float tdy = shooter->movement.position.y - bot->movement.position.y;
            evt_log(bot, "bot_alert", "action=answer target=%u dist=%.1f", shooter->id, sqrtf(tdx * tdx + tdy * tdy));
        }
    }
    /* footsteps: every bot hears them, not only a camper at its post; the
       one heard is looked for outside the view cone (_bot_pick_target) and
       looked at (_bot_snap_shot). Bots walked past enemies a step away */
    _bot_hear_steps(server, bot, ai, t->now);
    if (t->target == NULL) {
        t->target = _bot_pick_target(server, bot, ai);
        if (t->target != NULL) {
            t->visible        = 1;
            ai->has_target = 1;
            ai->target_id  = t->target->id;
            ai->last_seen  = t->now;
            float tdx = t->target->movement.position.x - bot->movement.position.x;
            float tdy = t->target->movement.position.y - bot->movement.position.y;
            evt_bot_target(bot, t->target, sqrtf(tdx * tdx + tdy * tdy), 1);
        }
    }
}

/* the guard after a dig heard near its post (1: the think is done) */
static uint8_t _think_guard_hunt(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    (void) server;
    if (t->guard && ai->heard_until > t->now) {
        float hx = ai->heard_x + 0.5f - t->gx, hy = ai->heard_y + 0.5f - t->gy;
        float bx = ai->heard_x + 0.5f - bot->movement.position.x, by = ai->heard_y + 0.5f - bot->movement.position.y;
        float px = t->gx - bot->movement.position.x, py = t->gy - bot->movement.position.y;
        float post = sqrtf(px * px + py * py);
        /* only a dig close to our intel, only from its post, and not for
           long or far: with 20 bots someone always digs somewhere, and the
           guard hunted all match and never held its post */
        uint8_t start = ai->hunting || post < 10.0f;
        if (ai->hunting && (t->now - ai->hunt_started > _bot_secs(BOT_GUARD_HUNT_MAX) || post > BOT_GUARD_LEASH)) {
            evt_log(bot, "bot_hear", "action=give_up post=%.1f", post);
            ai->heard_until = 0;
        } else if (start && hx * hx + hy * hy < BOT_GUARD_HUNT * BOT_GUARD_HUNT) {
            if (bx * bx + by * by < 1.5f * 1.5f) {
                ai->heard_until = 0; /* here, and nobody: back to the post */
            } else {
                if (!ai->hunting) {
                    ai->hunt_started = t->now;
                    evt_log(bot, "bot_hear", "action=hunt cell=%d,%d,%d dist=%.1f", ai->heard_x, ai->heard_y,
                            ai->heard_z, sqrtf(bx * bx + by * by));
                }
                ai->hunting = 1;
                t->gx          = ai->heard_x + 0.5f;
                t->gy          = ai->heard_y + 0.5f;
                t->gfloor      = ai->heard_z + 2;
                t->exact       = 0;
                t->guard       = 0;
            }
        }
    }
    if (ai->heard_until <= t->now) {
        ai->hunting = 0;
    }
    return 0;
}

/* the mode's climb in the goal's column (1: the think is done) */
static uint8_t _think_climb(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    if (ai->goal_climb != 0 && !_bot_is_wet(bot) && (int) bot->movement.position.x == (int) t->gx &&
        (int) bot->movement.position.y == (int) t->gy)
    {
        int fz = _bot_floor_cell(server, bot);
        if (ai->goal_climb == 1 && (fz > t->gfloor || ai->pillar_floor != 0)) {
            /* centred on the column first: the body is 0.9 wide, off the
               centre its head was under the next column (Babel's slab, which
               cannot be dug) and every jump wanted to dig it */
            float ox = (float) (int) t->gx + 0.5f - bot->movement.position.x;
            float oy = (float) (int) t->gy + 0.5f - bot->movement.position.y;
            if (!bot->airborne && ai->pillar_floor == 0 && ox * ox + oy * oy > 0.03f * 0.03f) {
                _bot_face(bot, (float) (int) t->gx + 0.5f, (float) (int) t->gy + 0.5f, bot->movement.position.z);
                bot->move_forward = 1;
                bot->sneaking     = 1;
                ai->next_think    = t->now + NANO_IN_MILLI * 30;
                return 1;
            }
            _bot_pillar_up(server, bot, ai, t->gfloor);
            return 1;
        }
        if (ai->goal_climb == 2 && fz < t->gfloor) {
            /* centred first, or a dig drops it next to its pillar */
            float ox = (float) (int) t->gx + 0.5f - bot->movement.position.x;
            float oy = (float) (int) t->gy + 0.5f - bot->movement.position.y;
            if (bot->airborne) {
                /* dropping onto the pillar (from the slab, or a dig): back
                   toward the centre, else nothing. The last think's walk
                   kept pushing and carried carriers over its far edge */
                _bot_clear_inputs(bot);
                if (ox * ox + oy * oy > 0.15f * 0.15f) {
                    _bot_face(bot, (float) (int) t->gx + 0.5f, (float) (int) t->gy + 0.5f, bot->movement.position.z);
                    bot->move_forward = 1;
                }
                ai->next_think = t->now + NANO_IN_MILLI * 30;
                return 1;
            }
            if (ox * ox + oy * oy > 0.15f * 0.15f) {
                _bot_face(bot, (float) (int) t->gx + 0.5f, (float) (int) t->gy + 0.5f, bot->movement.position.z);
                bot->move_forward = 1;
                bot->sneaking     = 1;
                ai->next_think    = t->now + NANO_IN_MILLI * 30;
                return 1;
            }
            if (!bot->airborne && _bot_block_ready(server, bot, BLOCKACTION_DESTROY_ONE)) {
                _bot_set_tool(server, bot, TOOL_SPADE);
                _bot_face(bot, t->gx, t->gy, (float) fz);
                uint8_t ok = _bot_block_action(server, bot, BLOCKACTION_DESTROY_ONE, (int) t->gx, (int) t->gy, fz);
                evt_log(bot, "bot_path", "action=dig_down cell=%d,%d,%d ok=%u goal_floor=%d", (int) t->gx, (int) t->gy, fz,
                        ok, t->gfloor);
            }
            ai->next_think = t->now + NANO_IN_MILLI * 100;
            return 1;
        }
    }
    return 0;
}

/* the goal's work on its spot: a block put or spaded (1: the think is done) */
static uint8_t _think_work(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    if (ai->work && !(t->visible && t->target != NULL &&
                      fabsf(t->target->movement.position.x - bot->movement.position.x) +
                              fabsf(t->target->movement.position.y - bot->movement.position.y) <
                          15.0f))
    {
        /* in the spot's own column: within a block of its centre the bot was
           still in the column of the cell it built, and refused its own body */
        if ((int) bot->movement.position.x == (int) t->gx && (int) bot->movement.position.y == (int) t->gy &&
            !bot->airborne)
        {
            _bot_clear_inputs(bot);
            uint8_t ok = 0;
            if (ai->work == 1 && bot->blocks == 0) {
                ok = _bot_harvest(server, bot);
                evt_log(bot, "bot_stair", "action=harvest ok=%u", ok);
            } else if (ai->work == 1) {
                if (ai->work_color != 0) {
                    bot->tool_color.raw = ai->work_color;
                }
                _bot_set_tool(server, bot, TOOL_BLOCK);
                _bot_face(bot, ai->work_x + 0.5f, ai->work_y + 0.5f, ai->work_z + 0.5f);
                ok = _bot_place_block(server, bot, (uint32_t) ai->work_x, (uint32_t) ai->work_y,
                                      (uint32_t) ai->work_z);
                evt_log(bot, ai->work_color != 0 ? "bot_repair" : "bot_stair",
                        "action=build cell=%d,%d,%d ok=%u blocks=%u", ai->work_x, ai->work_y, ai->work_z, ok,
                        bot->blocks);
            } else {
                _bot_set_tool(server, bot, TOOL_SPADE);
                _bot_face(bot, ai->work_x + 0.5f, ai->work_y + 0.5f, ai->work_z + 0.5f);
                ok = _bot_block_action(server, bot, BLOCKACTION_DESTROY_ONE, ai->work_x, ai->work_y, ai->work_z);
                evt_log(bot, "bot_stair", "action=break cell=%d,%d,%d ok=%u", ai->work_x, ai->work_y, ai->work_z, ok);
            }
            ai->worked_at  = t->now;
            ai->next_think = t->now + BOT_BLOCK_WAIT;
            return 1;
        }
    }
    return 0;
}

/* a camper's spot, or a forward post, as the goal */
static uint8_t _think_camp(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    t->camping = 0;
    t->chasing  = (ai->goal_intent & GM_INTENT_CHASE) != 0;
    t->briefing = (ai->goal_intent & GM_INTENT_BRIEF) != 0;
    /* a Marksman camper carries its landmine to our intel first
       (plant-mine), then goes to its spot */
    uint8_t mining = (ai->goal_intent & GM_INTENT_PLANT) != 0;
    if (t->has_goal && ai->camper && !t->rush && !t->chasing && !t->briefing && !mining) {
        /* home moved (the CTF's intel dropped where its carrier died, or
           back at its base): the overwatch takes a new spot behind cover
           that sees it there. It went back to its old spot and watched an
           empty tent */
        if (ai->overwatch && ai->camp_set && ai->home_set) {
            float mx = ai->home_x - ai->camp_hx, my = ai->home_y - ai->camp_hy;
            if (mx * mx + my * my > 4.0f * 4.0f) {
                evt_log(bot, "bot_camp", "action=move why=home_moved home=%.0f,%.0f,%.0f", ai->home_x, ai->home_y,
                        ai->home_z);
                ai->camp_set = 0;
            }
        }
        if (!ai->camp_set) {
            _bot_camp_spot(server, bot, ai);
        }
        if (ai->camp_set) {
            t->camping = 1;
            t->gx      = ai->camp_x + 0.5f;
            t->gy      = ai->camp_y + 0.5f;
            t->gfloor  = ai->camp_z;
            t->exact   = 0;
            if (ai->overwatch && ai->home_set) {
                t->wx = ai->home_x; /* the overwatch keeps its eyes on home */
                t->wy = ai->home_y;
                /* a spot the planner reaches that the bot does not: 2Fort's
                   tower roof is ten one block steps up, the bot stuck on one
                   of them life after life. 25 s walking on the way (a fight
                   is not walking: on 2Fort it gave up spot after spot it was
                   only fighting its way to), counted over its lives, and it
                   takes another; once there the count starts over (it leaves
                   for a carrier and comes back) */
                if (ai->camp_try_x != ai->camp_x || ai->camp_try_y != ai->camp_y) {
                    ai->camp_try_x = ai->camp_x;
                    ai->camp_try_y = ai->camp_y;
                    ai->camp_try   = 0.0f;
                }
                float cdx = t->gx - bot->movement.position.x, cdy = t->gy - bot->movement.position.y;
                uint8_t fighting = ai->phase != BOT_PHASE_PATROL || ai->engage_until > t->now;
                if (cdx * cdx + cdy * cdy > 2.5f * 2.5f) {
                    /* the clock runs near it only: a spot 300 blocks off
                       (a hill's, from the spawn) takes longer than 25 s */
                    if (ai->camp_try_at != 0 && !fighting &&
                        cdx * cdx + cdy * cdy < BOT_CAMP_JUDGE * BOT_CAMP_JUDGE) {
                        ai->camp_try += (float) (t->now - ai->camp_try_at) / (float) NANO_IN_SECOND;
                    }
                    ai->camp_try_at = t->now;
                    if (ai->camp_try > 25.0f) {
                        _bot_camp_ban(bot, ai, "slow");
                    }
                } else {
                    ai->camp_try_at = 0;
                    ai->camp_try    = 0.0f;
                }
            } else {
                gamemode_patrol_anchor(server, bot, &t->wx, &t->wy);
            }
        }
    }

    /* a forward post's holder (CTF, large maps): at its cell like a camper
       at its spot, a wall toward the enemy, fighting from it */
    if (t->has_goal && !t->rush && (ai->goal_intent & GM_INTENT_POST)) {
        t->camping = 1;
    }
    return 0;
}

/* out of the water to dry ground (1: the think is done) */
/* Running from its own dynamite while the fuse burns (_bot_flee_from):
   the planned way there, sprinting, or straight away from the charge
   while the plan comes; nothing else until it is BOT_FLEE_SAFE off */
static uint8_t _think_flee(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    if (ai->flee_until == 0) {
        return 0;
    }
    float dx = bot->movement.position.x - ai->flee_from_x, dy = bot->movement.position.y - ai->flee_from_y;
    if (t->now >= ai->flee_until || dx * dx + dy * dy >= BOT_FLEE_SAFE * BOT_FLEE_SAFE) {
        evt_log(bot, "bot_perk", "action=fled dist=%.1f", sqrtf(dx * dx + dy * dy));
        ai->flee_until = 0;
        return 0;
    }
    if (_bot_route_to(server, bot, ai, ai->flee_x, ai->flee_y, 0, 0, 1)) {
        return 1;
    }
    _bot_face(bot, ai->flee_x, ai->flee_y, bot->movement.position.z);
    bot->move_forward = 1;
    bot->sprinting    = 1;
    return 1;
}

static uint8_t _think_water(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    t->floor_now = _bot_floor_cell(server, bot);
    /* out once it stands about as high as the dry ground, on its feet (in
       the middle of a jump it is only passing by) */
    if (ai->escaping == 1 && !_bot_is_wet(bot) && !bot->airborne) {
        /* and with its centre over that ground: hanging on the edge of the
           shore it slid back into the water at the first move */
        if (t->floor_now <= ai->escape_floor + 1 &&
            map_solid(&server->s_map.map, (int) bot->movement.position.x, (int) bot->movement.position.y,
                            t->floor_now))
        {
            evt_log(bot, "bot_water", "action=out pos=%.1f,%.1f,%.1f", bot->movement.position.x,
                    bot->movement.position.y, bot->movement.position.z);
            ai->escaping = 0;
            _bot_pick_objective(server, bot, ai);
            /* plain walking led it into this water: go on making its own way
               (it walked straight back in from the shore otherwise) */
            ai->clear_until   = t->now + _bot_secs(8.0f);
            ai->goal_progress = t->now;
        }
    }
    if (_bot_is_wet(bot) || ai->escaping == 1) {
        /* with an objective, the planned way out of the water is the one
           toward it: the nearest shore took the carrier back to the bank it
           had come from, into the same water, over and over */
        if (t->has_goal && _bot_route_to(server, bot, ai, t->gx, t->gy, t->gfloor, t->exact, t->rush)) {
            ai->escaping = 0;
            return 1;
        }
        if (!ai->escaping) {
            float x, y;
            ai->escaping  = 2; /* no land in reach (an ocean map): fight on as usual */
            ai->route_len = 0;
            if (_bot_water_route(server, bot, ai)) {
                ai->escaping = 1;
                evt_log(bot, "bot_water", "action=route cells=%u shore=%.1f,%.1f", ai->route_len, ai->obj_x,
                        ai->obj_y);
            } else if (_bot_nearest_dry(server, bot, &x, &y)) {
                ai->obj_x    = x;
                ai->obj_y    = y;
                ai->escaping = 1;
                if (!_bot_column_usable(server, (uint16_t) x, (uint16_t) y, &ai->escape_floor)) {
                    ai->escape_floor = t->floor_now - 2;
                }
            }
            evt_log(bot, "bot_water", "action=%s pos=%.1f,%.1f,%.1f dry=%.1f,%.1f",
                    ai->escaping == 1 ? "leave" : "stuck", bot->movement.position.x, bot->movement.position.y,
                    bot->movement.position.z, ai->obj_x, ai->obj_y);
        }
        if (ai->escaping == 1) {
            ai->has_cover = 0;
            if (ai->phase != BOT_PHASE_PATROL) {
                _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
            }
            /* swim the route cell by cell (centre to centre: a corner of the
               tunnel stops a straight line), then climb the shore */
            while (ai->route_i < ai->route_len) {
                float rdx = ai->route[ai->route_i][0] + 0.5f - bot->movement.position.x;
                float rdy = ai->route[ai->route_i][1] + 0.5f - bot->movement.position.y;
                if (rdx * rdx + rdy * rdy > 0.35f * 0.35f) {
                    break;
                }
                ai->route_i++;
            }
            if (ai->route_i < ai->route_len) {
                float rx = ai->route[ai->route_i][0] + 0.5f, ry = ai->route[ai->route_i][1] + 0.5f;
                _bot_face(bot, rx, ry, bot->movement.position.z);
                bot->move_forward = 1;
                ai->next_think    = t->now + NANO_IN_MILLI * 100;
                return 1;
            }
            float angle = atan2f(ai->obj_y - bot->movement.position.y, ai->obj_x - bot->movement.position.x);
            if (ai->detour_until > t->now) {
                angle += ai->detour_turn;
            }
            uint8_t path = _bot_clear_path(server, bot, ai, angle, ai->escape_floor);
            if (path == BOT_PATH_WAIT) {
                ai->next_think = t->now + NANO_IN_MILLI * 50;
                return 1;
            }
            if (path == BOT_PATH_STOP) {
                _bot_start_detour(server, bot, ai, t->now);
            }
            if (path != BOT_PATH_WALK) {
                /* dug or built: wait for the block delay (a pillar jump in
                   progress set its own, shorter wake up) */
                if (ai->pillar_floor == 0 && ai->next_think < t->now + BOT_BLOCK_WAIT) {
                    ai->next_think = t->now + BOT_BLOCK_WAIT;
                }
                return 1;
            }
            /* straight at it: steering around would walk away from the steps
               it is cutting */
            _bot_face(bot, bot->movement.position.x + cosf(angle), bot->movement.position.y + sinf(angle),
                      bot->movement.position.z);
            /* from the water the shore is a two block step (feet at 64, land
               at 62): only a jump against it climbs it, which _bot_clear_path
               times (it jumps once the bot touches the step) */
            bot->move_forward = 1;
            if (t->target != NULL && t->visible) {
                _bot_aim_with_error(server, bot, t->target->movement.position.x, t->target->movement.position.y,
                                    t->target->movement.position.z + BOT_BODY_CENTER, ai->skill * 0.5f);
                _bot_fire(server, bot, t->target);
            }
            return 1;
        }
    } else if (ai->escaping == 2) {
        ai->escaping = 0;
        _bot_pick_objective(server, bot, ai);
    }
    return 0;
}

/* no enemy to fight: the way to the goal, walls, cover and the walk */
static void _think_walk(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    /* within reach of a shared bridge cell: place it now, even when the
       planner found a way to the cell itself (it is walking a hole) */
    if (bot_project_step(server, bot, t->gfloor)) {
        ai->next_think = t->now + BOT_BLOCK_WAIT;
        return;
    }
    /* in the stand a structure assigned it: put the reserved cell */
    if (bot_fort_step(server, bot)) {
        ai->next_think = t->now + BOT_BLOCK_WAIT;
        return;
    }
    if (ai->phase != BOT_PHASE_PATROL) {
        _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
    }
    ai->has_cover = 0;

    /* The mode's objective comes first while no enemy is in range: when
       walking around obstacles gets the bot no closer for a while (a
       building between it and the intel, the sea between the bases), it
       heads straight for it and makes its own way: digs through walls,
       bridges water and gaps, builds steps up. */
    if (t->has_goal) {
        float pdx = t->gx - bot->movement.position.x;
        float pdy = t->gy - bot->movement.position.y;
        float pd  = sqrtf(pdx * pdx + pdy * pdy);
        /* the planned way; on the exact goal cell the creep below takes
           over (pickup and capture want it still on that cell) */
        int  fdiff   = t->gfloor > 0 ? abs(_bot_floor_cell(server, bot) - t->gfloor) : 0;
        if ((t->guard || t->camping) && pd < 1.5f && fdiff <= 2) {
            /* a camper hearing steps leaves its watch and turns to them,
               standing, until it sees who it is (then it is a fight) or
               the sound is old */
            if (t->camping && _bot_hear_steps(server, bot, ai, t->now)) {
                _bot_set_tool(server, bot, TOOL_GUN);
                bot->crouching = 0;
                _bot_face(bot, ai->steps_x, ai->steps_y, ai->steps_z + BOT_BODY_CENTER);
                ai->next_think = t->now + NANO_IN_MILLI * 100;
                return;
            }
            _bot_defend_hold(server, bot, ai, t->wx, t->wy, t->now, t->camping);
            return;
        }
        if (ai->goal_hold > 0.0f && pd < ai->goal_hold && !t->camping) {
            /* close enough (the bodyguard beside the infiltrator, a squad
               behind its leader): watch where it looks, gun out */
            _bot_set_tool(server, bot, TOOL_GUN);
            _bot_face(bot, t->wx, t->wy, bot->movement.position.z);
            ai->next_think = t->now + NANO_IN_MILLI * 150;
            return;
        }
        /* an exact goal on an edge (the top of a pillar one cell wide):
           the last cells slowly and straight, the planned way's flat
           steps run and a carrier ran past Babel's pillars */
        uint8_t narrow = t->exact && t->gfloor > 0 && fdiff <= 2 && pd < 3.0f &&
                         _nav_edge(&server->s_map.map, (int) t->gx, (int) t->gy, t->gfloor);
        /* back on the way after something else (a fight, a hold): the time
           spent there is no time spent failing to get closer. The clock ran
           through the fight, and the first think after it, its new plan not
           back yet, read as stuck and walked straight into the nearest wall
           (`bot_path action=stuck` a second after a kill, most of the
           arena's goal=hunt-enemy ones) */
        if (t->now - ai->walk_at > _bot_secs(1.0f)) {
            ai->goal_progress = t->now;
        }
        ai->walk_at = t->now;
        if (!(t->exact && pd < 1.0f && fdiff <= 2) && !narrow &&
            _bot_route_to(server, bot, ai, t->gx, t->gy, t->gfloor, t->exact, t->rush)) {
            ai->goal_progress = t->now;
            return;
        }
        if (pd < ai->goal_best - 1.0f) {
            ai->goal_best     = pd;
            ai->goal_progress = t->now;
        } else if (ai->clear_until < t->now && t->now - ai->goal_progress > _bot_secs(4.0f) && !ai->work &&
                   ai->plan_ticket == 0) {
            /* (never with a work to do at the goal: a staircase builder
               read as stuck under its spot dug away the step it had just
               put, 199 times a match; nor while a plan is on its way: the
               planner may still find the way round) */
            ai->clear_until   = t->now + _bot_secs(8.0f);
            ai->goal_progress = t->now;
            ai->goal_best     = pd;
            evt_log(bot, "bot_path",
                    "action=stuck goal=%s dist=%.1f floor=%d goal_floor=%d pos=%.1f,%.1f,%.1f air=%u wet=%u escaping=%u fort=%u",
                    ai->mode_goal, pd, _bot_floor_cell(server, bot), t->gfloor, bot->movement.position.x,
                    bot->movement.position.y, bot->movement.position.z, bot->airborne, bot->wade, ai->escaping,
                    bot_fort_builder(bot));
            plugins_on_bot_stuck(server, bot, "no_progress");
        }
        if (ai->clear_until > t->now && pd > 0.6f && ai->role != GM_BOT_COVER) {
            float angle = atan2f(pdy, pdx);
            if (ai->detour_until > t->now) {
                angle += ai->detour_turn;
            }
            uint8_t path = _bot_clear_path(server, bot, ai, angle, t->gfloor);
            if (path == BOT_PATH_WAIT) {
                ai->next_think = t->now + NANO_IN_MILLI * 50;
                return;
            }
            if (path == BOT_PATH_STOP) {
                _bot_start_detour(server, bot, ai, t->now);
            }
            if (path != BOT_PATH_WALK) {
                if (ai->pillar_floor == 0 && ai->next_think < t->now + BOT_BLOCK_WAIT) {
                    ai->next_think = t->now + BOT_BLOCK_WAIT;
                }
                return;
            }
            _bot_face(bot, bot->movement.position.x + cosf(angle), bot->movement.position.y + sinf(angle),
                      bot->movement.position.z);
            bot->move_forward = 1;
            return;
        }
    }
    _bot_set_tool(server, bot, TOOL_GUN);

    if (!t->has_goal) {
        t->gx = ai->obj_x;
        t->gy = ai->obj_y;
        if (t->target != NULL) {
            t->gx = t->target->movement.position.x;
            t->gy = t->target->movement.position.y;
        } else {
            float dx = t->gx - bot->movement.position.x;
            float dy = t->gy - bot->movement.position.y;
            if ((dx * dx + dy * dy) < 6.0f * 6.0f) {
                _bot_pick_objective(server, bot, ai);
                t->gx = ai->obj_x;
                t->gy = ai->obj_y;
            }
        }
    }

    float dx = t->gx - bot->movement.position.x;
    float dy = t->gy - bot->movement.position.y;
    float gd = sqrtf(dx * dx + dy * dy);
    if (t->exact && gd < BOT_GOAL_STOP) {
        /* standing by the intel / in the tent: the server checks pickup and
           capture every tick on a box of 3 blocks round it, so walking
           through is enough; keeping still on the cell is only what makes
           the take certain */
        return;
    }
    if (t->exact && gd < 3.0f) {
        /* creep onto the exact cell: a full think of walking overshoots it;
           onto a narrow one sneaking. Right over it the heading to it
           flips, and facing it each think the bot turned round on the
           spot: within 0.8 it keeps its heading and steps only when the
           cell is ahead */
        if (gd > 0.8f) {
            _bot_face(bot, t->gx, t->gy, bot->movement.position.z);
            bot->move_forward = 1;
        } else {
            /* toward the cell without turning: forward, back or sideways */
            float hx = bot->movement.forward_orientation.x, hy = bot->movement.forward_orientation.y;
            float sx = bot->movement.strafe_orientation.x, sy = bot->movement.strafe_orientation.y;
            float a = dx * hx + dy * hy, b = dx * sx + dy * sy;
            bot->move_forward   = a > 0.1f;
            bot->move_backwards = a < -0.1f;
            bot->move_right     = b > 0.1f;
            bot->move_left      = b < -0.1f;
        }
        bot->sneaking     = t->gfloor > 0 && _nav_edge(&server->s_map.map, (int) t->gx, (int) t->gy, t->gfloor);
        /* the last cell and a half onto an edge crouched, the slowest gait:
           a body starts to drop off the slab only with its centre already
           over the pillar, and in the air nothing slows it (Babel's
           carriers sneaked onto their pillar and over its far side) */
        bot->crouching    = bot->sneaking && gd < 1.5f;
        /* the exact cell a level up (a staircase step): crouched or sneaking
           nobody climbs. A Babel builder one step under its spot stood
           there 500 s, the staircase stuck at 10 cells of 206. Standing,
           walking into the step climbs it; a jump too threw bots off the
           sides of a staircase two wide (100 falls a match) */
        if (t->gfloor > 0 && t->gfloor < _bot_floor_cell(server, bot) && !bot->airborne) {
            bot->sneaking = bot->crouching = 0;
        }
        ai->next_think    = t->now + NANO_IN_MILLI * (bot->sneaking ? 30 : 50);
        return;
    }
    _bot_walk_to(server, bot, ai, t->gx, t->gy);
    /* near an exact goal (the intel, the tent) it walks in: running and
       hopping, a bot at the top of Raindance's intel tower went 10
       blocks past the intel in a second and off the edge (the plan was
       done 5 blocks short, the creep starts at 3) */
    if (t->exact && gd < 8.0f) {
        bot->sneaking  = t->gfloor > 0 && (_nav_edge(&server->s_map.map, (int) t->gx, (int) t->gy, t->gfloor) ||
                                       _nav_edge(&server->s_map.map, (int) bot->movement.position.x,
                                                 (int) bot->movement.position.y, _bot_floor_cell(server, bot)));
        ai->next_think = t->now + NANO_IN_MILLI * 50;
        return;
    }
    /* on the way, players run; the infiltrator and its cover sneak */
    if (t->rush || !ai->path_stealth) {
        bot->sprinting = 1;
    }
    if (ai->next_hop <= t->now) {
        if (!bot->airborne && gen_rand(&server->rand) < 0.25f) {
            bot->jumping = 1;
        }
        ai->next_hop = t->now + _bot_secs(1.5f + gen_rand(&server->rand) * 2.0f);
    }
    return;
}

/* an enemy to fight: the role's way of fighting it, the phase (1: the think is done) */
static uint8_t _think_fight(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    t->tx = t->target->movement.position.x;
    t->ty = t->target->movement.position.y;
    t->tz = t->target->movement.position.z;

    /* a bot on a roof or a tree top it flew to fights from up there: the
       Rocketeer's whole point (and a jetpack perk's) is the high ground.
       Walking down to trade shots on the street threw the height away, and
       the fight's own cover search took it off the roof to duck behind
       something on the ground. An enemy out of the gun's reach is not
       fought from here (an SMG does not reach the street below): the bot
       closes in and the next roof is on the way */
    /* on the post of a watchtower or a barricade's deck: the same, the
       height is what the team raised it for (the fight's cover search took
       the watch down the stair to duck behind a wall on the ground) */
    if (t->visible && bot_fort_manning(server, bot) && t->hdist <= _bot_weapon_range(bot) * BOT_FIRE_STRETCH) {
        _bot_clear_inputs(bot);
        if (ai->phase != BOT_PHASE_PATROL) {
            _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
        }
        _bot_set_tool(server, bot, TOOL_GUN);
        _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
        if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
            _bot_fire(server, bot, t->target);
        }
        if (t->now >= ai->perch_logged) {
            ai->perch_logged = t->now + _bot_secs(2.0f);
            evt_log(bot, "bot_fort", "action=post_fight target=%u dist=%.1f", t->target->id, t->hdist);
        }
        ai->next_think = t->now + NANO_IN_MILLI * 100;
        return 1;
    }
    if (t->visible && _bot_perched(server, bot, ai, t->now) &&
        t->hdist <= _bot_weapon_range(bot) * BOT_FIRE_STRETCH) {
        _bot_clear_inputs(bot);
        if (ai->phase != BOT_PHASE_PATROL) {
            _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
        }
        _bot_set_tool(server, bot, TOOL_GUN);
        _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
        if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
            _bot_fire(server, bot, t->target);
        }
        /* the hold is the fight's: while it can shoot from up here it stays,
           and leaves it a hold after the last enemy */
        float hold = ai->goal_perch > 0.0f ? BOT_PERCH_HOLD_GOAL : BOT_PERCH_HOLD;
        if (ai->perch_until < t->now + _bot_secs(hold)) {
            ai->perch_until = t->now + _bot_secs(hold);
        }
        if (t->now >= ai->perch_logged) {
            ai->perch_logged = t->now + _bot_secs(2.0f);
            evt_log(bot, "bot_perch", "action=hold cell=%.0f,%.0f,%d target=%u dist=%.1f", ai->jet_tx - 0.5f,
                    ai->jet_ty - 0.5f, ai->jet_tfloor, t->target->id, t->hdist);
        }
        ai->next_think = t->now + NANO_IN_MILLI * 100;
        return 1;
    }

    /* the camper at its spot does not move: head over its wall, rifle on
       whoever shows (low only to reload) */
    if (t->camping) {
        float cdx = t->gx - bot->movement.position.x, cdy = t->gy - bot->movement.position.y;
        if (cdx * cdx + cdy * cdy < 2.5f * 2.5f) {
            if (ai->phase != BOT_PHASE_PATROL) {
                _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
            }
            bot->crouching = ai->overwatch || bot->reloading;
            if (t->visible) {
                _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
                if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
                    _bot_fire(server, bot, t->target);
                }
            } else {
                _bot_face(bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER);
            }
            ai->next_think = t->now + NANO_IN_MILLI * 150;
            return 1;
        }
    }
    /* the overwatch after the carrier of our intel: with it in sight and in
       its rifle's reach, it stands where it is and shoots it (out in the
       open is where it went to see it); anyone else is shot on the move on
       the way to the carrier. It fought from its post instead: a wall, a
       cover 26 blocks off, ducking, while the carrier ran. Every camper on
       the hunt does the same: it walked all the way up to the carrier and
       fought it at SMG distance with its rifle */
    t->ow_hunt = ai->camper && t->chasing;
    /* an attacker on its way, the enemy far: on, shooting on the move
       (_bot_pushes) */
    t->push_dx = t->tx - bot->movement.position.x, t->push_dy = t->ty - bot->movement.position.y;
    t->pushing = t->has_goal && _bot_pushes(server, ai, bot, sqrtf(t->push_dx * t->push_dx + t->push_dy * t->push_dy)) &&
                      t->mate_d > (ai->goal_hold > 0.0f ? ai->goal_hold : 1.5f);
    if (t->pushing && ai->phase != BOT_PHASE_PATROL) {
        _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
    }
    if (t->ow_hunt && t->visible && _bot_carries(server, t->target)) {
        float cdx = t->tx - bot->movement.position.x, cdy = t->ty - bot->movement.position.y;
        if (cdx * cdx + cdy * cdy < _bot_weapon_range(bot) * _bot_weapon_range(bot)) {
            _bot_clear_inputs(bot);
            if (ai->phase != BOT_PHASE_PATROL) {
                _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
            }
            _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
            if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
                _bot_fire(server, bot, t->target);
            }
            if (t->now >= ai->hunt_logged) {
                ai->hunt_logged = t->now + _bot_secs(2.0f);
                evt_log(bot, "bot_overwatch", "action=shoot_carrier target=%u dist=%.1f", t->target->id,
                        sqrtf(cdx * cdx + cdy * cdy));
            }
            ai->next_think = t->now + NANO_IN_MILLI * 100;
            return 1;
        }
    }
    /* the rusher shoots on the move: it keeps walking its planned way to
       the intel and turns its gun on the enemy, the feet go on (the input is
       relative to where it looks, so the step is turned into that frame) */
    /* a squad on its lane, and the escort of our carrier: shooting on the
       move, they keep up with whom they go with */
    t->lane_move = ((ai->goal_intent & GM_INTENT_WITH) &&
                         t->mate_d > (ai->goal_hold > 0.0f ? ai->goal_hold : 1.5f)) ||
                        /* a camper on its way to its post: it got stuck building
                           walls in every fight a few cells short of it */
                        (t->camping && t->mate_d > 1.5f) || t->ow_hunt || t->pushing;
    if (((ai->role == GM_BOT_RUSH && t->has_goal) || (t->guarding && t->mate_d > BOT_COVER_NEAR) || t->lane_move) && !t->rush && t->visible) {
        if (_bot_route_to(server, bot, ai, t->gx, t->gy, t->gfloor, t->exact, t->rush) && bot->move_forward) {
            float fx = bot->movement.forward_orientation.x, fy = bot->movement.forward_orientation.y;
            float fl = sqrtf(fx * fx + fy * fy);
            uint8_t sprint = bot->sprinting;
            _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
            if (fl > 0.01f) {
                fx /= fl;
                fy /= fl;
                float ax = bot->movement.forward_orientation.x, ay = bot->movement.forward_orientation.y;
                float al = sqrtf(ax * ax + ay * ay);
                ax /= al > 0.01f ? al : 1.0f;
                ay /= al > 0.01f ? al : 1.0f;
                float ahead = fx * ax + fy * ay;
                float side  = fx * bot->movement.strafe_orientation.x + fy * bot->movement.strafe_orientation.y;
                bot->move_forward   = ahead > 0.38f;
                bot->move_backwards = ahead < -0.38f;
                bot->move_right     = side > 0.38f;
                bot->move_left      = side < -0.38f;
            }
            (void) sprint;
            bot->sprinting = 0; /* no shooting while sprinting */
            if (ai->phase != BOT_PHASE_PATROL) {
                _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
            }
            if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
                _bot_fire(server, bot, t->target);
            }
            ai->next_think = t->now + NANO_IN_MILLI * 100;
            return 1;
        }
    }
    /* the bodyguard fights in the open around the infiltrator, like the
       carrier's team: no wall */
    if (ai->role == GM_BOT_COVER) {
        t->assault = 1;
    }
    /* an enemy right there: attack it, hiding takes longer than the fight
       (the guard keeps its wall) */
    if (ai->role != GM_BOT_DEFEND) {
        ai->close_fight = t->hdist < BOT_CLOSE_FIGHT || (ai->close_fight && t->hdist < BOT_CLOSE_FIGHT * 1.4f);
        if (ai->close_fight) {
            t->assault = 1;
        }
    }
    /* out of its gun's reach (an SMG at 50 blocks): closer first, walking
       around what is in the way, before stopping to fight. Not a camper:
       its rifle is for range, and it walked up to enemies it then fought
       at SMG distance */
    t->reach = _bot_weapon_range(bot);
    if (!t->assault && ai->role != GM_BOT_DEFEND && !ai->camper && t->hdist > t->reach * 0.9f) {
        if (ai->phase != BOT_PHASE_PATROL) {
            _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
        }
        ai->has_cover = 0;
        _bot_set_tool(server, bot, TOOL_GUN);
        _bot_walk_to(server, bot, ai, t->tx, t->ty);
        bot->sprinting = t->hdist > t->reach * 1.5f;
        ai->next_think = t->now + NANO_IN_MILLI * 150;
        return 1;
    }
    /* the carrier's team fights in the open; the old way back once the
       intel is home or lost */
    if (t->assault && ai->phase != BOT_PHASE_ASSAULT) {
        ai->has_cover = 0;
        ai->ducked    = 0;
        _bot_set_phase(bot, ai, BOT_PHASE_ASSAULT, 0);
    } else if (!t->assault && ai->phase == BOT_PHASE_ASSAULT) {
        _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
    }
    if (ai->phase == BOT_PHASE_PATROL && ai->role == GM_BOT_DEFEND) {
        /* the guard fights from behind its own wall, never builds another,
           and does not wander off its post */
        float px = ai->dpost_x + 0.5f - bot->movement.position.x, py = ai->dpost_y + 0.5f - bot->movement.position.y;
        ai->dodge_dir       = (gen_rand(&server->rand) < 0.5f) ? 1 : -1;
        ai->next_dodge_flip = t->now;
        ai->next_posture    = t->now;
        ai->ducked          = 0;
        ai->burst_left      = 0;
        ai->next_rebuild    = t->now + _bot_secs(3600.0f);
        ai->has_cover       = ai->dwall_planned && px * px + py * py < 2.0f * 2.0f;
        ai->natural_cover   = 0; /* the post's wall, never map cover */
        ai->cover_x         = ai->has_cover ? ai->dpost_x + 0.5f : bot->movement.position.x;
        ai->cover_y         = ai->has_cover ? ai->dpost_y + 0.5f : bot->movement.position.y;
        if (ai->has_cover) {
            ai->wall_x       = ai->dwall[0][0];
            ai->wall_y       = ai->dwall[0][1];
            ai->wall_floor   = ai->dwall_floor;
            ai->cover_blocks = 2;
        }
        _bot_set_phase(bot, ai, BOT_PHASE_DODGE, 0);
    } else if ((t->ow_hunt || t->pushing) && ai->phase == BOT_PHASE_PATROL) {
        /* an enemy out of sight (or far, for an attacker on its way): no
           cover, on toward the goal */
        if (!_bot_route_to(server, bot, ai, t->gx, t->gy, t->gfloor, t->exact, t->rush)) {
            _bot_walk_to(server, bot, ai, t->gx, t->gy);
        }
        ai->next_think = t->now + NANO_IN_MILLI * 100;
        return 1;
    } else if (ai->phase == BOT_PHASE_PATROL) {
        _bot_engage(server, bot, ai, t->target, t->hdist, t->now);
    }
    return 0;
}

/* the fight in its phase: assault, cover, dodge */
static void _think_phase(server_t* server, player_t* bot, bot_ai_t* ai, bot_think_t* t)
{
    switch (ai->phase) {
        case BOT_PHASE_ASSAULT: {
            /* No wall: straight at the enemy and shooting, and once it is
               close, still, crouched and shooting, as a player in the open
               does (crouched, the aim is steadier). It zig-zagged there, which
               read as a bot swinging about. The team's carrier is on its way
               home and every enemy busy with this bot is not on it */
            _bot_set_tool(server, bot, TOOL_GUN);
            if (t->visible) {
                _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
                /* the bodyguard holds by the infiltrator, it does not charge */
                if (t->hdist > 8.0f && ai->role != GM_BOT_COVER) {
                    bot->move_forward = 1;
                } else {
                    /* crouched only if the target stays in sight from there
                       (a low wall between them: it stands to shoot over it) */
                    float low      = bot->movement.position.z + (t->body_low ? 0.0f : 0.9f); /* z down */
                    bot->crouching = physics_can_see(server, bot->movement.position.x, bot->movement.position.y, low,
                                                     t->tx, t->ty, t->tz + BOT_BODY_CENTER);
                    uint8_t stance = bot->crouching ? 1 : 2;
                    if (stance != ai->close_stance) {
                        ai->close_stance = stance;
                        evt_log(bot, "bot_close", "stance=%s dist=%.1f target=%u", bot->crouching ? "crouch" : "stand",
                                t->hdist, t->target->id);
                    }
                }
                if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
                    _bot_fire(server, bot, t->target);
                }
            } else if (ai->role != GM_BOT_COVER) {
                /* lost sight of it: after it, around what hides it (the
                   bodyguard stays with the infiltrator) */
                _bot_walk_to(server, bot, ai, t->tx, t->ty);
                bot->sprinting = 1;
            }
            break;
        }

        case BOT_PHASE_BUILD: {
            /* Stand still, look at the cell like a player would and place the
               two blocks one think apart: BLOCK_DELAY drops a second block in
               the same instant. */
            _bot_face(bot, (float) ai->wall_x + 0.5f, (float) ai->wall_y + 0.5f, (float) (ai->wall_floor - 1 - ai->cover_blocks));
            if (_bot_build_step(server, bot, ai)) {
                ai->build_fails = 0;
                ai->next_think  = t->now + BOT_BLOCK_WAIT;
            } else if (++ai->build_fails < 2) {
                ai->next_think = t->now + BOT_BLOCK_WAIT; /* likely the rate limit, try once more */
            }
            if (ai->cover_blocks >= 2 || ai->build_fails >= 2 || t->now >= ai->phase_until) {
                evt_log(bot, "bot_wall", "%s column=%d,%d blocks_up=%u blocks_left=%u",
                        ai->cover_blocks >= 2 ? "done" : "gave_up", ai->wall_x, ai->wall_y, ai->cover_blocks,
                        bot->blocks);
                if (ai->cover_blocks == 0) {
                    ai->has_cover = 0; /* nothing to hide behind, dodge in the open */
                }
                _bot_set_phase(bot, ai, BOT_PHASE_DODGE, 0);
            }
            break;
        }

        case BOT_PHASE_DODGE:
        default: {
            /* A wall of its own stops hiding it as soon as the enemy is more
               than 45 degrees off it: it crouched behind nothing while the
               enemy walked round the wall, until the rebuild look below ran
               (every 8 s), and stood fully exposed. Out of cover it fights
               the open way, dodging wide, and the next rebuild look may find
               map cover or put a wall up again. The guard's post is not this
               wall: it holds it and sweeps its aim, from the L it built. */
            if (ai->has_cover && !ai->natural_cover && !(ai->role == GM_BOT_DEFEND && ai->dwall_planned)) {
                float ex = t->tx - ai->cover_x, ey = t->ty - ai->cover_y;
                float wdx = (float) ai->wall_x + 0.5f - ai->cover_x, wdy = (float) ai->wall_y + 0.5f - ai->cover_y;
                float el = sqrtf(ex * ex + ey * ey), wl = sqrtf(wdx * wdx + wdy * wdy);
                if (el > 0.01f && wl > 0.01f && (ex * wdx + ey * wdy) / (el * wl) < BOT_WALL_IN_FRONT) {
                    ai->has_cover = 0;
                    ai->cover_x   = bot->movement.position.x;
                    ai->cover_y   = bot->movement.position.y;
                    evt_log(bot, "bot_wall", "action=abandon why=flanked");
                }
            }
            /* the wall is gone or no longer between us: raise a new one where
               the bot stands now, as long as there are blocks to spend */
            if (t->now >= ai->next_rebuild && t->hdist > BOT_WALL_MIN_DIST) {
                uint8_t rebuild = 0;
                if (ai->natural_cover) {
                    /* the map's cover: still between them? */
                    rebuild = physics_can_see(server, t->tx, t->ty, t->tz - 1.0f, ai->cover_x, ai->cover_y,
                                              (float) _bot_floor_cell(server, bot) - 1.6f) != 0;
                } else if (ai->has_cover && !_bot_wall_standing(server, ai)) {
                    rebuild = 1;
                } else if (ai->has_cover) {
                    float ex = t->tx - ai->cover_x, ey = t->ty - ai->cover_y;
                    float wdx = (float) ai->wall_x + 0.5f - ai->cover_x, wdy = (float) ai->wall_y + 0.5f - ai->cover_y;
                    float el = sqrtf(ex * ex + ey * ey), wl = sqrtf(wdx * wdx + wdy * wdy);
                    if (el > 0.01f && wl > 0.01f && (ex * wdx + ey * wdy) / (el * wl) < BOT_WALL_IN_FRONT) {
                        rebuild = 1; /* enemy flanked it */
                    }
                } else if (!_bot_is_wet(bot)) {
                    rebuild = 1; /* fought in the open so far */
                }
                ai->next_rebuild = t->now + _bot_secs(BOT_REBUILD_DELAY);
                float cx, cy;
                if (rebuild && _bot_cover_from(server, bot, t->tx, t->ty, t->tz, BOT_COVER_RADIUS, &cx, &cy)) {
                    ai->has_cover     = 1;
                    ai->natural_cover = 1;
                    ai->in_cover      = 0;
                    ai->cover_x       = cx;
                    ai->cover_y       = cy;
                    evt_log(bot, "bot_cover", "action=moved spot=%.1f,%.1f dist=%.1f", cx, cy, t->hdist);
                } else if (rebuild && bot->blocks > 0 && gen_rand(&server->rand) < BOT_WALL_CHANCE &&
                           _bot_plan_wall(server, bot, t->target, ai))
                {
                    ai->natural_cover = 0;
                    _bot_set_phase(bot, ai, BOT_PHASE_BUILD, t->now + _bot_secs(3.0f));
                    break;
                }
            }
            /* to the cover it found first, gun on the enemy */
            if (ai->natural_cover && !ai->in_cover) {
                float mx = ai->cover_x - bot->movement.position.x, my = ai->cover_y - bot->movement.position.y;
                if (mx * mx + my * my < 0.4f * 0.4f) {
                    ai->in_cover = 1;
                } else {
                    _bot_face(bot, ai->cover_x, ai->cover_y, bot->movement.position.z);
                    bot->move_forward = 1;
                    ai->next_think    = t->now + NANO_IN_MILLI * 100;
                    break;
                }
            }

            /* axis across the line of fire, measured from the spot behind the
               wall so the bot never strays from its cover */
            float ex = t->tx - ai->cover_x;
            float ey = t->ty - ai->cover_y;
            float el = sqrtf(ex * ex + ey * ey);
            if (el < 0.01f) {
                ex = 1.0f;
                ey = 0.0f;
                el = 1.0f;
            }
            ex /= el;
            ey /= el;
            float lx = -ey;
            float ly = ex;
            float ox = bot->movement.position.x - ai->cover_x;
            float oy = bot->movement.position.y - ai->cover_y;
            float lateral = ox * lx + oy * ly;
            float depth   = ox * ex + oy * ey;

            /* unpredictable rhythm: good bots change direction more often */
            if (t->now >= ai->next_dodge_flip) {
                ai->dodge_dir       = (int8_t) -ai->dodge_dir;
                ai->next_dodge_flip = t->now + _bot_secs(0.3f + gen_rand(&server->rand) * (1.0f - 0.5f * ai->skill));
            }
            float span = ai->has_cover ? BOT_DODGE_SPAN : BOT_DODGE_SPAN * 2.5f;
            if (lateral > span) {
                ai->dodge_dir = -1;
            } else if (lateral < -span) {
                ai->dodge_dir = 1;
            }
            float sx = lx * (float) ai->dodge_dir;
            float sy = ly * (float) ai->dodge_dir;
            float px = bot->movement.position.x + sx * 2.0f;
            float py = bot->movement.position.y + sy * 2.0f;
            if (px < 0.0f || py < 0.0f || is_water_column(server, (uint16_t) px, (uint16_t) py)) {
                ai->dodge_dir = (int8_t) -ai->dodge_dir; /* never dodge into the sea */
                sx = -sx;
                sy = -sy;
            }
            if (ai->has_cover && !ai->natural_cover) {
                /* behind a two block wall of its own there is no room to
                   strafe: a step aside and the enemy sees it round the wall
                   (it moved 1.2 blocks either way, out of a one cell wall's
                   shadow, and stood in the open crouched as if covered). It
                   stays in the wall's shadow and only the head moves, up to
                   fire and down to reload; map cover is wider and keeps the
                   whole dodge */
                sx = 0.0f;
                sy = 0.0f;
            }

            /* duck behind the wall, then stand up and shoot over it */
            if (t->now >= ai->next_posture) {
                ai->ducked = ai->has_cover && !ai->ducked;
                if (ai->ducked) {
                    ai->next_posture = t->now + _bot_secs(0.4f + gen_rand(&server->rand) * 0.8f);
                } else {
                    ai->next_posture = t->now + _bot_secs(0.6f + gen_rand(&server->rand) * (1.4f - 0.6f * ai->skill));
                    ai->burst_left   = 1 + (uint8_t) (gen_rand(&server->rand) * (1.0f + ai->skill * 3.0f));
                }
            }

            if (t->visible) {
                _bot_aim_with_error(server, bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER, ai->skill);
            } else {
                _bot_face(bot, t->tx, t->ty, t->tz + BOT_BODY_CENTER);
            }
            if (sx != 0.0f || sy != 0.0f) {
                _bot_strafe_world(bot, sx, sy); /* a zero step would read as "left" */
            }
            if (depth < -BOT_DODGE_SPAN) {
                bot->move_forward = 1; /* drifted away from the wall */
            } else if (depth > 0.3f) {
                bot->move_backwards = 1; /* do not walk into or past it */
            }

            if (ai->ducked) {
                bot->crouching = 1;
                _bot_set_tool(server, bot, TOOL_GUN);
            } else {
                if (!bot->airborne && gen_rand(&server->rand) < 0.08f + 0.12f * ai->skill) {
                    bot->jumping = 1; /* a hop makes the head harder to hit */
                }
                if (t->visible && ai->burst_left > 0) {
                    /* deliberately skip some shots: even a good bot does not
                       fire on every window, and weak ones waste a lot */
                    if (gen_rand(&server->rand) > (1.0f - ai->skill) * 0.35f) {
                        _bot_fire(server, bot, t->target);
                    }
                    ai->burst_left--;
                } else {
                    _bot_set_tool(server, bot, TOOL_GUN);
                }
            }
            break;
        }
    }
}

void _bot_think(server_t* server, player_t* bot, bot_ai_t* ai)
{
    bot_think_t  think;
    bot_think_t* t = &think;
    memset(&think, 0, sizeof(think));
    t->now = get_nanos();
    ai->next_think = t->now + BOT_THINK_DELAY;

    if (!ai->has_objective) {
        _bot_pick_objective(server, bot, ai);
        ai->has_objective = 1;
    }

    /* Keep the target for a while after losing sight of it: ducking behind
       our own wall breaks the line of sight on purpose, and forgetting the
       enemy right then would send the bot back on patrol. */
    _think_target(server, bot, ai, t);

    if (ai->next_strafe <= t->now) {
        ai->strafe_dir  = (gen_rand(&server->rand) < 0.5) ? (uint8_t) 1 : (uint8_t) 0;
        ai->next_strafe = t->now + _bot_secs(0.8f + gen_rand(&server->rand) * 0.8f);
    }

    /* the body is still crouched until the next physics step, whatever the
       flag says from here on */
    t->body_low = bot->crouching;
    _bot_clear_inputs(bot);
    bot->crouching = 0;

    /* no gun (the mode's rules: a zombie): the melee brain runs at its prey
       and scratches (BotMelee.c) */
    if (gamemode_player_rules(server, bot) & GM_RULE_NO_GUN) {
        _bot_melee(server, bot, ai, t->now);
        return;
    }

    /* Water reflex: wet, the bot is easy prey, so it heads for the nearest
       dry ground first, shooting back on the way. A shore it cannot jump
       (a pit, a well with walls: bots sat at the bottom of 2Fort's for the
       whole match) is climbed like a player does it: steps dug into the wall
       or built from blocks (_bot_clear_path). The escape lasts until the bot
       stands about as high as that dry ground, not only while it is wet: on
       the first step out of the water it is dry and still in the pit. */
    /* the mode's objective (logged when it changes) */
    t->gx = 0.0f, t->gy = 0.0f;
    t->exact = 0, t->rush = 0, t->assault = 0;
    t->gfloor = 0;
    t->wx = 0.0f, t->wy = 0.0f;
    t->has_goal    = _bot_mode_goal(server, bot, ai, &t->gx, &t->gy, &t->exact, &t->rush, &t->gfloor, &t->assault, &t->wx, &t->wy);
    {
        /* a shared bridge takes the walk over: the owner and its partner each
           go to their reserved cell (and the partner crosses once it is
           whole), then the mode's goal returns */
        float px, py;
        int   pf;
        if (bot_project_goal(server, bot, &px, &py, &pf)) {
            t->has_goal    = 1;
            t->gx     = px;
            t->gy     = py;
            t->gfloor = pf;
            t->exact  = 0;
        }
    }
    {
        /* a structure the team raises takes the walk over too: its cell's
           stand is the goal and the mode's post, hold and climb wait until it
           is up. Its intent is cleared: a post bot assigned to the ring is not
           camping, it is building */
        float fx, fy;
        int   ff;
        float wx, wy;
        if (bot_fort_goal(server, bot, &fx, &fy, &ff)) {
            t->has_goal     = 1;
            t->gx           = fx;
            t->gy           = fy;
            t->gfloor       = ff;
            t->exact        = 0;
            t->rush         = 0;
            t->assault      = 0;
            ai->goal_intent = 0;
            ai->goal_hold   = 0.0f;
            ai->goal_climb  = 0;
            ai->work        = 0;
        } else if (bot_fort_post_goal(server, bot, &fx, &fy, &ff, &wx, &wy)) {
            /* the watch of a finished lookout: up its stair onto the post,
               then look at the objective it was raised for. The hold is
               under a cell: a bot at the tower's foot is a cell from its
               top's column and must not read as there */
            t->has_goal     = 1;
            t->gx           = fx;
            t->gy           = fy;
            t->gfloor       = ff;
            t->exact        = 1;
            t->rush         = 0;
            t->assault      = 0;
            t->wx           = wx;
            t->wy           = wy;
            ai->goal_intent = 0;
            ai->goal_hold   = 0.7f;
            ai->goal_climb  = 0;
            ai->work        = 0;
        }
    }
    t->guard  = t->has_goal && ai->role == GM_BOT_DEFEND && (ai->goal_intent & GM_INTENT_GUARD);
    /* the guard goes after a dig heard near its post, through the ground if
       it must; then back to the post */
    if (_think_guard_hunt(server, bot, ai, t)) {
        return;
    }
    /* in the goal's column, straight up or down it (the mode's climb): a
       pillar of blocks like a player builds one, or the spade into the
       block under its feet, one cell at a time. Fights wait: it is busy */
    if (_think_climb(server, bot, ai, t)) {
        return;
    }
    if (_think_flee(server, bot, ai, t)) {
        return;
    }
    if (_bot_sentry(server, bot, ai, t->target, t->visible, t->now)) {
        return;
    }
    if (_bot_jetpack(server, bot, ai, t->gx, t->gy, t->gfloor, t->now)) {
        return;
    }
    if (_bot_grenade(server, bot, ai, t->target, t->visible, t->now)) {
        return;
    }
    if (_bot_shotgun(server, bot, ai, t->target, t->visible, t->now)) {
        return;
    }
    if (_bot_close_quarters(server, bot, ai, t->target, t->visible, t->now)) {
        return;
    }
    /* the goal's work (Babel's staircases): on the spot, the block put or
       spaded away; an enemy close by in sight is fought first */
    if (_think_work(server, bot, ai, t)) {
        return;
    }
    /* the camper's objective is its spot (with the intel it runs home like
       anyone) */
    if (_think_camp(server, bot, ai, t)) {
        return;
    }

    if (_think_water(server, bot, ai, t)) {
        return;
    }

    /* Shot at by someone it cannot see: duck into cover nearby (a spot a
       wall hides from the shooter), then stand up aiming where the shot came
       from, so the shooter is in its view when it shows. The carrier keeps
       running home */
    if (ai->alert_until > t->now && t->now >= ai->alert_react_at && t->target == NULL && !t->rush) {
        if (ai->phase != BOT_PHASE_PATROL) {
            _bot_set_phase(bot, ai, BOT_PHASE_PATROL, 0);
        }
        ai->has_cover = 0;
        _bot_set_tool(server, bot, TOOL_GUN);
        ai->next_think = t->now + NANO_IN_MILLI * 100;
        if (t->now < ai->alert_hide_until) {
            if (ai->alert_spot == 0) {
                ai->alert_spot = _bot_find_cover(server, bot, ai, &ai->alert_cx, &ai->alert_cy) ? 1 : 2;
                evt_log(bot, "bot_alert", "action=hide cover=%s spot=%.1f,%.1f from=%.1f,%.1f",
                        ai->alert_spot == 1 ? "found" : "none", ai->alert_cx, ai->alert_cy, ai->alert_x, ai->alert_y);
            }
            float cdx = ai->alert_cx - bot->movement.position.x, cdy = ai->alert_cy - bot->movement.position.y;
            if (ai->alert_spot == 1 && cdx * cdx + cdy * cdy > 0.3f * 0.3f) {
                _bot_face(bot, ai->alert_cx, ai->alert_cy, bot->movement.position.z);
                bot->move_forward = 1;
            } else {
                /* in cover, or none around: low, side-stepping, looking there */
                _bot_face(bot, ai->alert_x, ai->alert_y, ai->alert_z + BOT_BODY_CENTER);
                bot->crouching = 1;
                if (ai->alert_spot == 2) {
                    if (ai->dodge_dir > 0) {
                        bot->move_right = 1;
                    } else {
                        bot->move_left = 1;
                    }
                }
            }
            return;
        }
        /* up again, aimed at where the shot came from */
        _bot_face(bot, ai->alert_x, ai->alert_y, ai->alert_z + BOT_BODY_CENTER);
        return;
    }

    t->hdist = 0.0f;
    if (t->target != NULL) {
        float dx = t->target->movement.position.x - bot->movement.position.x;
        float dy = t->target->movement.position.y - bot->movement.position.y;
        t->hdist    = sqrtf(dx * dx + dy * dy);
    }

    /* ------------------------------------------------------------------ */
    /* no enemy, or one too far to shoot: play the objective on land       */
    /* ------------------------------------------------------------------ */
    /* the carrier does not stop to build or trade shots: it runs home */
    /* the bodyguard left behind (it stopped to fight and the infiltrator
       went on: it was found fighting 35 blocks back) only catches up */
    t->mate_dx = t->gx - bot->movement.position.x, t->mate_dy = t->gy - bot->movement.position.y;
    t->mate_d  = sqrtf(t->mate_dx * t->mate_dx + t->mate_dy * t->mate_dy);
    t->guarding = ai->role == GM_BOT_COVER && (ai->goal_intent & GM_INTENT_COVER);
    /* a builder of a structure keeps at it while no enemy is on top of it
       and it is not being shot at: the fort goes up while its team fights */
    uint8_t fort_build = bot_fort_builder(bot) && (t->target == NULL || t->hdist > BOT_FORT_FIGHT) &&
                         t->now >= ai->alert_until;
    if (t->target == NULL || t->rush || (t->guarding && t->mate_d > BOT_COVER_LOST) || fort_build ||
        (t->hdist > BOT_MAX_SHOT_RANGE && (ai->phase == BOT_PHASE_PATROL || t->hdist > BOT_MAX_SHOT_RANGE * 1.25f)))
    {
        _think_walk(server, bot, ai, t);
        return;
    }

    if (_think_fight(server, bot, ai, t)) {
        return;
    }

    _think_phase(server, bot, ai, t);
}

/* After a think: what it set as the look is where the bot wants to look,
   its keys the step it wants in the world; the body goes back to where it
   really looks and turns from there (_bot_turn) */
void _bot_after_think(player_t* bot, bot_ai_t* ai)
{
    vector3f_t f  = bot->movement.forward_orientation;
    float      fl = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
    if (fl < 0.001f) {
        return;
    }
    ai->look_want = (vector3f_t) {f.x / fl, f.y / fl, f.z / fl};
    float fx = f.x, fy = f.y, hl = sqrtf(fx * fx + fy * fy);
    float sx = bot->movement.strafe_orientation.x, sy = bot->movement.strafe_orientation.y;
    float sl    = sqrtf(sx * sx + sy * sy);
    float ahead = (float) bot->move_forward - (float) bot->move_backwards;
    float side  = (float) bot->move_right - (float) bot->move_left;
    float mx = (hl > 0.01f ? fx / hl : 0.0f) * ahead + (sl > 0.01f ? sx / sl : 0.0f) * side;
    float my = (hl > 0.01f ? fy / hl : 0.0f) * ahead + (sl > 0.01f ? sy / sl : 0.0f) * side;
    float ml = sqrtf(mx * mx + my * my);
    ai->move_wx     = ml > 0.01f ? mx / ml : 0.0f;
    ai->move_wy     = ml > 0.01f ? my / ml : 0.0f;
    /* walking with the look well off the way: what turned it (players read
       it as a bot glancing left and right on its way) */
    if (ml > 0.01f && hl > 0.01f) {
        float c = (ai->move_wx * fx + ai->move_wy * fy) / hl;
        if (c < 0.707f && get_nanos() >= ai->look_logged) {
            ai->look_logged = get_nanos() + _bot_secs(1.0f);
            evt_log(bot, "bot_look", "away=%.0f cause=%s goal=%s phase=%s", acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 57.3f,
                    ai->look_cause ? ai->look_cause : ai->aimed ? "fight" : "think",
                    ai->mode_goal ? ai->mode_goal : "none", _bot_phase_name(ai->phase));
        }
    }
    ai->turn_combat = ai->aimed;
    {
        float yaw = atan2f(ai->look_want.y, ai->look_want.x) * 57.2958f;
        float d   = fabsf(remainderf(yaw - ai->want_yaw, 360.0f));
        if (ml > 0.01f && !ai->aimed && ai->phase == BOT_PHASE_PATROL && !ai->has_target && d > 30.0f) {
            evt_log(bot, "bot_turnjump", "deg=%.0f src=%s path=%u->%u/%u pos=%.2f,%.2f aim=%.2f,%.2f air=%u", d,
                    ai->move_src ? ai->move_src : "other", ai->want_path, ai->path_i, ai->path_len,
                    bot->movement.position.x, bot->movement.position.y, ai->aim_px, ai->aim_py, bot->airborne);
        }
        ai->want_yaw  = yaw;
        ai->want_path = ai->path_i;
    }
    if (!ai->look_ok) {
        ai->look_cur = ai->look_want; /* the first think of a life */
        ai->look_ok  = 1;
    }
    physics_reorient_player(bot, &ai->look_cur);
}
