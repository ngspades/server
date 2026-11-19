// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/Proto10/NetStats.h>
#include <Server/EventLog.h>
#include <Server/Grenade.h>
#include <Server/Proto10/Entities.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Rockets.h>
#include <Server/Proto10/Session.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Packets/Packets.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <enet/enet.h>
#include <math.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <string.h>

#define ROCKET_MAX  64
#define ROCKET_STEP 0.25f /* blocks between two collision samples */

typedef struct
{
    uint32_t   id; /* 0: a free slot */
    uint8_t    owner;
    uint8_t    team;
    vector3f_t pos;
    vector3f_t dir; /* unit */
    uint64_t   born;
    uint64_t   last;
} rocket_t;

static rocket_t rockets[ROCKET_MAX];

static int _peer_in_game(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME;
}

/* to everyone, the shooter too: its client draws the rocket the server flies
   (the drill bits go the same way, Drills.c) */
void rockets_broadcast(server_t* server, const p10_writer* w)
{
    if (w->error) {
        LOG_ERROR("proto10: rocket packet did not fit its buffer");
        return;
    }
    player_t *  p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_peer_in_game(p)) {
            continue;
        }
        conn_send_reliable(p->conn, w->data, w->size);
    }
}

static void _item(const rocket_t* r, uint8_t traj, p10_entity_item* it)
{
    memset(it, 0, sizeof *it);
    it->entity_id           = r->id;
    it->mask                = P10_EU_CREATE | P10_EU_FLAGS | P10_EU_TRAJECTORY;
    it->entity_type         = P10_ENT_ROCKET;
    it->flags               = 0;
    it->trajectory.type     = traj;
    it->trajectory.origin.x = r->pos.x;
    it->trajectory.origin.y = r->pos.y;
    it->trajectory.origin.z = r->pos.z;
    if (traj == P10_TRAJ_LINEAR) {
        it->trajectory.velocity.x = r->dir.x * ROCKET_SPEED;
        it->trajectory.velocity.y = r->dir.y * ROCKET_SPEED;
        it->trajectory.velocity.z = r->dir.z * ROCKET_SPEED;
    }
}

static player_t* _player(server_t* server, uint8_t id)
{
    player_t* p = NULL;
    HASH_FIND(hh, server->players, &id, sizeof(id), p);
    return p;
}

static void _refuse(server_t* server, player_t* player, const char* why)
{
    evt_log(player, "rocket", "action=refused why=%s clip=%u reserve=%u", why, (unsigned) player->weapon_clip,
            (unsigned) player->weapon_reserve);
    const char* parts[] = {"rocket-refused", why};
    proto10_bridge_generic(server, player, 2, parts);
}

int rockets_fire(server_t* server, player_t* player, vector3f_t origin, vector3f_t dir, const char* by)
{
    uint64_t now = get_nanos();
    float    len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    float    ox = origin.x - player->movement.eye_pos.x, oy = origin.y - player->movement.eye_pos.y,
          oz = origin.z - player->movement.eye_pos.z;
    const char* why = NULL;
    if (player->weapon != GAME_WEAPON_ROCKET) {
        why = "weapon";
    } else if (player->state != STATE_READY || !player->alive || player->team == TEAM_SPECTATOR) {
        why = "not_alive";
    } else if (player->item != TOOL_GUN) {
        why = "tool";
    } else if (player->reloading) {
        why = "reloading";
    } else if (!player_gun_raised(player, now)) {
        why = "raise";
    } else if (player->weapon_clip == 0) {
        why = "empty";
    } else if (player->sprinting) {
        why = "sprinting";
    } else if (player->rocket_at != 0 && now - player->rocket_at < (uint64_t) (ROCKET_INTERVAL * NANO_IN_SECOND)) {
        why = "rate";
    } else if (!(len > 0.5f && len < 2.0f)) {
        why = "direction";
    } else if (ox * ox + oy * oy + oz * oz > 2.5f * 2.5f) {
        why = "origin_far";
    }
    if (why != NULL) {
        _refuse(server, player, why);
        return 0;
    }
    rocket_t* r = NULL;
    for (int k = 0; k < ROCKET_MAX; k++) {
        if (rockets[k].id == 0) {
            r = &rockets[k];
            break;
        }
    }
    if (r == NULL) {
        _refuse(server, player, "too_many");
        return 0;
    }
    player->weapon_clip--;
    player->rocket_at = now;
    memset(r, 0, sizeof *r);
    r->id    = proto10_entities_alloc_id();
    r->owner = player->id;
    r->team  = player->team;
    r->dir   = (vector3f_t) {dir.x / len, dir.y / len, dir.z / len};
    r->pos   = origin;
    r->born  = now;
    r->last  = now;
    evt_log(player, "rocket", "action=fire entity=%u by=%s at=%.1f,%.1f,%.1f dir=%.2f,%.2f,%.2f clip=%u reserve=%u",
            r->id, by, origin.x, origin.y, origin.z, r->dir.x, r->dir.y, r->dir.z, (unsigned) player->weapon_clip,
            (unsigned) player->weapon_reserve);
    uint8_t         buf[128];
    p10_writer      w;
    p10_entity_item it;
    _item(r, P10_TRAJ_LINEAR, &it);
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    rockets_broadcast(server, &w);
    return 1;
}

