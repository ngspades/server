// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Helicopter.h>
#include <Server/Proto10/Mg.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Objects.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* a `use` press each way: a client may hold the key, and the command is a
   GenericCommand (not a stream of them) */
#define MOUNT_USE_RATE (NANO_IN_MILLI * 250)
/* the gunner's eye over the feet (the physics' box, Util/Physics.c) */
#define MOUNT_EYE_HEIGHT 2.25f

static uint64_t use_at[256];

/* the distance from a point to an object's box (the box is the one bullets
   hit, Objects.c): the reach of a `use` is judged from it, not from the
   object's middle, so a helicopter is boarded from beside its body */
static float _box_distance(vector3f_t p, const p10_object_t* o)
{
    float hl = o->half_len > 0.0f ? o->half_len : o->half_width;
    float lx = p.x - o->base.x, ly = p.y - o->base.y;
    if (o->half_len > 0.0f) {
        float c = cosf(o->box_yaw), s = sinf(o->box_yaw);
        float rx = lx * c + ly * s, ry = -lx * s + ly * c;
        lx       = rx;
        ly       = ry;
    }
    if (lx < -hl) {
        lx = -hl;
    } else if (lx > hl) {
        lx = hl;
    }
    if (ly < -o->half_width) {
        ly = -o->half_width;
    } else if (ly > o->half_width) {
        ly = o->half_width;
    }
    if (o->half_len > 0.0f) {
        float c  = cosf(o->box_yaw), s = sinf(o->box_yaw);
        float rx = lx * c - ly * s, ry = lx * s + ly * c;
        lx       = rx;
        ly       = ry;
    }
    float x = o->base.x + lx, y = o->base.y + ly, z = p.z;
    if (z > o->base.z) {
        z = o->base.z;
    }
    if (z < o->base.z - o->height) {
        z = o->base.z - o->height;
    }
    float dx = p.x - x, dy = p.y - y, dz = p.z - z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

int mount_mounted(const player_t* player)
{
    return player != NULL && player->mount_entity != P10_NO_OWNER;
}

int mount_in_vehicle(const player_t* player)
{
    return player != NULL && player->mount_entity != P10_NO_OWNER && player->mount_vehicle;
}

player_t* mount_occupant(server_t* server, uint32_t entity, int seat)
{
    if (entity == P10_NO_OWNER || entity == 0) {
        return NULL;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->mount_entity == entity && p->mount_seat == seat) {
            return p;
        }
    }
    return NULL;
}

int mount_lift_input(const player_t* player)
{
    /* the reference's player has no held "jump" bit on the wire: the 1.0
       input's jetpack bit carries a held jump (creative flight reads it the
       same way), and a PlayerAction Jump is the one tick pulse */
    if ((player->p10_input_prev & P10_IN_JETPACK) || player->jumping) {
        return 1;
    }
    return player->crouching ? -1 : 0;
}

/* the seat's eye, in map coordinates, whatever the player rides */
static void _seat_eye(server_t* server, const p10_object_t* o, int seat, vector3f_t* out)
{
    (void) server;
    if (o->type == P10_ENT_HELICOPTER) {
        heli_seat_eye(o, seat, out);
        return;
    }
    vector3f_t feet = mg_gunner_feet(o);
    out->x          = feet.x;
    out->y          = feet.y;
    out->z          = feet.z - MOUNT_EYE_HEIGHT;
}

/* `mount <player> <entity> <seat>`; -1 0 when the player is on nothing */
static void _mount_broadcast(server_t* server, const player_t* p)
{
    char        id[8], entity[16], seat[8];
    const char* parts[4] = {"mount", id, entity, seat};
    snprintf(id, sizeof id, "%u", (unsigned) p->id);
    if (p->mount_entity == P10_NO_OWNER) {
        snprintf(entity, sizeof entity, "-1");
        snprintf(seat, sizeof seat, "0");
    } else {
        snprintf(entity, sizeof entity, "%u", p->mount_entity);
        snprintf(seat, sizeof seat, "%u", (unsigned) p->mount_seat);
    }
    proto10_bridge_generic(server, NULL, 4, parts);
}

