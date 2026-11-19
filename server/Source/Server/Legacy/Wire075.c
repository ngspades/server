// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The 0.75 senders of the 0.75 profile (Wire075.h). Each one is the sender
 * ba72777 / da14df7 removed (git show ba72777^:server/Source/Server/Packets/
 * <file>), with the same packet id, field order and recipients, moved from
 * enet_peer_send onto conn_send: reliable on channel 0 (the 0.75 protocol has
 * one channel), counted by the network statistics, and never to a bot
 * (conn == NULL: sending to a NULL peer was the historic crash).
 */
#include <Server/Legacy/Wire075.h>

#include <Server/BotPlanner.h>
#include <Server/EventLog.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Net/Conn.h>
#include <Server/Profile.h>
#include <Server/Proto10/GameMode.h>
#include <Server/Proto10/NetStats.h>
#include <Server/Proto10/World.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/DataStream.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <Server/MapStore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- transport ---- */

int legacy_send(player_t* receiver, const uint8_t* data, size_t len)
{
    if (receiver == NULL || receiver->conn == NULL) {
        return -1; /* a bot */
    }
    return conn_send_reliable(receiver->conn, data, len);
}

void legacy_broadcast(server_t* server, player_t* except, const uint8_t* data, size_t len)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != except && legacy_past_state_data(p)) {
            legacy_send(p, data, len);
        }
    }
}

static stream_t _stream(uint8_t* buf, size_t size)
{
    stream_t s = {buf, (uint32_t) size, 0};
    return s;
}

/* ---- players ---- */

void legacy_send_create_player(server_t* server, player_t* receiver, player_t* child)
{
    if (server->protocol.num_players == 0 || receiver->conn == NULL) {
        return;
    }
    uint8_t  buf[32];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_CREATE_PLAYER);
    stream_write_u8(&s, child->id);                      // ID
    stream_write_u8(&s, child->weapon);                  // WEAPON
    stream_write_u8(&s, child->team);                    // TEAM
    stream_write_vector3f(&s, child->movement.position); // X Y Z
    stream_write_array(&s, child->name, 16);             // NAME
    if (legacy_send(receiver, buf, sizeof buf) != 0) {
        LOG_WARNING("Failed to send player state");
    }
}

void legacy_send_existing_player(server_t* server, player_t* receiver, player_t* existing)
{
    if (server->protocol.num_players == 0 || receiver->conn == NULL) {
        return;
    }
    uint8_t  buf[28];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_EXISTING_PLAYER);
    stream_write_u8(&s, existing->id);                               // ID
    stream_write_u8(&s, existing->team);                             // TEAM
    stream_write_u8(&s, existing->weapon);                           // WEAPON
    stream_write_u8(&s, existing->item);                             // HELD ITEM
    stream_write_u32(&s, existing->kills);                           // KILLS
    stream_write_color_rgb(&s, existing->tool_color);                // COLOR
    stream_write_array(&s, existing->name, PLAYER_NAME_STRLEN);      // NAME
    if (legacy_send(receiver, buf, sizeof buf) != 0) {
        LOG_WARNING("Failed to send player state");
    }
}

/* the spawn: every client past the state data, the player's own included
   (that is how a 0.75 client learns where it spawned) */
void legacy_send_respawn(server_t* server, player_t* player)
{
    player_t *receiver, *tmp;
    HASH_ITER(hh, server->players, receiver, tmp)
    {
        if (legacy_past_state_data(receiver)) {
            legacy_send_create_player(server, receiver, player);
        }
    }
    /* Create Player turns the light off on every client (Flashlight) */
    ext075_light_off(player->id);
    ext075_player_changed(server, player);
}

