// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Flood.h>
#include <Server/Proto10/NetStats.h>
#include <Server/EventLog.h>
#include <Server/Net/Master.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/GameMode.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Sentry.h>
#include <Server/Proto10/Session.h>
#include <Server/Proto10/World.h>
#include <Server/Sky.h>
#include <Server/Outfit.h>
#include <Server/Profile.h>
#include <Util/Compress.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Queue.h>
#include <uthash/utlist.h>
#include <enet/enet.h>
#include <errno.h>
#include <ngmap/ngmap.h>
#include <proto10/codec.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <Util/Physics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* handshake deadlines (docs/protocol-1.0.md 5.3). The reference server
   waited 10 s and 60 s; a client answers both in milliseconds, and an
   unfinished handshake holds a player slot all that time (a few hundred
   forged connects filled a server), so they are short */
#define P10_TIMEOUT_INITIATE (5ull * NANO_IN_SECOND)
#define P10_TIMEOUT_CERT     (10ull * NANO_IN_SECOND)
#define P10_TIMEOUT_MAP      (300ull * NANO_IN_SECOND)
/* how long the GameStateHeader waits for the client's "map-cached" (7.1):
   real time, as every join deadline; a client that never answers gets the
   map, so this only costs a join that does not speak the command 3 s */
#define P10_TIMEOUT_MAP_ANSWER (3ull * NANO_IN_SECOND)

/* the map fragments keep what the connection still holds under 64 KiB: the
   reference's figure, what its `GetPendingBytes` was meant to read before it
   always said 0 (D9). A fixed count is wrong both ways: a small one leaves a
   fast link idle for minutes on a big map, and a large one buries a slow one
   under a queue it cannot drain (docs/protocol-1.0.md 7.2) */
#define P10_MAP_PENDING_MAX (64 * 1024)
/* and a cap on the same loop: a transport only flushes on its next service
   (the next tick), so `pending` does not grow while this loop runs and one
   tick would otherwise write the whole map. 64 x 8 KiB: never megabytes */
#define P10_MAP_FRAGMENTS_MAX 64

#define P10_SERVER_NONCE_LEN 128

/* the quality every 1.0 join gets, whatever its mapQuality asked: the
   map's stored stream, the maps repository's ngmap q100, sent as it is. It
   is smaller than the q80 encoding the server made of it at every new map
   (Raindance 648 against 715 KB) and has the colours exactly, and sending
   it costs no CPU (docs/map-memory.md) */
#define P10_NGMAP_SERVED 100
/* encodings kept: the stored stream, and one made after its edit log
   overflowed */
#define P10_NGMAP_CACHE_SLOTS 4
/* a cached encoding stays usable while the edits made since its snapshot fit
   here: replaying a few thousand cells as TerrainUpdate is far cheaper than
   encoding again (~1-2 s of CPU on a big map) */
#define P10_NGMAP_CACHE_MAX_EDITS 4096
/* a running encoding logs every edit (it lasts seconds); past this the log is
   dropped and its joiners start over with a new snapshot */
#define P10_NGMAP_JOB_MAX_EDITS (1u << 20)

static const char* _state_name(uint8_t state)
{
    switch (state) {
        case P10_STATE_NOT_INITIATED:
            return "not_initiated";
        case P10_STATE_WAITING_CERT:
            return "waiting_cert";
        case P10_STATE_PREPARING_MAP:
            return "preparing_map";
        case P10_STATE_WAITING_MAP_ANSWER:
            return "waiting_map_answer";
        case P10_STATE_MAP_TRANSFER:
            return "map_transfer";
        case P10_STATE_COMPLETING_MAP:
            return "completing_map";
        case P10_STATE_GAME:
            return "game";
        case P10_STATE_CLOSING:
            return "closing";
        default:
            return "unknown";
    }
}

static void _set_state(player_t* player, uint8_t state, uint64_t timeout)
{
    player->p10_state    = state;
    player->p10_deadline = timeout ? get_real_nanos() + timeout : 0;
    evt_log(player, "proto10", "state=%s", _state_name(state));
}

static void _send(player_t* player, const p10_writer* w)
{
    if (player->conn == NULL || w->error) {
        LOG_ERROR("proto10: could not build a packet for %s (#%hhu)", player->name, player->id);
        return;
    }
    conn_send_reliable(player->conn, w->data, w->size);
}

/* every refusal says why (CONTRIBUTING.md, "Logging") */
static void _disconnect(player_t* player, p10_disconnect_reason reason, const char* why)
{
    evt_log(player, "proto10", "disconnect reason=%d why=%s", (int) reason, why);
    LOG_WARNING("proto10: disconnecting %s (#%hhu): %s", player->name, player->id, why);
    player->p10_state    = P10_STATE_CLOSING;
    player->p10_deadline = 0;
    if (player->conn != NULL) {
        conn_disconnect(player->conn, (uint32_t) reason, CONN_CLOSE);
    }
}

static void _kick(player_t* player, const char* reason)
{
    uint8_t    buf[512];
    p10_writer w;
    p10_kick   kick = {p10_bytes_from_cstring(reason)};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_kick(&w, &kick);
    _send(player, &w);
    evt_log(player, "proto10", "kick why=%s", reason);
    player->p10_state    = P10_STATE_CLOSING;
    player->p10_deadline = 0;
    /* later: the Kick text has to arrive before the disconnect */
    conn_disconnect(player->conn, P10_DISCONNECT_MISC, CONN_CLOSE_LATER);
}

static void _random_bytes(uint8_t* out, size_t n)
{
    /* the nonce only matters once certificates are validated; until then
       /dev/urandom is plenty and a time-seeded fallback keeps it working */
    FILE* f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) {
            return;
        }
    }
    uint64_t x = get_real_nanos() | 1;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = (uint8_t) x;
    }
}