static void _mount_refuse(server_t* server, player_t* player, const char* why)
{
    const char* parts[2] = {"use-refused", why};
    proto10_bridge_generic(server, player, 2, parts);
    evt_log(player, "mount", "action=refused player=%u why=%s", (unsigned) player->id, why);
}

static void _mount(server_t* server, player_t* player, p10_object_t* o, int seat)
{
    player->mount_entity  = o->id;
    player->mount_seat    = (uint8_t) seat;
    player->mount_vehicle = o->type == P10_ENT_HELICOPTER;
    /* the seat holds the body from here: no walk, no fall, no crouch */
    player->movement.velocity = (vector3f_t) {0, 0, 0};
    player->airborne           = 0;
    player->wade               = 0;
    player->sneaking           = 0;
    player->sprinting          = 0;
    player->fall_dist          = 0;
    player->jumping            = 0;
    vector3f_t seat_eye;
    _seat_eye(server, o, seat, &seat_eye);
    player->movement.prev_position  = player->movement.position;
    player->movement.position       = seat_eye;
    player->movement.eye_pos        = seat_eye;
    player->movement.prev_legit_pos = seat_eye;
    _mount_broadcast(server, player);
    evt_log(player, "mount", "action=mount player=%u entity=%u seat=%d type=%s", (unsigned) player->id, o->id, seat,
            o->type == P10_ENT_HELICOPTER ? "heli" : "mg");
    if (o->type == P10_ENT_HELICOPTER) {
        evt_log(player, "heli", "action=mount entity=%u seat=%d", o->id, seat);
    }
}

void mount_dismount(server_t* server, player_t* player, const char* why)
{
    if (player == NULL || player->mount_entity == P10_NO_OWNER) {
        return;
    }
    uint32_t      entity  = player->mount_entity;
    int           seat    = player->mount_seat;
    int           vehicle = player->mount_vehicle;
    p10_object_t* o       = proto10_objects_find(entity);
    vector3f_t    at      = player->movement.position;
    if (o != NULL) {
        _seat_eye(server, o, seat, &at);
    }
    player->mount_entity  = P10_NO_OWNER;
    player->mount_seat    = 0;
    player->mount_vehicle = 0;
    player->movement.prev_position  = player->movement.position;
    player->movement.position       = at;
    player->movement.eye_pos        = at;
    player->movement.prev_legit_pos = at;
    player->movement.velocity       = (vector3f_t) {0, 0, 0};
    player->fall_dist               = 0;
    player->jumping                 = 0;
    _mount_broadcast(server, player);
    evt_log(player, "mount", "action=dismount player=%u entity=%u seat=%d why=%s", (unsigned) player->id, entity, seat,
            why);
    if (vehicle) {
        evt_log(player, "heli", "action=dismount entity=%u seat=%d why=%s", entity, seat, why);
    }
}

int mount_player_step(server_t* server, player_t* player)
{
    if (player->mount_entity == P10_NO_OWNER) {
        return 0;
    }
    p10_object_t* o = proto10_objects_find(player->mount_entity);
    if (o == NULL) {
        mount_dismount(server, player, "gone");
        return 0;
    }
    vector3f_t seat;
    _seat_eye(server, o, player->mount_seat, &seat);
    player->movement.prev_position = player->movement.position;
    player->movement.position      = seat;
    player->movement.eye_pos       = seat;
    player->movement.velocity      = (vector3f_t) {0, 0, 0};
    player->wade                   = 0;
    player->fall_dist              = 0;
    return 1;
}

