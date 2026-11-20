// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_BOTFORTS_H
#define SERVER_BOTFORTS_H

/*
 * Cooperative fortifications: a castle ring, a roofed bunker and a stepped
 * watchtower, each a reserved set of cells one to four bots of a team raise
 * together where the mode asks for it (king of the hill's hill, territory
 * control's territories). It is the shape half of BattleSpades' schematics
 * and cooperative projects (AGPL-3.0); this is our own, much smaller
 * implementation of the idea, not a copy of that code, and every placement
 * still goes through the ordinary block action, so the server validates
 * rate, reach, support and protected cells exactly as for a player.
 *
 * Bounded on purpose: at most BOT_FORTS live structures, BOT_FORT_CELLS
 * cells each, BOT_FORT_BUILDERS builders, 90 s to build one (renewed by
 * every cell placed) and 26 blocks for a bot to join. A structure whose
 * cells are already solid is created complete: the mode then gets its top
 * (a tower) and the cells are repaired if they are shot down.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

#define BOT_FORTS         16
#define BOT_FORT_CELLS    64
#define BOT_FORT_BUILDERS 4
#define BOT_FORT_JOIN     26.0f /* a builder is this close to the site when it is asked for */
#define BOT_FORT_FIGHT    12.0f /* an enemy this close stops the building: it fights */

typedef enum
{
    BOT_FORT_RING = 0, /* a battlemented ring with two gates */
    BOT_FORT_BUNKER,   /* three by three, a firing wall, a roof and a back door */
    BOT_FORT_TOWER,    /* two by two, three high, a stair and a parapet */
} bot_fort_kind_t;

/* [bots] cooperative_forts (default on); SP_BOT_FORTS=0 for tests */
void    bot_forts_init(server_t* server);
void    bot_forts_reset(void);
void    bot_forts_tick(server_t* server);

/* Ask for a structure at the ground cell (x, y, z), facing (fx, fy) along one
   axis. 1 when one was started; 0 when one of the kind stands there already,
   the site cannot take it or no bots are free to build it. The site is
   shared: two teams never raise the same kind twice. */
uint8_t bot_fort_request(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy);

/* what the mode asks of a site (any team's: the structure is cover) */
uint8_t bot_fort_exists(uint8_t kind, int x, int y, int radius);
uint8_t bot_fort_complete(uint8_t kind, int x, int y, int radius);
/* a complete structure's standable top in its anchor column, for a bot the
   mode wants on it (a tower): the floor cell (z) and its column */
uint8_t bot_fort_top(uint8_t kind, int x, int y, int radius, float* out_x, float* out_y, int* out_floor);

/* the site changed hands or the round moved: the team's structures there are
   given up (the cells stay; the modes may ask for one again) */
void bot_fort_discard(uint8_t team, int x, int y, int radius);

/* --- the bots' side: the think, the walk and the placement --- */
/* the stand this builder walks to (1: this bot is building, the goal is the
   stand cell). It also supplies the walk destination when an enemy is not
   on top of the bot. */
uint8_t bot_fort_goal(server_t* server, player_t* bot, float* x, float* y, int* floor);
/* in the assigned stand: put the reserved cell (1: it acted, so the think
   ends there) */
uint8_t bot_fort_step(server_t* server, player_t* bot);
/* this bot is on an unfinished structure and has blocks */
uint8_t bot_fort_builder(const player_t* bot);

#endif /* SERVER_BOTFORTS_H */
