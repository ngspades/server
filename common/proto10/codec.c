// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int p10_packet_type_of(const uint8_t* data, size_t size, uint8_t* type)
{
    if (!data || size == 0)
        return 0;
    *type = data[0];
    return 1;
}

/* positions the reader after the type byte; 0 when the type does not match */
static int begin(p10_reader* r, const uint8_t* data, size_t size, uint8_t type)
{
    p10_reader_init(r, data, size);
    return p10_read_u8(r) == type && !r->error;
}

static void start(p10_writer* w, uint8_t type)
{
    p10_write_u8(w, type);
}

/* ---- session ---- */

void p10_encode_greeting(p10_writer* w, const p10_greeting* p)
{
    start(w, P10_PKT_GREETING);
    p10_write_cstring(w, P10_GREETING_MAGIC);
    p10_write_bytes(w, p->nonce);
}

int p10_decode_greeting(const uint8_t* data, size_t size, p10_greeting* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_GREETING))
        return 0;
    p10_bytes magic = p10_read_string(&r);
    p->nonce        = p10_read_bytes(&r);
    return p10_at_end(&r) && p10_bytes_equal(magic, P10_GREETING_MAGIC);
}

void p10_encode_initiate_connection(p10_writer* w, const p10_initiate_connection* p)
{
    start(w, P10_PKT_INITIATE_CONNECTION);
    p10_write_bytes(w, p->protocol_name);
    p10_write_u16(w, p->major);
    p10_write_u16(w, p->minor);
    p10_write_u16(w, p->revision);
    p10_write_bytes(w, p->package);
    p10_write_bytes(w, p->environment);
    p10_write_bytes(w, p->locale);
    p10_write_bytes(w, p->player_name);
    p10_write_bytes(w, p->nonce);
    p10_write_u8(w, p->map_quality);
}

int p10_decode_initiate_connection(const uint8_t* data, size_t size, p10_initiate_connection* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_INITIATE_CONNECTION))
        return 0;
    p->protocol_name = p10_read_string(&r);
    p->major         = p10_read_u16(&r);
    p->minor         = p10_read_u16(&r);
    p->revision      = p10_read_u16(&r);
    p->package       = p10_read_string(&r);
    p->environment   = p10_read_string(&r);
    p->locale        = p10_read_string(&r);
    p->player_name   = p10_read_string(&r);
    p->nonce         = p10_read_bytes(&r);
    p->map_quality   = p10_read_u8(&r);
    return p10_at_end(&r);
}

void p10_encode_certificate(p10_writer* w, uint8_t type, const p10_certificate* p)
{
    start(w, type);
    p10_write_u8(w, p->is_valid ? 1 : 0);
    if (p->is_valid) {
        p10_write_bytes(w, p->certificate);
        p10_write_bytes(w, p->signature);
    }
}

int p10_decode_certificate(const uint8_t* data, size_t size, uint8_t type, p10_certificate* p)
{
    p10_reader r;
    if (type != P10_PKT_SERVER_CERTIFICATE && type != P10_PKT_CLIENT_CERTIFICATE)
        return 0;
    if (!begin(&r, data, size, type))
        return 0;
    memset(p, 0, sizeof *p);
    p->is_valid = p10_read_u8(&r) != 0;
    if (p->is_valid) {
        p->certificate = p10_read_bytes(&r);
        p->signature   = p10_read_bytes(&r);
    }
    return p10_at_end(&r);
}

void p10_encode_kick(p10_writer* w, const p10_kick* p)
{
    start(w, P10_PKT_KICK);
    p10_write_bytes(w, p->reason);
}

int p10_decode_kick(const uint8_t* data, size_t size, p10_kick* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_KICK))
        return 0;
    p->reason = p10_read_string(&r);
    return p10_at_end(&r);
}

/* ---- empty packets ---- */

void p10_encode_empty(p10_writer* w, uint8_t type)
{
    start(w, type);
}

int p10_decode_empty(const uint8_t* data, size_t size, uint8_t type)
{
    p10_reader r;
    return begin(&r, data, size, type) && p10_at_end(&r);
}

/* ---- maps ---- */

void p10_put_map_entry_bytes(p10_writer* w, p10_bytes key, p10_bytes value)
{
    /* an empty key is the terminator, so it can never be a real key */
    if (key.len == 0)
        return;
    p10_write_bytes(w, key);
    p10_write_bytes(w, value);
}

void p10_put_map_entry(p10_writer* w, const char* key, const char* value)
{
    p10_put_map_entry_bytes(w, p10_bytes_from_cstring(key), p10_bytes_from_cstring(value));
}

void p10_put_map_end(p10_writer* w)
{
    p10_write_varint(w, 0);
}

int p10_next_map_entry(p10_reader* r, p10_bytes* key, p10_bytes* value)
{
    *key = p10_read_string(r);
    if (r->error || key->len == 0)
        return 0;
    *value = p10_read_string(r);
    return !r->error;
}

/* ---- game state transfer ---- */

void p10_start_game_state_header(p10_writer* w)
{
    start(w, P10_PKT_GAME_STATE_HEADER);
}

