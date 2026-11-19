// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_SESSION_H
#define PROTO10_SESSION_H

/*
 * Protocol 1.0 connections (docs/protocol-1.0.md). A peer that connected with
 * P10_CONNECT_MAGIC sits in STATE_PROTO10 and is driven only from here; the
 * server sends no 0.75 packets at all.
 *
 * Handshake, map transfer, GameStateFinal. The map goes as ngmap when the
 * client's InitiateConnection.mapQuality is 1..100 and as zlib VXL when it is
 * 0 (docs/protocol-1.0.md 6.2, 7.1). ngmap is encoded on a background thread
 * from a VXL snapshot and cached per (map, quality); the edits made after the
 * snapshot are replayed to the joiner as TerrainUpdate.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    P10_STATE_NOT_INITIATED,    /* Greeting sent, waiting for InitiateConnection */
    P10_STATE_WAITING_CERT,     /* ServerCertificate sent, waiting for ClientCertificate */
    P10_STATE_PREPARING_MAP,    /* ngmap being encoded in the background, nothing sent yet */
    P10_STATE_WAITING_MAP_ANSWER, /* GameStateHeader sent, waiting for "map-cached" (7.1) */
    P10_STATE_MAP_TRANSFER,     /* sending MapData fragments */
    P10_STATE_COMPLETING_MAP,   /* MapDataFinal sent, waiting for MapDataAcknowledge */
    P10_STATE_GAME,             /* GameStateFinal sent */
    P10_STATE_CLOSING,          /* disconnect requested, ignore everything until ENet confirms */
} p10_state_t;

/* called once the player slot exists; sends the Greeting */
void proto10_begin(server_t* server, player_t* player);
/* called every tick for PROTOCOL_10 players: timeouts and map pacing */
void proto10_update(server_t* server, player_t* player);
/* still in the first handshake steps (Greeting answered or not, no
   certificate yet): what a real client leaves in milliseconds */
int proto10_in_handshake(const player_t* player);
/* closes such a connection to make room (Player.c's half-open cap) */
void proto10_evict(player_t* player, const char* why);
/* a new map transfer (map reset): GameStateHeader, MapData..., as after the handshake */
void proto10_restart_map(server_t* server, player_t* player);
/* once per server tick: hands finished background map encodings to their joiners */
void proto10_session_tick(server_t* server);
/* a map cell changed (from proto10_world_cell): map snapshots being encoded
   or cached record it, to replay it to whoever receives that snapshot */
void proto10_session_map_edit(int x, int y, int z);
/* a new map was loaded: drops the ngmap cache, running encodings go stale */
void proto10_session_map_loaded(void);
/* the map was loaded from (or matches) this ngmap stream: every 1.0 join gets
   it as it is, with the edits since as TerrainUpdate. from names it
   for the log */
void proto10_session_map_stored(server_t* server, const uint8_t* data, size_t size, const char* from);
/* the map is loaded: with no stored stream it is encoded now, on a thread,
   and the stream written to save (NULL: not kept) */
void proto10_session_map_ready(server_t* server, const char* save);
/* called for every packet a PROTOCOL_10 peer sends */
void proto10_on_packet(server_t* server, player_t* player, const uint8_t* data, size_t size);

#endif
