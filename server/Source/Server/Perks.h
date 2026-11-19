// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PERKS_H
#define PERKS_H

/*
 * Perks (docs/protocol-1.0.md 12.5): streak points per life earn perks, given
 * with no button to press: the jetpack is worn the moment it is earned, the
 * sentry goes to the tool bar and is placed with fire (CONTRIBUTING.md: everything playable with a gamepad).
 *
 *   kill 1, intel pickup 1, intel capture 3 (config.toml [perks])
 *   jetpack at 3 points, sentry gun at 5
 *
 * A perk is granted once per life, kept until used or until death; death
 * resets the points and empties the inventory. Bots earn points too and use
 * them the same way (Bots.c: _bot_sentry calls perks_on_use, _bot_jetpack
 * holds the thrust). The owner sees its state through the PlayerUpdate
 * P10_PU_PERKS bit and places the sentry with PlayerAction P10_EV_USE_PERK; a
 * refused use is answered with GenericCommand "perk-refused <perk> <why>".
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

/* send_kill_action_packet: the victim loses everything, the killer scores */
void perks_on_kill(server_t* server, player_t* killer, player_t* victim, uint8_t reason);
/* an intel was picked up (capture = 0) or captured (capture = 1) */
void perks_on_intel(server_t* server, player_t* carrier, int capture);
/* the perk exists in the running mode (enabled, and the mode has perks) */
int perks_enabled(server_t* server, int perk);
/* the perk is held, enabled and the mode has perks */
int perks_holds(server_t* server, player_t* player, int perk);
/* a class whose life starts with its perk (spawn_perk, the Marksman's
   landmine): one charge in the inventory, not counted as earned, so the
   streak still gives one once it is used */
void perks_class_spawn(server_t* server, player_t* player);
/* the admin's /perk: grant a perk now (1, or 0 when the mode has none) */
int perks_grant(server_t* server, player_t* player, int perk);
/* a class change: the old class's charge and progress go, and the jetpack
   is taken off when the new class does not wear one */
void perks_class_changed(server_t* server, player_t* player);
/* a held perk was used up */
void perks_consume(player_t* player, int perk);
/* PlayerAction P10_EV_USE_PERK from a 1.0 client */
void perks_on_use(server_t* server, player_t* player, uint64_t param);
/* every tick: push changed perk state to its owner */
void perks_tick(server_t* server);

#endif
