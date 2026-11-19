// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    What a bot knows of the others: sight (from the eye to the head, chest
    or legs, cached per pair), the enemy it picks, what it hears (digs,
    steps) and being shot at.
*/

#include <Server/BotsInternal.h>
#include <Server/Proto10/Mount.h>
#include <Server/Sky.h>

/* ------------------------------------------------------------------ */
/* AI                                                                  */
/* ------------------------------------------------------------------ */

/* a solid cell in the way of a sight or a shot: the water layer is none (a
   bullet goes through it, the client turns a hit there into a miss) and
   neither is anything past the map's edge */
/* every cell the segment crosses is free: physics_line_clear (Util/Physics.c,
   shared with the 1.0 hit check). physics_can_see stops looking after 32
   cells and calls the rest of the way clear: past ~20 blocks bots saw, aimed
   and fired through walls (2Fort's and de_dust's `bot_shot_wall`) */
uint8_t _bot_line_clear(server_t* server, float x0, float y0, float z0, float x1, float y1, float z1)
{
    return physics_line_clear(server, x0, y0, z0, x1, y1, z1);
}

/* what a player sees: from its eye, any of the target's head, chest or
   legs (z grows downwards; position is about the eye). One ray from hip to
   hip, the old test, was blocked by any low wall or step with the head and
   chest in plain sight: ~80% of the enemies bots walked past (bot_blind
   why=low_ray) */
static uint8_t _bot_sees_body(server_t* server, player_t* bot, player_t* target)
{
    static const float part[3] = {0.1f, 0.9f, 1.7f};
    for (int k = 0; k < 3; k++) {
        if (_bot_line_clear(server, bot->movement.position.x, bot->movement.position.y, bot->movement.position.z,
                            target->movement.position.x, target->movement.position.y,
                            target->movement.position.z + part[k]))
        {
            return 1;
        }
    }
    return 0;
}

/* sight checks counted for the log every 30 s (bots_update): how many were
   asked, how many cast a ray */
uint64_t sight_asked, sight_rays;

/* a pair's answer is kept for BOT_SIGHT_KEEP: one think asks about the same
   enemy several times (the kept target, the pick, the snap layer) and a
   body moves less than a block in that time */
#define BOT_SIGHT_KEEP (NANO_IN_MILLI * 150)
typedef struct {
    uint64_t until;
    uint8_t  seen;
} bot_sight_t;
static bot_sight_t sight_cache[BOT_MAX_PLAYERS][BOT_MAX_PLAYERS];

uint8_t _bot_can_see(server_t* server, player_t* bot, player_t* target)
{
    sight_asked++;
    float dx = target->movement.position.x - bot->movement.position.x;
    float dy = target->movement.position.y - bot->movement.position.y;
    float dz = target->movement.position.z - bot->movement.position.z;
    if ((dx * dx + dy * dy + dz * dz) > (BOT_VIEW_DISTANCE * BOT_VIEW_DISTANCE)) {
        return 0;
    }
    /* by night and in rain, snow or fog a body is seen closer (Sky.c), as a
       player sees it: the bots never saw through the dark players cannot */
    float sight = sky_sight_range(server, bot, target, BOT_VIEW_DISTANCE);
    if (sight < BOT_VIEW_DISTANCE && (dx * dx + dy * dy + dz * dz) > sight * sight) {
        return 0;
    }
    uint64_t     now = get_nanos();
    bot_sight_t* c   = &sight_cache[bot->id][target->id];
    if (now < c->until) {
        return c->seen;
    }
    sight_rays++;
    c->until = now + BOT_SIGHT_KEEP;
    c->seen  = _bot_sees_body(server, bot, target);
    return c->seen;
}

