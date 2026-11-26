// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_OBJECTS_H
#define PROTO10_OBJECTS_H

/*
 * Server-side registry of non-player world entities that live longer than a
 * grenade and can be shot: the sentry gun now, the helicopter later
 * (docs/protocol-1.0.md 12.5). Grenades stay in Entities.c because they are
 * the 0.75 grenade_t; everything here is a 1.0-only object.
 *
 * The registry owns what every such object has in common: an entity id,
 * a type, an owner and a team, health, a box that bullets hit, a position
 * and an aim (sent as a Constant trajectory with a rotation), the per-tick
 * EntityUpdate delta, the GameStateFinal snapshot, EntityEvent/EntityRemove,
 * and damage from players' HitEntity, spades and grenades. What an object
 * does is its type's business: the `think` callback runs every tick.
 *
 * Owned objects disappear when the owner leaves or changes team; the
 * registry checks that every tick, so no join/leave/team hook is needed.
 * The MG stays through its owner's team change (keep_on_team_change) and
 * the helicopter has no owner at all (no_owner, P10_NO_OWNER on the wire).
 * Every object goes at a map start (proto10_objects_map_start).
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <proto10/codec.h>
#include <stddef.h>
#include <stdint.h>
#include <vehicles/heli.h>

/* above the grenade range (256 .. 65791), so the two allocators never meet */
#define P10_FIRST_OBJECT_ID (256u + 65536u)
#define P10_MAX_OBJECTS     64

typedef struct p10_object p10_object_t;

struct p10_object
{
    uint32_t id; /* 0 = free slot */
    uint8_t  type;
    uint8_t  owner_id;
    uint8_t  team; /* TEAM_A, TEAM_B */
    int      health;
    /* map coordinates (z down): the centre of the base, on the ground */
    vector3f_t base;
    float      half_width; /* box: base.xy +- half_width */
    float      height;     /* box: base.z - height .. base.z */
    /* aim, as Player euler angles (x pitch, z yaw), docs 8.5 */
    p10_vec3 euler;
    /* the type's behaviour; think runs every tick while the object lives */
    void (*think)(server_t* server, p10_object_t* obj, uint64_t now);
    uint64_t created;
    uint64_t last_think;
    /* type state */
    uint8_t  target_id;
    uint8_t  has_target;
    uint64_t spot_at; /* the last sentry-spot we sent (at most one per 2 s) */
    float    home_yaw;  /* a sentry's front, its owner's look when placed: it
                           watches and fires only within SENTRY_ARC_HALF of it */
    int8_t   scan_dir;  /* the idle sweep's way: +1 or -1 (0 counts as +1) */
    uint8_t  idle_logged;
    uint64_t next_shot;
    /* an explosive (Explosives.c): health 0 does not remove it, it sets
       detonate and its think blows it up on the next tick (chain blasts
       happen a tick apart, never inside another blast) */
    uint8_t  blast_on_death;
    uint8_t  detonate;
    uint8_t  armed;
    /* one event to send after this tick's EntityUpdate (so a Fire is shown
       with the rotation it was fired at) */
    uint8_t  has_pending_event;
    uint8_t  pending_event;
    uint64_t pending_param;
    /* the MG and the helicopter (Mg.c, Helicopter.c) */
    uint8_t    no_owner;            /* the helicopter: P10_NO_OWNER, no owner checks */
    uint8_t    keep_on_team_change; /* the MG stands whatever its owner does */
    int        max_health;          /* > 0: the wire's health is the percent of it */
    float      half_len;            /* > 0: the box is half_len along box_yaw, half_width across */
    float      box_yaw;             /* radians, the long side's heading in map x/y (0 = +x) */
    uint8_t    linear;              /* a Linear trajectory with vel (blocks/s) */
    vector3f_t vel;
    uint8_t    has_rotation;        /* rotation sent as is (the helicopter's roll), else from euler */
    p10_vec3   rotation;
    float      recoil_x, recoil_z;  /* the MG barrel's kick (radians), easing back */
    float      mg_heat;             /* the MG barrel's heat, 0..1 (common/mg/mg.h) */
    uint8_t    mg_locked;           /* overheated: no fire until it cooled to GAME_MG_HEAT_RESUME */
    int16_t    mg_heat_sent;        /* the last permille told to the gunner, -1 never */
    uint64_t   mg_heat_at;          /* when */
    uint8_t    last_attacker;       /* the last enemy who hurt it (credited for a wreck) */
    uint8_t    has_last_attacker;
    uint64_t   wreck_at;
    /* the helicopter (Helicopter.c): its whole flight state, which is also
       what the wire's trajectory and rotation are made from, and the flag
       that says 0 health leaves it a falling wreck instead of an explosion */
    heli_state heli;
    uint8_t    keeps_wreck;
    /* network state */
    uint8_t         announced;
    uint8_t         force_trajectory;
    uint64_t        trajectory_sent_at;
    p10_entity_item last;
};

