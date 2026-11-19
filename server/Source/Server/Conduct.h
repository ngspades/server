// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_CONDUCT_H
#define SERVER_CONDUCT_H

/*
 * Player conduct: AFK kicks and team-grief accounting. Both run on the
 * gameplay thread and use only the chat/notice and kick paths a moderator
 * already uses.
 *
 * AFK counts a real input change (movement, fire, aim, hover or a visible
 * orientation change), never packet arrival. The clock pauses while the
 * player loads, is dead or waits for respawn. Team grief counts HP removed
 * from a teammate and a bonus for a team kill, decays linearly and warns
 * before it kicks. Staff are exempt from both.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

/* [conduct] in Resources/config.toml; call once at start-up */
void conduct_init(server_t* server);

/* once a tick for every player */
void conduct_player_tick(server_t* server, player_t* player);

/* a damage event after the mode/plugin filters; attacker may be NULL */
void conduct_on_damage(server_t* server, player_t* attacker, player_t* victim, long damage);

/* a real death, after the mode's own kill hook */
void conduct_on_kill(server_t* server, player_t* killer, player_t* victim);

#endif
