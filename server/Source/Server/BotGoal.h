// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOT_GOAL_H
#define BOT_GOAL_H

/*
 * What a bot is asked to do: the one contract between whoever decides (a
 * game mode's bot_goal hook; a plugin's on_bot_think later) and
 * the bots' core (Bot*.c), which plays it: plans the way, walks, fights,
 * digs and builds. The core never asks which mode it plays, nor a goal's
 * name: a new mode drives bots by filling this struct alone.
 * docs/bots.md "Asking bots for something" has every field with what the
 * core does with it, and an example from each mode.
 *
 * The rest of the contract is the mode's ops (Gamemodes/Gamemode.h):
 * player_rules (GM_RULE_*: no gun is the melee brain, BotMelee.c; the high
 * jump; no digging), player_carries (the carrier), patrol_anchor (where a
 * bot with no goal patrols).
 *
 * The goal is asked for every think (5 a second); a zeroed one is "nothing
 * in particular": walk to the patrol anchor, fight what comes.
 */

#include <stdint.h>

/* what a bot does for its team (the mode hands roles out) */
enum
{
    GM_BOT_ATTACK = 0, /* the default: the objective, fights on the way */
    GM_BOT_DEFEND,     /* guards home from cover */
    GM_BOT_INFILTRATE, /* goes for the objective unseen: tunnels, cover */
    GM_BOT_COVER,      /* follows the infiltrator and fights for it, builds nothing */
    GM_BOT_RUSH,       /* the objective at any cost: fights without stopping */
};

/* what a goal is for (gm_bot_goal_t.intent), the only thing the bots' core
   reads of it: the name is for the logs. A mode, or a plugin later, says
   what its goal means and the core plays it, never asking which mode it is
   (it once compared the CTF's goal names, and KotH had to call its holders
   "post" to be treated as campers) */
#define GM_INTENT_TAKE  0x0001 /* to take the objective: close to it, no stop for a fight; a sentry or dynamite by it */
#define GM_INTENT_PUSH  0x0002 /* an attacker's way to it: no stop for an enemy past BOT_PUSH_FIGHT */
#define GM_INTENT_SQUAD 0x0004 /* on its squad's lane: sneaks only off it, picks enemies within BOT_LANE_RANGE */
#define GM_INTENT_WITH  0x0008 /* keeping up with someone (a leader, a carrier): shoots on the move */
#define GM_INTENT_POST  0x0010 /* holds a post: fights from it like a camper, never steps aside */
#define GM_INTENT_HOME  0x0020 /* the defender's work at home: its plans dig nothing there */
#define GM_INTENT_GUARD 0x0040 /* the defender's post by home */
#define GM_INTENT_CHASE 0x0080 /* after the enemy carrying the team's objective */
#define GM_INTENT_BRIEF 0x0100 /* listening to the team's plan: holds still */
#define GM_INTENT_PLANT 0x0200 /* taking an explosive to where it goes */
#define GM_INTENT_COVER 0x0400 /* covering a mate (the infiltrator) */

typedef struct
{
    /* -- what for: the name for the logs (EVT bot_goal goal=), and what
          the core reads (gm_goal_name sets both) -- */
    const char* name;
    uint16_t    intent; /* GM_INTENT_* */
    uint8_t     role;   /* GM_BOT_*: also picks the bot's weapon */
    uint8_t     seated; /* the mode chose the campers itself (camp below); 0:
                           the bots pick them among their rifles */
    uint8_t     camp;   /* seated: 1 a camper, 2 the overwatch (watches home) */
    uint8_t     camp_ground; /* the overwatch's spot on solid ground only (a
                                hill, a slope), never a crown or a roof:
                                king of the hill's towns, whose highest
                                columns were trees no one can climb */

    /* -- where -- */
    float   x, y;  /* the destination */
    float   z;     /* its floor cell (z grows downwards), 0 unknown: a bot
                      below it digs or builds its way up */
    uint8_t exact; /* the very cell (pickup and capture are checked on it) */
    float   hold;  /* > 0: within this of the destination it stops and watches */
    float   watch_x; /* where to look while holding (the threat's side, the leader's view) */
    float   watch_y;

    /* -- how to get there -- */
    uint8_t rush;    /* run there, ignore fights (a carrier) */
    uint8_t assault; /* fight in the open: no wall, no dodging, just push and
                        shoot (the carrier's team clearing its way) */
    uint8_t walk_only; /* on foot: no digging, no sneaking (the zombie mode's
                          high ground: sneaky bots tunnelled into the hill
                          under it and got stuck) */
    uint8_t keep_terrain; /* not even the spade out of a jam (the plan digs
                             nothing either): Babel's staircase builders
                             spaded the steps they had put */

    /* -- what to do there -- */
    uint8_t  climb; /* standing in the goal's column: 1 builds a pillar up to
                       floor z, 2 digs the column down to floor z (Babel's
                       way to the platform at the top of the world) */
    uint8_t  work;  /* within reach at the destination: 1 puts a block at
                       work_*, 2 spades it away (Babel's staircases) */
    int      work_x, work_y, work_z;
    uint32_t work_color; /* work 1: the block's colour, 0 the one in hand */

    /* -- what the team defends -- */
    uint8_t has_home; /* home_*: what the team protects (the CTF's own intel):
                         the guard's and the overwatch's post, their first
                         targets near it */
    float   home_x, home_y, home_z;
    uint8_t has_guard; /* guard_*: something else the team defends (Babel:
                          its staircase's foot): enemies there come first */
    float   guard_x, guard_y;
    /* -- a post's wall (GM_INTENT_POST) -- */
    uint8_t wall_style; /* 0 the castle wall (a camper's), 1 a bunker: two
                           high with three high merlons, a loophole a block
                           high in its middle (king of the hill's posts) */
    int8_t  wall_side;  /* a pair's holder: -1 or 1 builds only that side's
                           wing, the mate the other (0: both) */

    /* -- up there -- */
    float   perch; /* > 0, a radius round the goal: a roof or a tree top within
                      it counts as being at the goal, so a jetpack wearer may
                      fly onto one, prefers it to the ground, stays up there and
                      fights from it (king of the hill's cylinder: anywhere over
                      the ring counts). 0: a roof is only ever a place on the
                      way (a wearer still takes those) */
} gm_bot_goal_t;

/* a goal's name and what it is for, set together: a mode that renames its
   goal on the way (Babel's ladder over the CTF's take-intel) cannot leave
   the old meaning behind */
static inline void gm_goal_name(gm_bot_goal_t* g, const char* name, uint16_t intent)
{
    g->name   = name;
    g->intent = intent;
}

#endif
