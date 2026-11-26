// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Solid players: in a mode that asks for it (the server's GM_RULE_SOLID_PLAYERS,
 * Plant the Bomb) one living body cannot walk into another. Ace of Spades lets
 * bodies pass through each other; the mode that has rounds without respawns
 * does not want two players standing in one place.
 *
 * Shared by the server's physics (physics_box_clip_move, for every body it
 * moves: bots and humans) and the client's prediction of its own body
 * (player_boxclipmove), so the two step the same way and nobody is sent a
 * position_corrected. Header only, no dependency on either program.
 *
 * The test is horizontal: two circles of GAME_SOLID_DIST between centres. A
 * move that would end closer than that is slid along the other body's edge.
 * A body already inside (a spawn, a reset, a teleport) is never held: it may
 * move apart freely and is eased out by GAME_SOLID_NUDGE a step, so nothing
 * traps a player.
 */
#ifndef SHARED_PHYSICS_SOLID_H
#define SHARED_PHYSICS_SOLID_H

#include <math.h>

/* two bodies of half width 0.45 touch at this distance between centres */
#define GAME_SOLID_DIST 0.90f
/* the server's check of a position a client reports: looser than the
   client's own, since the client sees the others a little late */
#define GAME_SOLID_DIST_REPORT 0.60f
/* how far an overlapping body is eased out each step */
#define GAME_SOLID_NUDGE 0.05f
/* bodies this far apart in height (z, down) walk over or under each other */
#define GAME_SOLID_HEIGHT 1.5f

/* Is another body at (bz) in the way of one at (az) vertically */
static inline int game_solid_height(float az, float bz) {
    return fabsf(az - bz) < GAME_SOLID_HEIGHT;
}

/* One body's move from (ox, oy) to (*nx, *ny) against another's centre
   (bx, by); `nudge` is how far an overlapping body is eased out (0: it only
   may not go deeper, the server's check of a report). `sign` (+1 or -1, from comparing the two ids) breaks the tie of
   two bodies on the very same point. Returns 1 when the move was changed. */
static inline int game_solid_clip(float ox, float oy, float* nx, float* ny, float bx, float by, float min_dist,
                                  float nudge, float sign) {
    float dx = *nx - bx, dy = *ny - by;
    float nd = sqrtf(dx * dx + dy * dy);
    float od = sqrtf((ox - bx) * (ox - bx) + (oy - by) * (oy - by));
    /* a clear body must stay clear; an overlapping one only has to not go
       deeper, and is eased out a little each step */
    float need = od >= min_dist ? min_dist : fminf(min_dist, od + nudge);

    if(nd >= need)
        return 0;
    if(nd > 0.0001f) {
        dx /= nd;
        dy /= nd;
    } else {
        dx = sign;
        dy = 0.0f;
    }
    *nx = bx + dx * need;
    *ny = by + dy * need;
    return 1;
}

#endif
