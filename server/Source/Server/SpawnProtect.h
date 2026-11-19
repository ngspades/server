// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_SPAWNPROTECT_H
#define SERVER_SPAWNPROTECT_H

/*
 * A short grace period after a spawn: enemy fire cannot hurt the player who
 * just appeared, so a spawn is not a free kill. Falling, water and boundary
 * damage still apply, and firing ends the protection immediately. The length
 * comes from [gameplay] spawn_protection_seconds (default 2, 0 turns it off);
 * SP_SPAWN_PROTECT overrides it for tests.
 *
 * Server-side only: no packet or client change.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

void    spawnprotect_init(server_t* server);
void    spawnprotect_on_spawn(player_t* player);
uint8_t spawnprotect_active(player_t* player);
void    spawnprotect_clear(player_t* player);

#endif