int p10_begin_game_state_header(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_GAME_STATE_HEADER);
}

void p10_encode_map_data(p10_writer* w, const p10_map_data* p)
{
    start(w, P10_PKT_MAP_DATA);
    p10_write_bytes(w, p->fragment);
}

int p10_decode_map_data(const uint8_t* data, size_t size, p10_map_data* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_MAP_DATA))
        return 0;
    p->fragment = p10_read_bytes(&r);
    return p10_at_end(&r);
}

void p10_start_game_state_final(p10_writer* w)
{
    start(w, P10_PKT_GAME_STATE_FINAL);
}

int p10_begin_game_state_final(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_GAME_STATE_FINAL);
}

void p10_put_count(p10_writer* w, uint32_t count)
{
    p10_write_varint(w, count);
}

uint32_t p10_read_count(p10_reader* r)
{
    return p10_read_varint32(r);
}

/* ---- entity parts ---- */

/* revision 1.2's Player trajectory (docs/protocol-1.0.md 12.7): x and y in
   1/64 of a block from 0 (u16, to 1024), z from -512 (i16), the velocity in
   1/1024 (i16, to +-32), pitch and yaw in i16 over their ranges. */
#define COMPACT_POS 64.0f
#define COMPACT_VEL 1024.0f
#define COMPACT_PITCH (32767.0f / 1.5707964f)
#define COMPACT_YAW (32767.0f / 3.1415927f)

static int fits(float v, float scale, float lo, float hi)
{
    float q = v * scale;
    return q >= lo && q <= hi; /* false for a NaN too */
}

static int compact_fits(const p10_trajectory* t)
{
    const p10_vec3 *o = &t->origin, *v = &t->velocity, *e = &t->euler;
    return fits(o->x, COMPACT_POS, 0.0f, 65535.0f) && fits(o->y, COMPACT_POS, 0.0f, 65535.0f) &&
        fits(o->z, COMPACT_POS, -32768.0f, 32767.0f) && fits(v->x, COMPACT_VEL, -32767.0f, 32767.0f) &&
        fits(v->y, COMPACT_VEL, -32767.0f, 32767.0f) && fits(v->z, COMPACT_VEL, -32767.0f, 32767.0f) &&
        fits(e->x, COMPACT_PITCH, -32767.0f, 32767.0f) && fits(e->z, COMPACT_YAW, -32767.0f, 32767.0f) &&
        e->y == 0.0f;
}

static void put_trajectory(p10_writer* w, const p10_trajectory* t)
{
    if (t->type == P10_TRAJ_PLAYER_COMPACT && compact_fits(t)) {
        p10_write_u8(w, (uint8_t) (P10_TRAJ_PLAYER_COMPACT | (t->has_parent ? 0x80 : 0)));
        p10_write_u16(w, (uint16_t) lroundf(t->origin.x * COMPACT_POS));
        p10_write_u16(w, (uint16_t) lroundf(t->origin.y * COMPACT_POS));
        /* z rounds up (map z grows down), never into the ground: a body
           standing 0.001 over a block's top read back exactly on it, and the
           client's step-up took the floor for a step, lifted the body a block
           and the next update dropped it again, bobbing every walking body */
        p10_write_u16(w, (uint16_t) (int16_t) floorf(t->origin.z * COMPACT_POS));
        p10_write_u16(w, (uint16_t) (int16_t) lroundf(t->velocity.x * COMPACT_VEL));
        p10_write_u16(w, (uint16_t) (int16_t) lroundf(t->velocity.y * COMPACT_VEL));
        p10_write_u16(w, (uint16_t) (int16_t) lroundf(t->velocity.z * COMPACT_VEL));
        p10_write_u16(w, (uint16_t) (int16_t) lroundf(t->euler.x * COMPACT_PITCH));
        p10_write_u16(w, (uint16_t) (int16_t) lroundf(t->euler.z * COMPACT_YAW));
        if (t->has_parent)
            p10_write_varint(w, t->parent);
        return;
    }
    /* asked for compact but a value does not fit: the float Player form */
    uint8_t type = t->type == P10_TRAJ_PLAYER_COMPACT ? P10_TRAJ_PLAYER : t->type;
    p10_write_u8(w, (uint8_t) ((type & 0x7f) | (t->has_parent ? 0x80 : 0)));
    p10_write_vec3(w, t->origin);
    p10_write_vec3(w, t->velocity);
    if (type == P10_TRAJ_PLAYER) {
        p10_write_vec3(w, t->euler);
    } else {
        p10_write_rotation(w, t->rotation);
        p10_write_vec3(w, t->angular_velocity);
    }
    if (t->has_parent)
        p10_write_varint(w, t->parent);
}

