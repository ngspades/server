// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The player's box and the one block step, shared by the server's physics
 * (server/Source/Util/Physics.c, physics_box_clip_move) and the client's
 * prediction (the client's player.c, player_boxclipmove).
 *
 * The two movers are copies of one another: the server runs the player's
 * physics between position reports, the client predicts the same step from
 * the same keys, and a number that changed on one side only put the
 * predicted body where the server's was not, which came back as a
 * position_corrected every tick. The numbers are these; a rule about who
 * climbs a step is below (flight.h is the same idea for the jetpack).
 *
 * Header only: std C99, no dependency on either program.
 */
#ifndef SHARED_PHYSICS_BOX_H
#define SHARED_PHYSICS_BOX_H

#include <devices/devices.h>

/* half the width of the body the box test casts: two 0.45 corners */
#define GAME_BOX_HALF_WIDTH 0.45f

/* the point the box is tested from, position.z + lift... */
#define GAME_BOX_LIFT_STAND  0.90f
#define GAME_BOX_LIFT_CROUCH 0.45f

/* ...and how far the scan starts from it, toward the feet: the body's
   height (the crouched body is shorter) */
#define GAME_BOX_SPAN_STAND  1.35f
#define GAME_BOX_SPAN_CROUCH 0.90f

/* how the scan walks the body, a third of a block at a time, and where it
   gives up (a step higher than this is a wall) */
#define GAME_BOX_SCAN_STEP 0.90f
#define GAME_BOX_SCAN_END  (-1.36f)

/* the second scan, for the climb of a one block step: it starts at the
   step's height and ends one body further up */
#define GAME_BOX_CLIMB_START 0.35f
#define GAME_BOX_CLIMB_END   (-2.36f)
/* and where the feet land on the step after a climb */
#define GAME_BOX_CLIMB_LAND  (-1.35f)

/* a body deeper than the map's water layer is wading (z of the water, the
   map's last layer) */
#define GAME_WADE_Z 61.0f

/* A phone's or a pad's sprint climbs a one block step: sprint there is a
   stick pushed to its edge or a held button, not the keyboard's key, and a
   step stopped the player. The keyboard keeps the original rule. Which
   device a player uses is the caller's to say (the server's copy of the
   client's 1.0 device, the client's own on a 0.75 server). */
static inline int game_device_sprint_climbs(int device) {
    return device == GAME_INPUT_PAD || device == GAME_INPUT_TOUCH;
}

#endif
