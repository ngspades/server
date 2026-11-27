// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LEGACY_WIRE075_H
#define LEGACY_WIRE075_H

/*
 * The 0.75 wire of the 0.75 profile (docs/plan-075-server.md section 8):
 * the senders the 2026-10-03 commits ba72777 and da14df7 removed, restored
 * byte for byte (packet ids, field order), on today's transport (conn_send,
 * channel 0; a NULL conn is a bot and gets nothing).
 *
 * One binary holds both profiles, so the 1.0 functions keep their names and
 * bodies: each place that tells clients something calls these only when
 * profile_is_075() (the Packets folder, Player.c, Server.c, Util/Notice.c, and a
 * dispatch at the top of the few proto10_* entry points called from files
 * that are not the wire's: proto10_world_cell, proto10_bridge_teleport,
 * proto10_bridge_generic, proto10_gamemode_intel_event). Nothing here runs
 * in the 1.0 profile.
 *
 *   Dispatch075.c  the receive table (on_packet_received_075)
 *   Join075.c      the join: VXL map in chunks, version request, state data
 *   Wire075.c      the senders, the terrain, the per-tick world update
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>
#include <stddef.h>
#include <stdint.h>

/* the 0.75 world update and every player id on the wire: 32 slots */
#define LEGACY_MAX_PLAYERS 32

/* ---- the connection and the join (Join075.c) ---- */

/* a 0.75/0.76 client was accepted (connect data 3 or 4) */
void legacy_on_connect(server_t* server, player_t* player, uint32_t connect_data);
/* a connected 0.75 client the 0.75 state data reached: JOINING (the team
   screen) and every later state. Bots (conn NULL) never are */
int legacy_past_state_data(const player_t* player);
/* on_player_update for a connected 0.75 client: STATE_STARTING_MAP sends the
   map, the version request and the state data, and moves it to JOINING;
   returns 1 when it handled the tick (nothing else to do this tick) */
int legacy_join_update(server_t* server, player_t* player);

/* ---- the receive side (Dispatch075.c) ---- */

void on_packet_received_075(server_t* server, player_t* player, const uint8_t* data, size_t len);

/* ---- the senders (Wire075.c) ---- */

/* reliable on channel 0, counted; 0 when queued */
int  legacy_send(player_t* receiver, const uint8_t* data, size_t len);
/* to every client past the state data, except one (NULL: nobody) */
void legacy_broadcast(server_t* server, player_t* except, const uint8_t* data, size_t len);

void legacy_send_create_player(server_t* server, player_t* receiver, player_t* child);
void legacy_send_existing_player(server_t* server, player_t* receiver, player_t* existing);
void legacy_send_respawn(server_t* server, player_t* player);
void legacy_send_state_data(server_t* server, player_t* player);
void legacy_send_world_update(server_t* server, player_t* player);
void legacy_send_set_tool(server_t* server, player_t* player, uint8_t tool);
void legacy_send_set_color(server_t* server, player_t* player, color_t color);
void legacy_send_weapon_input(server_t* server, player_t* player, uint8_t input);
void legacy_send_input_data(server_t* server, player_t* player);
/* start_animation 1: the player started a reload, the others are told (with
   its clip and reserve); 0: the owner learns its own clip and reserve */
void legacy_send_weapon_reload(server_t* server, player_t* player, uint8_t start_animation);
void legacy_send_position(player_t* player, float x, float y, float z);
void legacy_send_restock(player_t* player);
void legacy_send_fog(server_t* server, player_t* to, color_t color);
void legacy_send_grenade(server_t* server, player_t* thrower, float fuse, vector3f_t position, vector3f_t velocity);
void legacy_send_set_hp(player_t* attacker, player_t* victim, uint8_t type_of_damage, uint8_t is_grenade,
                        vector3f_t position);
void legacy_send_kill_action(server_t* server,
                             player_t* killer,
                             player_t* victim,
                             uint8_t   reason,
                             uint8_t   respawn_time,
                             uint8_t   make_invisible);
void legacy_send_player_left(server_t* server, player_t* player);
void legacy_send_chat(server_t* server, player_t* sender, uint8_t meant_for, const char* message);
/* a server line, one player or everyone in the game: type is the chat type
   (EXT075_CHAT_SYSTEM, or a Message Types one, lowered to SYSTEM for a
   client that did not agree to that extension) */
void legacy_send_notice(player_t* player, uint8_t type, const char* message);
void legacy_broadcast_notice(server_t* server, uint8_t type, const char* message);
/* the intel of team changed hands (CTF's events: pickup, capture, drop) */
void legacy_intel_event(server_t* server, uint8_t team, int event);
/* territory control's two packets (Gamemodes/Tc.c calls them in the 0.75 profile) */
void legacy_tc_progress(server_t* server, int index, int capturing, int rate, float progress);
void legacy_tc_capture(server_t* server, int index, int team, int winning);
/* a GenericCommand the 1.0 bridge would have sent: its 0.75 packet where
   there is one (restock, fog), nothing else */
void legacy_generic(server_t* server, player_t* to, int n, const char* const* parts);

/* ---- the terrain and the tick (Wire075.c) ---- */

/* a map cell changed (proto10_world_cell's dispatch) */
void legacy_world_cell(server_t* server, int x, int y, int z, uint8_t cause);
/* once a tick, after the receive and the bots' work of the last tick: the
   terrain as block actions, the intel and tents that moved */
void legacy_world_tick(server_t* server);

#endif