static void read_trajectory(p10_reader* r, p10_trajectory* t)
{
    uint8_t b = p10_read_u8(r);
    memset(t, 0, sizeof *t);
    t->type       = b & 0x7f;
    t->has_parent = (b & 0x80) != 0;
    /* the layout depends on the type, so an unknown one cannot be skipped */
    if (t->type >= P10_TRAJ_COUNT) {
        r->error = 1;
        return;
    }
    if (t->type == P10_TRAJ_PLAYER_COMPACT) {
        t->type       = P10_TRAJ_PLAYER;
        t->origin.x   = p10_read_u16(r) / COMPACT_POS;
        t->origin.y   = p10_read_u16(r) / COMPACT_POS;
        t->origin.z   = (int16_t) p10_read_u16(r) / COMPACT_POS;
        t->velocity.x = (int16_t) p10_read_u16(r) / COMPACT_VEL;
        t->velocity.y = (int16_t) p10_read_u16(r) / COMPACT_VEL;
        t->velocity.z = (int16_t) p10_read_u16(r) / COMPACT_VEL;
        t->euler.x    = (int16_t) p10_read_u16(r) / COMPACT_PITCH;
        t->euler.z    = (int16_t) p10_read_u16(r) / COMPACT_YAW;
        if (t->has_parent)
            t->parent = p10_read_varint32(r);
        return;
    }
    t->origin   = p10_read_vec3(r);
    t->velocity = p10_read_vec3(r);
    if (t->type == P10_TRAJ_PLAYER) {
        t->euler = p10_read_vec3(r);
    } else {
        t->rotation         = p10_read_rotation(r);
        t->angular_velocity = p10_read_vec3(r);
    }
    if (t->has_parent)
        t->parent = p10_read_varint32(r);
}

static void put_input(p10_writer* w, const p10_player_input* in)
{
    p10_write_u8(w, (uint8_t) ((in->flags & 0x1f) | ((in->stance & 3) << 6)));
    p10_write_u8(w, (uint8_t) in->xmove);
    p10_write_u8(w, (uint8_t) in->ymove);
}

static void read_input(p10_reader* r, p10_player_input* in)
{
    uint8_t b  = p10_read_u8(r);
    in->flags  = b & 0x1f;
    in->stance = (b >> 6) & 3;
    in->xmove  = (int8_t) p10_read_u8(r);
    in->ymove  = (int8_t) p10_read_u8(r);
    /* bit 4 is the jetpack (revision 1.1), bit 5 is unused and stance 3 does
       not exist */
    if ((b & 0x20) || in->stance > P10_STANCE_PRONE)
        r->error = 1;
}

/* wire order differs from the struct order: reload and raise come before damage */
static void put_weapon(p10_writer* w, const p10_weapon_params* p)
{
    p10_write_u8(w, (uint8_t) ((p->full_auto ? 1 : 0) | (p->slow_reload ? 2 : 0) | (p->penetrative ? 4 : 0) |
                               ((p->type & 3) << 3)));
    p10_write_u16(w, p->fire_interval_ms);
    p10_write_u16(w, p->reload_ms);
    p10_write_u16(w, p->raise_ms);
    p10_write_u8(w, p->max_damage);
    p10_write_u8(w, p->magazine);
}

static void read_weapon(p10_reader* r, p10_weapon_params* p)
{
    uint8_t f           = p10_read_u8(r);
    p->full_auto        = f & 1;
    p->slow_reload      = (f >> 1) & 1;
    p->penetrative      = (f >> 2) & 1;
    p->type             = (f >> 3) & 3;
    p->fire_interval_ms = p10_read_u16(r);
    p->reload_ms        = p10_read_u16(r);
    p->raise_ms         = p10_read_u16(r);
    p->max_damage       = p10_read_u8(r);
    p->magazine         = p10_read_u8(r);
    if (f & 0xe0)
        r->error = 1;
}

/* a skin names a folder or file under the game's resources: lowercase ASCII
   and the path's own signs, nothing a path could climb out with */
static int skin_ok(p10_bytes b)
{
    if (b.len > 32)
        return 0;
    for (size_t i = 0; i < b.len; i++) {
        uint8_t c = b.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '-'))
            return 0;
    }
    return 1;
}

void p10_put_entity_item(p10_writer* w, const p10_entity_item* it)
{
    uint8_t mask = it->mask & P10_EU_KNOWN;
    p10_write_varint(w, it->entity_id);
    p10_write_u8(w, mask);
    if (mask & P10_EU_CREATE) {
        p10_write_u8(w, it->entity_type);
        if (it->entity_type == P10_ENT_PLAYER) {
            p10_write_bytes(w, it->body_skin);
            for (int i = 0; i < 3; i++)
                p10_write_bytes(w, it->weapon_skin[i]);
            for (int i = 0; i < 3; i++)
                put_weapon(w, &it->weapon[i]);
        }
        if (P10_ENT_HAS_OWNER(it->entity_type)) {
            p10_write_varint(w, it->owner);
            p10_write_u8(w, it->team);
        }
    }
    if (mask & P10_EU_FLAGS)
        p10_write_u8(w, it->flags);
    if (mask & P10_EU_TRAJECTORY)
        put_trajectory(w, &it->trajectory);
    if (mask & P10_EU_PLAYER_INPUT)
        put_input(w, &it->input);
    if (mask & P10_EU_TOOL)
        p10_write_u8(w, it->tool);
    if (mask & P10_EU_BLOCK_COLOR)
        p10_write_color(w, it->block_color);
    if (mask & P10_EU_HEALTH)
        p10_write_u8(w, it->health);
}

