// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Grenade.h>
#include <Server/Perks.h>
#include <Server/Proto10/Explosives.h>
#include <Server/Proto10/Sentry.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <math.h>
#include <proto10/codec.h>

static const char* _name(const p10_object_t* o)
{
    return o->type == P10_ENT_LANDMINE ? "landmine" : "dynamite";
}

static void _blow_up(server_t* server, p10_object_t* o, const char* why)
{
    player_t* owner = proto10_objects_owner(server, o);
    /* the blast's centre: just over the floor block it lies on */
    vector3f_t at = {o->base.x, o->base.y, o->base.z - 0.5f};
    int        dyn = o->type == P10_ENT_DYNAMITE;
    /* proto10_objects_destroy logs the explode with the why */
    /* the object goes first: the blast must not hurt it again, and clients
       see its Explode event where the crater opens */
    uint8_t type = o->type;
    /* the victims' damage points at the object: a copy, it is gone below */
    p10_object_t copy = *o;
    proto10_objects_set_damage_source(&copy);
    proto10_objects_destroy(server, o, 1, why);
    if (owner != NULL) {
        explosion_blast(server, owner, at, dyn ? 2 : 1, dyn ? DYNAMITE_DAMAGE_SCALE : 1.0f,
                        dyn ? DYNAMITE_DAMAGE_SCALE : 1.0f, KILL_REASON_EXPLOSIVE,
                        type == P10_ENT_DYNAMITE ? "dynamite" : "landmine");
    }
    proto10_objects_set_damage_source(NULL);
}

/* an enemy's feet within LANDMINE_TRIGGER of the mine */
static player_t* _mine_victim(server_t* server, const p10_object_t* o)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive || p->team == o->team || p->team == TEAM_SPECTATOR) {
            continue;
        }
        float dx = p->movement.position.x - o->base.x, dy = p->movement.position.y - o->base.y;
        /* position.z is the hip (z down): the feet are ~2 to 3 below it */
        float below = o->base.z - p->movement.position.z;
        if (dx * dx + dy * dy <= LANDMINE_TRIGGER * LANDMINE_TRIGGER && below > -0.5f && below < 4.5f) {
            return p;
        }
    }
    return NULL;
}

static void _landmine_think(server_t* server, p10_object_t* o, uint64_t now)
{
    if (o->detonate) {
        _blow_up(server, o, "shot");
        return;
    }
    if (!o->armed) {
        if (now - o->created >= (uint64_t) (LANDMINE_ARM_SECONDS * NANO_IN_SECOND)) {
            o->armed = 1;
            evt_log(proto10_objects_owner(server, o), "landmine", "action=armed entity=%u", o->id);
        }
        return;
    }
    player_t* owner = proto10_objects_owner(server, o);
    if (owner == NULL || !owner->alive) {
        return; /* rests: its blast would hurt nobody */
    }
    player_t* victim = _mine_victim(server, o);
    if (victim != NULL) {
        evt_log(owner, "landmine", "action=trigger entity=%u victim=%u", o->id, (unsigned) victim->id);
        _blow_up(server, o, "stepped_on");
    }
}

static void _dynamite_think(server_t* server, p10_object_t* o, uint64_t now)
{
    if (o->detonate) {
        _blow_up(server, o, "shot");
        return;
    }
    if (now - o->created >= (uint64_t) (GAME_DYNAMITE_FUSE * NANO_IN_SECOND)) {
        _blow_up(server, o, "fuse");
    }
}

static int _owned_count(uint8_t owner_id, uint8_t type)
{
    int n = 0;
    for (int k = 0; k < P10_MAX_OBJECTS; k++) {
        p10_object_t* o = proto10_objects_at(k);
        n += o != NULL && o->owner_id == owner_id && o->type == type;
    }
    return n;
}

int explosives_room(const player_t* owner, int perk)
{
    if (perk == P10_PERK_LANDMINE) {
        return _owned_count(owner->id, P10_ENT_LANDMINE) < LANDMINE_MAX;
    }
    return _owned_count(owner->id, P10_ENT_DYNAMITE) < 1;
}

p10_object_t* explosives_place(server_t* server, player_t* owner, int perk, int x, int y, int z, const char** why)
{
    uint8_t type = perk == P10_PERK_LANDMINE ? P10_ENT_LANDMINE : P10_ENT_DYNAMITE;
    if (perk != P10_PERK_LANDMINE && perk != P10_PERK_DYNAMITE) {
        *why = "unknown_perk";
        return NULL;
    }
    if (!perks_enabled(server, perk)) {
        *why = "mode";
        return NULL;
    }
    if (owner->state != STATE_READY || !owner->alive || owner->team == TEAM_SPECTATOR) {
        *why = "not_alive";
        return NULL;
    }
    if (!explosives_room(owner, perk)) {
        *why = "already_have_one";
        return NULL;
    }
    /* the sentry's rules (the client's preview is the same) */
    if (!sentry_cell_ok(server, owner, x, y, z, why)) {
        return NULL;
    }
    vector3f_t    base = {x + 0.5f, y + 0.5f, (float) z};
    int           mine = type == P10_ENT_LANDMINE;
    p10_object_t* o    = proto10_objects_spawn(server, type, owner, base, mine ? LANDMINE_HEALTH : DYNAMITE_HEALTH,
                                                mine ? GAME_LANDMINE_HALF_WIDTH : GAME_DYNAMITE_HALF_WIDTH,
                                                mine ? GAME_LANDMINE_HEIGHT : GAME_DYNAMITE_HEIGHT);
    if (o == NULL) {
        *why = "too_many_objects";
        return NULL;
    }
    o->blast_on_death = 1;
    o->think          = type == P10_ENT_LANDMINE ? _landmine_think : _dynamite_think;
    evt_log(owner, _name(o), "action=place entity=%u cell=%d,%d,%d health=%d", o->id, x, y, z, o->health);
    *why = "ok";
    return o;
}