void mount_use(server_t* server, player_t* player)
{
    if (player == NULL || profile_is_075()) {
        return;
    }
    if (player->mount_entity != P10_NO_OWNER) {
        mount_dismount(server, player, "use");
        return;
    }
    if (player->state != STATE_READY || !player->alive || player->team == TEAM_SPECTATOR) {
        _mount_refuse(server, player, "dead");
        return;
    }
    uint64_t now = get_nanos();
    if (now - use_at[player->id] < MOUNT_USE_RATE) {
        _mount_refuse(server, player, "rate");
        return;
    }
    use_at[player->id] = now;

    vector3f_t eye = player->movement.eye_pos;

    /* a helicopter first, and its pilot's seat first */
    p10_object_t* seat_heli   = NULL;
    int           seat        = -1;
    float         best        = 1.0e9f;
    int           saw_other   = 0, saw_full = 0, saw_wrecked = 0;
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o == NULL || o->type != P10_ENT_HELICOPTER) {
            continue;
        }
        float d = _box_distance(eye, o);
        if (d > GAME_HELI_USE_RANGE) {
            continue;
        }
        if (o->team != player->team) {
            saw_other = 1;
            continue;
        }
        if (!heli_boardable(o)) {
            saw_wrecked = 1;
            continue;
        }
        int free_seat = -1;
        for (int s = 0; s < GAME_HELI_SEATS; s++) {
            if (mount_occupant(server, o->id, s) == NULL) {
                free_seat = s;
                break;
            }
        }
        if (free_seat < 0) {
            saw_full = 1;
            continue;
        }
        if (d < best) {
            best      = d;
            seat_heli = o;
            seat      = free_seat;
        }
    }
    if (seat_heli != NULL) {
        _mount(server, player, seat_heli, seat);
        return;
    }

    /* else a machine gun of the player's team, one gunner */
    p10_object_t* best_mg = NULL;
    int           saw_busy = 0;
    best                  = GAME_MG_USE_RANGE + 1.0f;
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o == NULL || o->type != P10_ENT_MG || o->health <= 0) {
            continue;
        }
        float d = _box_distance(eye, o);
        if (d > GAME_MG_USE_RANGE) {
            continue;
        }
        if (o->team != player->team) {
            saw_other = 1;
            continue;
        }
        if (mount_occupant(server, o->id, 0) != NULL) {
            saw_busy = 1;
            continue;
        }
        if (d < best) {
            best    = d;
            best_mg = o;
        }
    }
    if (best_mg != NULL) {
        _mount(server, player, best_mg, 0);
        return;
    }

    /* nothing of ours free: say what was in reach, else nothing at all */
    if (saw_full) {
        _mount_refuse(server, player, "full");
    } else if (saw_busy) {
        _mount_refuse(server, player, "busy");
    } else if (saw_wrecked) {
        _mount_refuse(server, player, "wrecked");
    } else if (saw_other) {
        _mount_refuse(server, player, "team");
    } else {
        _mount_refuse(server, player, "none");
    }
}

void mount_player_left(server_t* server, player_t* player)
{
    if (player->mount_entity == P10_NO_OWNER) {
        return;
    }
    /* no `mount` line: the player's PlayerRemove follows, and every client
       drops its seats with the body. The leaving player is not a peer to
       write to here either */
    evt_log(player, "mount", "action=dismount player=%u entity=%u seat=%d why=left", (unsigned) player->id,
            player->mount_entity, player->mount_seat);
    if (player->mount_vehicle) {
        evt_log(player, "heli", "action=dismount entity=%u seat=%d why=left", player->mount_entity,
                player->mount_seat);
    }
    player->mount_entity  = P10_NO_OWNER;
    player->mount_seat    = 0;
    player->mount_vehicle = 0;
    use_at[player->id]    = 0;
    (void) server;
}

void mount_team_change(server_t* server, player_t* player)
{
    mount_dismount(server, player, "team");
}

void mount_map_start(server_t* server)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        mount_dismount(server, p, "map");
        use_at[p->id] = 0;
    }
}

void proto10_bridge_mounts_to(server_t* server, player_t* to)
{
    if (to == NULL || profile_is_075()) {
        return;
    }
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->mount_entity == P10_NO_OWNER) {
            continue;
        }
        char        id[8], entity[16], seat[8];
        const char* parts[4] = {"mount", id, entity, seat};
        snprintf(id, sizeof id, "%u", (unsigned) p->id);
        snprintf(entity, sizeof entity, "%u", p->mount_entity);
        snprintf(seat, sizeof seat, "%u", (unsigned) p->mount_seat);
        proto10_bridge_generic(server, to, 4, parts);
        n++;
    }
    if (n > 0) {
        evt_log(to, "mount", "action=state mounted=%d", n);
    }
}