int p10_next_entity_item(p10_reader* r, p10_entity_item* it)
{
    if (!p10_has_more(r))
        return 0;
    memset(it, 0, sizeof *it);
    it->entity_id = p10_read_varint32(r);
    it->mask      = p10_read_u8(r);
    /* a bit we do not know has a payload we cannot measure */
    if (it->mask & ~P10_EU_KNOWN) {
        r->error = 1;
        return 0;
    }
    if (it->mask & P10_EU_CREATE) {
        it->entity_type = p10_read_u8(r);
        if (it->entity_type >= P10_ENT_COUNT) {
            r->error = 1;
            return 0;
        }
        if (it->entity_type == P10_ENT_PLAYER) {
            it->body_skin = p10_read_bytes(r);
            for (int i = 0; i < 3; i++)
                it->weapon_skin[i] = p10_read_bytes(r);
            /* a skin the client would turn into a path is refused at the wire */
            if (!skin_ok(it->body_skin))
                r->error = 1;
            for (int i = 0; i < 3; i++)
                if (!skin_ok(it->weapon_skin[i]))
                    r->error = 1;
            for (int i = 0; i < 3; i++)
                read_weapon(r, &it->weapon[i]);
        }
        if (P10_ENT_HAS_OWNER(it->entity_type)) {
            it->owner = p10_read_varint32(r);
            it->team  = p10_read_u8(r);
            if (it->team != P10_TEAM_1 && it->team != P10_TEAM_2 && it->team != P10_TEAM_SPECTATOR)
                r->error = 1;
        }
    }
    if (it->mask & P10_EU_FLAGS) {
        it->flags = p10_read_u8(r);
        if (it->flags & ~(P10_EF_PLAYER_CLIP | P10_EF_WEAPON_CLIP | P10_EF_FLY | P10_EF_JETPACK))
            r->error = 1;
    }
    if (it->mask & P10_EU_TRAJECTORY)
        read_trajectory(r, &it->trajectory);
    if (it->mask & P10_EU_PLAYER_INPUT)
        read_input(r, &it->input);
    if (it->mask & P10_EU_TOOL) {
        it->tool = p10_read_u8(r);
        if (it->tool > P10_TOOL_WEAPON3)
            r->error = 1;
    }
    if (it->mask & P10_EU_BLOCK_COLOR)
        it->block_color = p10_read_color(r);
    if (it->mask & P10_EU_HEALTH)
        it->health = p10_read_u8(r);
    return !r->error;
}

void p10_start_entity_update(p10_writer* w, int forced)
{
    start(w, P10_PKT_ENTITY_UPDATE);
    p10_write_u8(w, forced ? 1 : 0);
}

int p10_begin_entity_update(p10_reader* r, const uint8_t* data, size_t size, int* forced)
{
    if (!begin(r, data, size, P10_PKT_ENTITY_UPDATE))
        return 0;
    *forced = p10_read_u8(r) != 0;
    return !r->error;
}

void p10_start_client_entity_update(p10_writer* w)
{
    start(w, P10_PKT_CLIENT_ENTITY_UPDATE);
}

int p10_begin_client_entity_update(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_CLIENT_ENTITY_UPDATE);
}

/* ---- players ---- */

void p10_put_player_item(p10_writer* w, const p10_player_item* it)
{
    uint8_t mask = it->mask & P10_PU_KNOWN;
    p10_write_varint(w, it->player_id);
    p10_write_u8(w, mask);
    if (mask & P10_PU_CREATE)
        p10_write_bytes(w, it->name);
    if (mask & P10_PU_FLAGS)
        p10_write_u8(w, it->flags);
    if (mask & P10_PU_SCORE)
        p10_write_varint(w, it->score);
    if (mask & P10_PU_TEAM)
        p10_write_u8(w, it->team);
    if (mask & P10_PU_STATS) {
        p10_write_varint(w, it->kills);
        p10_write_varint(w, it->deaths);
    }
    if (mask & P10_PU_PING)
        p10_write_varint(w, it->ping_ms);
    if (mask & P10_PU_PERKS) {
        p10_write_u8(w, it->perk_points);
        p10_write_u8(w, it->perk_inventory);
        p10_write_u8(w, it->perk_next);
    }
}

