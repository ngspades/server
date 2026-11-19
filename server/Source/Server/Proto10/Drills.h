// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_DRILLS_H
#define PROTO10_DRILLS_H

/*
 * The Miner's drill bit (docs/protocol-1.0.md 12.6, the figures in
 * common/classes/classes.h). The GenericCommand "drill" (classes_drill)
 * fires one along the look: a P10_ENT_DRILL entity, owner and team, sent to
 * everyone. In the air it falls (a Gravity trajectory, GAME_DRILL_GRAVITY);
 * in the ground it bores straight on (Linear), clearing the 3 x 3 x 3 cube
 * round each block it enters (the cells classes_cell_diggable lets go,
 * through proto10_world_cell) and losing GAME_DRILL_DRAG of its speed a
 * block. It stops after GAME_DRILL_DEPTH blocks of ground, under
 * GAME_DRILL_MIN_SPEED, at a cell nothing may dig (the map's or the mode's,
 * or the water's level), off the map or after GAME_DRILL_LIFETIME: an
 * EntityRemove, which the client shows as a puff of dust. Its trajectory
 * is sent again when it goes into or out of the ground and every
 * DRILL_RESEND seconds while it bores (the drag). EVT class action=drill
 * (fired) and action=drill_stop why= dug= cleared=.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>

#define DRILL_RESEND 0.15f

/* fire one from the player's eye along dir (a unit vector) */
void drills_fire(server_t* server, player_t* player, vector3f_t dir, const char* by);
/* every server tick: fly them, bore */
void drills_tick(server_t* server);

#endif
