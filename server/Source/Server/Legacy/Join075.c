// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The 0.75 join (docs/plan-075-server.md 8.2), from ba72777^'s Player.c
 * on_player_update and MapStart.c / MapChunk.c / StateData.c:
 *
 *   STATE_STARTING_MAP  (a new connection, or everyone on a map reset)
 *       MapStart, every MapChunk, VersionRequest, then the joining data
 *       (ExistingPlayer of everyone in the game, StateData, CreatePlayer of
 *       the living), all in this one tick, and STATE_JOINING
 *   STATE_JOINING       the team screen, until the client's ExistingPlayer
 *       (game_player_join) moves it to STATE_SPAWNING
 *
 * The original spread the map over two ticks and buffered every block
 * action of the download for the joiner (the bug CONTRIBUTING.md lists was the
 * buffer on the wrong player). Sent in one tick on the one reliable channel,
 * everything after the snapshot reaches the client after its state data, in
 * order, and an edit the snapshot already holds arrives as the same block
 * again: there is nothing to buffer. It also keeps the client out of
 * JOINING (the ZeroSpades extensions' "on the map") while it downloads.
 *
 * Bots never come here (no conn): Player.c walks them through the same
 * states without a map.
 */
#include <Server/Legacy/Wire075.h>

#include <Server/EventLog.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Player.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Compress.h>
#include <Util/Nanos.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Compress.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Queue.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Server/MapStore.h>
#include <stdlib.h>
#include <string.h>

int legacy_past_state_data(const player_t* player)
{
    /* the conn first: PROTOCOL_075 (0) is also every bot's value */
    if (player->conn == NULL || player->protocol != PROTOCOL_075) {
        return 0;
    }
    return player->state == STATE_JOINING || player->state == STATE_SPAWNING ||
           player->state == STATE_WAITING_FOR_RESPAWN || player->state == STATE_READY;
}

void legacy_on_connect(server_t* server, player_t* player, uint32_t connect_data)
{
    (void) server;
    char address[64];
    player_address(player, address, sizeof address);
    evt_log(player, "wire", "profile=075 action=connect id=%u address=%s version=%s", (unsigned) player->id, address,
            connect_data == VERSION_0_76 ? "0.76" : "0.75");
}

/* The map as compressed for the last join, kept while the map is the same
   (lsd's pristine copy): at a map start everyone joins at once, and each
   join wrote and deflated the whole map on the tick */
static queue_t* g_map_zlib;
static uint64_t g_map_zlib_version;
static size_t   g_map_zlib_vxl;

static queue_t* _map_zlib(server_t* server, size_t* vxl_bytes, int* cached)
{
    map_store_t* map = &server->s_map.map;
    *cached          = g_map_zlib != NULL && g_map_zlib_version == map->version;
    if (!*cached) {
        compress_queue_free(&g_map_zlib);
        g_map_zlib         = compress_map(map, DEFAULT_COMPRESS_CHUNK_SIZE, &g_map_zlib_vxl);
        g_map_zlib_version = map->version;
    }
    *vxl_bytes = g_map_zlib_vxl;
    return g_map_zlib;
}

/* MapStart and the zlib VXL in chunks (MapStart.c, MapChunk.c) */
static int _send_map(server_t* server, player_t* player)
{
    uint64_t began = get_real_nanos();
    size_t   size;
    int      cached;
    queue_t* queue = _map_zlib(server, &size, &cached);
    if (queue == NULL) {
        LOG_ERROR("0.75: could not compress the map for %s (#%hhu)", player->name, player->id);
        return 0;
    }
    double ms = (double) (get_real_nanos() - began) / 1e6;

    uint32_t compressed = 0;
    int      chunks     = 0;
    queue_t* node;
    DL_FOREACH(queue, node)
    {
        compressed += node->length;
    }

    LOG_INFO("Sending map info to %s (#%hhu)", player->name, player->id);
    uint8_t  start[5];
    stream_t s = {start, sizeof start, 0};
    stream_write_u8(&s, PACKET_TYPE_MAP_START);
    stream_write_u32(&s, compressed);
    int ok = legacy_send(player, start, sizeof start) == 0;

    LOG_INFO("Sending map chunks to %s (#%hhu)", player->name, player->id);
    uint8_t* chunk = (uint8_t*) malloc(DEFAULT_COMPRESS_CHUNK_SIZE + 1);
    DL_FOREACH(queue, node)
    {
        if (ok && chunk != NULL && node->length <= DEFAULT_COMPRESS_CHUNK_SIZE) {
            chunk[0] = PACKET_TYPE_MAP_CHUNK;
            memcpy(chunk + 1, node->block, node->length);
            ok = legacy_send(player, chunk, node->length + 1) == 0;
            chunks++;
        } else if (ok) {
            ok = 0;
            LOG_ERROR("0.75: a map chunk of %u bytes did not fit", (unsigned) node->length);
        }
    }
    free(chunk);
    LOG_INFO("Finished sending map chunks to %s (#%hhu)", player->name, player->id);
    /* cached and ms: a log of this machine, kept off the EVT line, which
       tools/bench.py check compares */
    LOG_INFO("0.75: map for %s (#%hhu) %s in %.1f ms", player->name, player->id, cached ? "kept" : "compressed",
             ms);
    evt_log(player, "wire", "profile=075 action=map_sent bytes=%u chunks=%d vxl=%zu ok=%d", (unsigned) compressed,
            chunks, size, ok);
    return ok;
}

static void _send_version_request(player_t* player)
{
    uint8_t id = PACKET_TYPE_VERSION_REQUEST;
    legacy_send(player, &id, 1);
}

/* send_joining_data of ba72777^'s Player.c */
static void _send_joining_data(server_t* server, player_t* player)
{
    player_t *receiver, *tmp;
    LOG_INFO("Sending state to %s (#%hhu)", player->name, player->id);
    HASH_ITER(hh, server->players, receiver, tmp)
    {
        if (receiver != player && receiver->id < LEGACY_MAX_PLAYERS && is_past_join_screen(receiver)) {
            legacy_send_existing_player(server, player, receiver);
        }
    }
    legacy_send_state_data(server, player);

    /* the living must be announced with CreatePlayer: it is the packet that
       carries a position, ExistingPlayer does not, and a join that only saw
       ExistingPlayer placed every bot at the origin. The original did it for
       bots only; a human standing still sends no position either */
    HASH_ITER(hh, server->players, receiver, tmp)
    {
        if (receiver != player && receiver->id < LEGACY_MAX_PLAYERS && receiver->state == STATE_READY &&
            receiver->alive)
        {
            legacy_send_create_player(server, player, receiver);
        }
    }
}

int legacy_join_update(server_t* server, player_t* player)
{
    if (!profile_is_075() || player->conn == NULL || player->protocol != PROTOCOL_075) {
        return 0;
    }
    switch (player->state) {
        case STATE_STARTING_MAP:
            if (!_send_map(server, player)) {
                return 1; /* tried again next tick */
            }
            _send_version_request(player);
            _send_joining_data(server, player);
            player->state = STATE_JOINING;
            /* every map, not the first only: the extensions offer again */
            ext075_on_join(server, player);
            return 1;
        case STATE_JOINING:
            return 1; /* the team screen: the client's ExistingPlayer ends it */
        default:
            return 0;
    }
}