int p10_next_player_item(p10_reader* r, p10_player_item* it)
{
    if (!p10_has_more(r))
        return 0;
    memset(it, 0, sizeof *it);
    it->player_id = p10_read_varint32(r);
    it->mask      = p10_read_u8(r);
    if (it->mask & ~P10_PU_KNOWN) {
        r->error = 1;
        return 0;
    }
    if (it->mask & P10_PU_CREATE)
        it->name = p10_read_string(r);
    if (it->mask & P10_PU_FLAGS) {
        it->flags = p10_read_u8(r);
        if (it->flags & ~P10_PF_ADMIN)
            r->error = 1;
    }
    if (it->mask & P10_PU_SCORE)
        it->score = p10_read_varint32(r);
    if (it->mask & P10_PU_TEAM) {
        it->team = p10_read_u8(r);
        if (it->team != P10_TEAM_1 && it->team != P10_TEAM_2 && it->team != P10_TEAM_SPECTATOR)
            r->error = 1;
    }
    if (it->mask & P10_PU_STATS) {
        it->kills  = p10_read_varint32(r);
        it->deaths = p10_read_varint32(r);
    }
    if (it->mask & P10_PU_PING)
        it->ping_ms = p10_read_varint32(r);
    if (it->mask & P10_PU_PERKS) {
        it->perk_points    = p10_read_u8(r);
        it->perk_inventory = p10_read_u8(r);
        it->perk_next      = p10_read_u8(r);
        /* a perk we do not know could not be shown nor used */
        if (it->perk_inventory & ~((1u << P10_PERK_COUNT) - 1))
            r->error = 1;
    }
    return !r->error;
}

uint64_t p10_perk_param(uint8_t perk, uint32_t x, uint32_t y, uint32_t z)
{
    return (uint64_t) perk | ((uint64_t) (x & 0xfff) << 8) | ((uint64_t) (y & 0xfff) << 20) |
        ((uint64_t) (z & 0xff) << 32);
}

int p10_perk_param_decode(uint64_t param, uint8_t* perk, uint32_t* x, uint32_t* y, uint32_t* z)
{
    if (param >> 40)
        return 0;
    *perk = (uint8_t) (param & 0xff);
    *x    = (uint32_t) ((param >> 8) & 0xfff);
    *y    = (uint32_t) ((param >> 20) & 0xfff);
    *z    = (uint32_t) ((param >> 32) & 0xff);
    return 1;
}

void p10_start_player_update(p10_writer* w)
{
    start(w, P10_PKT_PLAYER_UPDATE);
}

int p10_begin_player_update(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_PLAYER_UPDATE);
}

void p10_start_player_remove(p10_writer* w)
{
    start(w, P10_PKT_PLAYER_REMOVE);
}

void p10_put_player_id(p10_writer* w, uint32_t player_id)
{
    p10_write_varint(w, player_id);
}

int p10_begin_player_remove(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_PLAYER_REMOVE);
}

int p10_next_player_id(p10_reader* r, uint32_t* player_id)
{
    if (!p10_has_more(r))
        return 0;
    *player_id = p10_read_varint32(r);
    return !r->error;
}

/* ---- generic command ---- */

void p10_start_generic_command(p10_writer* w)
{
    start(w, P10_PKT_GENERIC_COMMAND);
}

void p10_put_generic_part(p10_writer* w, const char* part)
{
    p10_write_cstring(w, part);
}

int p10_begin_generic_command(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_GENERIC_COMMAND);
}

int p10_next_generic_part(p10_reader* r, p10_bytes* part)
{
    if (!p10_has_more(r))
        return 0;
    *part = p10_read_string(r);
    return !r->error;
}

/* ---- events ---- */

void p10_encode_entity_event(p10_writer* w, const p10_entity_event* p)
{
    start(w, P10_PKT_ENTITY_EVENT);
    p10_write_varint(w, p->entity_id);
    p10_write_u8(w, p->type);
    p10_write_varint(w, p->param);
}

int p10_decode_entity_event(const uint8_t* data, size_t size, p10_entity_event* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_ENTITY_EVENT))
        return 0;
    p->entity_id = p10_read_varint32(&r);
    p->type      = p10_read_u8(&r);
    p->param     = p10_read_varint(&r);
    return p10_at_end(&r);
}

void p10_encode_entity_die(p10_writer* w, const p10_entity_die* p)
{
    start(w, P10_PKT_ENTITY_DIE);
    p10_write_varint(w, p->entity_id);
    p10_write_u8(w, p->type);
    p10_write_varint(w, p->param);
    if (p->has_damage) {
        const p10_damage_info* d = &p->damage;
        p10_write_u8(w, (uint8_t) ((d->damage_type & 0x7f) | (d->has_from_entity ? 0x80 : 0)));
        p10_write_vec3(w, d->fire_position);
        p10_write_vec3(w, d->hit_position);
        if (d->has_from_entity)
            p10_write_varint(w, d->from_entity);
    }
}

int p10_decode_entity_die(const uint8_t* data, size_t size, p10_entity_die* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_ENTITY_DIE))
        return 0;
    memset(p, 0, sizeof *p);
    p->entity_id = p10_read_varint32(&r);
    p->type      = p10_read_u8(&r);
    p->param     = p10_read_varint(&r);
    /* D3: the reference read the damage type before this check */
    if (p10_has_more(&r)) {
        p10_damage_info* d = &p->damage;
        uint8_t b          = p10_read_u8(&r);
        p->has_damage      = 1;
        d->damage_type     = b & 0x7f;
        d->has_from_entity = (b & 0x80) != 0;
        d->fire_position   = p10_read_vec3(&r);
        d->hit_position    = p10_read_vec3(&r);
        if (d->has_from_entity)
            d->from_entity = p10_read_varint32(&r);
    }
    return p10_at_end(&r);
}

