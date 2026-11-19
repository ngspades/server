// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_GAMEMODE_H
#define PROTO10_GAMEMODE_H

/*
 * GameModeState (80) for protocol 1.0 clients (docs/protocol-1.0.md 12.2.3).
 * The state is computed from the same variables 0.75's StateData reads
 * (server->protocol.gamemode, has_intel), so both protocols always describe
 * the same match. A snapshot goes to a client right after GameStateFinal
 * and to everyone at a round start; deltas carry only the keys whose value
 * changed since the last broadcast.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

typedef enum {
    P10_GM_EVENT_PICKUP,
    P10_GM_EVENT_DROP,
    P10_GM_EVENT_CAPTURE,
} p10_gm_event_t;

/* "ctf", "tc" or "arena": the mode as protocol 1.0 names it */
const char* proto10_gamemode_name(server_t* server);
/* right after GameStateFinal: the full state to that client only */
void proto10_gamemode_send_snapshot(server_t* server, player_t* player);
/* gamemode_init: a round starts (first start, win, /reset) */
void proto10_gamemode_round_start(server_t* server);
/* an intel changed hands; team is the team the intel belongs to. Called by
   Ctf.c (pickup, drop, capture) once their state is updated, so
   the delta goes out before anything that follows (a win resets the round
   in the same call) */
void proto10_gamemode_intel_event(server_t* server, uint8_t team, p10_gm_event_t event);
/* every tick: anything else that moved (MoveObject: intel and tents sinking
   or rising with the terrain, the intel respawning) */
void proto10_gamemode_tick(server_t* server);

#endif
