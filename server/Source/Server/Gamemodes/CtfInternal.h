// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CTFINTERNAL_H
#define CTFINTERNAL_H

/*
 * The capture the flag hooks Ctf.c defines and the CTF modes share: classic
 * CTF (ctf_ops, Ctf.c) and the next gen one (ngctf_ops, NgCtf.c) play the
 * same intels, tents and bots, ngCTF adding its features on top. Only the
 * CTF mode files include this; the server reaches a mode through its ops.
 */

#include <Server/Gamemodes/Gamemode.h>

void handleTentAndIntel(server_t* server, player_t* player);
void _ctf_init(server_t* server);
void _ctf_tick(server_t* server);
void _ctf_player_out(server_t* server, player_t* player);
void _ctf_terrain_changed(server_t* server, int built);
void _ctf_state_keys(server_t* server, gm_keys_t* out);
/* the match summary's column: the player's captures */
uint32_t _ctf_player_captures(server_t* server, player_t* player);
/* the match summary's column: the player's captures */
uint32_t _ctf_player_captures(server_t* server, player_t* player);
int  _ctf_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out);
void _ctf_patrol_anchor(server_t* server, player_t* bot, float* x, float* y);
int  _ctf_spawn_facing(server_t* server, player_t* player, float* x, float* y);
int  _ctf_player_carries(server_t* server, player_t* player);
int  _ctf_escape_watch_player(server_t* server, player_t* player);

#endif
