// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CTF_H
#define CTF_H

/*
 * Capture the flag and Babel (Ctf.c): take the enemy intel and bring it into
 * your tent. Babel puts both intels on a tower in the middle of the map and
 * protects its platform. Everything about the intels and the tents lives in
 * Ctf.c; the server reaches it only through the game mode interface. The
 * next gen CTF is NgCtf.c, on the same hooks (CtfInternal.h).
 */

#include <Server/Gamemodes/Gamemode.h>

extern const gamemode_ops_t ctf_ops;

/* a bot with a role of its own (guard, infiltrator, cover, camper): kept out
   of squads (Squads.c) */
int ctf_bot_has_post(server_t* server, player_t* player);
extern const gamemode_ops_t babel_ops;

/* a script's place for team's intel (kind MAP_META_FLAG) or tent
   (MAP_META_BASE), on the dry ground nearest x, y: used now (the intel
   unless carried) and at every capture, before the map's own. A new map
   clears them (Lua's push.lua) */
void ctf_set_spot(server_t* server, uint8_t team, int kind, float x, float y);

#endif
