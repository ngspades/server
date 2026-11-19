// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LEGACY_EXT075_H
#define LEGACY_EXT075_H

/*
 * The 0.75 protocol extensions on the 0.75 server
 * (docs/plan-075-server.md section 7, the aosprotocol specification:
 * https://github.com/zerospades/aosprotocol): ExtInfo (60), Player
 * Properties (64), Damage Markers (96), Teamplay (112), Flashlight (114),
 * the packetless Player Limit, Message Types and Kick Reason, UTF-8 chat
 * (the 0xFF prefix) and the Extra Packets' version handshake and version
 * get (31/32, 33/34, and the extended answer on packet 9). Only in the 0.75
 * profile, and each packet only to the players that agreed to its
 * extension. The 0.75 wire (Dispatch075.c, Join075.c, Wire075.c) and
 * Util/Notice.c call these; nothing else does.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/DataStream.h>
#include <tomlc99/toml.h>

/* the extension ids (a packet at 0x40 + id below 192, none above) */
#define EXT075_PLAYER_PROPERTIES 0x00
#define EXT075_DAMAGE_MARKERS    0x20
#define EXT075_TEAMPLAY          0x30
#define EXT075_FLASHLIGHT        0x32
#define EXT075_DAYTIME           0x33
#define EXT075_PLAYER_LIMIT      0xC0
#define EXT075_MESSAGE_TYPES     0xC1
#define EXT075_KICK_REASON       0xC2

/* the packets of the Extra Packets and of the Flashlight */
#define EXT075_PACKET_HANDSHAKE_INIT     31
#define EXT075_PACKET_HANDSHAKE_RESPONSE 32
#define EXT075_PACKET_FLASHLIGHT         (0x40 + EXT075_FLASHLIGHT)
#define EXT075_PACKET_DAYTIME            (0x40 + EXT075_DAYTIME)
/* the extended version answer reuses ExistingPlayer (9), its first byte 'x' */
#define EXT075_VERSION_EXT_MARKER 'x'

/* the chat types: the base protocol's three, then Message Types' four */
#define EXT075_CHAT_ALL     0
#define EXT075_CHAT_TEAM    1
#define EXT075_CHAT_SYSTEM  2
#define EXT075_CHAT_BIG     3
#define EXT075_CHAT_INFO    4
#define EXT075_CHAT_WARNING 5
#define EXT075_CHAT_ERROR   6
/* the sender of a server line that is no player's (Kick Reason) */
#define EXT075_SERVER_ID 255

/* [ext075] of the profile's config; called once from main after the read */
void ext075_configure(toml_table_t* root);
/* a 0.75 client finished the state data: offer ExtInfo, then Config */
void ext075_on_join(server_t* server, player_t* player);
/* the client's ExtInfo answer (packet 60) and its Teamplay packet (112),
   the stream at the byte after the packet id */
void ext075_receive_info(server_t* server, player_t* player, stream_t* data);
void ext075_receive_teamplay(server_t* server, player_t* player, stream_t* data);
/* a hit applied (send_set_hp's path): amount > 0 damage, < 0 a heal;
   attacker NULL for a fall, the water, the boundary */
void ext075_on_damage(server_t* server, player_t* attacker, player_t* victim, int amount);
/* the player's health, blocks, grenades, ammo or score may have changed */
void ext075_player_changed(server_t* server, player_t* player);
/* the slot is free again: forget what it agreed */
void ext075_player_left(server_t* server, uint8_t id);
/* once a tick: Player Properties pacing, ping expiry */
void ext075_tick(server_t* server);

/* the player agreed to that extension (EXT075_*) on this connection */
int ext075_agreed(const player_t* player, uint8_t ext);
/* the server's own Teamplay Ping (player 255) for a team (TEAM_A, TEAM_B,
   or TEAM_SPECTATOR for everyone): map coordinates (z down), life in
   seconds (0 removes it, INFINITY lasts), colour, a label of at most 64
   bytes. Players that did not agree get nothing */
void ext075_server_ping(server_t* server, uint8_t team, float x, float y, float z, float life, color_t color,
                        const char* label);
/* the Teamplay ESP Mark: target shown through the walls to a team (as
   above) for life seconds, its name over it when show_name */
void ext075_mark(server_t* server, uint8_t team, const player_t* target, float life, uint8_t show_name,
                 color_t color, const char* label);

/* ---- Flashlight (0x32, packet 114) ---- */

/* the client's Light request (packet 114, the stream past the id) */
void ext075_receive_flashlight(server_t* server, player_t* player, stream_t* data);
/* switch a player's light (the server's own decision, or an accepted
   request): every client that agreed is told; 1 when it changed */
int ext075_set_light(server_t* server, player_t* player, int on, const char* why);
/* Daytime and Weather (0x33): night or day for every client that agreed
   ([ext075] night, /night, a plugin later); 1 when it changed */
int ext075_set_night(server_t* server, int night, const char* why);
int ext075_night(void);
/* the light goes off without a word (Create Player, Kill Action, Player
   Left: every client turns it off itself) */
void ext075_light_off(uint8_t id);
/* a new map: every light off (the clients do it on Map Start) */
void ext075_map_start(server_t* server);
/* the player's light is on */
int ext075_light_on(uint8_t id);

/* ---- Chat: Message Types, Kick Reason, UTF-8 ---- */

/* a Chat Message (17) for receiver into buf (cap bytes, at least 8): type
   is EXT075_CHAT_*, lowered to SYSTEM for a client without Message Types;
   text is CP437 (the server's own), sent as UTF-8 behind 0xFF to a client
   that reads it when it holds a character beyond ASCII. with_nul adds the
   NUL the original chat relay carried. Returns the length */
size_t ext075_chat_packet(uint8_t*        buf,
                          size_t          cap,
                          const player_t* receiver,
                          uint8_t         sender,
                          uint8_t         type,
                          const char*     text,
                          int             with_nul);
/* the receiver reads UTF-8 chat (its version answer said so, it sent a
   0xFF line itself, or it is OpenSpades) */
int ext075_utf8(const player_t* player);
/* a UTF-8 line (after its 0xFF) arrives: the CP437 the server handles is
   written to cp437 (cap bytes) and the original kept, so the relay of that
   same line gives UTF-8 readers the characters CP437 lacks; done ends it */
void ext075_chat_utf8_begin(player_t* sender, const uint8_t* utf8, size_t len, char* cp437, size_t cap);
void ext075_chat_utf8_done(void);
/* Kick Reason: the line from player 255 before the disconnect, only to a
   client that agreed; 1 when it was sent (the disconnect must then let it
   go out first) */
int ext075_kick_reason(player_t* player, const char* text);

/* ---- Extra Packets: version handshake (31/32), version get (33/34, 9) ---- */

/* the client answered the plain Version Get (34): it speaks the Extra
   Packets, so it is sent the handshake and the extended Version Get */
void ext075_on_version(server_t* server, player_t* player);
/* Handshake Response (32) and the extended version answer (9, 'x'), the
   stream past the packet id */
void ext075_receive_handshake(server_t* server, player_t* player, stream_t* data);
void ext075_receive_version_ext(server_t* server, player_t* player, stream_t* data);

#endif
