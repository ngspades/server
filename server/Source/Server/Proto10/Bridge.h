// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_BRIDGE_H
#define PROTO10_BRIDGE_H

/*
 * Protocol 1.0 view of the one game the server runs (docs/protocol-1.0.md
 * 8 and 13.1). Every player_t, whatever its protocol (bots included), is a
 * 1.0 player record and, while alive, a Player entity whose id is its player
 * id. Inputs from 1.0 clients are replayed through the 0.75 receive
 * handlers, so both protocols share the same validation and side effects.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <stddef.h>
#include <stdint.h>

/* once per server tick: broadcast player and entity deltas to 1.0 clients */
void proto10_bridge_tick(server_t* server);
/* 1 when this tick sends to clients: always, but with a sped up clock
   (SP_TIME_SCALE) 60 times a real second; set by proto10_bridge_tick */
extern uint8_t proto10_net_frame;
/* before a player slot is freed: tell 1.0 clients it is gone */
void proto10_bridge_player_left(server_t* server, player_t* player);
/* the 1.0 flashlight goes off (death, spawn, team change, leave, map):
   the clients do it themselves too, this keeps the server's copy right */
void proto10_bridge_light_off(player_t* player, const char* why);
void proto10_bridge_lights_to(server_t* server, player_t* to);
/* every known "device" (scoreboard icons), to a 1.0 client entering the game */
void proto10_bridge_devices_to(server_t* server, player_t* to);
/* GameStateFinal tail: entity count + items, player count + items, all full;
   the receiver's protocol revision decides which bits it may see */
void proto10_bridge_put_snapshot(server_t* server, p10_writer* w, player_t* receiver);
/* a death applied by send_kill_action_packet: 1.0 clients get
   EntityDie (param = reason | respawn << 8, docs/protocol-1.0.md 8.9) */
void proto10_bridge_kill(server_t* server, player_t* killer, player_t* victim, uint8_t reason, uint8_t respawn);
/* "hit-protected <victim>" to the shooter: its shot met a body the spawn
   protection kept from harm (it read as a shot that did nothing) */
void proto10_bridge_hit_protected(player_t* attacker, player_t* victim);
/* damage applied by send_set_hp: a 1.0 victim gets Damage, a 1.0 attacker
   gets the "hit" GenericCommand (hit marker); head = headshot */
void proto10_bridge_damage(server_t*  server,
                           player_t*  attacker,
                           player_t*  victim,
                           long       amount,
                           uint8_t    type_of_damage,
                           uint8_t    is_grenade,
                           uint8_t    kill_reason, /* the 0.75 one: 1 headshot, 2 melee */
                           vector3f_t source);
/* a reload started by game_player_reload: every other 1.0 client gets
   EntityEvent ReloadWeapon on that player (the sound, its clip refilled) */
void proto10_bridge_reload(server_t* server, player_t* player);
/* the body changed in place (the arena's countdown picks a gun): every 1.0
   client takes it again, so its Create carries the new weapon and skin */
void proto10_bridge_body_recreate(player_t* player);
/* a jump the physics just made: EntityEvent Jump to the other 1.0 clients */
void proto10_bridge_jump(server_t* server, player_t* player);
/* a GenericCommand of text parts to one player, or to every 1.0 client in
   the game when `to` is NULL: what plugins tell clients (restock, fog) */
void proto10_bridge_generic(server_t* server, player_t* to, int n, const char* const* parts);
/* the block wallet the server keeps, to its owner: protocol 1.0 carries
   no block count anywhere else, and the client's prediction of a dig's
   refund (the Miner starts at 0) never drifted from the server again */
void proto10_bridge_wallet(server_t* server, player_t* player);
/* the mode marks a body for a team (the arena's last survivors): the mark
   follows it and keeps its updates coming, without a sight test or a rate;
   every 1.0 client of that team is told, and nothing goes anywhere else */
void proto10_bridge_spot_mark(server_t* server, player_t* target, uint8_t team);
/* a bot switches its flashlight (bots_lights): 1 when it changed */
int proto10_bridge_bot_light(server_t* server, player_t* bot, int on, const char* why);
/* the server moved this player (a plugin's teleport): its client takes the
   new trajectory, or its next report, closer than 6 blocks, put it back */
void proto10_bridge_teleport(player_t* player);
/* gameplay packets of a 1.0 client in the game; returns 0 when malformed */
int proto10_bridge_on_packet(server_t* server, player_t* player, uint8_t type, const uint8_t* data, size_t size);

#endif
