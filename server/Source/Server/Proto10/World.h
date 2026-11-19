// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_WORLD_H
#define PROTO10_WORLD_H

/*
 * Protocol 1.0 view of the terrain and the chat (docs/protocol-1.0.md 9,
 * 12.2, milestone 5).
 *
 * Terrain: every place that changes the map calls proto10_world_cell() right
 * after the map_set_* call. Once per tick the changed cells are read back
 * from the map and sent to 1.0 clients as one TerrainUpdate batch, so the
 * packet always carries the map's real state (a cell built and dug in the
 * same tick goes out once, as air). Clients still downloading the map get the
 * batches too and apply them after decoding it (dossier 7.3).
 *
 * Inputs (BuildBlock, BuildLine, HitTerrain, ChatMessage) are replayed through
 * the 0.75 receive handlers, like the rest of the bridge.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stddef.h>
#include <stdint.h>

/* why a cell changed: becomes the TerrainUpdate create/destroy cause */
#define P10_WORLD_CAUSE_NONE     0 /* resync */
#define P10_WORLD_CAUSE_PLAYER   1 /* built by a player, or destroyed by damage (spade, gun, grenade) */
#define P10_WORLD_CAUSE_COLLAPSE 2 /* detached when its support was removed */

/* a cell of server->s_map.map was just changed */
void proto10_world_cell(server_t* server, int x, int y, int z, uint8_t cause);
/* once per server tick: flush the changed cells as TerrainUpdate */
void proto10_world_tick(server_t* server);
/* a map cell packed in 32 bits: x and y below 4096, z below 256 */
#define P10_CELL_PACK(x, y, z) ((uint32_t) (x) | ((uint32_t) (y) << 12) | ((uint32_t) (z) << 24))
#define P10_CELL_X(c)          ((c) & 0xfffu)
#define P10_CELL_Y(c)          (((c) >> 12) & 0xfffu)
#define P10_CELL_Z(c)          ((c) >> 24)
/* sends the current state of these cells to one player as TerrainUpdate (an
   ngmap joiner gets the edits made after its map snapshot this way, see
   Session.c); returns the edits sent, -1 when out of memory */
int proto10_world_send_cells(server_t* server, player_t* player, const uint32_t* cells, size_t count);
/* every tick for a PROTOCOL_10 player. When a map reset moved it to
   STATE_STARTING_MAP, it downloads the new map over 1.0 and picks a team
   again, as a 0.75 client does; returns 1 then (skip the 0.75 states) */
int proto10_world_player_update(server_t* server, player_t* player);

/* chat the 0.75 handler accepted, for the same recipients as its broadcast */
void proto10_world_chat(server_t* server, player_t* sender, uint8_t meant_for, const char* message);
/* send_server_notice / broadcast_server_notice, as ChatBroadcast channel 2 */
void proto10_world_notice(player_t* player, const char* message);
void proto10_world_broadcast_notice(server_t* server, const char* message);

/* BuildBlock, BuildLine, HitTerrain and ChatMessage of a 1.0 client in the
   game; returns 0 when the packet is malformed */
int proto10_world_on_packet(server_t* server, player_t* player, uint8_t type, const uint8_t* data, size_t size);

#endif