void proto10_begin(server_t* server, player_t* player)
{
    (void) server;
    uint8_t      nonce[P10_SERVER_NONCE_LEN];
    uint8_t      buf[P10_SERVER_NONCE_LEN + 16];
    p10_writer   w;
    p10_greeting greeting;

    _random_bytes(nonce, sizeof nonce);
    _Static_assert(sizeof player->p10_nonce == P10_SERVER_NONCE_LEN, "the token checks the nonce sent");
    memcpy(player->p10_nonce, nonce, sizeof nonce);
    greeting.nonce.data = nonce;
    greeting.nonce.len  = sizeof nonce;
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_greeting(&w, &greeting);
    _send(player, &w);
    player->p10_map_quality = 0;
    player->p10_map_job     = NULL;
    _set_state(player, P10_STATE_NOT_INITIATED, P10_TIMEOUT_INITIATE);
}

static void _on_initiate(server_t* server, player_t* player, const uint8_t* data, size_t size)
{
    (void) server;
    p10_initiate_connection in;
    if (!p10_decode_initiate_connection(data, size, &in)) {
        _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed InitiateConnection");
        return;
    }
    if (!p10_bytes_equal(in.protocol_name, P10_PROTOCOL_NAME)) {
        char name[64];
        if (!p10_bytes_copy(in.protocol_name, name, sizeof name)) {
            snprintf(name, sizeof name, "(%zu bytes)", in.protocol_name.len);
        }
        evt_log(player, "proto10", "protocol_mismatch got=\"%s\"", name);
        _disconnect(player, P10_DISCONNECT_PROTOCOL_MISMATCH, "protocol name mismatch");
        return;
    }

    /* names must be valid UTF-8 and fit the 0.75 name buffer; a bad one is
       replaced instead of refused so a misconfigured client can still play */
    char name[PLAYER_NAME_STRLEN + 1];
    if (in.player_name.len == 0 || !p10_utf8_valid(in.player_name) ||
        !p10_bytes_copy(in.player_name, name, sizeof name))
    {
        evt_log(player, "proto10", "bad_name len=%zu replaced=Deuce", in.player_name.len);
        snprintf(name, sizeof name, "Deuce");
    }
    snprintf(player->name, sizeof player->name, "%s", name);

    char package[64], locale[32];
    if (!p10_bytes_copy(in.package, package, sizeof package)) {
        snprintf(package, sizeof package, "?");
    }
    if (!p10_bytes_copy(in.locale, locale, sizeof locale)) {
        snprintf(locale, sizeof locale, "?");
    }
    LOG_INFO("proto10: %s (#%hhu) uses %s, protocol %hu.%hu, locale %s",
             player->name,
             player->id,
             package,
             in.major,
             in.minor,
             locale);
    evt_log(player,
            "proto10",
            "initiate version=%hu.%hu.%hu package=\"%s\" locale=%s map_quality=%hhu",
            in.major,
            in.minor,
            in.revision,
            package,
            locale,
            in.map_quality);
    player->p10_map_quality = in.map_quality;

    /* no server certificate yet (docs/protocol-1.0.md 6.3) */
    uint8_t         buf[16];
    p10_writer      w;
    p10_certificate cert = {0, {NULL, 0}, {NULL, 0}};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_certificate(&w, P10_PKT_SERVER_CERTIFICATE, &cert);
    _send(player, &w);
    _set_state(player, P10_STATE_WAITING_CERT, P10_TIMEOUT_CERT);
}

/* ---- map transfer (docs/protocol-1.0.md 7) ----
 *
 * mapQuality 0 asks for the zlib VXL, 1..100 for ngmap at that quality
 * (clamped to 20..100). The VXL is compressed right here, as 0.75 did. ngmap
 * takes ~1-2 s to encode a big map, which must not stall the 60 Hz tick, so:
 *
 * - the map's blocks are copied (one memcpy) on the main thread, and the VXL
 *   is written from that copy and encoded on a detached thread (writing it on
 *   the tick took 31 ms on Busan_Port, a stall for everyone at every join);
 *   a join that finds the encoding cached or running takes no snapshot; the joiner waits in P10_STATE_PREPARING_MAP and nothing
 *   is sent to it yet, so if encoding fails it still gets a plain VXL
 *   transfer (a second GameStateHeader mid-download is not allowed);
 * - every edit made after the snapshot is logged on the encoding (and on the
 *   cache entry made from it) and replayed to the joiner as one TerrainUpdate
 *   batch right after its GameStateHeader; the client keeps it until the map
 *   is decoded, like the live TerrainUpdates of 7.3;
 * - joiners asking for the same quality share a running encoding, and the
 *   result is cached per (map, quality) while its edit log stays small. A new
 *   map (map_load) bumps the epoch, which drops the cache.
 *
 * Only the encoder thread touches vxl/out/err/encode_ns of its job until it
 * sets done; everything else is main thread (under the server lock).
 */

typedef struct {
    uint32_t* cells; /* P10_CELL_PACK */
    size_t    count, cap;
    uint8_t   overflow;
} p10_cell_log_t;

typedef struct p10_map_job {
    struct p10_map_job* next;
    uint32_t            epoch;
    int                 quality, width, height, depth;
    uint8_t*            vxl; /* written and freed by the thread */
    size_t              vxl_size;
    map_store_t            raw; /* the map's blocks at the snapshot, read by the thread */
    uint8_t*            out;
    size_t              out_size;
    int                 err;
    uint64_t            encode_ns;
    uint8_t             fail_for_test; /* SP_NGMAP_FAIL: exercise the VXL fallback */
    char                save[1200];    /* the map file's own encoding: kept for its next load */
    atomic_int          done;
    p10_cell_log_t      edits;
} p10_map_job_t;

typedef struct {
    uint8_t        used;
    uint8_t        stored; /* the map file's own stream, not an encoding of the live map */
    uint32_t       epoch;
    int            quality;
    uint8_t*       data;
    size_t         size, vxl_size;
    uint64_t       encode_ns, last_use;
    p10_cell_log_t edits;
} p10_map_cache_t;

