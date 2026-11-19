// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_EXPLOSIVES_H
#define PROTO10_EXPLOSIVES_H

/*
 * The classes' placed perks (docs/protocol-1.0.md 12.6), objects of the
 * sentry's registry (Objects.c), placed like it (PlayerAction
 * P10_EV_USE_PERK on a floor cell, sentry_cell_ok's rules):
 *
 *   the landmine (Marksman, P10_ENT_LANDMINE): armed LANDMINE_ARM_SECONDS
 *   after it is put down, it blows up when an enemy's feet come within
 *   LANDMINE_TRIGGER blocks of it: a grenade's blast and a 3 x 3 x 3 crater
 *
 *   the dynamite (Miner, P10_ENT_DYNAMITE): blows up GAME_DYNAMITE_FUSE seconds
 *   after it is put down: a grenade's blast times DYNAMITE_DAMAGE_SCALE and
 *   a 5 x 5 x 5 crater
 *
 * Both are shot like the sentry; at no health they blow up (a tick later,
 * so blasts chain a tick apart). Kills are their owner's with
 * KILL_REASON_EXPLOSIVE. While the owner is dead a landmine rests (it would
 * hurt nobody: damage needs a living attacker), the dynamite's fuse runs.
 * EVT landmine|dynamite action=place|trigger|explode.
 */

#include <Server/Proto10/Objects.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <explosives/explosives.h>

#define KILL_REASON_EXPLOSIVE 8 /* after KILL_REASON_SENTRY (7): no perk progress */

#define LANDMINE_HEALTH       30
#define LANDMINE_ARM_SECONDS  1.5f
#define LANDMINE_TRIGGER      1.5f
#define LANDMINE_MAX          2 /* a player's mines down at once */
#define DYNAMITE_HEALTH       60
#define DYNAMITE_DAMAGE_SCALE 1.5f

/* 1 while the player may put down another of that perk's (the landmine:
   LANDMINE_MAX, the dynamite: one at a time) */
int explosives_room(const player_t* owner, int perk);
/* perk: P10_PERK_LANDMINE or P10_PERK_DYNAMITE; NULL and *why when refused */
p10_object_t* explosives_place(server_t* server, player_t* owner, int perk, int x, int y, int z, const char** why);

#endif
