// Copyright DarkNeutrino 2021
#ifndef PACKETS_H
#define PACKETS_H

#include <Server/Structs/ServerStruct.h>
#include <Util/DataStream.h>
#include <Util/Line.h>
#include <Util/Queue.h>
#include <Util/Types.h>

/* a gun taken out has been up its raise time (SetTool.c) */
uint8_t player_gun_raised(player_t* player, uint64_t now);
uint8_t allow_shot(server_t*  server,
                   player_t*  player,
                   player_t*  player_hit,
                   uint64_t   time_now,
                   float      distance,
                   long*      x,
                   long*      y,
                   long*      z,
                   vector3f_t shot_pos,
                   vector3f_t shot_orien,
                   vector3f_t hit_pos,
                   vector3f_t shot_eye_pos,
                   uint8_t    last_round); // Remove me from here later

/* The server speaks protocol 1.0 only, so none of these sends a 0.75 packet
   any more. They keep their historical names because they are the gameplay
   API (damage, death, intel, spawn, leave): each applies the change and calls
   its protocol 1.0 hook (Proto10/Bridge.c, Proto10/GameMode.c). */
void send_player_left(server_t* server, player_t* player);
void send_kill_action_packet(server_t* server,
                             player_t* killer,
                             player_t* player,
                             uint8_t   killReason,
                             uint8_t   respawnTime,
                             uint8_t   makeInvisible);
void send_set_hp(server_t*  server,
                 player_t*  player,
                 player_t*  player_hit,
                 long       hp_chnage,
                 uint8_t    type_of_damage,
                 uint8_t    kill_reason,
                 uint8_t    respawn_time,
                 uint8_t    is_grenade,
                 vector3f_t position);
void send_respawn(server_t* server, player_t* respawn_player);

#endif
