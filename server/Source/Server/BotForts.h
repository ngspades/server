// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_BOTFORTS_H
#define SERVER_BOTFORTS_H

/*
 * Cooperative fortifications: a castle ring, a roofed bunker, a stepped
 * tower, a tall watchtower and a two floor barricade, each a reserved set of
 * cells one to four bots of a team raise together where the mode asks for
 * it (the CTF's bases and lanes, king of the hill's hill, territory
 * control's territories); the watchtower and the barricade on ground that
 * sees the objective, watched from their tops once finished. It is the shape half of BattleSpades' schematics
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
#define BOT_FORT_CELLS    96 /* the tall watchtower and the barricade on a slope need up to ~80 */
#define BOT_FORT_BUILDERS 4
#define BOT_FORT_JOIN     26.0f /* a builder is this close to the site when it is asked for */
#define BOT_FORT_HELP     40.0f /* a bot helps an unfinished site of its team this close */
#define BOT_FORT_FIGHT    12.0f /* an enemy this close stops the building: it fights */
#define BOT_FORT_POSTS    2     /* bots holding a finished watchtower's top or barricade's deck */

typedef enum
{
    BOT_FORT_RING = 0, /* a battlemented ring with two gates */
    BOT_FORT_BUNKER,   /* three by three, a firing wall, a roof and a back door */
    BOT_FORT_TOWER,    /* two by two, three high, a stair and a parapet */
    BOT_FORT_WATCHTOWER, /* two by two, six high and a parapet, a stair round it: a lookout over an objective */
    BOT_FORT_BARRICADE,  /* two floors: a ground wall and a firing deck behind merlons, steps up to it */
} bot_fort_kind_t;

/* [bots] cooperative_forts (default on); SP_BOT_FORTS=0 for tests.
   [bots] build_chance, help_chance (percent) and build_cooldown (seconds):
   a bot asked to start a structure builds only on a build_chance roll, one
   near an unfinished one of its team helps on a help_chance roll, and after
   its part (or a watch on a finished one) it fights build_cooldown seconds
   before it is asked again. SP_BOT_BUILD_CHANCE, SP_BOT_HELP_CHANCE and
   SP_BOT_BUILD_COOLDOWN override them. */
void    bot_forts_init(server_t* server);
void    bot_forts_reset(void);
void    bot_forts_tick(server_t* server);

/* Ask for a structure at the ground cell (x, y, z), facing (fx, fy) along one
   axis. 1 when one was started; 0 when one of the kind stands there already,
   the site cannot take it or no bots are free to build it. The site is
   shared: two teams never raise the same kind twice. */
uint8_t bot_fort_request(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy);
/* The same, with the point the structure is for (the objective: the intel,
   the hill, a territory; z its ground cell): a finished watchtower's top and
   barricade's deck are held by up to BOT_FORT_POSTS bots looking at it. */
uint8_t bot_fort_request_watch(server_t* server, uint8_t team, uint8_t kind, int x, int y, int z, int fx, int fy,
                               int wx, int wy, int wz);

/* A site for a structure of this kind between rmin and rmax of the objective
   (ox, oy; oz its ground cell) whose top sees it: the ray from the eye of a
   bot standing on the finished structure to a body standing on the objective
   is clear of the map (physics_line_clear), on the highest ground and nearest
   the middle of the range. The structure's whole plan must fit there (water,
   protected cells, other structures, a builder's stand). 1 and the ground
   cell when there is one. */
uint8_t bot_fort_overlook(server_t* server, uint8_t team, uint8_t kind, int ox, int oy, int oz, int rmin, int rmax,
                          int fx, int fy, int* out_x, int* out_y, int* out_z);

/* The best hilltop within radius of (cx, cy): level 3x3 dry ground at least
   min_rise blocks above the middle's, the highest and nearest. 1 and the
   ground cell (z: the top block) when there is one with no structure on it.
   For the modes whose objective is not a place to stand on (the CTF's intel):
   a bunker or a tower on the high ground beside it. */
uint8_t bot_fort_hilltop(server_t* server, int cx, int cy, int radius, int min_rise, int* out_x, int* out_y, int* out_z);

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
/* a cell of one of the team's structures: its own bots never spade it (the
   stair's first step was dug by mates walking through once the 30 s of
   bots_team_built had passed, and the tower above it could not be climbed) */
uint8_t bot_fort_cell_kept(uint8_t team, int x, int y, int z);
/* a bot given the watch of a finished watchtower or barricade: its post (the
   floor cell and its column) and the objective it looks at. 1 while it has
   one. */
uint8_t bot_fort_post_goal(server_t* server, player_t* bot, float* x, float* y, int* floor, float* wx, float* wy);
/* on its post now: it fights from there instead of leaving the height */
uint8_t bot_fort_manning(server_t* server, const player_t* bot);

#endif /* SERVER_BOTFORTS_H */
