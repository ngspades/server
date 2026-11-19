// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_LAGCOMP_H
#define SERVER_LAGCOMP_H

/*
 * Bounded lag compensation: a hit is judged against the body the shooter
 * could see, not only against the body the server has now.
 *
 * Every player keeps LAG_HISTORY_SIZE samples (position and alive flag) at
 * the game clock. A human's Hit is dated by its arrival and the peer's RTT;
 * the target is interpolated at that time, clamped by an absolute maximum and
 * a per-RTT allowance. Death, respawn and a teleport (a jump of more than 5
 * blocks between two samples) never rewind. Terrain and line of sight stay in
 * the present. Only the hitbox test uses the rewound body; damage and
 * knockback use the live one.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

/* [net] lag_compensation (default on), max_ms, extra_ms; SP_LAGCOMP=0 and
   SP_LAGCOMP_MAX_MS / SP_LAGCOMP_EXTRA_MS override them for tests. */
void lagcomp_init(server_t* server);

/* once a tick for every player, before hits are handled */
void lagcomp_record(server_t* server, player_t* player);

/* 1 and *out when a rewound position applies to this shooter and target */
int lagcomp_rewind(server_t* server, player_t* shooter, player_t* target, vector3f_t* out);

/* the hitbox tolerance: 1.5 blocks with compensation, 5 without */
float lagcomp_hit_tolerance(void);

#endif