static p10_map_job_t*  p10_map_jobs;
static p10_map_cache_t p10_map_cache[P10_NGMAP_CACHE_SLOTS];
static uint32_t        p10_map_epoch;

static void _cell_log_free(p10_cell_log_t* log)
{
    free(log->cells);
    log->cells    = NULL;
    log->count    = 0;
    log->cap      = 0;
    log->overflow = 0;
}

static void _cell_log_add(p10_cell_log_t* log, uint32_t cell, size_t limit)
{
    if (log->overflow) {
        return;
    }
    if (log->count == limit) {
        _cell_log_free(log);
        log->overflow = 1;
        return;
    }
    if (log->count == log->cap) {
        size_t    cap   = log->cap ? log->cap * 2 : 256;
        uint32_t* cells = (uint32_t*) realloc(log->cells, cap * sizeof *cells);
        if (cells == NULL) {
            _cell_log_free(log);
            log->overflow = 1;
            return;
        }
        log->cells = cells;
        log->cap   = cap;
    }
    log->cells[log->count++] = cell;
}

static void _cache_drop(p10_map_cache_t* slot)
{
    free(slot->data);
    _cell_log_free(&slot->edits);
    memset(slot, 0, sizeof *slot);
}

void proto10_session_map_edit(int x, int y, int z)
{
    uint32_t cell = P10_CELL_PACK(x, y, z);
    for (p10_map_job_t* job = p10_map_jobs; job != NULL; job = job->next) {
        _cell_log_add(&job->edits, cell, P10_NGMAP_JOB_MAX_EDITS);
    }
    for (int i = 0; i < P10_NGMAP_CACHE_SLOTS; i++) {
        p10_map_cache_t* slot = &p10_map_cache[i];
        if (slot->used) {
            _cell_log_add(&slot->edits, cell, P10_NGMAP_CACHE_MAX_EDITS);
            if (slot->edits.overflow) {
                _cache_drop(slot);
            }
        }
    }
}

void proto10_session_map_loaded(void)
{
    p10_map_epoch++;
    for (int i = 0; i < P10_NGMAP_CACHE_SLOTS; i++) {
        if (p10_map_cache[i].used) {
            _cache_drop(&p10_map_cache[i]);
        }
    }
}

static void _free_map_queue(player_t* player)
{
    queue_t *node, *tmp;
    DL_FOREACH_SAFE(player->map_queue, node, tmp)
    {
        DL_DELETE(player->map_queue, node);
        free(node->block);
        free(node);
    }
    player->map_queue = NULL;
    /* a new queue is a new transfer: its figures start at zero */
    player->p10_map_sent_bytes = 0;
    player->p10_map_sent_ticks = 0;
    player->p10_map_sent_at    = 0;
}

/* ngmap is already compressed: only cut it into MapData fragments */
static int _queue_bytes(player_t* player, const uint8_t* data, size_t size)
{
    for (size_t at = 0; at < size; at += DEFAULT_COMPRESS_CHUNK_SIZE) {
        size_t   len  = size - at < DEFAULT_COMPRESS_CHUNK_SIZE ? size - at : DEFAULT_COMPRESS_CHUNK_SIZE;
        queue_t* node = (queue_t*) calloc(1, sizeof *node);
        if (node == NULL || (node->block = (uint8_t*) malloc(len)) == NULL) {
            free(node);
            _free_map_queue(player);
            return 0;
        }
        memcpy(node->block, data + at, len);
        node->length   = (uint32_t) len;
        node->capacity = (uint32_t) len;
        DL_APPEND(player->map_queue, node);
    }
    return 1;
}

/* a copy of the map's blocks: the last job's, reused (fresh pages cost a
   page fault each the first time they are written) */
static map_store_t p10_raw_spare;
static int      p10_raw_spare_ok;
static uint64_t p10_raw_spare_at; /* when it was given back (freed 60 s later) */

static int _raw_copy(server_t* server, map_store_t* out)
{
    map_store_t* map   = &server->s_map.map;
    uint64_t  began = get_real_nanos();
    if (p10_raw_spare_ok && p10_raw_spare.size_x == map->size_x && p10_raw_spare.size_y == map->size_y &&
        p10_raw_spare.size_z == map->size_z)
    {
        *out             = p10_raw_spare;
        p10_raw_spare_ok = 0;
    } else {
        if (!map_store_create(out, map->size_x, map->size_y, map->size_z)) {
            map_store_free(out);
            return 0;
        }
    }
    map_store_copy(out, map);
    LOG_INFO("proto10: map blocks copied in %.1f ms", (double) (get_real_nanos() - began) / (double) NANO_IN_MILLI);
    return 1;
}

/* the copy back for the next job (or freed when one is kept already or the
   map changed size) */
static void _raw_release(map_store_t* raw)
{
    if (!p10_raw_spare_ok) {
        p10_raw_spare    = *raw;
        p10_raw_spare_ok = 1;
        p10_raw_spare_at = get_real_nanos();
    } else {
        map_store_free(raw);
    }
}

static uint8_t* _vxl_of(map_store_t* map, size_t* size)
{
    /* map_store_write_bound: the old bound, size_z / 2 bytes per column,
       is smaller than real maps (Gridwar writes 11.5 MB into its 8 MB and
       the heap overflow crashed the server on the next join). malloc, not
       calloc: only the written pages get touched */
    uint8_t* vxl = (uint8_t*) malloc(map_store_write_bound(map));
    if (vxl != NULL) {
        *size = map_store_write(map, vxl);
    }
    return vxl;
}

