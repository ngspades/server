// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAPMETA_H
#define MAPMETA_H

/*
 * The pyspades map metadata (Resources/maps/<Name>/<Name>.txt, from
 * aos.party). The file is a Python script: nothing is executed. The
 * declarative parts (name, author, extensions, fog, protected sectors) are
 * read as Python literals, and the common script idioms are recognised by
 * their shape: fixed intel and tent cells (get_entity_location), spawn point
 * lists and rectangles (get_spawn_location), protected coordinates
 * (on_block_destroy / on_block_build_attempt / is_indestructable). Whatever
 * else the script defines is named in one warning at load.
 *
 * What the author wrote here wins over the map's .toml for the same thing;
 * the .toml keeps our own extras (capture limit, arena timings, ...).
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Types.h>
#include <stdint.h>

/* the pyspades entity kinds of get_entity_location, per team */
#define MAP_META_FLAG 0 /* the team's intel */
#define MAP_META_BASE 1 /* the team's tent */

/* parses the .txt next to the map; a missing file only clears the previous
   map's rules. Call it after map_load: get_z() in a script reads the map */
void map_meta_load(server_t* server, const char* path);

/* fog, water damage and spawn rectangles over what the .toml set */
void map_meta_apply(server_t* server);

/* team 0 = blue (pyspades BLUE_*), 1 = green. The cell the author chose,
   settled on the ground the way pyspades settles entities. 0 when the map
   leaves it to the server */
int map_meta_entity(server_t* server, int team, int kind, vector3f_t* out);

/* one of the author's spawn points for the team (cell centre, z = the floor
   block the author named). 0 when the map has no point list */
int map_meta_spawn(server_t* server, int team, vector3f_t* out);

/* how many spawn points the map gives the team (the arena's own list, or
   get_spawn_location's) and one of them by index: a mode that keeps a spawn
   per round (the arena's rotation) picks the slot, a spawn picks at random */
int map_meta_spawn_count(server_t* server, int team);
int map_meta_spawn_from(server_t* server, int team, int index, vector3f_t* out);

/* the arena script's own settings: -1 and 0 when the map does not say */
int   map_meta_arena_spawn_change(server_t* server);
float map_meta_arena_max_spawn_distance(server_t* server);

/* a map rule (protected sector or coordinate) forbids building or digging */
int map_meta_cell_protected(server_t* server, int x, int y, int z);

/* the cells, inclusive, where boundary_damage does not hurt. Returns 0 and
   the whole map when the map has no boundary */
int map_meta_play_area(server_t* server, int* x0, int* y0, int* x1, int* y1);

/* the arena_gates seed cells; 0 when the map names none */
int map_meta_arena_gates(server_t* server, int (*seeds)[3], int max);

/* Babel's platform colour (heavens_color), raw ARGB */
uint32_t map_meta_babel_color(server_t* server, uint32_t fallback);

/* once a second per player: boundary_damage */
void map_meta_player_tick(server_t* server, player_t* player);

#endif