void legacy_send_state_data(server_t* server, player_t* player)
{
    if (server->protocol.num_players == 0 || player->conn == NULL) {
        return;
    }
    gamemode_vars_t* gm = &server->protocol.gamemode;
    /* 104 as the original: the CTF state fills 84 of them, the rest is 0 */
    uint8_t  buf[104];
    stream_t s = _stream(buf, sizeof buf);
    memset(buf, 0, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_STATE_DATA);
    stream_write_u8(&s, player->id);
    stream_write_color_rgb(&s, server->protocol.color_fog);
    stream_write_color_rgb(&s, server->protocol.color_team[0]);
    stream_write_color_rgb(&s, server->protocol.color_team[1]);
    stream_write_array(&s, server->protocol.name_team[0], 10);
    stream_write_array(&s, server->protocol.name_team[1], 10);
    if (server->protocol.current_gamemode == 0 || server->protocol.current_gamemode == 1) {
        stream_write_u8(&s, server->protocol.current_gamemode);
    } else {
        stream_write_u8(&s, 0);
    }

    // MODE CTF:
    stream_write_u8(&s, gm->score[0]);    // SCORE TEAM A
    stream_write_u8(&s, gm->score[1]);    // SCORE TEAM B
    stream_write_u8(&s, gm->score_limit); // SCORE LIMIT

    /* bit 0: team 1's intel is carried (by team 2), bit 1: team 2's. The
       original code set the bits the other way round and wrote the carrier
       when the bit was clear, so a joiner read both intels as carried and
       their positions from the 255 padding; the clients (BetterSpades'
       read_PacketStateData, pyspades' CTFState) read it this way */
    uint8_t flags = (uint8_t) ((gm->intel_held[0] ? INTEL_TEAM_A : 0) | (gm->intel_held[1] ? INTEL_TEAM_B : 0));
    stream_write_u8(&s, flags); // INTEL FLAGS
    for (int team = 0; team < 2; team++) {
        if (gm->intel_held[team]) {
            /* the carrier of team's intel is on the other team */
            stream_write_u8(&s, gm->player_intel_team[team ^ 1]);
            for (int i = 0; i < 11; ++i) {
                stream_write_u8(&s, 255);
            }
        } else {
            stream_write_vector3f(&s, gm->intel[team]);
        }
    }
    stream_write_vector3f(&s, gm->base[0]);
    stream_write_vector3f(&s, gm->base[1]);

    legacy_send(player, buf, sizeof buf);
    evt_log(player, "wire", "profile=075 action=state_data id=%u", (unsigned) player->id);
}

static uint64_t wu_bytes, wu_packets, wu_logged_at;

void legacy_send_world_update(server_t* server, player_t* player)
{
    if (server->protocol.num_players == 0 || player->conn == NULL || !legacy_past_state_data(player)) {
        return;
    }
    /* 32 slots, the 0.75 client's: an entry the receiver must not see is
       zeroes (the clients keep the last known position for it) */
    uint8_t  buf[1 + LEGACY_MAX_PLAYERS * 24];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_WORLD_UPDATE);
    player_t* connected_player;
    for (uint8_t player_id = 0; player_id < LEGACY_MAX_PLAYERS; ++player_id) {
        HASH_FIND(hh, server->players, &player_id, sizeof(player_id), connected_player);
        if ((connected_player != NULL && connected_player->state != STATE_DISCONNECTED) &&
            player_to_player_visibile(player, connected_player) && connected_player->is_invisible == 0)
        {
            stream_write_vector3f(&s, connected_player->movement.position);
            stream_write_vector3f(&s, connected_player->movement.forward_orientation);
        } else {
            vector3f_t empty       = {0};
            vector3f_t orientation = {1.0f, 0.0f, 0.0f};
            stream_write_vector3f(&s, empty);
            stream_write_vector3f(&s, orientation);
        }
    }
    /* unreliable, as the original (a lost snapshot is replaced by the next
       one; over WebRTC channel 0 is the reliable DataChannel anyway) */
    if (conn_send(player->conn, 0, buf, sizeof buf, 0) == 0) {
        proto10_netstats_add(buf, sizeof buf);
        wu_bytes += sizeof buf;
        wu_packets++;
    }
    uint64_t now = get_real_nanos();
    if (wu_logged_at == 0) {
        wu_logged_at = now;
    } else if (now - wu_logged_at >= 10ULL * NANO_IN_SECOND) {
        evt_log(NULL, "wire", "profile=075 action=world_update bytes=%llu packets=%llu seconds=%.1f",
                (unsigned long long) wu_bytes, (unsigned long long) wu_packets,
                (double) (now - wu_logged_at) / (double) NANO_IN_SECOND);
        wu_bytes = wu_packets = 0;
        wu_logged_at          = now;
    }
}

