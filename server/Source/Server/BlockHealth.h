// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BLOCKHEALTH_H
#define BLOCKHEALTH_H

/*
 * Block health (protocol 1.0): a gun's hit on a
 * block takes its damage (the class's block_damage or the gun's,
 * game_weapon_block_damage) from the cell's health, GAME_BLOCK_BREAK, and
 * the block breaks only at zero. A hit that does not break the block is
 * queued as a TerrainUpdate edit (proto10_world_cell) and goes out in the
 * tick's TerrainUpdate as the cell's health (100 - the damage taken), so
 * everyone sees the crack. A cell is forgotten (whole again) when nobody
 * hit it for
 * GAME_BLOCK_HEAL_SECONDS, the time the client stops drawing a crack, when
 * the map changes there (proto10_world_cell) and when a map loads.
 *
 * Only the tick touches it. The bots' planner (BotPlanner.c, its own
 * thread) never reads it: to the bots a block is whole until it breaks.
 * The 0.75 profile does not use it: there the client counts, as in 0.75.
 */

#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

/* map_load: a new map has no cracks */
void block_health_reset(void);
/* a hit of `damage` on the cell (map coordinates); returns the damage the
   block has now taken, at most GAME_BLOCK_BREAK (it breaks there) */
int block_health_hit(server_t* server, int x, int y, int z, int damage);
/* what block_health_hit would return, changing nothing: a hit that would
   break the block asks the plugins first (on_destroy is a destroy) */
int block_health_peek(server_t* server, int x, int y, int z, int damage);
/* the damage the cell has now (0 when it has none, healed or another
   colour): the TerrainUpdate edit's health is GAME_BLOCK_BREAK - this */
int block_health_taken(server_t* server, int x, int y, int z);
/* proto10_world_cell: the cell changed, any crack it had is gone */
void block_health_forget(server_t* server, int x, int y, int z);
/* counts the hits that damaged a block without breaking it: the HitTerrain
   handler (Proto10/World.c) tells such a hit from a refusal by it */
uint32_t block_health_serial(void);

#endif
