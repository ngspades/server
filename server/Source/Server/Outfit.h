// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OUTFIT_H
#define OUTFIT_H

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

/* Player Outfit over 1.0: how each player looks, the numbers
   of the aosprotocol specification's draft (github.com/zerospades/aosprotocol,
   pull request #14, extensions/player-outfit.md: 0 soldier, 7 miner, 64
   zombie, 255 the team's). Cosmetic only: hits and collisions are the
   player's. Every 1.0 client is told `team-outfits <team> <outfit>...`
   and `outfit <player> <outfit> <colour>` when it enters the game and on a
   change (docs/protocol-1.0.md 10). Set by plugins and /outfit. */

#define OUTFIT_TEAM 255

/* colour: 0 or 1, the team whose colour the body wears, 255 its own */
int  outfit_set(server_t* server, player_t* player, int outfit, int colour, const char* why);
int  outfit_set_team(server_t* server, int team, int outfit, const char* why);
int  outfit_of(const player_t* player); /* what it was set to, 255 when only its team's */
int  outfit_of_team(int team);
void outfit_send_to(server_t* server, player_t* player);
/* a player gone: its id wears its team's again (the client resets it itself) */
void outfit_player_left(player_t* player);
/* a new map: every player's outfit is reset, the teams' are kept (the draft) */
void outfit_map_start(void);

#endif