static void _send_game_state_header(server_t* server, player_t* player, const char* format, size_t total)
{
    map_store_t* map = &server->s_map.map;
    char      size_str[48], bytes_str[24], crc_str[16];
    snprintf(size_str, sizeof size_str, "%d,%d,%d", (int) map->size_x, (int) map->size_y, (int) map->size_z);
    snprintf(bytes_str, sizeof bytes_str, "%zu", total);
    /* the stream's own CRC32, not the map's: it is what the client caches by
       (the same key 0.76 uses), so the same map at another quality is
       another file, and an edit after the snapshot changes it */
    snprintf(crc_str, sizeof crc_str, "%08x", (unsigned) player->p10_map_crc32);

    uint8_t    buf[512];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_game_state_header(&w);
    p10_put_map_entry(&w, "map-name", server->map_name);
    p10_put_map_entry(&w, "map-format", format);
    p10_put_map_entry(&w, "map-size", size_str);
    p10_put_map_entry(&w, "map-bytes", bytes_str);
    p10_put_map_entry(&w, "map-crc32", crc_str);
    p10_put_map_end(&w);
    _send(player, &w);
    _set_state(player, P10_STATE_WAITING_MAP_ANSWER, P10_TIMEOUT_MAP_ANSWER);
}

/* zlib's CRC32 over a stream, in pieces: one call takes a uInt length and
   the map streams are a few MB */
static uint32_t _crc32_of(uint32_t crc, const uint8_t* data, size_t size)
{
    while (size > 0) {
        uInt   len = size > (1u << 20) ? (1u << 20) : (uInt) size;
        crc        = (uint32_t) crc32(crc, data, len);
        data += len;
        size -= len;
    }
    return crc;
}

/* zlib VXL transfer of the map as it is now, written into deflate a slice
   at a time on the tick (compress_map); edits from now on reach the player
   as live TerrainUpdates. 0 out of memory */
static int _begin_vxl(server_t* server, player_t* player, const char* why)
{
    _free_map_queue(player);
    uint64_t began    = get_real_nanos();
    size_t   vxl_size = 0;
    player->map_queue = compress_map(&server->s_map.map, DEFAULT_COMPRESS_CHUNK_SIZE, &vxl_size);
    if (player->map_queue == NULL) {
        return 0;
    }
    /* on the tick: the stall a join costs everyone */
    LOG_INFO("proto10: map written and compressed, %zu bytes of VXL in %.1f ms", vxl_size,
             (double) (get_real_nanos() - began) / (double) NANO_IN_MILLI);
    player->p10_map_sent_at = get_real_nanos();

    size_t   total = 0;
    uint32_t crc   = (uint32_t) crc32(0L, Z_NULL, 0);
    queue_t* node;
    DL_FOREACH(player->map_queue, node)
    {
        total += node->length;
        crc = _crc32_of(crc, node->block, node->length);
    }
    player->p10_map_crc32 = crc;
    _send_game_state_header(server, player, "vxl", total);
    LOG_INFO("proto10: sending map %s to %s (#%hhu) as vxl, %zu bytes (VXL %zu), %s",
             server->map_name,
             player->name,
             player->id,
             total,
             vxl_size,
             why);
    evt_log(player,
            "proto10",
            "map_start name=%s format=vxl bytes=%zu vxl_bytes=%zu why=%s",
            server->map_name,
            total,
            vxl_size,
            why);
    return 1;
}

/* ngmap transfer of an encoded snapshot, plus the edits made since then */
static void _begin_ngmap(server_t*             server,
                         player_t*             player,
                         int                   quality,
                         const uint8_t*        data,
                         size_t                size,
                         size_t                vxl_size,
                         uint64_t              encode_ns,
                         const p10_cell_log_t* edits,
                         const char*           source)
{
    _free_map_queue(player);
    if (!_queue_bytes(player, data, size)) {
        _kick(player, "Server out of memory while preparing the map");
        return;
    }
    player->p10_map_sent_at = get_real_nanos();
    player->p10_map_crc32   = _crc32_of((uint32_t) crc32(0L, Z_NULL, 0), data, size);
    _send_game_state_header(server, player, "ngmap", size);
    /* after the header: the client resets its saved edits there */
    if (proto10_world_send_cells(server, player, edits->cells, edits->count) < 0) {
        _kick(player, "Server out of memory while preparing the map");
        return;
    }
    LOG_INFO("proto10: sending map %s to %s (#%hhu) as ngmap q%d, %zu bytes (VXL %zu) from %s (encoded in %llu ms), "
             "%zu edits replayed",
             server->map_name,
             player->name,
             player->id,
             quality,
             size,
             vxl_size,
             source,
             (unsigned long long) (encode_ns / 1000000ull),
             edits->count);
    evt_log(player,
            "proto10",
            "map_start name=%s format=ngmap quality=%d bytes=%zu vxl_bytes=%zu source=%s encode_ms=%llu replayed=%zu",
            server->map_name,
            quality,
            size,
            vxl_size,
            source,
            (unsigned long long) (encode_ns / 1000000ull),
            edits->count);
}

static void* _encode_thread(void* arg)
{
    p10_map_job_t* job = (p10_map_job_t*) arg;
    uint64_t       t0  = get_real_nanos();
    job->vxl           = _vxl_of(&job->raw, &job->vxl_size);
    if (job->vxl == NULL) {
        job->err = NGMAP_ERR_INTERNAL;
    } else if (job->fail_for_test) {
        job->err = NGMAP_ERR_INTERNAL;
    } else {
        job->err = ngmap_encode(
        job->vxl, job->vxl_size, job->width, job->height, job->depth, job->quality, &job->out, &job->out_size);
    }
    job->encode_ns = get_real_nanos() - t0;
    free(job->vxl);
    job->vxl = NULL;
    /* last access: from here on the main thread owns the job */
    atomic_store_explicit(&job->done, 1, memory_order_release);
    return NULL;
}

/* a background encoding of the map as it is now, at the served quality; a
   save path keeps the result for the map's next load. NULL when no thread or
   no memory */