void legacy_send_player_left(server_t* server, player_t* player)
{
    if (server->protocol.num_players == 0) {
        ext075_player_left(server, player->id);
        return;
    }
    uint8_t buf[2] = {PACKET_TYPE_PLAYER_LEFT, player->id};
    player_t *connected_player, *tmp;
    HASH_ITER(hh, server->players, connected_player, tmp)
    {
        if (connected_player->id != player->id && legacy_past_state_data(connected_player)) {
            if (legacy_send(connected_player, buf, sizeof buf) != 0) {
                LOG_WARNING("Failed to send player left event");
            }
        }
    }
    /* the slot is free: forget what it agreed to */
    ext075_player_left(server, player->id);
}

/* ---- tools, input, weapons ---- */

void legacy_send_set_tool(server_t* server, player_t* player, uint8_t tool)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    uint8_t buf[3] = {PACKET_TYPE_SET_TOOL, player->id, tool};
    legacy_broadcast(server, player, buf, sizeof buf);
}

static void _set_color_packet(uint8_t* buf, uint8_t id, color_t color)
{
    stream_t s = _stream(buf, 5);
    stream_write_u8(&s, PACKET_TYPE_SET_COLOR);
    stream_write_u8(&s, id);
    stream_write_color_rgb(&s, color);
}

void legacy_send_set_color(server_t* server, player_t* player, color_t color)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    uint8_t buf[5];
    _set_color_packet(buf, player->id, color);
    legacy_broadcast(server, player, buf, sizeof buf);
}

void legacy_send_weapon_input(server_t* server, player_t* player, uint8_t input)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    if (player->sprinting) {
        input = 0;
    }
    uint8_t buf[3] = {PACKET_TYPE_WEAPON_INPUT, player->id, input};
    legacy_broadcast(server, player, buf, sizeof buf);
}

void legacy_send_input_data(server_t* server, player_t* player)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    uint8_t buf[3] = {PACKET_TYPE_INPUT_DATA, player->id, player->input};
    /* send_packet_except_sender_dist_check: the ones in range, and spectators */
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != player && legacy_past_state_data(p) &&
            (player_to_player_visibile(player, p) || p->team == TEAM_SPECTATOR))
        {
            legacy_send(p, buf, sizeof buf);
        }
    }
}

void legacy_send_weapon_reload(server_t* server, player_t* player, uint8_t start_animation)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    uint8_t buf[4] = {PACKET_TYPE_WEAPON_RELOAD, player->id, player->weapon_clip, (uint8_t) player->weapon_reserve};
    if (start_animation) {
        legacy_broadcast(server, player, buf, sizeof buf);
    } else if (player->conn != NULL && is_past_join_screen(player)) {
        legacy_send(player, buf, sizeof buf);
        ext075_player_changed(server, player);
    }
}

void legacy_send_position(player_t* player, float x, float y, float z)
{
    if (player->conn == NULL) {
        return;
    }
    uint8_t  buf[13];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_POSITION_DATA);
    stream_write_f(&s, x);
    stream_write_f(&s, y);
    stream_write_f(&s, z);
    legacy_send(player, buf, sizeof buf);
}

void legacy_send_restock(player_t* player)
{
    if (player->conn == NULL) {
        return;
    }
    uint8_t buf[2] = {PACKET_TYPE_RESTOCK, player->id};
    legacy_send(player, buf, sizeof buf);
}

