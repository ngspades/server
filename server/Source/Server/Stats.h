// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_STATS_H
#define SERVER_STATS_H

/*
 * Per-match statistics and the end-of-match summary. The counters live on
 * player_t (kills, deaths, assists, score, captures, shots, hits, blocks) and
 * are reset when the map starts. They are server-side only: /stats and the
 * summary use the chat, so no client or renderer change is needed.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stddef.h>

/* called once per map, after the players are reset and before they spawn */
void stats_match_start(server_t* server);

/* the chat summary, sent just before the next map is loaded */
void stats_match_summary(server_t* server);

/* one player's line for /stats (the caller sends it) */
void stats_player_line(server_t* server, player_t* player, char* out, size_t size);

/* the map the rotation will load next (for the summary and /nextmap) */
const char* stats_next_map(server_t* server);

#endif