static p10_map_job_t* _start_job(server_t* server, const char* save)
{
    map_store_t*   map = &server->s_map.map;
    p10_map_job_t* job = (p10_map_job_t*) calloc(1, sizeof *job);
    if (job == NULL || !_raw_copy(server, &job->raw)) {
        free(job);
        return NULL;
    }
    job->epoch   = p10_map_epoch;
    job->quality = P10_NGMAP_SERVED;
    job->width   = map->size_x;
    job->height  = map->size_y;
    job->depth   = map->size_z;
    if (save != NULL) {
        snprintf(job->save, sizeof job->save, "%s", save);
    }
    /* test hook: the fallback cannot be reached with a valid map otherwise */
    job->fail_for_test = getenv("SP_NGMAP_FAIL") != NULL;
    atomic_init(&job->done, 0);
    pthread_t thread;
    if (pthread_create(&thread, NULL, _encode_thread, job) != 0) {
        _raw_release(&job->raw);
        free(job);
        LOG_WARNING("proto10: could not start the ngmap encoder thread");
        return NULL;
    }
    pthread_detach(thread);
    job->next    = p10_map_jobs;
    p10_map_jobs = job;
    LOG_INFO("proto10: encoding map %s as ngmap q%d in the background", server->map_name, P10_NGMAP_SERVED);
    return job;
}

static void _start_map_transfer(server_t* server, player_t* player)
{
    map_store_t* map = &server->s_map.map;
    _free_map_queue(player);
    player->p10_map_job = NULL;

    /* the client's mapQuality is no longer read: every 1.0 join is ngmap at
       the served quality, VXL only for a map ngmap cannot hold */
    if (map->size_x > NGMAP_MAX_WIDTH || map->size_y > NGMAP_MAX_HEIGHT || map->size_z > NGMAP_MAX_DEPTH) {
        if (!_begin_vxl(server, player, "map_too_large_for_ngmap")) {
            _kick(player, "Server out of memory while preparing the map");
        }
        return;
    }
    int quality = P10_NGMAP_SERVED;

    for (int i = 0; i < P10_NGMAP_CACHE_SLOTS; i++) {
        p10_map_cache_t* slot = &p10_map_cache[i];
        if (slot->used && slot->epoch == p10_map_epoch && slot->quality == quality) {
            slot->last_use = get_real_nanos();
            _begin_ngmap(server,
                         player,
                         quality,
                         slot->data,
                         slot->size,
                         slot->vxl_size,
                         slot->encode_ns,
                         &slot->edits,
                         slot->stored ? "stored" : "cache");
            return;
        }
    }

    p10_map_job_t* job;
    for (job = p10_map_jobs; job != NULL; job = job->next) {
        if (job->epoch == p10_map_epoch && job->quality == quality && !job->edits.overflow) {
            break;
        }
    }
    if (job == NULL) {
        job = _start_job(server, NULL);
        if (job == NULL) {
            if (!_begin_vxl(server, player, "no_ngmap_encoder")) {
                _kick(player, "Server out of memory while preparing the map");
            }
            return;
        }
    }
    player->p10_map_job = job;
    evt_log(player, "proto10", "map_prepare format=ngmap quality=%d", quality);
    _set_state(player, P10_STATE_PREPARING_MAP, P10_TIMEOUT_MAP);
}

/* the stream into path, through a temporary file and a rename (a crash
   mid-write leaves no half stream to be sent as a map) */
static void _save_stream(const char* path, const uint8_t* data, size_t size)
{
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE* fp = fopen(tmp, "wb");
    if (fp == NULL) {
        LOG_WARNING("proto10: could not keep the map's ngmap in %s: %s", path, strerror(errno));
        return;
    }
    int ok = fwrite(data, 1, size, fp) == size;
    ok     = fclose(fp) == 0 && ok;
    remove(path); /* Windows' rename does not replace */
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        LOG_WARNING("proto10: could not keep the map's ngmap in %s", path);
        return;
    }
    LOG_INFO("proto10: the map's ngmap kept in %s (%zu bytes)", path, size);
}

void proto10_session_map_stored(server_t* server, const uint8_t* data, size_t size, const char* from)
{
    if (profile_is_075()) {
        return;
    }
    /* map_load dropped the cache (proto10_session_map_loaded): slot 0 is free */
    p10_map_cache_t* slot = &p10_map_cache[0];
    if (slot->used) {
        _cache_drop(slot);
    }
    slot->data = (uint8_t*) malloc(size);
    if (slot->data == NULL) {
        return;
    }
    memcpy(slot->data, data, size);
    slot->used     = 1;
    slot->stored   = 1;
    slot->epoch    = p10_map_epoch;
    slot->quality  = P10_NGMAP_SERVED;
    slot->size     = size;
    slot->vxl_size = server->s_map.map_size;
    slot->last_use = get_real_nanos();
    /* a log line, no EVT: a benchmark's second run finds what the first
       kept, and the matches would differ by it (tools/bench.py check) */
    LOG_INFO("proto10: map %s served from its stored ngmap (%s, %zu bytes)", server->map_name, from, size);
}

void proto10_session_map_ready(server_t* server, const char* save)
{
    if (profile_is_075()) {
        return;
    }
    for (int i = 0; i < P10_NGMAP_CACHE_SLOTS; i++) {
        if (p10_map_cache[i].used && p10_map_cache[i].epoch == p10_map_epoch) {
            return; /* stored */
        }
    }
    /* no stored stream: the map is encoded now, not at the first join, and
       kept for its next load */
    if (_start_job(server, save) == NULL) {
        LOG_WARNING("proto10: map %s not encoded at load; the first join encodes it", server->map_name);
    }
}

