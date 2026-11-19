// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The server's internal game API: what a player does, in the game's words.
 *
 * There is one game (the 0.75 rules it inherited from SpadesX). Two wires
 * drive it: the 0.75 dispatcher (Legacy/Dispatch075.c, which parses a 0.75
 * packet and calls the matching receive_* in Packets/) and the 1.0 bridge
 * (Proto10/), which decodes a 1.0 packet and calls these functions directly.
 * The bots call these too, like the players they are, instead of assembling
 * packets no socket ever sends.
 *
 * A function here validates and applies, exactly as the 0.75 handler of the
 * same name always did: the rules (rate limits, respawn state, team rules,
 * plugins, anticheat) live here, never in a wire. Its receive_* is a parse
 * front end only: it reads the 0.75 fields and calls this, so a 0.75 client
 * and a 1.0 client cannot drift apart.
 *
 * A new operation is one function here, implemented beside its 0.75 parse
 * (Packets/) or its own module, and called from both wires and the bots.
 * Never add a second code path for a second wire.
 */
#ifndef GAMEAPI_H
#define GAMEAPI_H

#include <Server/Server.h>
#include <Util/Enums.h>

/* ---- movement and look ---- */

/* the body's position, in map coordinates (z grows downwards); a position
   the server refuses is dropped and the 0.75 wire is sent the one it has */
void game_player_move(server_t* server, player_t* player, vector3f_t position);

/* where the body looks: the forward vector, normalised when longer than 1 */
void game_player_look(server_t* server, player_t* player, vector3f_t forward);

/* the movement keys, one bit each: forward, back, left, right, jump,
   crouch, sneak, sprint (Util/Enums.h's input bits) */
void game_player_move_input(server_t* server, player_t* player, uint8_t keys);

/* the trigger: bit 0 the primary fire, bit 1 the secondary. Firing spends
   the gun's round and starts the spade's or the block's timer */
void game_player_fire_input(server_t* server, player_t* player, uint8_t buttons);

/* ---- the tool in hand ---- */

void game_player_set_tool(server_t* server, player_t* player, uint8_t tool);
void game_player_set_color(server_t* server, player_t* player, color_t color);
void game_player_reload(server_t* server, player_t* player);

/* ---- joining, teams and weapons ---- */

/* an announced player: its team, gun, tool in hand and name. The name has
   already been cut to PLAYER_NAME_STRLEN characters by the caller */
void game_player_join(server_t*   server,
                      player_t*   player,
                      uint8_t     team,
                      uint8_t     weapon,
                      uint8_t     tool,
                      uint32_t    kills,
                      const char* raw_name);

void game_player_change_team(server_t* server, player_t* player, uint8_t team);
void game_player_change_weapon(server_t* server, player_t* player, uint8_t weapon);

/* ---- combat ---- */

/* a hit on a body: the victim's id and HIT_TYPE_* from Util/Enums.h */
void game_player_hit(server_t* server, player_t* player, uint8_t target, uint8_t hit_type);

/* a grenade leaves the hand: its fuse in seconds (capped at 3), where it
   starts and the velocity the client gave it (blocks per 1/32 s) */
void game_player_throw_grenade(server_t* server,
                               player_t* player,
                               float      fuse,
                               vector3f_t position,
                               vector3f_t velocity);

/* ---- building ---- */

/* BLOCKACTION_* (Util/Enums.h): 0 build, 1 destroy one, 2 destroy three */
void game_player_block_action(server_t* server,
                              player_t* player,
                              uint8_t   action,
                              uint32_t  x,
                              uint32_t  y,
                              uint32_t  z);

/* a block line drawn by a block in hand, both ends included */
void game_player_block_line(server_t* server, player_t* player, vector3i_t from, vector3i_t to);

/* ---- chat ---- */

/* channel 0 global, 1 team, 2 system; the text is CP437, as the server's
   commands and translations read it. Commands ("/kill") come through here
   too, from either wire */
void game_player_chat(server_t* server, player_t* player, int channel, const char* text);

#endif
