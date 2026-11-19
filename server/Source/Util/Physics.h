/*
    Copyright (c) Mathias Kaerlev 2011-2012.

    This file is part of pyspades.

    pyspades is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    pyspades is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with pyspades.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef PHYSICS_H
#define PHYSICS_H

// SpadesX
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>

enum damage_index { BODY_TORSO, BODY_HEAD, BODY_ARMS, BODY_LEGS, BODY_MELEE };

float physics_distance_3d(float x1, float y1, float z1, float x2, float y2, float z2);
int   physics_validate_hit(vector3f_t shooter, vector3f_t orientation, vector3f_t otherPos, float tolerance);
/* the jump and the landing speed falls hurt from: 0.75's figures, the 1.0
   server's [gameplay] keys (Server/GameplayPhysics.c) */
#define PHYSICS_JUMP_VELOCITY        0.36f
#define PHYSICS_FALL_DAMAGE_VELOCITY 0.58f
#define PHYSICS_FALL_DAMAGE_SCALAR   4096
extern float physics_jump_velocity;
extern float physics_fall_damage_velocity;
long  physics_can_see(server_t* server, float x0, float y0, float z0, float x1, float y1, float z1);
/* 1 when no solid cell lies on the segment, every cell walked */
uint8_t physics_line_clear(server_t* server, float x0, float y0, float z0, float x1, float y1, float z1);
long  physics_cast_ray(server_t* server,
                       float     x0,
                       float     y0,
                       float     z0,
                       float     x1,
                       float     y1,
                       float     z1,
                       float     length,
                       long*     x,
                       long*     y,
                       long*     z);

void physics_reorient_player(player_t* player, vector3f_t* orientation);
int  physics_try_uncrouch(server_t* server, player_t* player);
void physics_box_clip_move(server_t* server, player_t* player, physics_t* physics);
long physics_move_player(server_t* server, player_t* player, physics_t* physics);
int physics_move_grenade(server_t* server, grenade_t* grenade, physics_t* physics);

#endif