static void _finish_job(server_t* server, p10_map_job_t* job)
{
    int              stale = job->epoch != p10_map_epoch || job->edits.overflow;
    p10_map_cache_t* slot  = NULL;

    if (job->err != NGMAP_OK) {
        LOG_WARNING("proto10: ngmap encoding of %s at q%d failed (%s), sending VXL instead",
                    server->map_name,
                    job->quality,
                    ngmap_strerror(job->err));
    } else {
        LOG_INFO("proto10: map %s encoded as ngmap q%d in %llu ms: %zu bytes (VXL %zu)%s",
                 server->map_name,
                 job->quality,
                 (unsigned long long) (job->encode_ns / 1000000ull),
                 job->out_size,
                 job->vxl_size,
                 stale ? ", stale" : "");
    }
    /* a load's encoding is of the map file as it was read (its snapshot was
       taken before any edit): kept for the next load of that file */
    if (job->err == NGMAP_OK && job->save[0] != '\0') {
        _save_stream(job->save, job->out, job->out_size);
    }
    if (job->err == NGMAP_OK && !stale && job->edits.count <= P10_NGMAP_CACHE_MAX_EDITS) {
        /* same quality first, then a free slot, then the least recently used */
        for (int i = 0; i < P10_NGMAP_CACHE_SLOTS && slot == NULL; i++) {
            if (p10_map_cache[i].used && p10_map_cache[i].quality == job->quality) {
                slot = &p10_map_cache[i];
            }
        }
        for (int i = 0; i < P10_NGMAP_CACHE_SLOTS && slot == NULL; i++) {
            if (!p10_map_cache[i].used) {
                slot = &p10_map_cache[i];
            }
        }
        if (slot == NULL) {
            slot = &p10_map_cache[0];
            for (int i = 1; i < P10_NGMAP_CACHE_SLOTS; i++) {
                if (p10_map_cache[i].last_use < slot->last_use) {
                    slot = &p10_map_cache[i];
                }
            }
        }
        if (slot->used) {
            _cache_drop(slot);
        }
        slot->used      = 1;
        slot->stored    = job->save[0] != '\0';
        slot->epoch     = job->epoch;
        slot->quality   = job->quality;
        slot->data      = job->out;
        slot->size      = job->out_size;
        slot->vxl_size  = job->vxl_size;
        slot->encode_ns = job->encode_ns;
        slot->last_use  = get_real_nanos();
        slot->edits     = job->edits;
        job->out        = NULL;
        memset(&job->edits, 0, sizeof job->edits);
    }

    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (player->protocol != PROTOCOL_10 || player->p10_map_job != job) {
            continue;
        }
        player->p10_map_job = NULL;
        if (player->p10_state != P10_STATE_PREPARING_MAP) {
            continue;
        }
        if (stale) {
            /* the map changed under it, or its edit log was dropped */
            _start_map_transfer(server, player);
        } else if (job->err != NGMAP_OK) {
            evt_log(player, "proto10", "ngmap_failed quality=%d err=%s", job->quality, ngmap_strerror(job->err));
            if (!_begin_vxl(server, player, "ngmap_encode_failed")) {
                _kick(player, "Server out of memory while preparing the map");
            }
        } else if (slot != NULL) {
            _begin_ngmap(server,
                         player,
                         slot->quality,
                         slot->data,
                         slot->size,
                         slot->vxl_size,
                         slot->encode_ns,
                         &slot->edits,
                         "fresh");
        } else {
            _begin_ngmap(server,
                         player,
                         job->quality,
                         job->out,
                         job->out_size,
                         job->vxl_size,
                         job->encode_ns,
                         &job->edits,
                         "fresh");
        }
    }
    free(job->out);
    _cell_log_free(&job->edits);
    _raw_release(&job->raw);
    free(job);
}

void proto10_session_tick(server_t* server)
{
    /* the kept copy of the map's blocks (64 MB for a 512 map) goes back to
       the system a minute after the last join used it: it was held for the
       rest of the match on a small server */
    if (p10_raw_spare_ok && get_real_nanos() - p10_raw_spare_at > 60ULL * NANO_IN_SECOND) {
        map_store_free(&p10_raw_spare);
        p10_raw_spare_ok = 0;
        LOG_INFO("proto10: map block copy freed (no join for 60 s)");
    }
    p10_map_job_t** link = &p10_map_jobs;
    while (*link != NULL) {
        p10_map_job_t* job = *link;
        if (!atomic_load_explicit(&job->done, memory_order_acquire)) {
            link = &job->next;
            continue;
        }
        /* unlinked first: _finish_job may start new jobs (at the head) */
        *link = job->next;
        _finish_job(server, job);
    }
}

void proto10_restart_map(server_t* server, player_t* player)
{
    _start_map_transfer(server, player);
}

/* MapDataFinal: the stream is in (or the client said it already has it), and
   the join goes on to the acknowledge (7.3) */
static void _send_map_final(player_t* player)
{
    uint8_t    fin[4];
    p10_writer w;
    p10_writer_init(&w, fin, sizeof fin);
    p10_encode_empty(&w, P10_PKT_MAP_DATA_FINAL);
    _send(player, &w);
    evt_log(player,
            "proto10",
            "map_sent bytes=%zu ticks=%u ms=%llu",
            player->p10_map_sent_bytes,
            (unsigned) player->p10_map_sent_ticks,
            (unsigned long long) (player->p10_map_sent_at != 0
                                      ? (get_real_nanos() - player->p10_map_sent_at) / NANO_IN_MILLI
                                      : 0));
    _set_state(player, P10_STATE_COMPLETING_MAP, P10_TIMEOUT_MAP);
}

static void _send_map_fragments(player_t* player)
{
    static uint8_t buf[DEFAULT_COMPRESS_CHUNK_SIZE + 16];
    int            sent = 0;
    /* pace by what the connection still holds (D9): a fast link gets the map
       at once, a slow one only what it can drain (docs/protocol-1.0.md 7.2) */
    while (sent < P10_MAP_FRAGMENTS_MAX && player->map_queue != NULL
           && conn_pending(player->conn) < P10_MAP_PENDING_MAX)
    {
        queue_t*     node = player->map_queue;
        p10_writer   w;
        p10_map_data frag;
        frag.fragment.data = node->block;
        frag.fragment.len  = node->length;
        p10_writer_init(&w, buf, sizeof buf);
        p10_encode_map_data(&w, &frag);
        _send(player, &w);
        player->p10_map_sent_bytes += node->length;
        DL_DELETE(player->map_queue, node);
        free(node->block);
        free(node);
        sent++;
    }
    if (sent > 0) {
        player->p10_map_sent_ticks++;
    }
    if (player->map_queue == NULL) {
        _send_map_final(player);
    }
}