void legacy_send_fog(server_t* server, player_t* to, color_t color)
{
    /* FogColor: alpha, blue, green, red */
    uint8_t buf[5] = {PACKET_TYPE_FOG_COLOR, 0, color.b, color.g, color.r};
    if (to == NULL) {
        legacy_broadcast(server, NULL, buf, sizeof buf);
    } else if (legacy_past_state_data(to)) {
        legacy_send(to, buf, sizeof buf);
    }
}

void legacy_send_grenade(server_t* server, player_t* thrower, float fuse, vector3f_t position, vector3f_t velocity)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    uint8_t  buf[30];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_GRENADE_PACKET);
    stream_write_u8(&s, thrower->id);
    stream_write_f(&s, fuse);
    stream_write_f(&s, position.x);
    stream_write_f(&s, position.y);
    stream_write_f(&s, position.z);
    stream_write_f(&s, velocity.x);
    stream_write_f(&s, velocity.y);
    stream_write_f(&s, velocity.z);
    legacy_broadcast(server, thrower, buf, sizeof buf);
    ext075_player_changed(server, thrower);
}

/* ---- combat ---- */

void legacy_send_set_hp(player_t* attacker, player_t* victim, uint8_t type_of_damage, uint8_t is_grenade,
                        vector3f_t position)
{
    if (victim->conn == NULL) {
        return;
    }
    uint8_t  buf[15];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_SET_HP);
    stream_write_u8(&s, (uint8_t) victim->hp);
    stream_write_u8(&s, type_of_damage);
    if (type_of_damage != 0 && is_grenade == 0 && attacker != NULL) {
        stream_write_vector3f(&s, attacker->movement.position);
    } else if (type_of_damage != 0 && is_grenade == 1) {
        stream_write_vector3f(&s, position);
    } else {
        vector3f_t zero = {0, 0, 0};
        stream_write_vector3f(&s, zero);
    }
    legacy_send(victim, buf, sizeof buf);
}

void legacy_send_kill_action(server_t* server,
                             player_t* killer,
                             player_t* victim,
                             uint8_t   reason,
                             uint8_t   respawn_time,
                             uint8_t   make_invisible)
{
    uint8_t buf[5] = {PACKET_TYPE_KILL_ACTION,
                      victim->id,                                    // Player that died.
                      killer != NULL ? killer->id : victim->id,      // Player that killed.
                      reason,                                        // Killing reason (1 is headshot)
                      respawn_time};                                 // Time before respawn happens
    player_t *connected_player, *tmp;
    HASH_ITER(hh, server->players, connected_player, tmp)
    {
        uint8_t is_past = (uint8_t) legacy_past_state_data(connected_player);
        if ((make_invisible && connected_player->id != victim->id && is_past) || (is_past && !make_invisible)) {
            legacy_send(connected_player, buf, sizeof buf);
        }
    }
    /* Kill Action turns the light off on every client (Flashlight); the
       victim missed it when made invisible, so it is told apart */
    if (make_invisible && ext075_light_on(victim->id)) {
        ext075_set_light(server, victim, 0, "invisible_death");
    }
    ext075_light_off(victim->id);
    ext075_player_changed(server, victim);
    if (killer != NULL && killer != victim) {
        ext075_player_changed(server, killer);
    }
}

/* ---- chat and notices (CP437 inside the server; UTF-8 behind 0xFF to a
   client that reads it, Ext075.c's ext075_chat_packet) ---- */

void legacy_send_chat(server_t* server, player_t* sender, uint8_t meant_for, const char* message)
{
    /* 255 bytes of text with its NUL, as the original relayed it */
    uint8_t buf[3 + 1 + 255];
    player_t *connected_player, *tmp;
    HASH_ITER(hh, server->players, connected_player, tmp)
    {
        if (is_past_join_screen(connected_player) && legacy_past_state_data(connected_player) &&
            ((connected_player->team == sender->team && meant_for == TEAM_B) || meant_for == TEAM_A))
        {
            size_t len = ext075_chat_packet(buf, sizeof buf, connected_player, sender->id, meant_for, message, 1);
            legacy_send(connected_player, buf, len);
        }
    }
}

