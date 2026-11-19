#ifndef WEAPONCHECKS_H
#define WEAPONCHECKS_H

#include <Server/Structs/CommandStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

uint8_t block_action_weapon_checks(server_t* server, player_t* player, uint64_t time_now);
/* the same checks for protocol 1.0, where every shot that meets a block is
   sent (block health): the gun's rate is kept on average, with some slack
   for the network's jitter */
uint8_t block_action_weapon_checks_paced(server_t* server, player_t* player, uint64_t time_now);

#endif