/* the rocket jump: the shooter near its own blast is thrown away from it,
   mostly up. The server's body moves at once (a bot's); a 1.0 client
   reports its own position, so it is told the push to add to its velocity */
static void _push(server_t* server, player_t* p, vector3f_t at)
{
    if (p->state != STATE_READY || !p->alive) {
        return;
    }
    /* from the chest (z down: the chest is over the hip) */
    float dx = p->movement.position.x - at.x, dy = p->movement.position.y - at.y,
          dz = p->movement.position.z + 0.9f - at.z;
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d >= ROCKET_PUSH_RADIUS) {
        return;
    }
    float s  = ROCKET_PUSH * (1.0f - d / ROCKET_PUSH_RADIUS);
    float hl = sqrtf(dx * dx + dy * dy);
    float vx = hl > 0.01f ? dx / hl * s * 0.5f : 0.0f;
    float vy = hl > 0.01f ? dy / hl * s * 0.5f : 0.0f;
    float vz = -s; /* up */
    p->movement.velocity.x += vx;
    p->movement.velocity.y += vy;
    p->movement.velocity.z += vz;
    p->airborne = 1;
    char        a[3][24];
    const char* parts[4] = {"rocket-push", a[0], a[1], a[2]};
    snprintf(a[0], sizeof a[0], "%.4f", vx);
    snprintf(a[1], sizeof a[1], "%.4f", vy);
    snprintf(a[2], sizeof a[2], "%.4f", vz);
    proto10_bridge_generic(server, p, 4, parts);
    evt_log(p, "rocket", "action=push dist=%.2f velocity=%.3f,%.3f,%.3f", d, vx, vy, vz);
}

static void _explode(server_t* server, rocket_t* r, vector3f_t at, const char* why, int victim)
{
    player_t* owner = _player(server, r->owner);
    r->pos          = at;
    if (owner != NULL && owner->state != STATE_DISCONNECTED) {
        evt_log(owner, "rocket", "action=explode entity=%u why=%s victim=%d at=%.1f,%.1f,%.1f flight=%.2f", r->id, why,
                victim, at.x, at.y, at.z, (double) (r->last - r->born) / NANO_IN_SECOND);
        explosion_blast_retail(server, owner, at, 1, ROCKET_DAMAGE, ROCKET_BLAST_RADIUS, ROCKET_SELF_SCALE, 3, "rocket");
        _push(server, owner, at);
    }
    /* clients draw it where the server says it went off, then the blast */
    uint8_t         buf[128];
    p10_writer      w;
    p10_entity_item it;
    _item(r, P10_TRAJ_CONSTANT, &it);
    it.mask = P10_EU_TRAJECTORY;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    rockets_broadcast(server, &w);
    p10_entity_event ev = {r->id, P10_EV_EXPLODE, 0};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_event(&w, &ev);
    rockets_broadcast(server, &w);
    p10_entity_remove rm = {r->id};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_remove(&w, &rm);
    rockets_broadcast(server, &w);
    memset(r, 0, sizeof *r);
}