void legacy_send_notice(player_t* player, uint8_t type, const char* message)
{
    if (player == NULL || player->conn == NULL) {
        return;
    }
    uint8_t buf[1100];
    size_t  len = ext075_chat_packet(buf, sizeof buf, player, player->id, type, message, 0);
    legacy_send(player, buf, len);
}

void legacy_broadcast_notice(server_t* server, uint8_t type, const char* message)
{
    uint8_t buf[1100];
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (is_past_join_screen(player) && legacy_past_state_data(player)) {
            size_t len = ext075_chat_packet(buf, sizeof buf, player, 33, type, message, 0);
            legacy_send(player, buf, len);
        }
    }
}

/* ---- the intel and the tents ---- */

static void _send_move_object(server_t* server, uint8_t object, uint8_t team, vector3f_t pos)
{
    if (server->protocol.num_players == 0) {
        return;
    }
    evt_log(NULL, "wire", "profile=075 action=move_object object=%u team=%u at=%.0f,%.0f,%.0f", object, team, pos.x,
            pos.y, pos.z);
    uint8_t  buf[15];
    stream_t s = _stream(buf, sizeof buf);
    stream_write_u8(&s, PACKET_TYPE_MOVE_OBJECT);
    stream_write_u8(&s, object);
    stream_write_u8(&s, team);
    stream_write_f(&s, pos.x);
    stream_write_f(&s, pos.y);
    stream_write_f(&s, pos.z);
    legacy_broadcast(server, NULL, buf, sizeof buf);
}

/* what the clients were last told of each intel (0, 1) and tent (2, 3): the
   CTF code moves them in many places (a block under one, a capture, Babel's
   tower), each of which used to call send_move_object; a change found once a
   tick tells them all, without a hook in the mode */
static vector3f_t legacy_objects[4];
static uint8_t    legacy_objects_known;
/* the carrier of each team's intel, from its pickup: the drop event comes
   after the mode cleared player_intel_team */
static uint8_t legacy_carrier[2] = {0xFF, 0xFF};

static int _same(vector3f_t a, vector3f_t b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static void _objects_tick(server_t* server)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    vector3f_t       now[4] = {gm->intel[0], gm->intel[1], gm->base[0], gm->base[1]};
    if (!legacy_objects_known) {
        memcpy(legacy_objects, now, sizeof now);
        legacy_objects_known = 1;
        return;
    }
    for (int k = 0; k < 4; k++) {
        if (_same(now[k], legacy_objects[k])) {
            continue;
        }
        /* a carried intel is drawn on its carrier */
        if (k < 2 && gm->intel_held[k]) {
            continue;
        }
        legacy_objects[k] = now[k];
        _send_move_object(server, (uint8_t) k, (uint8_t) (k & 1), now[k]);
    }
}

void legacy_intel_event(server_t* server, uint8_t team, int event)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    if (team > 1 || server->protocol.num_players == 0) {
        return;
    }
    uint8_t carrier_team = team ^ 1;
    switch (event) {
        case P10_GM_EVENT_PICKUP:
        {
            uint8_t id           = gm->player_intel_team[carrier_team];
            legacy_carrier[team] = id;
            uint8_t buf[2]       = {PACKET_TYPE_INTEL_PICKUP, id};
            legacy_broadcast(server, NULL, buf, sizeof buf);
            break;
        }
        case P10_GM_EVENT_CAPTURE:
        {
            uint8_t id      = gm->player_intel_team[carrier_team];
            uint8_t winning = gm->score[carrier_team] >= gm->score_limit;
            uint8_t buf[3]  = {PACKET_TYPE_INTEL_CAPTURE, id, winning};
            legacy_broadcast(server, NULL, buf, sizeof buf);
            legacy_carrier[team] = 0xFF;
            break;
        }
        case P10_GM_EVENT_DROP:
        {
            uint8_t  buf[14];
            stream_t s = _stream(buf, sizeof buf);
            stream_write_u8(&s, PACKET_TYPE_INTEL_DROP);
            stream_write_u8(&s, legacy_carrier[team]);
            stream_write_vector3f(&s, gm->intel[team]);
            legacy_broadcast(server, NULL, buf, sizeof buf);
            legacy_objects[team] = gm->intel[team];
            legacy_carrier[team] = 0xFF;
            break;
        }
        default:
            break;
    }
}

