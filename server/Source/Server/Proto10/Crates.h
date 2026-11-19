// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_CRATES_H
#define PROTO10_CRATES_H

/*
 * Supply crates (Ben Aksoy's Ace of Spades 1.0 alpha had
 * AMMO_CRATE and HEALTH_CRATE entities). A kill drops one by the mode's
 * crate rule (gamemode_ops_t.crate_rule: the zombie mode's, for the humans'
 * kills, 12 to 24 blocks away, one every 20 s at most), else, only in a
 * match with the classes (GM_FEAT_CLASSES), when made among more than 3
 * players (within CRATE_CROWD_R of the killer, both teams, bots too): 15%
 * of the time, within 8 blocks; ammo or health at even odds. It
 * falls from the sky (a Linear trajectory) and lands (Constant); the clients
 * draw it hovering and turning. A landed crate whose support block is taken
 * (dug, shot, blown up or collapsed) falls again, from where it is to the
 * first solid cell below, and a block built back under a falling one stops
 * it there (crates_cell_changed, from proto10_world_cell: EVT crate
 * action=fall). The first player in it who needs what it
 * holds takes it: ammo fills the reserves (both guns, the drill), health
 * fills the health; a GenericCommand "crate ammo|health" tells the taker's
 * client. Gone when taken or after CRATE_LIFETIME (2 minutes). The admins'
 * /crate and the plugins' p:drop_crate / game.drop_crate drop one on demand
 * (a reward). EVT crate action=drop|fall|land|take|taken|expire|drop_skipped kind= by=.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <stdint.h>

/* drop one of type (P10_ENT_AMMO_CRATE, P10_ENT_HEALTH_CRATE) over a dry
   column within scatter blocks of x, y (0: right there, or the nearest dry
   one); why names who asked (kill, command, plugin). 1 when it falls; 0 with
   CRATE_MAX in the air or on the ground, or no ground there. Any mode on
   the 1.0 profile: the classes' rule is only the kills' (crates_on_kill) */
int      crates_drop(server_t* server, float x, float y, float scatter, uint8_t type, const char* why);
/* "ammo" or "health" into a type, 0 for anything else */
uint8_t  crates_type(const char* word);
/* a death: maybe a crate (proto10_bridge_kill) */
void     crates_on_kill(server_t* server, player_t* killer, player_t* victim);
/* every tick: fall, land, be taken, expire */
void     crates_tick(server_t* server);
/* a map cell changed (proto10_world_cell): a crate on it falls, or stops */
void     crates_cell_changed(server_t* server, int x, int y, int z, int solid);
/* a new map: none left */
void     crates_reset(void);
/* GameStateFinal */
uint32_t crates_count(void);
void     crates_put(p10_writer* w);

#endif