player_t* _bot_pick_target_ex(server_t* server, player_t* bot, bot_ai_t* ai, uint8_t any_range)
{
    /* the infiltrator does not go looking for fights */
    /* a squad on its lane does not stop for enemies far away: with 64 a
       side everyone died fighting across the middle and nobody reached the
       intel */
    uint8_t   on_lane   = (ai->goal_intent & (GM_INTENT_SQUAD | GM_INTENT_WITH)) != 0;
    float     max_range = any_range                     ? BOT_VIEW_DISTANCE
                          : _bot_sneaking(ai)           ? BOT_SNEAK_RANGE
                          : ai->role == GM_BOT_COVER    ? BOT_COVER_RANGE
                          : on_lane                     ? BOT_LANE_RANGE
                                                        : BOT_VIEW_DISTANCE;
    uint64_t  now       = get_nanos();
    player_t* best      = NULL;
    float     best_dist = BOT_VIEW_DISTANCE * BOT_VIEW_DISTANCE;
    struct {
        player_t* p;
        float     d;
    } cand[BOT_MAX_PLAYERS];
    int n = 0;

    player_t *other, *tmp;
    HASH_ITER(hh, server->players, other, tmp)
    {
        /* any enemy counts, bots included: a blue bot that ignored red bots
           made multi-bot matches a stroll past each other */
        if (other->id == bot->id || other->state != STATE_READY) {
            continue;
        }
        if (!other->alive || other->team == bot->team || other->team == TEAM_SPECTATOR) {
            continue;
        }
        /* a helicopter's occupant cannot be hit by a bullet (Proto10/Mount.c):
           a bot that picked it would shoot at the hull all match */
        if (mount_in_vehicle(other)) {
            continue;
        }
        /* one it gave up on for a while: seen, but no shot of it gets past
           the wall (_bot_fire) */
        if (ai->wall_ignore_until > now && ai->wall_ignore_id == other->id) {
            continue;
        }
        float dx = other->movement.position.x - bot->movement.position.x;
        float dy = other->movement.position.y - bot->movement.position.y;
        float dz = other->movement.position.z - bot->movement.position.z;
        float dist = dx * dx + dy * dy + dz * dz;
        /* only what is in front of it, or close enough to be heard: a bot
           that saw all around never needed to react to being shot at */
        float hd = sqrtf(dx * dx + dy * dy);
        if (hd > max_range) {
            continue;
        }
        /* a digger it heard is looked for wherever it is */
        uint8_t heard = (ai->heard_until > now && ai->heard_from == other->id) ||
                        (ai->steps_until > now && ai->steps_from == other->id);
        /* the intel's watchers know who took it (everyone is told): they
           look for its carrier wherever it is, before anyone else */
        uint8_t carrier = (ai->overwatch || ai->role == GM_BOT_DEFEND) && _bot_carries(server, other);
        if (hd > BOT_NEAR_SENSE && !heard && !carrier) {
            float fx = bot->movement.forward_orientation.x, fy = bot->movement.forward_orientation.y;
            float fl = sqrtf(fx * fx + fy * fy);
            if (fl > 0.01f && (dx * fx + dy * fy) / (hd * fl) < BOT_FOV_COS) {
                continue;
            }
        }
        if (dist < best_dist && n < BOT_MAX_PLAYERS) {
            cand[n].p = other;
            cand[n].d = dist;
            /* the intel's watchers take on whoever is at our intel first
               (the overwatch hid while enemies took it) */
            if ((ai->overwatch || ai->role == GM_BOT_DEFEND) && ai->home_set) {
                float ix = other->movement.position.x - ai->home_x, iy = other->movement.position.y - ai->home_y;
                if (ix * ix + iy * iy < 20.0f * 20.0f) {
                    cand[n].d *= 0.05f;
                }
            }
            if (carrier) {
                cand[n].d *= 0.001f;
            }
            /* whoever is at what else the team defends (Babel: the foot of
               our staircase, where the enemy spades it down) */
            if (ai->guard_set) {
                float gx = other->movement.position.x - ai->guard_x, gy = other->movement.position.y - ai->guard_y;
                if (gx * gx + gy * gy < 15.0f * 15.0f) {
                    cand[n].d *= 0.05f;
                }
            }
            /* a camper takes on the enemy's campers first: the duel between
               rifles at range is its job, the rest is for the attackers.
               After the enemy carrying our objective and the enemies at home */
            if (ai->camper && other->is_bot && _bot_ai(other->id)->camper) {
                cand[n].d *= 0.1f;
            }
            n++;
        }
    }
    /* nearest first, the first in sight wins: one ray per candidate in the
       old order, which was the player table's */
    for (int i = 0; i < n; i++) {
        int m = i;
        for (int j = i + 1; j < n; j++) {
            if (cand[j].d < cand[m].d) {
                m = j;
            }
        }
        if (m != i) {
            __typeof__(cand[0]) t = cand[i];
            cand[i]              = cand[m];
            cand[m]              = t;
        }
        if (_bot_can_see(server, bot, cand[i].p)) {
            best = cand[i].p;
            break;
        }
    }
    (void) best_dist;
    return best;
}