/* ---- the 1.0 GenericCommands that have a 0.75 packet ---- */

void legacy_generic(server_t* server, player_t* to, int n, const char* const* parts)
{
    if (n < 1) {
        return;
    }
    if (!strcmp(parts[0], "restock")) {
        if (to != NULL) {
            legacy_send_restock(to);
            ext075_player_changed(server, to);
        }
        return;
    }
    if (!strcmp(parts[0], "fog") && n >= 4) {
        color_t c;
        c.raw = 0;
        c.r   = (uint8_t) atoi(parts[1]);
        c.g   = (uint8_t) atoi(parts[2]);
        c.b   = (uint8_t) atoi(parts[3]);
        legacy_send_fog(server, to, c);
        return;
    }
    /* the rest are 1.0 inventions (perks, classes, squads, the host's menu):
       a 0.75 client has nothing to show them with */
    if (evt_verbose()) {
        evt_log(to, "wire", "profile=075 action=generic_dropped what=%s", parts[0]);
    }
}

/* ---- the terrain ---- */

/* A 0.75 client computes collapses and craters itself (it runs its own
   physics after each block it is told about), so it is told the cells a
   player changed, as BlockActions, and not the ones that fell because of
   them. The shared code only says "this cell changed" (proto10_world_cell),
   so the cells are kept for the tick and read back from the map then. */
typedef struct {
    int32_t x, y, z;
    uint8_t cause;
} legacy_cell_t;

static legacy_cell_t* legacy_cells;
static size_t         legacy_cell_count, legacy_cell_cap;

void legacy_world_cell(server_t* server, int x, int y, int z, uint8_t cause)
{
    map_store_t* map = &server->s_map.map;
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z) {
        return;
    }
    /* the bots' planner keeps its own copy of the map (BotPlanner.c): the
       1.0 body does this too, the dispatch replaces it */
    planner_cell(server, x, y, z);
    if (legacy_cell_count == legacy_cell_cap) {
        size_t         cap = legacy_cell_cap ? legacy_cell_cap * 2 : 1024;
        legacy_cell_t* c   = (legacy_cell_t*) realloc(legacy_cells, cap * sizeof *c);
        if (c == NULL) {
            LOG_ERROR("0.75: out of memory for terrain edits, 0.75 clients will miss one");
            return;
        }
        legacy_cells    = c;
        legacy_cell_cap = cap;
    }
    legacy_cell_t* c = &legacy_cells[legacy_cell_count++];
    c->x             = x;
    c->y             = y;
    c->z             = z;
    c->cause         = cause;
}

static int _cell_cmp(const void* a, const void* b)
{
    const legacy_cell_t* p = (const legacy_cell_t*) a;
    const legacy_cell_t* q = (const legacy_cell_t*) b;
    if (p->x != q->x)
        return p->x < q->x ? -1 : 1;
    if (p->y != q->y)
        return p->y < q->y ? -1 : 1;
    if (p->z != q->z)
        return p->z < q->z ? -1 : 1;
    return (int) q->cause - (int) p->cause; /* a player's cause first */
}

static void _block_action_packet(uint8_t* buf, uint8_t id, uint8_t action, int x, int y, int z)
{
    /* id + player + action + 3 * u32 = 15 (one byte short once silently
       dropped every placement) */
    stream_t s = _stream(buf, 15);
    stream_write_u8(&s, PACKET_TYPE_BLOCK_ACTION);
    stream_write_u8(&s, id);
    stream_write_u8(&s, action);
    stream_write_u32(&s, (uint32_t) x);
    stream_write_u32(&s, (uint32_t) y);
    stream_write_u32(&s, (uint32_t) z);
}

