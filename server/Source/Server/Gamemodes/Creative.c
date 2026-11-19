// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Gamemodes/Creative.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <proto10/codec.h>
#include <string.h>

/* what a builder carries; topped up every tick, so it never runs out */
#define CREATIVE_BLOCKS 50

static unsigned _creative_player_rules(server_t* server, player_t* player)
{
    (void) server;
    (void) player;
    return GM_RULE_NO_GUN | GM_RULE_NO_GRENADE | GM_RULE_NO_FALL | GM_RULE_FLY;
}

/* nothing hurts: guns and grenades are refused anyway, and the fall, the
   water and the map's boundary come through here too */
static long _creative_hit_damage(server_t* server, player_t* attacker, player_t* victim, long damage,
                                 uint8_t kill_reason)
{
    (void) server;
    (void) attacker;
    (void) victim;
    (void) damage;
    (void) kill_reason;
    return 0;
}

/* everyone flies (free flight, like the spectator's camera) and the
   blocks never run out (the client keeps its own count full too) */
static void _creative_tick(server_t* server)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive) {
            continue;
        }
        if (p->blocks < CREATIVE_BLOCKS) {
            p->blocks = CREATIVE_BLOCKS;
        }
        p->flying = 1;
        if (p->hp < 100) {
            p->hp = 100;
        }
    }
}

static void _creative_init(server_t* server)
{
    (void) server;
    evt_log(NULL, "creative", "action=start");
}

/* bots have no fight here: they stay round their spawn */
static int _creative_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    (void) server;
    memset(out, 0, sizeof *out);
    gm_goal_name(out, "creative-wander", 0);
    out->x         = bot->movement.position.x;
    out->y         = bot->movement.position.y;
    out->hold      = 6.0f;
    out->walk_only = 1;
    return 1;
}

const gamemode_ops_t creative_ops = {
.id           = GAME_MODE_CREATIVE,
.name         = "creative",
.wire_name    = "creative",
.features     = GM_FEAT_ONE_TEAM | GM_FEAT_NO_BOTS,
.init         = _creative_init,
.tick         = _creative_tick,
.player_rules = _creative_player_rules,
.hit_damage   = _creative_hit_damage,
.bot_goal     = _creative_bot_goal,
};
