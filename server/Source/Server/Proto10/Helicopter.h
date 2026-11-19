// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_HELICOPTER_H
#define PROTO10_HELICOPTER_H

/*
 * The helicopter (docs/protocol-1.0.md 12.8): two per team at their base on a
 * 1024 x 1024 map, a pilot and two passengers, and nobody fires from it. It
 * is a world object (Objects.c) of type P10_ENT_HELICOPTER with no owner:
 * its flight is common/vehicles/heli.h, run every tick with the pilot's
 * keys, and the pilot's look for its heading. Bullets, the MG, rockets,
 * grenades and explosives damage it through the sentry's path; at 0 health
 * it is a wreck that falls and blows up, killing whoever is still in it
 * (kill reason KILL_REASON_EXPLOSIVE, the last attacker credited). A new one
 * stands at the base GAME_HELI_RESPAWN seconds later.
 *
 * One left with nobody at the stick falls, and a landing hard enough to hurt
 * (heli_impact) blows it up, whoever is aboard with it. Its hull is sampled
 * cell by cell against the map; a wall, a beam or a ceiling stops it and a
 * strike over GAME_HELI_SAFE_LANDING hurts like a landing does.
 *
 * The client draws it from its Linear trajectory (the skids' bottom middle,
 * blocks a second) and its rotation (yaw, tilt forward, tilt side).
 *
 * EVT heli action=spawn|mount|dismount|damage|wreck|respawn|crash|base
 */

#include <Server/Proto10/Objects.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

/* a new map: the helicopters of a 1024 map, at their teams' bases */
void heli_map_start(server_t* server);
/* a plugin's helicopter: one in front of the player, on dry ground with room
   for its hull, not a base slot (it does not respawn); NULL and why not */
p10_object_t* heli_spawn_in_front(server_t* server, player_t* player, const char** why);
/* every tick: the occupants' 60 Hz updates, a wreck's respawn */
void heli_tick(server_t* server);
/* the eye of one seat, in map coordinates (0 the pilot) */
void heli_seat_eye(const p10_object_t* heli, int seat, vector3f_t* out);
/* 1 when it can still be boarded (not a wreck, health left) */
int heli_boardable(const p10_object_t* heli);

#endif
