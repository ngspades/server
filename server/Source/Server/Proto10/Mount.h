// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_MOUNT_H
#define PROTO10_MOUNT_H

/*
 * Mounting (docs/protocol-1.0.md 12.8): a player boards a helicopter's seat or
 * mans a machine gun with the `use` GenericCommand, and leaves it the same
 * way. A mounted player's physics does not run: the seat holds its body
 * (Mount.c's mount_player_step, called from the physics), the pilot's
 * ordinary movement keys fly the helicopter and its look aims the MG.
 *
 * Protocol 1.0 only: the 0.75 profile has neither. The seat is player_t's
 * mount_entity (an object id, P10_NO_OWNER when on nothing) and mount_seat;
 * every change goes to every 1.0 client as `mount <player> <entity> <seat>`,
 * and `mount <player> -1 0` when the player leaves.
 *
 * EVT mount action=mount|dismount|refused player= entity= seat= why=
 */

#include <Server/Proto10/Objects.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <stdint.h>

/* "use" from a 1.0 client: board the nearest thing in reach of the player's
   own team, or leave what this player is on */
void mount_use(server_t* server, player_t* player);
/* leaves what the player is on; the body then falls from the seat */
void mount_dismount(server_t* server, player_t* player, const char* why);
/* 1 while the player is on anything (a seat or an MG): its physics is off */
int mount_mounted(const player_t* player);
/* 1 while the player rides a helicopter: no bullet can hit it (in_vehicle) */
int mount_in_vehicle(const player_t* player);
/* the player in that seat of that entity, or NULL */
player_t* mount_occupant(server_t* server, uint32_t entity, int seat);
/* the pilot's lift: -1 descend, 0 hold, 1 climb. The held jump (the
   PlayerInput's jetpack bit, or a PlayerAction Jump) climbs, the crouch key
   descends */
int mount_lift_input(const player_t* player);
/* the seat holds the body this tick: 1 while mounted, its physics skipped */
int mount_player_step(server_t* server, player_t* player);
/* a death, a team change, a leave and a map start take a player off */
void mount_player_left(server_t* server, player_t* player);
void mount_team_change(server_t* server, player_t* player);
void mount_map_start(server_t* server);
/* everyone mounted, to a 1.0 client entering the game (after the lights) */
void proto10_bridge_mounts_to(server_t* server, player_t* to);

#endif
