// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Proto10/Drills.h>
#include <Server/Proto10/Entities.h>
#include <Server/Proto10/Rockets.h>
#include <Server/Proto10/World.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <classes/classes.h>
#include <math.h>
#include <proto10/codec.h>
#include <string.h>

#define DRILL_MAX  32
#define DRILL_STEP 0.25f /* blocks between two samples */

typedef struct
{
    uint32_t   id; /* 0: a free slot */
    uint8_t    owner;
    uint8_t    team;
    vector3f_t pos;
    vector3f_t vel;
    uint8_t    boring; /* in the ground: straight on, no gravity */
    int        cell[3];
    int        dug;     /* blocks of ground bored */
    int        cleared; /* cells cleared */
    int        path[GAME_DRILL_DEPTH][3];
    uint64_t   born;
    uint64_t   last;
    uint64_t   sent;
} drill_t;

static drill_t drills[DRILL_MAX];

static player_t* _drill_player(server_t* server, uint8_t id)
{
    player_t* p = NULL;
    HASH_FIND(hh, server->players, &id, sizeof(id), p);
    return p;
}

static void _send(server_t* server, drill_t* d, uint8_t create)
{
    p10_entity_item it;
    memset(&it, 0, sizeof it);
    it.entity_id                   = d->id;
    it.mask                        = P10_EU_TRAJECTORY | (create ? P10_EU_CREATE | P10_EU_FLAGS : 0);
    it.entity_type                 = P10_ENT_DRILL;
    it.owner                       = d->owner;
    it.team                        = d->team == TEAM_A ? P10_TEAM_1 : P10_TEAM_2;
    it.trajectory.type             = d->boring ? P10_TRAJ_LINEAR : P10_TRAJ_GRAVITY;
    it.trajectory.origin.x         = d->pos.x;
    it.trajectory.origin.y         = d->pos.y;
    it.trajectory.origin.z         = d->pos.z;
    it.trajectory.velocity.x       = d->vel.x;
    it.trajectory.velocity.y       = d->vel.y;
    it.trajectory.velocity.z       = d->vel.z;
    it.trajectory.angular_velocity.x = 25.0f; /* it spins round its way (the client draws it so) */
    uint8_t    buf[160];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    rockets_broadcast(server, &w);
    d->sent = d->last;
}

void drills_fire(server_t* server, player_t* player, vector3f_t dir, const char* by)
{
    drill_t* d = NULL;
    for (int k = 0; k < DRILL_MAX; k++) {
        if (drills[k].id == 0) {
            d = &drills[k];
            break;
        }
    }
    if (d == NULL) {
        evt_log(player, "class", "action=drill_dropped why=too_many by=%s", by);
        return;
    }
    uint64_t now = get_nanos();
    memset(d, 0, sizeof *d);
    d->id    = proto10_entities_alloc_id();
    d->owner = player->id;
    d->team  = player->team;
    /* from just ahead of the eye, as the gun's muzzle */
    vector3f_t eye = player->movement.eye_pos;
    d->pos         = (vector3f_t) {eye.x + dir.x * 0.6f, eye.y + dir.y * 0.6f, eye.z + dir.z * 0.6f};
    d->vel         = (vector3f_t) {dir.x * GAME_DRILL_SPEED, dir.y * GAME_DRILL_SPEED, dir.z * GAME_DRILL_SPEED};
    d->cell[0]     = (int) floorf(d->pos.x);
    d->cell[1]     = (int) floorf(d->pos.y);
    d->cell[2]     = (int) floorf(d->pos.z);
    d->born = d->last = now;
    _send(server, d, 1);
}

static void _stop(server_t* server, drill_t* d, const char* why)
{
    player_t* owner = _drill_player(server, d->owner);
    if (d->cleared > 0) {
        /* what hung on the hole falls: the shell of every other block */
        for (int k = 0; k < d->dug; k += 2) {
            classes_check_shell(server, d->path[k][0], d->path[k][1], d->path[k][2], 1);
        }
        gamemode_terrain_changed(server, 0);
    }
    if (owner != NULL && owner->state != STATE_DISCONNECTED) {
        evt_log(owner, "class", "action=drill_stop entity=%u why=%s dug=%d cleared=%d at=%.1f,%.1f,%.1f flight=%.2f",
                d->id, why, d->dug, d->cleared, d->pos.x, d->pos.y, d->pos.z,
                (double) (d->last - d->born) / NANO_IN_SECOND);
    }
    uint8_t           buf[16];
    p10_writer        w;
    p10_entity_remove rm = {d->id};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_remove(&w, &rm);
    rockets_broadcast(server, &w);
    memset(d, 0, sizeof *d);
}