/* "map-cached <0|1>": the client's answer to the header's map-crc32 (7.1).
   A hit skips every MapData fragment; the edits replayed after the header
   already brought the client's copy up to date. A miss, or no answer at all
   (P10_TIMEOUT_MAP_ANSWER), sends the map as before */
static void _on_map_answer(server_t* server, player_t* player, const uint8_t* data, size_t size)
{
    p10_reader r;
    p10_bytes  parts[2];
    int        n = 0;
    if (!flood_take(server, player, FLOOD_COMMAND)) {
        return;
    }
    if (!p10_begin_generic_command(&r, data, size)) {
        _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed map-cached");
        return;
    }
    p10_bytes part;
    while (p10_next_generic_part(&r, &part)) {
        if (n < 2) {
            parts[n] = part;
        }
        n++;
    }
    char answer[4] = "";
    if (r.error || n != 2 || !p10_bytes_equal(parts[0], "map-cached") ||
        !p10_bytes_copy(parts[1], answer, sizeof answer) ||
        (strcmp(answer, "0") != 0 && strcmp(answer, "1") != 0))
    {
        /* another command in the map's window: the timeout sends the map */
        evt_log(player, "proto10", "map_cached_ignored");
        return;
    }
    size_t bytes = 0;
    for (queue_t* node = player->map_queue; node != NULL; node = node->next) {
        bytes += node->length;
    }
    evt_log(player,
            "proto10",
            "map_cached hit=%c bytes=%zu crc=%08x why=answer",
            answer[0],
            bytes,
            (unsigned) player->p10_map_crc32);
    if (answer[0] == '1') {
        _free_map_queue(player);
        _send_map_final(player);
        return;
    }
    _set_state(player, P10_STATE_MAP_TRANSFER, P10_TIMEOUT_MAP);
}

static void _put_color(p10_writer* w, const char* key, color_t c)
{
    char value[16];
    snprintf(value, sizeof value, "%d,%d,%d", c.r, c.g, c.b);
    p10_put_map_entry(w, key, value);
}

static void _send_game_state_final(server_t* server, player_t* player)
{
    char fog[48], capture[8], fog_distance[8];
    /* world parameters use 0..1 floats for the fog (reference format) */
    snprintf(fog,
             sizeof fog,
             "%.3f,%.3f,%.3f",
             server->protocol.color_fog.r / 255.0,
             server->protocol.color_fog.g / 255.0,
             server->protocol.color_fog.b / 255.0);
    /* the map's limit (the arena counts rounds), not the config default */
    snprintf(capture, sizeof capture, "%u", (unsigned) server->protocol.gamemode.score_limit);
    snprintf(fog_distance, sizeof fog_distance, "%u", (unsigned) server->fog_distance);

    static uint8_t buf[65536];
    p10_writer     w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_game_state_final(&w);
    /* the physics the server plays by ([gameplay], GameplayPhysics.c); the
       client predicts with them. Fatal: where a fall takes 100 */
    char phys[32];
    snprintf(phys, sizeof phys, "%.3f", physics_jump_velocity);
    p10_put_map_entry(&w, "player-jump-vel", phys);
    snprintf(phys, sizeof phys, "%.3f", physics_fall_damage_velocity);
    p10_put_map_entry(&w, "fall-damage-vel", phys);
    snprintf(phys, sizeof phys, "%.3f", physics_fall_damage_velocity + sqrtf(100.0f / PHYSICS_FALL_DAMAGE_SCALAR));
    p10_put_map_entry(&w, "fall-damage-fatal-vel", phys);
    p10_put_map_entry(&w, "fog-distance", fog_distance);
    p10_put_map_entry(&w, "fog-color", fog);
    p10_put_map_entry(&w, "gamemode", proto10_gamemode_name(server));
    p10_put_map_entry(&w, "team1-name", server->protocol.name_team[0]);
    _put_color(&w, "team1-color", server->protocol.color_team[0]);
    p10_put_map_entry(&w, "team2-name", server->protocol.name_team[1]);
    _put_color(&w, "team2-color", server->protocol.color_team[1]);
    p10_put_map_entry(&w, "capture-limit", capture);
    jetpack_put_world_params(server, &w);
    sentry_put_world_params(server, &w);
    /* the assist runs on the client; a server that wants raw aim says so */
    p10_put_map_entry(&w, "aim-assist", server->aim_assist ? "1" : "0");
    p10_put_map_end(&w);
    /* every live entity and every player already in the match (fixes D6) */
    proto10_bridge_put_snapshot(server, &w, player);
    _send(player, &w);

    LOG_INFO("proto10: %s (#%hhu) is in the game", player->name, player->id);
    _set_state(player, P10_STATE_GAME, 0);
    proto10_gamemode_send_snapshot(server, player);
    gamemode_client_in_game(server, player);
    proto10_bridge_lights_to(server, player);
    proto10_bridge_mounts_to(server, player);
    sky_send_to(server, player);
    outfit_send_to(server, player);
    proto10_bridge_devices_to(server, player);
}

/* 1 when a gameplay packet may be handled now. Before the first map it is a
   protocol error; during a later map download (map reset) packets the client
   sent before it saw the new GameStateHeader are still arriving: drop those */
