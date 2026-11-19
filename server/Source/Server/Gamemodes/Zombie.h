// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ZOMBIE_H
#define ZOMBIE_H

/*
 * Zombies (the zombie mode of the Steam Ace of Spades): team 1 are humans
 * with the usual tools, team 2 are zombies with their hands only (the
 * spade's slot: no gun, no grenade, no block), a jump of ~3.5 blocks and no
 * fall damage. A round: a countdown, then a fifth of the players (at least
 * one) start as zombies, the rest as humans; a human who dies during the
 * fight comes back as a zombie. The humans win when the clock runs out with
 * one of them alive, the zombies when none is left. The map's
 * capture_limit is the rounds that win the match.
 *
 * Logged as EVT zombie action=...; 1.0 clients learn it from the arena-*
 * keys of GameModeState (state, round, time, winner) and zombie-team /
 * zombie-humans. The server reaches it only through zombie_ops.
 */

#include <Server/Gamemodes/Gamemode.h>

extern const gamemode_ops_t zombie_ops;

#endif