void p10_encode_entity_remove(p10_writer* w, const p10_entity_remove* p)
{
    start(w, P10_PKT_ENTITY_REMOVE);
    p10_write_varint(w, p->entity_id);
}

int p10_decode_entity_remove(const uint8_t* data, size_t size, p10_entity_remove* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_ENTITY_REMOVE))
        return 0;
    p->entity_id = p10_read_varint32(&r);
    return p10_at_end(&r);
}

void p10_encode_player_action(p10_writer* w, const p10_player_action* p)
{
    start(w, P10_PKT_PLAYER_ACTION);
    p10_write_u8(w, p->type);
    p10_write_varint(w, p->param);
}

int p10_decode_player_action(const uint8_t* data, size_t size, p10_player_action* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_PLAYER_ACTION))
        return 0;
    p->type  = p10_read_u8(&r);
    p->param = p10_read_varint(&r);
    return p10_at_end(&r);
}

/* ---- terrain ---- */

void p10_start_terrain_update(p10_writer* w, p10_terrain_cursor* c)
{
    start(w, P10_PKT_TERRAIN_UPDATE);
    c->cx = c->cy = c->cz = 0;
}

static int fits_delta(int64_t d)
{
    return d >= -127 && d <= 127;
}

void p10_put_map_edit(p10_writer* w, p10_terrain_cursor* c, const p10_map_edit* e)
{
    int64_t dx = (int64_t) e->x - c->cx, dy = (int64_t) e->y - c->cy, dz = (int64_t) e->z - c->cz;
    if (e->x < 0 || e->y < 0 || e->z < 0) {
        /* the absolute cursor is an unsigned varint on the wire */
        w->error = 1;
        return;
    }
    if (!fits_delta(dx) || !fits_delta(dy) || !fits_delta(dz)) {
        c->cx = e->x;
        c->cy = e->y;
        c->cz = e->z;
        p10_write_u8(w, 0x80); /* -128: an absolute cursor follows */
        p10_write_varint(w, (uint32_t) c->cx);
        p10_write_varint(w, (uint32_t) c->cy);
        p10_write_varint(w, (uint32_t) c->cz);
        dx = dy = dz = 0;
    }
    p10_write_u8(w, (uint8_t) (int8_t) dx);
    p10_write_u8(w, (uint8_t) (int8_t) dy);
    p10_write_u8(w, (uint8_t) (int8_t) dz);
    p10_write_u8(w, e->health);
    if (e->health) {
        /* the reference stores the colour as health << 24 | b << 16 | g << 8 | r
           and writes the low three bytes first: red, green, blue. this is the
           opposite order of p10_write_color (entity block colour) */
        p10_write_u8(w, e->color.r);
        p10_write_u8(w, e->color.g);
        p10_write_u8(w, e->color.b);
    }
    p10_write_u8(w, e->cause);
}

int p10_begin_terrain_update(p10_reader* r, const uint8_t* data, size_t size, p10_terrain_cursor* c)
{
    c->cx = c->cy = c->cz = 0;
    return begin(r, data, size, P10_PKT_TERRAIN_UPDATE);
}

static int32_t read_cursor_coord(p10_reader* r)
{
    uint32_t v = p10_read_varint32(r);
    if (v > INT32_MAX) {
        r->error = 1;
        return 0;
    }
    return (int32_t) v;
}

int p10_next_map_edit(p10_reader* r, p10_terrain_cursor* c, p10_map_edit* e)
{
    if (!p10_has_more(r))
        return 0;
    memset(e, 0, sizeof *e);
    int8_t dx = (int8_t) p10_read_u8(r);
    if (dx == -128) {
        c->cx = read_cursor_coord(r);
        c->cy = read_cursor_coord(r);
        c->cz = read_cursor_coord(r);
        dx    = (int8_t) p10_read_u8(r);
        /* encoders never emit -128 as a delta, so a second one is garbage */
        if (dx == -128)
            r->error = 1;
    }
    int8_t dy = (int8_t) p10_read_u8(r);
    int8_t dz = (int8_t) p10_read_u8(r);
    /* in 64 bits: a cursor near INT32_MAX plus a delta must not overflow */
    int64_t x = (int64_t) c->cx + dx, y = (int64_t) c->cy + dy, z = (int64_t) c->cz + dz;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || z < INT32_MIN || z > INT32_MAX) {
        r->error = 1;
        return 0;
    }
    e->x      = (int32_t) x;
    e->y      = (int32_t) y;
    e->z      = (int32_t) z;
    e->health = p10_read_u8(r);
    if (e->health) {
        e->color.r = p10_read_u8(r);
        e->color.g = p10_read_u8(r);
        e->color.b = p10_read_u8(r);
    }
    e->cause = p10_read_u8(r);
    return !r->error;
}

/* ---- hits and damage ---- */

void p10_encode_hit_entity(p10_writer* w, const p10_hit_entity* p)
{
    start(w, P10_PKT_HIT_ENTITY);
    p10_write_varint(w, p->entity_id);
    p10_write_u8(w, p->hit_type);
    p10_write_u8(w, p->damage_type);
    p10_write_vec3(w, p->fire_position);
    p10_write_vec3(w, p->hit_position);
}