/* the enemy the bot takes on: within the range its role fights at */
player_t* _bot_pick_target(server_t* server, player_t* bot, bot_ai_t* ai)
{
    return _bot_pick_target_ex(server, bot, ai, 0);
}

void bots_on_dig(server_t* server, player_t* digger, int x, int y, int z)
{
    if (digger->team != TEAM_A && digger->team != TEAM_B) {
        return;
    }
    uint64_t  now = get_nanos();
    player_t *b, *tmp;
    HASH_ITER(hh, server->players, b, tmp)
    {
        if (!b->is_bot || !b->alive || b->state != STATE_READY || b->team == digger->team) {
            continue;
        }
        float dx = x + 0.5f - b->movement.position.x, dy = y + 0.5f - b->movement.position.y;
        float dz = z + 0.5f - b->movement.position.z;
        float d  = sqrtf(dx * dx + dy * dy + dz * dz);
        bot_ai_t* ai = _bot_ai(b->id);
        if (d > BOT_HEAR_RANGE || ai == NULL || !ai->active) {
            continue;
        }
        ai->heard_from  = digger->id;
        ai->heard_x     = x;
        ai->heard_y     = y;
        ai->heard_z     = z;
        ai->heard_until = now + _bot_secs(BOT_HEAR_TIME);
        if (now - ai->heard_logged > _bot_secs(2.0f)) {
            ai->heard_logged = now;
            evt_log(b, "bot_hear", "action=dig from=%u cell=%d,%d,%d dist=%.1f role=%u", digger->id, x, y, z, d,
                    ai->role);
        }
    }
}

/* A camper's ears: the nearest enemy walking within BOT_STEPS_WALK or
   running within BOT_STEPS_RUN, on its feet (crouched or sneaking is
   silent, in the air there are no steps). Remembered BOT_STEPS_TIME. 1 while
   it has steps to turn to */
uint8_t _bot_hear_steps(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    float     best = 1e9f;
    player_t *p, *tmp, *heard = NULL;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || p->team == TEAM_SPECTATOR || p->state != STATE_READY || !p->alive ||
            p->crouching || p->sneaking || p->airborne)
        {
            continue;
        }
        float vx = p->movement.velocity.x, vy = p->movement.velocity.y;
        float speed = sqrtf(vx * vx + vy * vy);
        if (speed < 0.05f) {
            continue; /* standing still */
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        float dz = p->movement.position.z - bot->movement.position.z;
        float d  = sqrtf(dx * dx + dy * dy + dz * dz);
        float range = p->sprinting ? BOT_STEPS_RUN : BOT_STEPS_WALK;
        if (d < range && d < best) {
            best  = d;
            heard = p;
        }
    }
    if (heard != NULL) {
        if (ai->steps_until <= now || ai->steps_from != heard->id) {
            evt_log(bot, "bot_hear", "action=steps from=%u dist=%.1f run=%u", heard->id, best, heard->sprinting);
        }
        ai->steps_from  = heard->id;
        ai->steps_x     = heard->movement.position.x;
        ai->steps_y     = heard->movement.position.y;
        ai->steps_z     = heard->movement.position.z;
        ai->steps_until = now + _bot_secs(BOT_STEPS_TIME);
    }
    return ai->steps_until > now;
}

