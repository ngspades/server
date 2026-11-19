// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ASSISTS_H
#define ASSISTS_H

/*
 * Kill assists (BF4): every damage a player takes from
 * an enemy is remembered for ASSIST_WINDOW seconds; on its death, each
 * enemy who hurt it in that time and is not the killer earns an assist.
 * Falls, water and the map's boundary have no attacker and credit nobody.
 * 1.0 clients are told with "kill-assist <killer> <assister>" (everyone:
 * the client tallies the scoreboard from it, as it does kills). Logged as
 * EVT assist killer= damage=.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

/* send_set_hp, once the damage is applied */
void assists_on_damage(player_t* attacker, player_t* victim, long amount);
/* send_kill_action_packet, before the victim is dead */
void assists_on_kill(server_t* server, player_t* killer, player_t* victim);

#endif
