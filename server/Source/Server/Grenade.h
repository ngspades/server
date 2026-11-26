#ifndef GRENADE_H
#define GRENADE_H

#include <Server/Structs/ServerStruct.h>

vector3i_t* getGrenadeNeighbors(vector3i_t pos);
uint8_t     get_grenade_damage(server_t* server, player_t* damaged_player, grenade_t* grenade);
void        handle_grenade(server_t* server, player_t* player);
/* every grenade in flight is removed (and the 1.0 clients told): a round
   mode calls it where a round ends or starts, so one thrown before the
   change never explodes in the next round. A thrower's death does NOT
   call it: a grenade already thrown still goes off (handle_grenade) */
void        grenades_clear_all(server_t* server);

/* get_grenade_damage at a point: 4096 / d^2 in sight, capped at 100 */
uint8_t explosion_damage_at(server_t* server, player_t* damaged_player, vector3f_t at);
/* a blast at pos like a grenade's (the landmine, the dynamite, the rocket):
   every player in sight takes explosion_damage_at times damage_scale from
   attacker (times self_scale for the attacker itself; kill_reason, the
   damage source object set by the caller), the objects too, and the cube of
   cells within crater (1: 3 x 3 x 3, 2: 5 x 5 x 5) round pos is cleared,
   cell by cell where the mode allows it, through proto10_world_cell.
   Returns the cells cleared. EVT explosion */
int explosion_blast(server_t*  server,
                    player_t*  attacker,
                    vector3f_t pos,
                    int        crater,
                    float      damage_scale,
                    float      self_scale,
                    uint8_t    kill_reason,
                    const char* cause);

/* the same with BattleSpades' geometry: (R^2 - d^2) / R^2 to the body
   centre, times the weighted sight rays' fraction (the retail rocket: peak
   140, radius 6) */
int explosion_blast_retail(server_t*  server,
                           player_t*  attacker,
                           vector3f_t pos,
                           int        crater,
                           float      peak,
                           float      radius,
                           float      self_scale,
                           uint8_t    kill_reason,
                           const char* cause);

#endif