void bots_on_damage(server_t* server, player_t* attacker, player_t* victim)
{
    if (attacker == NULL || attacker == victim || attacker->team == victim->team) {
        return; /* a fall, or friendly fire */
    }
    uint64_t  now = get_nanos();
    player_t *b, *tmp;
    HASH_ITER(hh, server->players, b, tmp)
    {
        if (!b->is_bot || !b->alive || b->state != STATE_READY || b->team != victim->team) {
            continue;
        }
        float dx = b->movement.position.x - victim->movement.position.x;
        float dy = b->movement.position.y - victim->movement.position.y;
        float dz = b->movement.position.z - victim->movement.position.z;
        if (b != victim && (fabsf(dx) > BOT_ALERT_RANGE || fabsf(dy) > BOT_ALERT_RANGE || fabsf(dz) > BOT_ALERT_RANGE)) {
            continue;
        }
        bot_ai_t* ai = _bot_ai(b->id);
        if (ai == NULL || !ai->active) {
            continue;
        }
        uint8_t fresh = ai->alert_until <= now || ai->alert_from != attacker->id;
        ai->alert_from = attacker->id;
        ai->alert_x    = attacker->movement.position.x;
        ai->alert_y    = attacker->movement.position.y;
        ai->alert_z    = attacker->movement.position.z;
        ai->alert_until = now + _bot_secs(BOT_ALERT_TIME);
        if (fresh) {
            /* a good bot reacts in ~0.15 s, a weak one in ~0.45 s; it hides
               for a moment, shorter the better it is */
            ai->alert_react_at   = now + _bot_secs(0.45f - 0.3f * ai->skill);
            ai->alert_hide_until = ai->alert_react_at + _bot_secs(0.5f + (1.0f - ai->skill) * 0.8f);
            ai->alert_spot       = 0;
            ai->dodge_dir        = (int8_t) (gen_rand(&server->rand) < 0.5f ? 1 : -1);
            if (ai->next_think > ai->alert_react_at) {
                ai->next_think = ai->alert_react_at;
            }
            evt_log(b, "bot_alert", "action=hit why=%s from=%u at=%.1f,%.1f,%.1f", b == victim ? "hit" : "mate_hit",
                    attacker->id, ai->alert_x, ai->alert_y, ai->alert_z);
        }
    }
}

/* An enemy in front of the bot (its cone, 128 blocks) that a player would
   see (_bot_sees_body), while the bot neither aims nor has a target: logged
   with why it went unseen (bot_blind), every 0.5 s per bot. Players saw
   bots walk past enemies on their screen */
void _bot_blind_check(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    if (now < ai->blind_at || !bot->alive || ai->aimed || ai->has_target) {
        return;
    }
    ai->blind_at = now + NANO_IN_MILLI * 500;
    float fx = bot->movement.forward_orientation.x, fy = bot->movement.forward_orientation.y;
    float fl = sqrtf(fx * fx + fy * fy);
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || p->team == TEAM_SPECTATOR || !p->alive || p->state != STATE_READY) {
            continue;
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        float hd = sqrtf(dx * dx + dy * dy);
        if (hd > BOT_VIEW_DISTANCE || hd < 0.5f || (fl > 0.01f && (dx * fx + dy * fy) / (hd * fl) < BOT_FOV_COS) ||
            !_bot_sees_body(server, bot, p))
        {
            continue;
        }
        uint8_t     low = _bot_can_see(server, bot, p);
        const char* why = !low                                                    ? "sight_cache"
                          : hd > _bot_weapon_range(bot) * BOT_FIRE_STRETCH         ? "gun_range"
                          : _bot_pick_target(server, bot, ai) == NULL              ? "role_range"
                          : bot->item != TOOL_GUN                                  ? "tool"
                                                                                   : "other";
        evt_log(bot, "bot_blind", "enemy=%u dist=%.1f why=%s goal=%s weapon=%u", p->id, hd, why,
                ai->mode_goal ? ai->mode_goal : "none", bot->weapon);
        return;
    }
}
