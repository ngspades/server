// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CREATIVE_H
#define CREATIVE_H

/*
 * Creative: building only. Everyone has the spade and the blocks, never
 * runs out of blocks, flies like the spectator's camera (free flight:
 * player->flying, common/flight's flight_free; no jetpack) and takes no
 * damage of any kind: no gun, no grenade,
 * no fall, no water. No objective, no score, no perk. Its bots wander by
 * their spawn.
 *
 * 1.0 clients learn it from GameModeState mode=creative (GAMEMODE_CREATIVE
 * on the client: no health on the HUD, the gun and the grenade locked, the
 * blocks kept full, the same free flight predicted). The server reaches it only through
 * creative_ops. Logged as EVT creative action=..
 */

#include <Server/Gamemodes/Gamemode.h>

extern const gamemode_ops_t creative_ops;

#endif
