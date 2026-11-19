// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/MapStore.h>
#include <Server/BotsInternal.h>
#include <modes/modes.h>
#include <Util/Nanos.h>

/* The melee brain: what a bot with no gun does, picked by the
   player's rules (GM_RULE_NO_GUN: _bot_think hands the think over;
   GM_RULE_HIGH_JUMP: it leaps). It was the zombie mode's brain inside
   BotCombat.c; nothing here asks which mode it plays. Its log lines keep
   the name they always had (bot_zombie), which tests and bench.py count */

/* a leap (GM_RULE_HIGH_JUMP) as high as it needs (blocks): the charge a player would
   hold for it (common/modes/modes.h; the height goes with the speed's
   square, 3.5 blocks at -0.62). A full one at every leap threw bots far past
   the ledge they were climbing */
void _bot_leap(player_t* bot, float up)
{
    float v = 0.62f * sqrtf((up + 0.5f) / 3.5f);
    float c = (v + GAME_ZOMBIE_LEAP_UP_MIN) / (GAME_ZOMBIE_LEAP_UP_MIN - GAME_ZOMBIE_JUMP_VELOCITY);
    c       = c < 0.05f ? 0.05f : c > 1.0f ? 1.0f : c;
    bot->jumping     = 1;
    bot->leap_charge = (uint8_t) game_zombie_leap_param(c);
}

/* The melee brain: a player the mode keeps without a gun
   (GM_RULE_NO_GUN; the zombie mode's zombies today, any mode's tomorrow:
   a knife round, a monster) has its hands only. It knows where the
   nearest human is at any range, as the mode's zombies hunt by smell, runs
   at it the planned way (digging through a fort when the way is shut), jumps
   high (GM_RULE_HIGH_JUMP: ~5 blocks) at a human above it or a wall in
   front, and scratches within reach (_bot_close_quarters, the spade's
   swing). With no human left it walks to its objective */
/* humans within MELEE_GROUP_R of p (p left out): its guns' cover */
#define MELEE_GROUP_R   8.0f
#define MELEE_HORDE_R   15.0f /* its team's melee bots this near the waiting one count as its horde */
#define MELEE_HORDE     3     /* a horde this big goes */
#define MELEE_HORDE_MIN 15.0f /* it waits between these from its target (horizontally) */
#define MELEE_HORDE_MAX 40.0f
#define MELEE_HORDE_WAIT 8.0f /* seconds at most, then alone */
#define MELEE_HORDE_RUSH 20.0f /* seconds it rushes after waiting, before it may wait again */

static int _melee_count_near(server_t* server, player_t* of, uint8_t team, float r, player_t* skip)
{
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == of || p == skip || p->team != team || p->state != STATE_READY || !p->alive) {
            continue;
        }
        float dx = p->movement.position.x - of->movement.position.x;
        float dy = p->movement.position.y - of->movement.position.y;
        float dz = p->movement.position.z - of->movement.position.z;
        n += dx * dx + dy * dy + dz * dz <= r * r;
    }
    return n;
}

