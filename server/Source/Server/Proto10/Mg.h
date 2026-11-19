// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_MG_H
#define PROTO10_MG_H

/*
 * The machine gun emplacement (docs/protocol-1.0.md 12.6.2), the Commando's
 * streak perk (P10_PERK_MG, it replaced the revive). A world object
 * (Objects.c) of type P10_ENT_MG put down like the sentry (the same cell
 * rules, sentry_cell_ok) and manned by a teammate through the `use`
 * GenericCommand (Mount.c). Its gunner's look turns the barrel within
 * GAME_MG_YAW_ARC of the way it was put down facing and the pitch limits,
 * and the gunner's trigger fires it: a hitscan from the muzzle every
 * GAME_MG_FIRE_INTERVAL, the SMG's damage by the part hit, the SMG's
 * block damage, enemies' sentries, MGs and helicopters too. Kills are the
 * gunner's, kill reason KILL_REASON_MG.
 *
 * It stays where it is whatever its owner does (a death, a class or a team
 * change); it goes when destroyed, when its owner leaves the server and at
 * a map start. The figures are common/mg/mg.h, shared with the client.
 *
 * EVT mg action=place|refused|mount|dismount|fire|damage|destroy.
 */

#include <Server/Proto10/Objects.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <mg/mg.h>

/* [1.0] kill reason after the sentry's (7) and the explosives' (8): shot by
   a manned MG, the gunner credited */
#define KILL_REASON_MG 9

/* places the owner's MG on the floor block (x, y, z); NULL and *why when
   refused (the sentry's reasons, plus already_have_one and mode) */
p10_object_t* mg_place(server_t* server, player_t* owner, int x, int y, int z, const char** why);
/* where its gunner's feet are: behind the barrel, on the MG's floor */
vector3f_t mg_gunner_feet(const p10_object_t* mg);

#endif