static int _gameplay_allowed(player_t* player, uint8_t type)
{
    if (player->p10_state == P10_STATE_GAME) {
        return 1;
    }
    if (player->p10_state == P10_STATE_PREPARING_MAP || player->p10_state == P10_STATE_WAITING_MAP_ANSWER ||
        player->p10_state == P10_STATE_MAP_TRANSFER || player->p10_state == P10_STATE_COMPLETING_MAP)
    {
        evt_log(player, "proto10", "packet_dropped type=%hhu why=map_download", type);
        return 0;
    }
    _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "gameplay packet before the game");
    return 0;
}

void proto10_on_packet(server_t* server, player_t* player, const uint8_t* data, size_t size)
{
    uint8_t type;
    if (player->p10_state == P10_STATE_CLOSING) {
        return;
    }
    if (!p10_packet_type_of(data, size, &type)) {
        _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "empty packet");
        return;
    }

    switch (type) {
        case P10_PKT_INITIATE_CONNECTION:
            if (player->p10_state != P10_STATE_NOT_INITIATED) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "unexpected InitiateConnection");
                return;
            }
            _on_initiate(server, player, data, size);
            return;
        case P10_PKT_CLIENT_CERTIFICATE:
        {
            p10_certificate cert;
            if (player->p10_state != P10_STATE_WAITING_CERT) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "unexpected ClientCertificate");
                return;
            }
            if (!p10_decode_certificate(data, size, P10_PKT_CLIENT_CERTIFICATE, &cert)) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed ClientCertificate");
                return;
            }
            /* the only certificate is the master's connect token (Net/Master.c);
               anything else is not validated yet (D12) */
            evt_log(player, "proto10", "certificate valid=%hhu", cert.is_valid);
            const char* need = master_token_needed(player->conn);
            if (need != NULL) {
                const char* why = "none";
                if (!cert.is_valid || !master_token_check(player->p10_nonce, sizeof player->p10_nonce,
                                                          cert.certificate.data, cert.certificate.len,
                                                          cert.signature.data, cert.signature.len, &why))
                {
                    evt_log(player, "proto10", "token action=refused why=%s address=%s", why, player->conn->address);
                    _kick(player, "Join this server from the server list");
                    return;
                }
                evt_log(player, "proto10", "token action=ok");
            }
            _start_map_transfer(server, player);
            return;
        }
        case P10_PKT_MAP_DATA_ACKNOWLEDGE:
            if (player->p10_state != P10_STATE_COMPLETING_MAP) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "unexpected MapDataAcknowledge");
                return;
            }
            if (!p10_decode_empty(data, size, P10_PKT_MAP_DATA_ACKNOWLEDGE)) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed MapDataAcknowledge");
                return;
            }
            _send_game_state_final(server, player);
            return;
        case P10_PKT_GENERIC_COMMAND:
            /* the one command a client may send before its map is in */
            if (player->p10_state == P10_STATE_WAITING_MAP_ANSWER) {
                _on_map_answer(server, player, data, size);
                return;
            }
            if (!_gameplay_allowed(player, type)) {
                return;
            }
            if (!proto10_bridge_on_packet(server, player, type, data, size)) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed gameplay packet");
            }
            return;
        case P10_PKT_CLIENT_ENTITY_UPDATE:
        case P10_PKT_PLAYER_ACTION:
        case P10_PKT_HIT_ENTITY:
        case P10_PKT_THROW_GRENADE:
            if (!_gameplay_allowed(player, type)) {
                return;
            }
            if (!proto10_bridge_on_packet(server, player, type, data, size)) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed gameplay packet");
            }
            return;
        case P10_PKT_HIT_TERRAIN:
        case P10_PKT_BUILD_BLOCK:
        case P10_PKT_BUILD_LINE:
        case P10_PKT_CHAT_MESSAGE:
            /* terrain and chat (milestone 5, Proto10/World.c) */
            if (!_gameplay_allowed(player, type)) {
                return;
            }
            if (!proto10_world_on_packet(server, player, type, data, size)) {
                _disconnect(player, P10_DISCONNECT_MALFORMED_PACKET, "malformed terrain or chat packet");
            }
            return;
        default:
            /* unknown or server-only ids are ignored, which is what lets the
               protocol grow without a new name (docs/protocol-1.0.md 3.2) */
            evt_log(player, "proto10", "packet_ignored type=%hhu state=%s", type, _state_name(player->p10_state));
            return;
    }
}

int proto10_in_handshake(const player_t* player)
{
    return player->p10_state == P10_STATE_NOT_INITIATED || player->p10_state == P10_STATE_WAITING_CERT;
}

void proto10_evict(player_t* player, const char* why)
{
    _disconnect(player, P10_DISCONNECT_SERVER_FULL, why);
}

void proto10_update(server_t* server, player_t* player)
{
    (void) server;
    if (player->p10_state == P10_STATE_CLOSING) {
        return;
    }
    /* the map's window: no answer is the map itself, never a disconnect */
    if (player->p10_state == P10_STATE_WAITING_MAP_ANSWER && player->p10_deadline != 0 &&
        get_real_nanos() > player->p10_deadline)
    {
        size_t bytes = 0;
        for (queue_t* node = player->map_queue; node != NULL; node = node->next) {
            bytes += node->length;
        }
        evt_log(player, "proto10", "map_cached hit=0 bytes=%zu crc=%08x why=timeout", bytes,
                (unsigned) player->p10_map_crc32);
        _set_state(player, P10_STATE_MAP_TRANSFER, P10_TIMEOUT_MAP);
    }
    if (player->p10_deadline != 0 && get_real_nanos() > player->p10_deadline) {
        char why[64];
        snprintf(why, sizeof why, "timeout in %s", _state_name(player->p10_state));
        evt_log(player, "net", "action=drop why=handshake_timeout state=%s", _state_name(player->p10_state));
        _disconnect(player, P10_DISCONNECT_TIMEOUT, why);
        return;
    }
    if (player->p10_state == P10_STATE_MAP_TRANSFER) {
        _send_map_fragments(player);
    }
}
