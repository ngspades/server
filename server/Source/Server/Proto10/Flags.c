// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Profile.h>
#include <Server/Proto10/Flags.h>
#include <Server/Proto10/Session.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Net/Conn.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <math.h>
#include <string.h>

/* the objectives' ids are fixed (codec.h), so a client and a joiner's
   snapshot always agree */
#define FLAG_ID_BASE P10_OBJECTIVE_ID_BASE

typedef struct {
    uint8_t wanted;  /* set since the last proto10_flags_begin */
    uint8_t live;    /* the clients have it */
    int32_t x, y, z; /* the cell */
    int     carrier; /* -1 on the ground */
    int32_t sx, sy, sz;
    int     scarrier; /* what the clients were last sent */
} flag_slot_t;

static flag_slot_t flag_slots[P10_FLAG_SLOTS];

void proto10_flags_begin(void)
{
    for (int k = 0; k < P10_FLAG_SLOTS; k++) {
        flag_slots[k].wanted = 0;
    }
}

void proto10_flags_set(int slot, float x, float y, float z, int carrier)
{
    if (slot < 0 || slot >= P10_FLAG_SLOTS) {
        return;
    }
    flag_slot_t* s = &flag_slots[slot];
    s->wanted      = 1;
    s->x           = (int32_t) floorf(x);
    s->y           = (int32_t) floorf(y);
    s->z           = (int32_t) floorf(z);
    s->carrier     = carrier;
}

static void _flag_item(int slot, const flag_slot_t* s, int create, p10_entity_item* it)
{
    memset(it, 0, sizeof *it);
    it->entity_id   = FLAG_ID_BASE + (uint32_t) slot;
    it->mask        = (uint8_t) (P10_EU_TRAJECTORY | (create ? P10_EU_CREATE : 0));
    it->entity_type = slot <= P10_FLAG_INTEL2 ? P10_ENT_FLAG
                    : slot <= P10_FLAG_TENT2  ? P10_ENT_COMMAND_POST
                                              : P10_ENT_CHECKPOINT;
    it->owner       = P10_NO_OWNER;
    it->team        = slot >= P10_FLAG_HILL ? P10_TEAM_SPECTATOR : (slot % 2) == 0 ? P10_TEAM_1 : P10_TEAM_2;
    it->trajectory.type = P10_TRAJ_CONSTANT;
    /* a carried intel rides its carrier (parentEntityId): where it lay is
       of no use to a client then */
    if (s->carrier >= 0) {
        it->trajectory.has_parent = 1;
        it->trajectory.parent     = (uint32_t) s->carrier;
    } else {
        it->trajectory.origin = (p10_vec3) {(float) s->x, (float) s->y, (float) s->z};
    }
}

static void _flag_broadcast(server_t* server, const p10_writer* w)
{
    if (w->error) {
        LOG_ERROR("proto10: a flag's update did not fit its buffer");
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME) {
            conn_send_reliable(p->conn, w->data, w->size);
        }
    }
}

void proto10_flags_send(server_t* server)
{
    if (profile_is_075()) {
        return;
    }
    /* every slot created at once: 7 territories and the tents took more
       than 256 bytes and the whole update was dropped */
    uint8_t    buf[2048];
    p10_writer w;
    int        items = 0;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    for (int k = 0; k < P10_FLAG_SLOTS; k++) {
        flag_slot_t* s = &flag_slots[k];
        if (!s->wanted) {
            if (s->live) {
                uint8_t           rbuf[16];
                p10_writer        rw;
                p10_entity_remove rm = {FLAG_ID_BASE + (uint32_t) k};
                p10_writer_init(&rw, rbuf, sizeof rbuf);
                p10_encode_entity_remove(&rw, &rm);
                _flag_broadcast(server, &rw);
                s->live = 0;
            }
            continue;
        }
        int changed = !s->live || s->carrier != s->scarrier ||
                      (s->carrier < 0 && (s->x != s->sx || s->y != s->sy || s->z != s->sz));
        if (!changed) {
            continue;
        }
        p10_entity_item it;
        _flag_item(k, s, !s->live, &it);
        p10_put_entity_item(&w, &it);
        items++;
        s->live     = 1;
        s->sx       = s->x;
        s->sy       = s->y;
        s->sz       = s->z;
        s->scarrier = s->carrier;
    }
    if (items) {
        _flag_broadcast(server, &w);
    }
}

uint32_t proto10_flags_count(void)
{
    uint32_t n = 0;
    for (int k = 0; k < P10_FLAG_SLOTS; k++) {
        n += flag_slots[k].wanted;
    }
    return n;
}

void proto10_flags_put(p10_writer* w)
{
    for (int k = 0; k < P10_FLAG_SLOTS; k++) {
        if (flag_slots[k].wanted) {
            p10_entity_item it;
            _flag_item(k, &flag_slots[k], 1, &it);
            p10_put_entity_item(w, &it);
        }
    }
}
