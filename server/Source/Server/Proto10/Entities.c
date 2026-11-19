// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/GameApi.h>
#include <Server/Proto10/NetStats.h>
#include <Server/Proto10/Entities.h>
#include <Server/Proto10/Session.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <enet/enet.h>
#include <string.h>

/* 0.75 grenade velocities are in blocks per 1/32 s (physics_move_grenade
   moves by velocity * dt * 32); 1.0 trajectories are in blocks per second */
#define AOS_VELOCITY_SCALE 32.0f

/* ids wrap inside this range; a grenade lives 3 s, so a reuse never meets
   a grenade that is still around */
#define P10_ENTITY_ID_SPAN 65536u

static uint32_t next_entity_id = P10_FIRST_ENTITY_ID;

static int _ent_peer_in_game(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME;
}

/* the thrower already shows its own grenade (the client adds it locally the
   moment it throws, as on 0.75 where the server does not echo it back), so
   every packet about a grenade skips its owner */
static void _ent_broadcast_except(server_t* server, const p10_writer* w, player_t* except)
{
    if (w->error) {
        LOG_ERROR("proto10: entity packet did not fit its buffer");
        return;
    }
    player_t *  p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == except || !_ent_peer_in_game(p)) {
            continue;
        }
        conn_send_reliable(p->conn, w->data, w->size);
    }
}

static void _ent_grenade_item(const grenade_t* g, uint8_t trajectory_type, p10_entity_item* it)
{
    memset(it, 0, sizeof *it);
    it->entity_id           = g->p10_id;
    it->mask                = P10_EU_CREATE | P10_EU_FLAGS | P10_EU_TRAJECTORY;
    it->entity_type         = P10_ENT_GRENADE;
    it->flags               = 0; /* nothing collides with a grenade */
    it->trajectory.type     = trajectory_type;
    it->trajectory.origin.x = g->position.x;
    it->trajectory.origin.y = g->position.y;
    it->trajectory.origin.z = g->position.z;
    if (trajectory_type != P10_TRAJ_CONSTANT) {
        it->trajectory.velocity.x = g->velocity.x * AOS_VELOCITY_SCALE;
        it->trajectory.velocity.y = g->velocity.y * AOS_VELOCITY_SCALE;
        it->trajectory.velocity.z = g->velocity.z * AOS_VELOCITY_SCALE;
    }
}

uint32_t proto10_entities_alloc_id(void)
{
    uint32_t id    = next_entity_id;
    next_entity_id = P10_FIRST_ENTITY_ID + (next_entity_id + 1 - P10_FIRST_ENTITY_ID) % P10_ENTITY_ID_SPAN;
    return id;
}

void proto10_entities_grenade_thrown(server_t* server, player_t* player, grenade_t* grenade)
{
    /* game_player_throw_grenade only arms (and broadcasts) a grenade thrown from
       a valid position; the others never move nor explode, so they are not
       entities either */
    grenade->p10_id = 0;
    if (!grenade->sent) {
        return;
    }
    grenade->p10_id = proto10_entities_alloc_id();

    evt_log(player,
            "grenade",
            "action=throw entity=%u fuse=%.2f pos=%.1f,%.1f,%.1f",
            grenade->p10_id,
            grenade->fuse,
            grenade->position.x,
            grenade->position.y,
            grenade->position.z);

    uint8_t         buf[128];
    p10_writer      w;
    p10_entity_item it;
    _ent_grenade_item(grenade, P10_TRAJ_GRAVITY, &it);
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    _ent_broadcast_except(server, &w, player);
}

static void _ent_remove(server_t* server, player_t* player, grenade_t* grenade)
{
    uint8_t           buf[16];
    p10_writer        w;
    p10_entity_remove rm = {grenade->p10_id};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_remove(&w, &rm);
    _ent_broadcast_except(server, &w, player);
    grenade->p10_id = 0;
}

void proto10_entities_grenade_exploded(server_t* server, player_t* player, grenade_t* grenade)
{
    if (grenade->p10_id == 0) {
        return;
    }
    evt_log(player,
            "grenade",
            "action=explode entity=%u pos=%.1f,%.1f,%.1f",
            grenade->p10_id,
            grenade->position.x,
            grenade->position.y,
            grenade->position.z);

    /* clients only simulate the Gravity arc and its bounces; they explode the
       grenade where the server says it is, not where their copy ended up */
    uint8_t         buf[128];
    p10_writer      w;
    p10_entity_item it;
    _ent_grenade_item(grenade, P10_TRAJ_CONSTANT, &it);
    it.mask = P10_EU_TRAJECTORY;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    _ent_broadcast_except(server, &w, player);

    p10_entity_event ev = {grenade->p10_id, P10_EV_EXPLODE, 0};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_event(&w, &ev);
    _ent_broadcast_except(server, &w, player);

    _ent_remove(server, player, grenade);
}

void proto10_entities_grenade_removed(server_t* server, player_t* player, grenade_t* grenade)
{
    if (grenade->p10_id != 0) {
        _ent_remove(server, player, grenade);
    }
}

uint32_t proto10_entities_count(server_t* server)
{
    uint32_t   n = 0;
    player_t * p, *tmp;
    grenade_t* g;
    HASH_ITER(hh, server->players, p, tmp)
    {
        DL_FOREACH(p->grenade, g)
        {
            n += g->p10_id != 0;
        }
    }
    return n;
}

void proto10_entities_put(server_t* server, p10_writer* w)
{
    player_t * p, *tmp;
    grenade_t* g;
    HASH_ITER(hh, server->players, p, tmp)
    {
        DL_FOREACH(p->grenade, g)
        {
            if (g->p10_id != 0) {
                p10_entity_item it;
                _ent_grenade_item(g, P10_TRAJ_GRAVITY, &it);
                p10_put_entity_item(w, &it);
            }
        }
    }
}

int proto10_entities_on_throw(server_t* server, player_t* player, const uint8_t* data, size_t size)
{
    p10_throw_grenade t;
    if (!p10_decode_throw_grenade(data, size, &t)) {
        return 0;
    }
    if (player->state != STATE_READY || !player->alive) {
        evt_log(player, "grenade", "throw_refused why=not_alive");
        return 1;
    }
    if (player->grenades == 0) {
        evt_log(player, "grenade", "throw_refused why=no_grenades");
        return 1;
    }
    /* the one throw handler judges it: tool, sprint, rate limit, fuse cap,
       speed and position checks are the ones a 0.75 client gets */
    uint8_t count = player->grenades;
    float   fuse  = t.fuse;
    float   px    = t.origin.x, py = t.origin.y, pz = t.origin.z;
    float   vx = t.velocity.x / AOS_VELOCITY_SCALE, vy = t.velocity.y / AOS_VELOCITY_SCALE,
            vz = t.velocity.z / AOS_VELOCITY_SCALE;
    game_player_throw_grenade(
        server, player, fuse, (vector3f_t) {px, py, pz}, (vector3f_t) {vx, vy, vz});
    if (player->grenades == count) {
        evt_log(player, "grenade", "throw_refused why=rejected_by_075_checks item=%hhu sprint=%hhu", player->item,
                player->sprinting);
    }
    return 1;
}
