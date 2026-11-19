// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_JETPACK_H
#define PROTO10_JETPACK_H

#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>

/*
 * The jetpack, a protocol 1.1 perk (docs/protocol-1.0.md 12.4).
 *
 * Nobody has one by default: the perk system calls jetpack_grant() when a
 * player earns it, and it lasts for the rest of that life. While equipped,
 * holding jump in the air thrusts (P10_IN_JETPACK); the fuel refills on the
 * ground. The physics is common/flight/flight.h, run by physics_move_player
 * on the server and by player_move on the client with the same numbers.
 * Clients learn that a body wears it from the P10_EF_JETPACK entity flag.
 */

/* reads the optional [jetpack] table of Resources/config.toml */
void  jetpack_init(server_t* server);

/* equips the player's jetpack with a full tank for the rest of the current
   life (cleared at the next spawn). Refused, with an EVT line saying why,
   when the jetpack is disabled or the player is not alive in the game. */
void  jetpack_grant(server_t* server, player_t* player);
int   jetpack_equipped(const player_t* player);
/* takes the jetpack off (the mode has no perks any more) */
void  jetpack_remove(server_t* server, player_t* player, const char* why);
/* seconds of thrust left; 0 when not equipped */
float jetpack_fuel(const player_t* player);

/* every spawn: a new life starts without the jetpack (unless the test-only
   grant_on_spawn is set) */
void  jetpack_on_spawn(server_t* server, player_t* player);
/* the owner's P10_IN_JETPACK bit: the 1.0 bridge for a client, _bot_jetpack
   for a bot */
void  jetpack_set_input(server_t* server, player_t* player, int want);
/* logs thrust start/stop with the reason and the refill; call right after
   physics_move_player with the state from before it */
void  jetpack_after_move(server_t* server, player_t* player, uint8_t was_thrusting, float fuel_before);
/* the rules as GameStateFinal world parameters, so the client predicts with
   the same numbers; nothing is written when the jetpack is disabled */
void  jetpack_put_world_params(server_t* server, p10_writer* w);

#endif
