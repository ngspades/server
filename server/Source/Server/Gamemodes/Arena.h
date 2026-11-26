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

/* the arena's rounds machinery, for Plant the Bomb (Ptb.c), which plays on
   the arena's maps: their [arena] gates and spawn points, the countdown's
   leash. The sides never swap there (spawn_change 0) */
void arena_kit_read_map(server_t* server, toml_table_t* map_toml);
void arena_kit_gates_init(server_t* server);
void arena_kit_set_gates(server_t* server, int closed);
int  arena_kit_gates_closed(void);
int  arena_kit_gate_cell(int x, int y, int z);
int  arena_kit_gate_count(void);
int  arena_kit_countdown_seconds(void);
int  arena_kit_spawn_point(server_t* server, player_t* player);
void arena_kit_side_middle(server_t* server, int side, float* x, float* y, float* z);
void arena_kit_keep_in_spawn(server_t* server);
int  arena_kit_land_near(server_t* server, player_t* bot, float x, float y, vector3f_t* out);

#endif
