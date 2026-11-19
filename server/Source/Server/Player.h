#ifndef PLAYER_H
#define PLAYER_H

#include <Server/Structs/ServerStruct.h>

void    on_new_player_connection(server_t* server, conn_t* conn, uint32_t connect_data);
/* 1 when Bans.json bans this connection's address (and logs it): an IPv4
   one by its IP or range, an IPv6 one by its /64. Expired bans go */
int     player_banned(const conn_t* conn);
struct json_object;
/* Bans.json with its "Bans" array there (made in memory when missing):
   free the root with json_object_put */
struct json_object* bans_open(struct json_object** array);
/* the address to show (logs, plugins' p.ip, the admins' tools): IPv4 or
   IPv6 as the connection came; player->ip is 0.0.0.0 for an IPv6 player */
void    player_address(const player_t* player, char* out, size_t size);
int     player_sort(player_t* a, player_t* b);
void    free_all_players(server_t* server);
void    for_players(server_t* server);
void    on_player_update(server_t* server, player_t* player);
void    init_player(server_t*  server,
                    player_t*  player,
                    uint8_t    reset,
                    uint8_t    disconnect,
                    vector3f_t empty,
                    vector3f_t forward,
                    vector3f_t strafe,
                    vector3f_t height);
void    set_player_respawn_point(server_t* server, player_t* player);
void    player_face_objective(server_t* server, player_t* player);
/* full health, grenades, blocks and ammo, and the client told ("restock":
   a 1.0 client heard of none before, its counters stayed low) */
void    player_restock(server_t* server, player_t* player);
uint8_t player_spawn_near(server_t* server, float x, float y, float z, vector3f_t* out);
/* the body position of a map spawn point (z = the floor block the author
   named): the floor there, or the next one when the ground moved */
uint8_t player_spawn_on_point(server_t* server, vector3f_t point, vector3f_t* out);
uint8_t is_water_column(server_t* server, uint16_t x, uint16_t y);
uint8_t get_player_unstuck(server_t* server, player_t* player);
uint8_t is_staff(server_t* server, player_t* player);
void    update_movement_and_grenades(server_t* server);

#endif