void _bot_melee(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now)
{
    player_t* target = NULL;
    float     best   = 1e18f;
    player_t *p, *tmp;
    /* the prey: the nearest, weighed by the guns round it. A human alone (a
       straggler, one still climbing) is worth a longer run than a group on
       its roof: going at the nearest, the zombies ran one by one into the
       group's fire */
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || (p->team != TEAM_A && p->team != TEAM_B) || p->state != STATE_READY || !p->alive) {
            continue;
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        float dz = p->movement.position.z - bot->movement.position.z;
        int   cover = _melee_count_near(server, p, p->team, MELEE_GROUP_R, NULL);
        float w     = 1.0f + 0.5f * (float) (cover < 4 ? cover : 4);
        float d     = (dx * dx + dy * dy + dz * dz) * w * w;
        if (d < best) {
            best   = d;
            target = p;
        }
    }
    _bot_set_tool(server, bot, TOOL_SPADE);
    ai->next_think = now + NANO_IN_MILLI * 100;
    if (target == NULL) {
        if (!_bot_route_to(server, bot, ai, ai->obj_x, ai->obj_y, 0, 0, 0)) {
            _bot_walk_to(server, bot, ai, ai->obj_x, ai->obj_y);
        }
        return;
    }
    float tdx = target->movement.position.x - bot->movement.position.x;
    float tdy = target->movement.position.y - bot->movement.position.y;
    float tdz = target->movement.position.z - bot->movement.position.z;
    if (ai->target_id != target->id) {
        ai->has_target = 1;
        ai->target_id  = target->id;
        evt_log(bot, "bot_zombie", "action=hunt target=%u dist=%.1f cover=%d", target->id,
                sqrtf(tdx * tdx + tdy * tdy + tdz * tdz), _melee_count_near(server, target, target->team, MELEE_GROUP_R, NULL));
    }
    uint8_t visible = _bot_can_see(server, bot, target);
    /* a horde, not a trickle: going at a group (a human with mates round
       it), out of its sight and still some way off, a zombie waits for
       others to come up, then they all go; never more than
       MELEE_HORDE_WAIT, and then it rushes MELEE_HORDE_RUSH before it may
       wait again. Seen, it keeps coming (standing in the guns is worse) */
    float tdh = sqrtf(tdx * tdx + tdy * tdy);
    if (!visible && now >= ai->horde_rush_until && tdh > MELEE_HORDE_MIN && tdh < MELEE_HORDE_MAX &&
        _melee_count_near(server, target, target->team, MELEE_GROUP_R, NULL) >= 1)
    {
        int horde = 1 + _melee_count_near(server, bot, bot->team, MELEE_HORDE_R, NULL);
        if (ai->horde_wait_at == 0) {
            ai->horde_wait_at = now;
            evt_log(bot, "bot_zombie", "action=horde_wait target=%u dist=%.0f horde=%d", target->id, tdh, horde);
        }
        if (horde < MELEE_HORDE && now - ai->horde_wait_at < _bot_secs(MELEE_HORDE_WAIT)) {
            _bot_clear_inputs(bot);
            _bot_face(bot, target->movement.position.x, target->movement.position.y, target->movement.position.z);
            bot->crouching = 1;
            return;
        }
        ai->horde_rush_until = now + _bot_secs(MELEE_HORDE_RUSH);
        evt_log(bot, "bot_zombie", "action=horde_go target=%u horde=%d waited_ms=%llu", target->id, horde,
                (unsigned long long) ((now - ai->horde_wait_at) / NANO_IN_MILLI));
        ai->horde_wait_at = 0;
    } else if (ai->horde_wait_at != 0 && now >= ai->horde_rush_until) {
        ai->horde_wait_at = 0; /* seen, or the prey moved off: on */
    }
    if (visible) {
        ai->last_seen = now;
    }
    if (_bot_close_quarters(server, bot, ai, target, visible, now)) {
        return;
    }
    float tx = target->movement.position.x, ty = target->movement.position.y, tz = target->movement.position.z;
    float hd = sqrtf((tx - bot->movement.position.x) * (tx - bot->movement.position.x) +
                     (ty - bot->movement.position.y) * (ty - bot->movement.position.y));
    /* close and in sight: straight at it, the jump when it stands higher.
       Higher than a leap reaches, the planned way up instead (ledges and
       notches cut for the leap, BotPlanner.c): jumping at the foot of a
       house the zombies never got onto its roof */
    if (visible && hd < 6.0f && bot->movement.position.z - tz < 4.0f) {
        _bot_face(bot, tx, ty, tz);
        bot->move_forward = 1;
        bot->sprinting    = 1;
        float up = bot->movement.position.z - tz;
        if (!bot->airborne && up > 1.5f && now >= ai->next_hop) {
            if (up <= (float) GAME_ZOMBIE_TAP_JUMP_UP + 0.5f) {
                bot->jumping = 1; /* the tap reaches it: no wait for the leap */
                ai->next_hop = now + _bot_secs(0.5f);
            } else if (now >= _bot_leap_back(bot)) {
                _bot_leap(bot, up + 1.0f);
                ai->next_hop = now + _bot_secs(0.8f);
                evt_log(bot, "bot_zombie", "action=leap target=%u up=%.1f", target->id, up);
            }
        }
        return;
    }
    if (!_bot_route_to(server, bot, ai, tx, ty, (int) (tz + 2.4f), 0, 1)) {
        _bot_walk_to(server, bot, ai, tx, ty);
        bot->sprinting = 1;
    }
    /* a wall in front on the way: over it rather than round it */
    float fx = bot->movement.forward_orientation.x, fy = bot->movement.forward_orientation.y;
    int   cx = (int) (bot->movement.position.x + fx * 1.2f), cy = (int) (bot->movement.position.y + fy * 1.2f);
    int   chest = (int) floorf(bot->movement.position.z);
    /* over the top of the world (z < 0) is sky: the survivors hold rooftops
       up there, and a check at z = -1 read out of the map */
    int wall = chest >= 0 && cx >= 0 && cy >= 0 && cx < server->s_map.map.size_x && cy < server->s_map.map.size_y &&
               map_solid(&server->s_map.map, cx, cy, chest);
    /* a wall no higher than the tap reaches (free two cells over the chest's):
       jumped at once; a higher one with the leap */
    int low = wall && chest >= 2 && !map_solid(&server->s_map.map, cx, cy, chest - 1) &&
              !map_solid(&server->s_map.map, cx, cy, chest - 2);
    if (!bot->airborne && now >= ai->next_hop && bot->move_forward && low) {
        bot->jumping = 1;
        ai->next_hop = now + _bot_secs(0.5f);
    } else if (!bot->airborne && now >= ai->next_hop && now >= _bot_leap_back(bot) && bot->move_forward && wall &&
               (chest < 4 || !map_solid(&server->s_map.map, cx, cy, chest - 4)))
    {
        _bot_leap(bot, 4.0f);
        ai->next_hop = now + _bot_secs(0.8f);
    }
    /* a wall between it and a human close by (the human bots' fort): spaded
       at chest height. Jumping at it only, the zombies never got in and the
       humans won 20 rounds of 21 on de_dust */
    if (!bot->airborne && hd < 12.0f && wall &&
        gamemode_cell_editable(server, cx, cy, chest) && _bot_block_ready(server, bot, BLOCKACTION_DESTROY_ONE))
    {
        _bot_face(bot, cx + 0.5f, cy + 0.5f, chest + 0.5f);
        uint8_t ok = _bot_block_action(server, bot, BLOCKACTION_DESTROY_ONE, cx, cy, chest);
        evt_log(bot, "bot_zombie", "action=break cell=%d,%d,%d ok=%u", cx, cy, chest, ok);
    }
}