/* NULL when the table is full */
p10_object_t* proto10_objects_spawn(server_t*  server,
                                    uint8_t    type,
                                    player_t*  owner,
                                    vector3f_t base,
                                    int        health,
                                    float      half_width,
                                    float      height);
/* an object of a team with no owner (the helicopter); NULL when full */
p10_object_t* proto10_objects_spawn_team(server_t*  server,
                                         uint8_t    type,
                                         uint8_t    team,
                                         vector3f_t base,
                                         int        health,
                                         float      half_width,
                                         float      height);
/* a new map: every object goes (the MG survives everything else) */
void          proto10_objects_map_start(server_t* server);
/* one object's full trajectory and health to one player (the helicopter's
   occupants get it every tick, the rest at 20 Hz through the tick) */
void          proto10_objects_send_to(player_t* player, const p10_object_t* obj);
/* explode = EntityEvent Explode before the EntityRemove */
void          proto10_objects_destroy(server_t* server, p10_object_t* obj, int explode, const char* why);
p10_object_t* proto10_objects_find(uint32_t id);
/* the object of that type owned by that player, or NULL */
p10_object_t* proto10_objects_owned(uint8_t owner_id, uint8_t type);
/* index based iteration: NULL for free slots */
p10_object_t* proto10_objects_at(int index);
player_t*     proto10_objects_owner(server_t* server, const p10_object_t* obj);
/* a player took another class: the landmine or dynamite they placed goes */
void          proto10_objects_owner_class_changed(server_t* server, player_t* player);

/* every tick, from the bridge: think, ownership checks, delta EntityUpdate */
void proto10_objects_tick(server_t* server, int anyone_listening);
/* GameStateFinal */
uint32_t proto10_objects_count(void);
void     proto10_objects_put(p10_writer* w);

/* damage from a player (attacker may be NULL); friendly damage is refused */
void proto10_objects_damage(server_t* server, p10_object_t* obj, player_t* attacker, int amount, const char* cause);
/* a HitEntity from a 1.0 client on an id >= 256 */
void proto10_objects_on_hit(server_t* server, player_t* player, const p10_hit_entity* hit);
/* a grenade went off */
void proto10_objects_grenade(server_t* server, player_t* thrower, vector3f_t position);
/* 1 when the segment from a to b (map coordinates) crosses the object's box;
   *t is then the fraction of the segment where it enters */
int proto10_objects_segment_hits(const p10_object_t* obj, vector3f_t a, vector3f_t b, float* t);

/* While an object hurts a player through send_set_hp, the bridge reads this
   to describe the damage as coming from the object (Damage/EntityDie fire
   position and damage type) while the kill is credited to its owner. */
void                proto10_objects_set_damage_source(const p10_object_t* obj);
const p10_object_t* proto10_objects_damage_source(void);

#endif
