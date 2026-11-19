// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_ENTITIES_H
#define PROTO10_ENTITIES_H

/*
 * Non-player entities seen from protocol 1.0 (docs/protocol-1.0.md 8.1, 8.5,
 * 12.2.6). Milestone 6: grenades. The grenade itself stays the 0.75
 * grenade_t on its thrower's list (fuse, physics, damage are the 0.75 code);
 * this file only announces it: EntityUpdate Create with a Gravity
 * trajectory when it is thrown, then a final position, EntityEvent Explode
 * and EntityRemove when it goes off. Entity ids start at P10_FIRST_ENTITY_ID
 * so they never collide with player ids (0..255).
 */

#include <Server/Structs/GrenadeStruct.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <stddef.h>
#include <stdint.h>

#define P10_FIRST_ENTITY_ID 256u

/* a grenade was appended to player->grenade by game_player_throw_grenade */
void proto10_entities_grenade_thrown(server_t* server, player_t* player, grenade_t* grenade);
/* the fuse ran out: called before the grenade is freed */
void proto10_entities_grenade_exploded(server_t* server, player_t* player, grenade_t* grenade);
/* the grenade is freed without exploding (player reset) */
void proto10_entities_grenade_removed(server_t* server, player_t* player, grenade_t* grenade);
/* a fresh id for a short lived entity (a grenade, a rocket): one allocator,
   so the two never share an id */
uint32_t proto10_entities_alloc_id(void);
/* GameStateFinal: number of live grenade entities, and their full items */
uint32_t proto10_entities_count(server_t* server);
void     proto10_entities_put(server_t* server, p10_writer* w);
/* a ThrowGrenade from a 1.0 client, replayed as the 0.75 grenade packet;
   returns 0 when malformed */
int proto10_entities_on_throw(server_t* server, player_t* player, const uint8_t* data, size_t size);

#endif
