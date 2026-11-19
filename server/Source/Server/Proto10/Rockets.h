// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_ROCKETS_H
#define PROTO10_ROCKETS_H

/*
 * The Commando's rocket launcher (GAME_WEAPON_ROCKET, docs/protocol-1.0.md
 * 12.6). The client asks with the GenericCommand
 *   rocket <ox> <oy> <oz> <dx> <dy> <dz>
 * (the muzzle and the direction, map coordinates, z down); the server
 * checks it, spends the clip's rocket and flies it: a P10_ENT_ROCKET entity
 * with a Linear trajectory at ROCKET_SPEED blocks a second, no gravity. It
 * goes off on the first block, player (not its shooter) or enemy object
 * on its way, or after ROCKET_LIFETIME: a grenade's blast
 * (explosion_blast) and a 3 x 3 x 3 crater, half the damage to its
 * shooter, who is also pushed away from it (the rocket jump: the server
 * applies the push and tells the shooter "rocket-push <vx> <vy> <vz>").
 * Bots fire through the same function. EVT rocket action=fire|refused|
 * explode|push.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>
#include <proto10/codec.h>

/* BattleSpades' figure (its weapon catalog, taken from the retail game:
   ROCKET_SPEED 75): 40 read as a missile hanging in the air, and the
   RocketSpades client's rocket crossed a room before ours left the tube */
#define ROCKET_SPEED        75.0f /* blocks a second */
#define ROCKET_LIFETIME     3.0f  /* seconds, then it goes off where it is */
#define ROCKET_INTERVAL     0.8f  /* seconds between two shots (the clip holds one) */
#define ROCKET_CLIP         1
#define ROCKET_RESERVE      4
#define ROCKET_RELOAD_MS    2500
/* the retail rocket's blast (BattleSpades' ROCKET_EXPLOSION_DAMAGE 140,
   BLAST_WAVE_RADIUS 6, falling as (R^2 - d^2) / R^2): a direct hit kills,
   one 3 blocks away still does 105. It was half a grenade's capped curve, 50
   at most, which never killed in one rocket */
#define ROCKET_DAMAGE       140.0f
#define ROCKET_BLAST_RADIUS 6.0f
#define ROCKET_SELF_SCALE   0.5f  /* the shooter's share (retail's self reduction) */
#define ROCKET_HIT_RADIUS   0.7f  /* a body point this close to the rocket stops it */
#define ROCKET_PUSH_RADIUS  4.0f  /* the rocket jump reaches this far */
#define ROCKET_PUSH         0.75f /* the push at the blast (velocity, -0.36 is a jump) */

/* fire one: 1 when it flew, 0 refused (the why logged, and told to a 1.0
   client as "rocket-refused <why>"). by: "client" or "bot" */
int rockets_fire(server_t* server, player_t* player, vector3f_t origin, vector3f_t dir, const char* by);
/* every server tick: fly them, blow them up */
void rockets_tick(server_t* server);
/* one reliable packet to every 1.0 client in the game */
void rockets_broadcast(server_t* server, const p10_writer* w);

#endif
