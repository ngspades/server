// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CLASSES_H
#define CLASSES_H

/*
 * The classes of ngCTF (common/classes/classes.h: Commando, Rocketeer,
 * Marksman, Miner), where the mode has GM_FEAT_CLASSES. Each player has
 * one (player->klass, Commando by default) that decides its gun, its
 * health, speed and jump, its blocks and its gear. In a mode without
 * classes every function here leaves the 0.75 rules as they were.
 *
 * Wire (docs/protocol-1.0.md): "classes 1" and every "class <player> <n>"
 * to a 1.0 client entering the game, "class <player> <n>" to everyone on a
 * change; the client asks with "class <n>" or a fourth part of "join".
 * Logged as EVT class action=pick|spawn|refused why=..
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <classes/classes.h>

/* every map starts from config.toml's figures ([classes.<name>], read
   once; a plugin's changes last the map): gamemode_init, before the
   plugins */
void classes_map_start(server_t* server);
/* one figure of class k from its text (game_class_set's keys and ranges),
   told to every 1.0 client ("class-info"); 0 when refused (logged), the
   last class enabled is never disabled. by: "plugin", "config" */
int classes_set(server_t* server, int k, const char* key, const char* value, const char* by);
/* the gun in hand and the second one swap (a client's tool slot, a bot's
   choice): clip and reserve go with each; the tool slot of the body's
   entity tells everyone. 0 when refused (no second gun, dead, rate) */
int classes_swap_gun(server_t* server, player_t* player, const char* by);
/* picks which of the class's guns is in hand (1.0's Weapon1 / Weapon2); a
   swap only when it is the other one. 0 when refused */
int classes_select_gun(server_t* server, player_t* player, int slot, const char* by);
/* the first class enabled (a join or a spawn in a class turned off) */
int classes_fallback(void);

/* the player's class, NULL when the mode has no classes */
const struct game_class_info* classes_of(server_t* server, player_t* player);
/* the mode has classes */
int classes_on(server_t* server);
/* what a spawn or a restock gives: the class's, else the usual 50 and 3 */
int classes_blocks(server_t* server, player_t* player);
/* the block wallet's size: what a restock fills to and a dig gives back up
   to (the class's blocks_max; the classic 50 without a class) */
int classes_blocks_max(server_t* server, player_t* player);
int classes_grenades(server_t* server, player_t* player);
/* a landing's fall damage, by the distance flown down (the class's curve in
   classes.h): -1 when the player has no class (the classic landing speed
   decides), 0 landed without damage, >0 the damage. It zeroes the fall. */
long classes_fall_damage(server_t* server, player_t* player);
/* the damage of the player's spade (or pickaxe) swing; 0 without a class
   (the classic 80) */
int classes_melee(server_t* server, player_t* player);
/* the gun of a class (WEAPON_* or GAME_WEAPON_ROCKET) */
uint8_t classes_weapon(int klass);
/* the one streak perk (P10_PERK_*) the player's class may earn, -1 when the
   mode has no classes (every perk of the mode then) */
int classes_perk(server_t* server, player_t* player);
/* the class wears the jetpack from every spawn */
int classes_jetpack(server_t* server, player_t* player);
/* picks a class (the "class" command, the join's fourth part, a bot's
   choice): a living player respawns with it, like a weapon change. by:
   "client", "join", "bot", "plugin". 0 when refused: a class turned off
   (why=disabled), or a plugin's on_class said no (why=plugin) */
int classes_pick(server_t* server, player_t* player, int klass, const char* by);
/* a player spawns: its class's gun, blocks and gear */
void classes_on_spawn(server_t* server, player_t* player);
/* every physics tick: the class's walk speed, sprint, water drag and jump
   (Util/Physics.c) */
void classes_movement(server_t* server, player_t* player);
/* the spade's dig: 3 for the Miner's 3 x 3 x 3 (Block.c), 1 the usual */
int classes_dig(server_t* server, player_t* player);
/* nanoseconds between two right button digs (the class's dig_delay; the
   usual THREEBLOCK_DELAY without classes): BlockChecks.c and the bots */
uint64_t classes_dig_delay(server_t* server, player_t* player);
/* how many blocks one shot of the player's gun may break: its pellets times
   the class's block_damage over a block's GAME_BLOCK_BREAK, at least 1 (the
   Miner's shotgun: 8, a pellet a block); 1 without a class or a figure */
int classes_gun_breaks(server_t* server, player_t* player);
/* what a pellet of the player's gun does to a block (block health): the
   class's block_damage, else the gun's own (game_weapon_block_damage) */
int classes_block_damage(server_t* server, player_t* player);
/* a cell a dig or the drill may clear: in the map, above the water layer,
   solid, and editable for the mode and the map (gamemode_cell_editable) */
int classes_cell_diggable(server_t* server, int x, int y, int z);
/* check_node on the shell of the cube of radius r round (x, y, z): what
   hung on the cleared cube falls */
void classes_check_shell(server_t* server, int x, int y, int z, int r);
/* a restock (the tent): the drill's charges back too */
void classes_restock(server_t* server, player_t* player);
/* the Miner's drill (the GenericCommand "drill [dx dy dz]"): a drill bit
   fired from the eye along dir (the look when dir is zero), which
   Proto10/Drills.c flies and bores with. 1 when fired, 0 refused (EVT class
   action=drill_refused why=, "drill-refused <why>" to the client) */
int classes_drill(server_t* server, player_t* player, vector3f_t dir, const char* by);

#define DRILL_INTERVAL 1.0f /* seconds between two */
#define DRILL_BASE_R   20.0f /* no drill this close to its own tent or intel */

/* whether x, y is within DRILL_BASE_R of the team's own tent, or of its
   intel where it lies: no drill fires there and a bit stops at its edge */
int classes_drill_base(server_t* server, uint8_t team, float x, float y);

/* gamemode hooks of a classes mode (ngCTF chains them) */
void classes_client_in_game(server_t* server, player_t* player);
int  classes_command(server_t* server, player_t* player, const char* command, const char* arg);
long classes_hit_damage(server_t* server, player_t* attacker, player_t* victim, long damage, uint8_t kill_reason);
/* tells every 1.0 client the classes are on (or off): the host turned them
   on or off in the middle of a match */
void classes_tell_all(server_t* server, int on);

#endif