/* a body point of a living player other than the shooter within reach */
static int _hits_player(server_t* server, const rocket_t* r, vector3f_t p)
{
    player_t *o, *tmp;
    HASH_ITER(hh, server->players, o, tmp)
    {
        if (o->id == r->owner || o->state != STATE_READY || !o->alive || o->team == TEAM_SPECTATOR) {
            continue;
        }
        static const float off[3] = {0.1f, 0.9f, 1.7f};
        for (int k = 0; k < 3; k++) {
            float dx = o->movement.position.x - p.x, dy = o->movement.position.y - p.y,
                  dz = o->movement.position.z + off[k] - p.z;
            if (dx * dx + dy * dy + dz * dz < ROCKET_HIT_RADIUS * ROCKET_HIT_RADIUS) {
                return o->id;
            }
        }
    }
    return -1;
}

static int _hits_object(const rocket_t* r, vector3f_t a, vector3f_t b)
{
    for (int k = 0; k < P10_MAX_OBJECTS; k++) {
        p10_object_t* o = proto10_objects_at(k);
        float         t;
        if (o != NULL && o->team != r->team && proto10_objects_segment_hits(o, a, b, &t)) {
            return 1;
        }
    }
    return 0;
}

void rockets_tick(server_t* server)
{
    map_store_t* map = &server->s_map.map;
    uint64_t  now = get_nanos();
    for (int k = 0; k < ROCKET_MAX; k++) {
        rocket_t* r = &rockets[k];
        if (r->id == 0) {
            continue;
        }
        player_t* owner = _player(server, r->owner);
        if (owner == NULL || owner->state == STATE_DISCONNECTED) {
            /* its shooter left: gone without a blast nobody would answer for */
            p10_entity_remove rm = {r->id};
            uint8_t           buf[16];
            p10_writer        w;
            p10_writer_init(&w, buf, sizeof buf);
            p10_encode_entity_remove(&w, &rm);
            rockets_broadcast(server, &w);
            memset(r, 0, sizeof *r);
            continue;
        }
        float dt = (float) (now - r->last) / NANO_IN_SECOND;
        if (dt > 0.05f) {
            dt = 0.05f;
        }
        r->last    = now;
        float left = ROCKET_SPEED * dt;
        int   done = 0;
        while (left > 0.0f && !done) {
            float      s    = left < ROCKET_STEP ? left : ROCKET_STEP;
            vector3f_t prev = r->pos;
            vector3f_t next = {prev.x + r->dir.x * s, prev.y + r->dir.y * s, prev.z + r->dir.z * s};
            left -= s;
            if (next.x < 0 || next.y < 0 || next.x >= map->size_x || next.y >= map->size_y || next.z >= 63.0f) {
                /* off the map's sides or into the sea: a blast where it left */
                _explode(server, r, prev, next.z >= 63.0f ? "water" : "edge", -1);
                done = 1;
                break;
            }
            int victim = _hits_player(server, r, next);
            if (victim >= 0) {
                _explode(server, r, next, "player", victim);
                done = 1;
                break;
            }
            if (_hits_object(r, prev, next)) {
                _explode(server, r, prev, "object", -1);
                done = 1;
                break;
            }
            if (next.z >= 0.0f &&
                map_solid(map, (uint16_t) next.x, (uint16_t) next.y, (uint16_t) next.z))
            {
                /* the blast just before the block, on the side it came from */
                _explode(server, r, prev, "block", -1);
                done = 1;
                break;
            }
            r->pos = next;
        }
        if (!done && now - r->born >= (uint64_t) (ROCKET_LIFETIME * NANO_IN_SECOND)) {
            _explode(server, r, r->pos, "lifetime", -1);
        }
    }
}
