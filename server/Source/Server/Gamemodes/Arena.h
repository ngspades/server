// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARENA_H
#define ARENA_H

/*
 * Arena (pyspades rules): rounds with no respawn. Each team waits behind
 * gates on its spawn cell during a countdown, the gates open, and the last
 * team with someone alive wins the round. The per-map capture_limit is the
 * number of rounds that wins the match (then the map reloads, like a CTF
 * win). With nobody on one of the teams the arena waits with the gates open
 * and respawns allowed.
 *
 * The map's toml carries the gates:
 *
 *   [arena]
 *   gates = [[x, y, z], ...]   # seed cells, each flooded to its whole gate
 *   countdown_seconds = 5
 *   round_seconds = 180        # then the round is a draw
 *
 * Logged as EVT arena action=...; 1.0 clients learn it from the arena-*
 * keys of GameModeState. The server reaches it only through arena_ops.
 */

#include <Server/Gamemodes/Gamemode.h>

extern const gamemode_ops_t arena_ops;

#endif
