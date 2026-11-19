// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GAMEPLAYPHYSICS_H
#define GAMEPLAYPHYSICS_H

#include <Server/Structs/ServerStruct.h>

/* [gameplay] jump_velocity and fall_damage_velocity (protocol 1.0 only):
   the physics' jump and the landing speed falls start to hurt at, which the
   server sends as the reference's world parameters player-jump-vel,
   fall-damage-vel and fall-damage-fatal-vel, and the client plays by. The
   0.75 profile keeps 0.36 and 0.58: its clients predict those */
void gameplay_physics_init(server_t* server);

#endif