int p10_decode_hit_entity(const uint8_t* data, size_t size, p10_hit_entity* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_HIT_ENTITY))
        return 0;
    p->entity_id     = p10_read_varint32(&r);
    p->hit_type      = p10_read_u8(&r);
    p->damage_type   = p10_read_u8(&r);
    p->fire_position = p10_read_vec3(&r);
    p->hit_position  = p10_read_vec3(&r);
    return p10_at_end(&r);
}

static int32_t read_block_coord(p10_reader* r)
{
    int64_t v = p10_read_svarint(r);
    if (v < INT32_MIN || v > INT32_MAX) {
        r->error = 1;
        return 0;
    }
    return (int32_t) v;
}

void p10_encode_hit_terrain(p10_writer* w, const p10_hit_terrain* p)
{
    start(w, P10_PKT_HIT_TERRAIN);
    p10_write_svarint(w, p->x);
    p10_write_svarint(w, p->y);
    p10_write_svarint(w, p->z);
    p10_write_u8(w, p->damage_type);
    p10_write_vec3(w, p->fire_position);
    p10_write_vec3(w, p->hit_position);
}

int p10_decode_hit_terrain(const uint8_t* data, size_t size, p10_hit_terrain* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_HIT_TERRAIN))
        return 0;
    p->x             = read_block_coord(&r);
    p->y             = read_block_coord(&r);
    p->z             = read_block_coord(&r);
    p->damage_type   = p10_read_u8(&r);
    p->fire_position = p10_read_vec3(&r);
    p->hit_position  = p10_read_vec3(&r);
    return p10_at_end(&r);
}

void p10_encode_damage(p10_writer* w, const p10_damage* p)
{
    const p10_damage_info* d = &p->damage;
    start(w, P10_PKT_DAMAGE);
    p10_write_varint(w, p->entity_id);
    p10_write_u8(w, (uint8_t) ((d->damage_type & 0x7f) | (d->has_from_entity ? 0x80 : 0)));
    /* D4: the reference set the flag but never wrote the id */
    if (d->has_from_entity)
        p10_write_varint(w, d->from_entity);
    p10_write_varint(w, p->to_entity);
    p10_write_vec3(w, d->fire_position);
    p10_write_vec3(w, d->hit_position);
    p10_write_u8(w, p->amount);
}

int p10_decode_damage(const uint8_t* data, size_t size, p10_damage* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_DAMAGE))
        return 0;
    memset(p, 0, sizeof *p);
    p->entity_id              = p10_read_varint32(&r);
    uint8_t b                 = p10_read_u8(&r);
    p->damage.damage_type     = b & 0x7f;
    p->damage.has_from_entity = (b & 0x80) != 0;
    if (p->damage.has_from_entity)
        p->damage.from_entity = p10_read_varint32(&r);
    p->to_entity              = p10_read_varint32(&r);
    p->damage.fire_position   = p10_read_vec3(&r);
    p->damage.hit_position    = p10_read_vec3(&r);
    p->amount                 = p10_read_u8(&r);
    return p10_at_end(&r);
}

/* ==== [1.0] packets of section 12.2 ==== */

/* ---- chat ---- */

static int chat_text_ok(p10_bytes text)
{
    return text.len <= P10_CHAT_MAX_TEXT && (text.len == 0 || memchr(text.data, 0, text.len) == NULL) &&
        p10_utf8_valid(text);
}

void p10_encode_chat_message(p10_writer* w, const p10_chat_message* p)
{
    if (p->channel > P10_CHAT_TEAM || !chat_text_ok(p->text)) {
        w->error = 1;
        return;
    }
    start(w, P10_PKT_CHAT_MESSAGE);
    p10_write_u8(w, p->channel);
    p10_write_bytes(w, p->text);
}

int p10_decode_chat_message(const uint8_t* data, size_t size, p10_chat_message* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_CHAT_MESSAGE))
        return 0;
    /* clients cannot speak on the system channel: that would forge notices */
    p->channel = p10_read_u8(&r);
    p->text    = p10_read_string(&r);
    return p10_at_end(&r) && p->channel <= P10_CHAT_TEAM && chat_text_ok(p->text);
}

void p10_encode_chat_broadcast(p10_writer* w, const p10_chat_broadcast* p)
{
    if (p->channel > P10_CHAT_SYSTEM || !chat_text_ok(p->text)) {
        w->error = 1;
        return;
    }
    start(w, P10_PKT_CHAT_BROADCAST);
    p10_write_varint(w, p->from_player);
    p10_write_u8(w, p->channel);
    p10_write_bytes(w, p->text);
}

int p10_decode_chat_broadcast(const uint8_t* data, size_t size, p10_chat_broadcast* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_CHAT_BROADCAST))
        return 0;
    p->from_player = p10_read_varint32(&r);
    p->channel     = p10_read_u8(&r);
    p->text        = p10_read_string(&r);
    return p10_at_end(&r) && p->channel <= P10_CHAT_SYSTEM && chat_text_ok(p->text);
}

