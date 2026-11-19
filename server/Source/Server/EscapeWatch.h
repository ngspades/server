// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_ESCAPEWATCH_H
#define SERVER_ESCAPEWATCH_H

/*
 * Reveal players who escaped the playable space or sealed themselves away.
 *
 * Checked once a second: outside the map, below the floor, above the sky,
 * embedded in solid blocks, or standing in a small air pocket with no route
 * to open air. The first four apply to everyone; the sealed-pocket test asks
 * the mode whether the player matters (gamemode_escape_watch_player), so a
 * normal bunker is never marked. A flagged player gets a danger ping on the
 * enemy team's screen and map, re-sent while the condition holds.
 */

#include <Server/Structs/ServerStruct.h>

/* [conduct] escape_watch, sky_seconds, embedded_seconds, entomb_seconds */
void escape_watch_init(server_t* server);

/* once a tick; it self-limits to 1 Hz */
void escape_watch_tick(server_t* server);

#endif
