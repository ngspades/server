// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTPLANNER_H
#define BOTPLANNER_H

/*
 * The bots' way finding on a thread of its own. A* over the cells a player
 * can stand in used to run on the server tick: ~7 ms a plan on average and up
 * to ~60 ms with 20 bots on 2Fort, a stall every client felt, and it would
 * only grow with big maps and 128 players.
 *
 * The thread never touches the server's map (64 MB of colours on a 512 map,
 * changed by every dig): it reads its own copy, one 64 bit word per column
 * (bit z = solid, the 64 cells of a column), 2 MB on a 512 map, kept up to
 * date by planner_cell() from proto10_world_cell (every map change already
 * goes through it) and refreshed column by column as a safety net. Reads and
 * writes of a word are atomic, so a search sees each column before or after
 * an edit, never torn; a plan can be a few edits old, and the bot walking it
 * checks every step on the real map anyway.
 *
 * What the copy does not know is which cells the game mode protects (Babel's
 * platform, closed arena gates): the main thread checks the dig steps of a
 * finished plan (Bots.c) and bans a protected cell for the next one.
 *
 * Requests and results are per bot (one outstanding request each, a newer
 * one replaces it); the main thread polls with the ticket it got. Maps
 * that are not 64 cells deep are not supported: planner_ready() says 0 and
 * the bots fall back to walking straight and clearing the way.
 */

#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

#define PLAN_PATH_MAX 256 /* steps of a plan: a longer way keeps its first part */

/* how a step is reached */
enum
{
    NAV_WALK = 0,
    NAV_DROP,   /* walk off an edge onto a lower floor */
    NAV_UP,     /* jump up a one block step */
    NAV_BRIDGE, /* put the floor of the next cell at our level, then walk */
    NAV_STAIR,  /* put a block on the next cell's floor, then jump onto it */
    NAV_DIG,    /* open the next cell with the spade, then walk or jump in */
    NAV_PILLAR, /* jump and put a block under its own feet */
    NAV_KINDS,
};

typedef struct
{
    int     sx, sy, sf; /* the start: column and floor cell (z down) */
    int     gx, gy, gz; /* the goal; gz 0 = any floor */
    uint8_t exact;      /* the very column (else within one) */
    uint8_t stealth;    /* the infiltrator's costs: open sky dear, digs cheap */
    uint8_t cover;      /* a player's taste: along walls rather than in the open */
    int     blocks;     /* 0: no step that puts a block */
    uint8_t nodig;      /* no step dug (the guard in its base: it dug through
                           the walls it mends) */
    uint8_t deadly_water; /* no step into the water (it kills at once: the
                             moat under Nine-Eye's tower) */
    uint8_t leap;       /* levels a jump climbs (the zombie's high jump, 3);
                           0 or 1: a step of one */
    uint8_t miner;      /* digs are cheap: the Miner's spade takes a 3x3x3
                           cube a swing and its drill bores 12 ahead */
    int     ban[3];     /* a cell to avoid, ban[0] -1 none */
} plan_request_t;

typedef struct
{
    int16_t  cell[PLAN_PATH_MAX][3]; /* x, y, floor of each step */
    uint8_t  act[PLAN_PATH_MAX];     /* NAV_* */
    uint16_t len;                    /* 0: no way at all */
    uint8_t  full;                   /* reached the goal (else the closest cell it found) */
    int      expanded;
    float    cost;
    float    ms; /* the search's own time, on the planner thread */
    int      counts[NAV_KINDS];
    int      open; /* steps on open ground (no wall at chest height, no roof) */
} plan_result_t;

/* after map_load: (re)builds the copy, starts the thread the first time */
void planner_map_loaded(server_t* server);
/* after any change of cell (x, y, z) of the server's map */
void planner_cell(server_t* server, int x, int y, int z);
/* refreshes n more columns from the server's map (heals a missed edit) */
void planner_refresh(server_t* server, int n);
/* 1 when plans can be asked for */
int planner_ready(void);
/* asks for a plan for bot id; returns the ticket to poll with */
uint32_t planner_submit(uint8_t id, const plan_request_t* request);
/* 1 and the plan when the request of that ticket is done */
int planner_poll(uint8_t id, uint32_t ticket, plan_result_t* out);

#endif