/* ---- GameModeState ---- */

void p10_start_game_mode_state(p10_writer* w)
{
    start(w, P10_PKT_GAME_MODE_STATE);
}

int p10_begin_game_mode_state(p10_reader* r, const uint8_t* data, size_t size)
{
    return begin(r, data, size, P10_PKT_GAME_MODE_STATE);
}

/* one decimal number at *pos, within min..max; advances *pos past it */
static int parse_number(p10_bytes s, size_t* pos, int64_t min, int64_t max, int64_t* out)
{
    size_t i  = *pos;
    int neg   = 0;
    int64_t v = 0;
    if (i < s.len && s.data[i] == '-') {
        neg = 1;
        i++;
    }
    size_t digits = i;
    while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9') {
        v = v * 10 + (s.data[i] - '0');
        /* past every 32-bit range already; stopping here keeps v from
           overflowing int64 on a long run of digits */
        if (v > 99999999999LL)
            return 0;
        i++;
    }
    if (i == digits)
        return 0;
    if (neg)
        v = -v;
    if (v < min || v > max)
        return 0;
    *pos = i;
    *out = v;
    return 1;
}

int p10_parse_ints(p10_bytes s, int32_t* out, int count)
{
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        int64_t v;
        if (i > 0) {
            if (pos >= s.len || s.data[pos] != ',')
                return 0;
            pos++;
        }
        if (!parse_number(s, &pos, INT32_MIN, INT32_MAX, &v))
            return 0;
        out[i] = (int32_t) v;
    }
    return count > 0 && pos == s.len;
}


int p10_parse_intel(p10_bytes s, p10_intel* out)
{
    memset(out, 0, sizeof *out);
    if (p10_bytes_equal(s, "base"))
        out->state = P10_INTEL_BASE;
    else if (p10_bytes_equal(s, "dropped"))
        out->state = P10_INTEL_DROPPED;
    else if (p10_bytes_equal(s, "carried"))
        out->state = P10_INTEL_CARRIED;
    else
        return 0;
    return 1;
}

int p10_format_intel(const p10_intel* in, char* out, size_t out_size)
{
    static const char* const words[] = {"base", "dropped", "carried"};
    int n;
    if (out_size == 0)
        return 0;
    n = in->state <= P10_INTEL_CARRIED ? snprintf(out, out_size, "%s", words[in->state]) : -1;
    if (n < 0 || (size_t) n >= out_size) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* ---- building and grenades ---- */

static void put_blockcoord(p10_writer* w, p10_blockcoord c)
{
    p10_write_svarint(w, c.x);
    p10_write_svarint(w, c.y);
    p10_write_svarint(w, c.z);
}

static p10_blockcoord read_blockcoord(p10_reader* r)
{
    p10_blockcoord c;
    c.x = read_block_coord(r);
    c.y = read_block_coord(r);
    c.z = read_block_coord(r);
    return c;
}

void p10_encode_build_block(p10_writer* w, const p10_build_block* p)
{
    start(w, P10_PKT_BUILD_BLOCK);
    put_blockcoord(w, p->pos);
    p10_write_color(w, p->color);
}

int p10_decode_build_block(const uint8_t* data, size_t size, p10_build_block* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_BUILD_BLOCK))
        return 0;
    p->pos   = read_blockcoord(&r);
    p->color = p10_read_color(&r);
    return p10_at_end(&r);
}

void p10_encode_build_line(p10_writer* w, const p10_build_line* p)
{
    start(w, P10_PKT_BUILD_LINE);
    put_blockcoord(w, p->from);
    put_blockcoord(w, p->to);
    p10_write_color(w, p->color);
}

int p10_decode_build_line(const uint8_t* data, size_t size, p10_build_line* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_BUILD_LINE))
        return 0;
    p->from  = read_blockcoord(&r);
    p->to    = read_blockcoord(&r);
    p->color = p10_read_color(&r);
    return p10_at_end(&r);
}

static int vec3_finite(p10_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static int grenade_ok(const p10_throw_grenade* p)
{
    return vec3_finite(p->origin) && vec3_finite(p->velocity) && isfinite(p->fuse) && p->fuse >= 0.0f;
}

void p10_encode_throw_grenade(p10_writer* w, const p10_throw_grenade* p)
{
    if (!grenade_ok(p)) {
        w->error = 1;
        return;
    }
    start(w, P10_PKT_THROW_GRENADE);
    p10_write_vec3(w, p->origin);
    p10_write_vec3(w, p->velocity);
    p10_write_f32(w, p->fuse);
}

int p10_decode_throw_grenade(const uint8_t* data, size_t size, p10_throw_grenade* p)
{
    p10_reader r;
    if (!begin(&r, data, size, P10_PKT_THROW_GRENADE))
        return 0;
    p->origin   = p10_read_vec3(&r);
    p->velocity = p10_read_vec3(&r);
    p->fuse     = p10_read_f32(&r);
    return p10_at_end(&r) && grenade_ok(p);
}