/* a built block takes its colour from the builder's on a 0.75 client: the
   player who holds that colour (the builder: a build is in its tool colour),
   else anybody in the game told the colour first and given its own back */
static player_t* _carrier_of(server_t* server, uint32_t color, int* exact)
{
    player_t *p, *tmp, *any = NULL;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->id >= LEGACY_MAX_PLAYERS || !is_past_join_screen(p)) {
            continue;
        }
        if ((p->tool_color.raw & 0xFFFFFFu) == (color & 0xFFFFFFu)) {
            *exact = 1;
            return p;
        }
        if (any == NULL) {
            any = p;
        }
    }
    *exact = 0;
    return any;
}

static void _flush_cells(server_t* server)
{
    if (legacy_cell_count == 0) {
        return;
    }
    int       receivers = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        receivers += legacy_past_state_data(p);
    }
    if (receivers == 0) {
        legacy_cell_count = 0;
        return;
    }
    qsort(legacy_cells, legacy_cell_count, sizeof *legacy_cells, _cell_cmp);
    map_store_t* map = &server->s_map.map;
    /* the cells that fell after a dig of this tick: the clients make them
       fall themselves (a plugin's clear in the same tick is the one miss) */
    int player_dug = 0;
    for (size_t i = 0; i < legacy_cell_count; i++) {
        const legacy_cell_t* c = &legacy_cells[i];
        if (c->cause == P10_WORLD_CAUSE_PLAYER && !map_solid(map, c->x, c->y, c->z)) {
            player_dug = 1;
            break;
        }
    }
    int     built = 0, destroyed = 0, skipped = 0;
    uint8_t buf[15];
    for (size_t i = 0; i < legacy_cell_count; i++) {
        const legacy_cell_t* c = &legacy_cells[i];
        if (i > 0 && legacy_cells[i - 1].x == c->x && legacy_cells[i - 1].y == c->y && legacy_cells[i - 1].z == c->z) {
            continue; /* sorted: the first of a cell has the player's cause */
        }
        if (map_solid(map, c->x, c->y, c->z)) {
            uint32_t  color = map_color(map, c->x, c->y, c->z);
            int       exact = 0;
            player_t* by    = _carrier_of(server, color, &exact);
            color_t   cc;
            cc.raw = color;
            HASH_ITER(hh, server->players, p, tmp)
            {
                if (!legacy_past_state_data(p)) {
                    continue;
                }
                player_t* carrier = by != NULL ? by : p;
                uint8_t   col[5];
                _set_color_packet(col, carrier->id, cc);
                legacy_send(p, col, sizeof col);
                _block_action_packet(buf, carrier->id, BLOCKACTION_BUILD, c->x, c->y, c->z);
                legacy_send(p, buf, sizeof buf);
                if (!exact) {
                    _set_color_packet(col, carrier->id, carrier->tool_color);
                    legacy_send(p, col, sizeof col);
                }
            }
            built++;
        } else if (c->cause == P10_WORLD_CAUSE_NONE && player_dug) {
            skipped++;
        } else {
            HASH_ITER(hh, server->players, p, tmp)
            {
                if (legacy_past_state_data(p)) {
                    _block_action_packet(buf, p->id, BLOCKACTION_DESTROY_ONE, c->x, c->y, c->z);
                    legacy_send(p, buf, sizeof buf);
                }
            }
            destroyed++;
        }
    }
    if (evt_verbose() || built + destroyed > 1) {
        evt_log(NULL, "wire", "profile=075 action=terrain built=%d destroyed=%d collapsed=%d receivers=%d", built,
                destroyed, skipped, receivers);
    }
    legacy_cell_count = 0;
}

void legacy_world_tick(server_t* server)
{
    _flush_cells(server);
    _objects_tick(server);
}