/* the 3 x 3 x 3 cube round the cell it entered: what the map and the mode
   let go. The cube reaches a block ahead, so in the ground every new cube
   clears the next face and the bit keeps boring; one that clears nothing
   means open air again */
static int _bore(server_t* server, drill_t* d, int cx, int cy, int cz)
{
    int n = 0;
    for (int z = cz - 1; z <= cz + 1; z++) {
        for (int x = cx - 1; x <= cx + 1; x++) {
            for (int y = cy - 1; y <= cy + 1; y++) {
                if (classes_cell_diggable(server, x, y, z)) {
                    map_set_air(&server->s_map.map, x, y, z);
                    proto10_world_cell(server, x, y, z, P10_WORLD_CAUSE_PLAYER);
                    n++;
                }
            }
        }
    }
    d->cleared += n;
    return n;
}

void drills_tick(server_t* server)
{
    map_store_t* map = &server->s_map.map;
    uint64_t  now = get_nanos();
    for (int k = 0; k < DRILL_MAX; k++) {
        drill_t* d = &drills[k];
        if (d->id == 0) {
            continue;
        }
        player_t* owner = _drill_player(server, d->owner);
        if (owner == NULL || owner->state == STATE_DISCONNECTED) {
            _stop(server, d, "owner_left");
            continue;
        }
        float dt = (float) (now - d->last) / NANO_IN_SECOND;
        if (dt > 0.05f) {
            dt = 0.05f;
        }
        d->last           = now;
        const char* why   = NULL;
        uint8_t     state = d->boring;
        while (dt > 0.0f && why == NULL) {
            float speed = sqrtf(d->vel.x * d->vel.x + d->vel.y * d->vel.y + d->vel.z * d->vel.z);
            if (speed < GAME_DRILL_MIN_SPEED) {
                why = "slow";
                break;
            }
            float sdt = DRILL_STEP / speed;
            if (sdt > dt) {
                sdt = dt;
            }
            dt -= sdt;
            if (!d->boring) {
                d->vel.z += GAME_DRILL_GRAVITY * sdt;
            }
            vector3f_t next = {d->pos.x + d->vel.x * sdt, d->pos.y + d->vel.y * sdt, d->pos.z + d->vel.z * sdt};
            if (next.x < 1 || next.y < 1 || next.x >= map->size_x - 1 || next.y >= map->size_y - 1) {
                why = "edge";
                break;
            }
            if (next.z >= 62.0f) {
                why = "water"; /* nothing digs the water's level */
                break;
            }
            int cx = (int) floorf(next.x), cy = (int) floorf(next.y), cz = (int) floorf(next.z);
            if (cx != d->cell[0] || cy != d->cell[1] || cz != d->cell[2]) {
                /* a block nothing may dig (the map's, the mode's): it stops
                   there; any other is bored, the first one it meets too */
                if (cz >= 0 && map_solid(map, cx, cy, cz) && !classes_cell_diggable(server, cx, cy, cz)) {
                    why = "protected";
                    break;
                }
                /* nor into its own base, fired from outside it */
                if (classes_drill_base(server, d->team, next.x, next.y)) {
                    why = "base";
                    break;
                }
                d->cell[0] = cx, d->cell[1] = cy, d->cell[2] = cz;
                int n      = cz >= -1 ? _bore(server, d, cx, cy, cz) : 0;
                d->boring  = n > 0;
                if (n > 0) {
                    memcpy(d->path[d->dug], d->cell, sizeof d->cell);
                    d->dug++;
                    d->vel.x *= 1.0f - GAME_DRILL_DRAG;
                    d->vel.y *= 1.0f - GAME_DRILL_DRAG;
                    d->vel.z *= 1.0f - GAME_DRILL_DRAG;
                    if (d->dug >= GAME_DRILL_DEPTH) {
                        d->pos = next;
                        why    = "depth";
                        break;
                    }
                }
            }
            d->pos = next;
        }
        if (why == NULL && now - d->born >= (uint64_t) (GAME_DRILL_LIFETIME * NANO_IN_SECOND)) {
            why = "lifetime";
        }
        if (why != NULL) {
            _stop(server, d, why);
            continue;
        }
        /* into or out of the ground, and the drag while it bores; in the
           air the Gravity trajectory holds, sent again now and then for the
           steps' rounding */
        float since = (float) (now - d->sent) / NANO_IN_SECOND;
        if (state != d->boring || since >= (d->boring ? DRILL_RESEND : 0.5f)) {
            _send(server, d, 0);
        }
    }
}
