// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/BlockHealth.h>
#include <Server/BotPlanner.h>
#include <Server/GameApi.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Proto10/NetStats.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Crates.h>
#include <Server/Proto10/Session.h>
#include <Server/Proto10/World.h>
#include <Util/Checks/BlockChecks.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <classes/classes.h>
#include <enet/enet.h>
#include <proto10/codec.h>
#include <proto10/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one TerrainUpdate stays well below the ENet fragment-heavy sizes; a big
   collapse is split into several packets of the same tick */
#define WORLD_PACKET_CAP 16384

/* server statics share one translation unit in the unity build: prefix them */
typedef struct {
    int32_t  x, y, z;
    uint32_t seq;
    uint8_t  cause;
} world_edit_t;

static world_edit_t* world_edits;
static size_t        world_edit_count;
static size_t        world_edit_cap;
static uint32_t      world_edit_seq;
static uint8_t       world_buf[WORLD_PACKET_CAP];

/* ---- sending ---- */

static void _world_send(player_t* receiver, const p10_writer* w)
{
    if (w->error || receiver->conn == NULL) {
        return;
    }
    conn_send_reliable(receiver->conn, w->data, w->size);
}

/* terrain goes to everyone holding a copy of the map, including clients that
   are still downloading it: their copy is the map as it was compressed, and
   only these packets bring it up to date (dossier 7.3) */
static int _world_has_map(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL &&
           (p->p10_state == P10_STATE_WAITING_MAP_ANSWER || p->p10_state == P10_STATE_MAP_TRANSFER ||
            p->p10_state == P10_STATE_COMPLETING_MAP || p->p10_state == P10_STATE_GAME);
}

static int _world_in_game(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME;
}

/* ---- terrain ---- */

void proto10_world_cell(server_t* server, int x, int y, int z, uint8_t cause)
{
    map_store_t* map = &server->s_map.map;
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z) {
        return;
    }
    /* a supply crate resting on a cleared cell falls: this is the one place
       every map change passes through, a player's dig or a collapse or a
       plugin's block alike. A handful of comparisons a cell, never a scan of
       the map (the intel and the tents move through the mode's
       terrain_changed hook, SpadesX's move_intel_and_tent_down) */
    int solid = map_solid(map, (uint16_t) x, (uint16_t) y, (uint16_t) z);
    crates_cell_changed(server, x, y, z, solid);
    if (profile_is_075()) { /* the 0.75 profile's wire (Legacy/Wire075.c) */
        /* its clients compute a collapse themselves, as when this was NONE */
        legacy_world_cell(server, x, y, z, cause == P10_WORLD_CAUSE_COLLAPSE ? P10_WORLD_CAUSE_NONE : cause);
        return;
    }
    /* the bots' planner keeps its own copy of the map (BotPlanner.c) */
    planner_cell(server, x, y, z);
    /* a changed cell is a new block, whole (BlockHealth.c); a resend of a
       cell that did not change (a refused edit, solid as it was) keeps its
       crack, or a gun hit refused for its rate would heal the block */
    if (cause != P10_WORLD_CAUSE_NONE || !map_solid(map, x, y, z)) {
        block_health_forget(server, x, y, z);
    }
    if (world_edit_count == world_edit_cap) {
        size_t        cap = world_edit_cap ? world_edit_cap * 2 : 1024;
        world_edit_t* e   = (world_edit_t*) realloc(world_edits, cap * sizeof *e);
        if (e == NULL) {
            LOG_ERROR("proto10: out of memory for terrain edits, 1.0 clients will miss one");
            return;
        }
        world_edits    = e;
        world_edit_cap = cap;
    }
    world_edit_t* e = &world_edits[world_edit_count++];
    e->x            = x;
    e->y            = y;
    e->z            = z;
    e->cause        = cause;
    e->seq          = world_edit_seq++;
    /* map snapshots still being encoded (or cached) for joiners must learn
       about it too, they are replayed as TerrainUpdate once the map is sent */
    proto10_session_map_edit(x, y, z);
}

static int _world_edit_cmp(const void* a, const void* b)
{
    const world_edit_t* p = (const world_edit_t*) a;
    const world_edit_t* q = (const world_edit_t*) b;
    if (p->x != q->x)
        return p->x < q->x ? -1 : 1;
    if (p->y != q->y)
        return p->y < q->y ? -1 : 1;
    if (p->z != q->z)
        return p->z < q->z ? -1 : 1;
    return p->seq < q->seq ? -1 : (p->seq > q->seq);
}

/* only == NULL: every connection holding the map, else just that one */
static void _world_send_batch(server_t* server, player_t* only, const p10_writer* w)
{
    if (only != NULL) {
        _world_send(only, w);
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_world_has_map(p)) {
            _world_send(p, w);
        }
    }
}

/* reads the cells back from the map and sends them as TerrainUpdate packets;
   sorts edits in place. Returns the number of edits, *packets the packets */
static int _world_emit(server_t* server, player_t* only, world_edit_t* edits, size_t count, int* packets)
{
    /* sorted: duplicates become neighbours (the last one wins, it has the
       newest cause) and close cells give small deltas */
    qsort(edits, count, sizeof *edits, _world_edit_cmp);

    map_store_t*          map = &server->s_map.map;
    p10_writer         w;
    p10_terrain_cursor cursor;
    int                done = 0, in_packet = 0;
    *packets                = 0;
    p10_writer_init(&w, world_buf, sizeof world_buf);
    p10_start_terrain_update(&w, &cursor);
    for (size_t i = 0; i < count; i++) {
        const world_edit_t* e = &edits[i];
        if (i + 1 < count && edits[i + 1].x == e->x && edits[i + 1].y == e->y && edits[i + 1].z == e->z) {
            continue;
        }
        p10_map_edit edit;
        memset(&edit, 0, sizeof edit);
        edit.x = e->x;
        edit.y = e->y;
        edit.z = e->z;
        if (map_solid(map, (uint16_t) e->x, (uint16_t) e->y, (uint16_t) e->z)) {
            uint32_t c    = map_color(map, (uint16_t) e->x, (uint16_t) e->y, (uint16_t) e->z);
            /* the health byte is the block's health: a crack is a solid cell
               with health under GAME_BLOCK_BREAK (block health, BlockHealth.c) */
            int taken     = block_health_taken(server, e->x, e->y, e->z);
            edit.health   = (uint8_t) (taken < GAME_BLOCK_BREAK ? GAME_BLOCK_BREAK - taken : 1);
            edit.color.r  = (uint8_t) (c >> 16);
            edit.color.g = (uint8_t) (c >> 8);
            edit.color.b = (uint8_t) c;
            edit.cause   = e->cause == P10_WORLD_CAUSE_PLAYER ? P10_BLOCK_CREATE_PLAYER : P10_BLOCK_CREATE_UNSPECIFIED;
        } else {
            edit.health = 0;
            edit.cause = e->cause == P10_WORLD_CAUSE_PLAYER
                ? P10_BLOCK_DESTROY_DAMAGE
                : e->cause == P10_WORLD_CAUSE_COLLAPSE ? P10_BLOCK_DESTROY_COLLAPSE : P10_BLOCK_DESTROY_UNSPECIFIED;
        }
        /* an edit is at most 3 + 3 * 5 + 6 bytes: start a new packet early */
        if (w.size + 32 > w.cap) {
            _world_send_batch(server, only, &w);
            (*packets)++;
            in_packet = 0;
            p10_writer_init(&w, world_buf, sizeof world_buf);
            p10_start_terrain_update(&w, &cursor);
        }
        p10_put_map_edit(&w, &cursor, &edit);
        done++;
        in_packet++;
    }
    if (in_packet) {
        _world_send_batch(server, only, &w);
        (*packets)++;
    }
    if (w.error) {
        LOG_ERROR("proto10: TerrainUpdate did not fit its buffer");
    }
    return done;
}

void proto10_world_tick(server_t* server)
{
    /* with a sped up clock the edits wait for the next send frame (Bridge.c) */
    if (world_edit_count == 0 || !proto10_net_frame) {
        return;
    }
    int       receivers = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        receivers += _world_has_map(p);
    }
    if (receivers == 0) {
        world_edit_count = 0;
        return;
    }

    int packets;
    int edits = _world_emit(server, NULL, world_edits, world_edit_count, &packets);
    if (evt_verbose() || edits > 1) {
        evt_log(NULL, "proto10", "terrain_update edits=%d packets=%d receivers=%d", edits, packets, receivers);
    }
    world_edit_count = 0;
}

int proto10_world_send_cells(server_t* server, player_t* player, const uint32_t* cells, size_t count)
{
    if (count == 0 || player->conn == NULL) {
        return 0;
    }
    world_edit_t* edits = (world_edit_t*) malloc(count * sizeof *edits);
    if (edits == NULL) {
        LOG_ERROR("proto10: out of memory replaying %zu terrain edits to %s", count, player->name);
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        edits[i].x     = (int32_t) P10_CELL_X(cells[i]);
        edits[i].y     = (int32_t) P10_CELL_Y(cells[i]);
        edits[i].z     = (int32_t) P10_CELL_Z(cells[i]);
        edits[i].seq   = (uint32_t) i;
        edits[i].cause = P10_WORLD_CAUSE_NONE;
    }
    int packets;
    int sent = _world_emit(server, player, edits, count, &packets);
    free(edits);
    evt_log(player, "proto10", "terrain_replay edits=%d packets=%d", sent, packets);
    return sent;
}

/* a map reset (init_player) wipes the names; a 0.75 client sends its name
   again with ExistingPlayer, a 1.0 client only gave it in the handshake */
static char world_names[256][PLAYER_NAME_STRLEN + 1];

int proto10_world_player_update(server_t* server, player_t* player)
{
    if (player->name[0] != '\0') {
        memcpy(world_names[player->id], player->name, sizeof world_names[0]);
    }
    if (player->state != STATE_STARTING_MAP) {
        return 0;
    }
    if (player->name[0] == '\0') {
        memcpy(player->name, world_names[player->id], sizeof world_names[0]);
    }
    /* the 0.75 client answers a new MapStart by downloading and joining
       again; a 1.0 client does the same through a new GameStateHeader and
       "join", which replays ExistingPlayer (and with it the team counts) */
    player->state = STATE_PROTO10;
    evt_log(player, "proto10", "map_reset map=%s", server->map_name);
    proto10_restart_map(server, player);
    return 1;
}

/* ---- building and digging ---- */

static int _world_solid(server_t* server, int32_t x, int32_t y, int32_t z)
{
    map_store_t* map = &server->s_map.map;
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z) {
        return 0;
    }
    return map_solid(map, (uint16_t) x, (uint16_t) y, (uint16_t) z);
}

static int _world_in_map(server_t* server, p10_blockcoord c)
{
    map_store_t* map = &server->s_map.map;
    return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < map->size_x && c.y < map->size_y && c.z < map->size_z;
}

/* game_player_block_action refuses silently in most branches; these are its
   checks without side effects, in its order, so the EVT line can say why.
   What is left after these is the timing (BLOCK_DELAY, fire rate) or, for
   builds, block_action_build, which logs its own reason. */
static const char* _world_why(server_t* server, player_t* player, uint8_t action, p10_blockcoord c)
{
    vector3f_t f = {(float) c.x, (float) c.y, (float) c.z};
    vector3i_t v = {c.x, c.y, c.z};
    if (!(player->can_build && server->global_ab)) {
        return "cannot-build";
    }
    if (player->sprinting) {
        return "sprinting";
    }
    if (!((player->item == TOOL_SPADE && (action == BLOCKACTION_DESTROY_ONE || action == BLOCKACTION_DESTROY_THREE)) ||
          (player->item == TOOL_BLOCK && action == BLOCKACTION_BUILD) ||
          (player->item == TOOL_GUN && action == BLOCKACTION_DESTROY_ONE)))
    {
        return "wrong-tool";
    }
    if ((distance_in_3d(f, player->movement.position) > 4 && player->item != TOOL_GUN) || !valid_pos_v3i(server, v)) {
        return "too-far-or-outside";
    }
    if (action == BLOCKACTION_BUILD) {
        /* block_action_build logged the precise reason just before */
        return player->blocks == 0 ? "no-blocks" : "see-block-event";
    }
    if (action == BLOCKACTION_DESTROY_ONE && c.z >= 62) {
        return "indestructible";
    }
    if (gamemode_player_rules(server, player) & GM_RULE_NO_DESTROY) {
        return "mode";
    }
    if (!gamemode_cell_editable(server, c.x, c.y, c.z)) {
        return gamemode_cell_why(server, c.x, c.y, c.z);
    }
    return "rate-limited";
}

static void _world_set_color(server_t* server, player_t* player, p10_color c)
{
    if (c.r == player->tool_color.r && c.g == player->tool_color.g && c.b == player->tool_color.b) {
        return;
    }
    /* the colour travels with the 1.0 build, a 0.75 client sends SetColor
       first: the same call tells the 0.75 clients the right colour too */
    color_t color;
    color.b = c.b;
    color.g = c.g;
    color.r = c.r;
    color.a = 0;
    game_player_set_color(server, player, color);
}

static void _world_replay_block_action(server_t* server, player_t* player, uint8_t action, p10_blockcoord c)
{
    game_player_block_action(server, player, action, (uint32_t) c.x, (uint32_t) c.y, (uint32_t) c.z);
}

static int _world_can_act(player_t* player, const char* what)
{
    if (player->state != STATE_READY || !player->alive) {
        evt_log(player, "proto10", "%s_refused why=not-alive", what);
        return 0;
    }
    return 1;
}

static void _on_build_block(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_build_block b;
    if (!p10_decode_build_block(data, size, &b)) {
        *ok = 0;
        return;
    }
    if (!_world_can_act(player, "build")) {
        return;
    }
    if (!_world_in_map(server, b.pos)) {
        evt_log(player, "proto10", "build_refused cell=%d,%d,%d why=outside-map", b.pos.x, b.pos.y, b.pos.z);
        return;
    }
    _world_set_color(server, player, b.color);
    uint32_t blocks = player->blocks;
    _world_replay_block_action(server, player, BLOCKACTION_BUILD, b.pos);
    if (player->blocks == blocks) {
        evt_log(player,
                "proto10",
                "build_refused cell=%d,%d,%d why=%s",
                b.pos.x,
                b.pos.y,
                b.pos.z,
                _world_solid(server, b.pos.x, b.pos.y, b.pos.z) ? "occupied"
                                                                : _world_why(server, player, BLOCKACTION_BUILD, b.pos));
        /* undoes whatever the client shows for that cell */
        proto10_world_cell(server, b.pos.x, b.pos.y, b.pos.z, P10_WORLD_CAUSE_NONE);
    }
}

static void _on_build_line(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_build_line l;
    if (!p10_decode_build_line(data, size, &l)) {
        *ok = 0;
        return;
    }
    if (!_world_can_act(player, "build_line")) {
        return;
    }
    if (!_world_in_map(server, l.from) || !_world_in_map(server, l.to)) {
        evt_log(player, "proto10", "build_line_refused why=outside-map");
        return;
    }
    _world_set_color(server, player, l.color);
    uint32_t blocks = player->blocks;
    game_player_block_line(server,
                           player,
                           (vector3i_t) {l.from.x, l.from.y, l.from.z},
                           (vector3i_t) {l.to.x, l.to.y, l.to.z});
    if (player->blocks == blocks) {
        /* game_player_block_line checks, in order: sprinting, blocks, tool and
           BLOCK_DELAY, both ends placeable, distance to where the drag began */
        const char* why = player->sprinting                                ? "sprinting"
                          : !(player->can_build && server->global_ab)      ? "cannot-build"
                          : player->item != TOOL_BLOCK                     ? "wrong-tool"
                          : player->blocks == 0                            ? "no-blocks"
                                                                           : "rate-limited-or-not-placeable-or-too-far";
        evt_log(player,
                "proto10",
                "build_line_refused from=%d,%d,%d to=%d,%d,%d why=%s",
                l.from.x,
                l.from.y,
                l.from.z,
                l.to.x,
                l.to.y,
                l.to.z,
                why);
        proto10_world_cell(server, l.from.x, l.from.y, l.from.z, P10_WORLD_CAUSE_NONE);
        proto10_world_cell(server, l.to.x, l.to.y, l.to.z, P10_WORLD_CAUSE_NONE);
    } else {
        evt_log(player,
                "block",
                "action=line from=%d,%d,%d to=%d,%d,%d accepted=1 blocks_left=%u",
                l.from.x,
                l.from.y,
                l.from.z,
                l.to.x,
                l.to.y,
                l.to.z,
                player->blocks);
    }
}

/* HitTerrain -> 0.75 BlockAction (docs/protocol-1.0.md 9.3):
 *   Melee   (spade, primary)   -> DESTROY_ONE   with the spade
 *   Dig     (spade, secondary) -> DESTROY_THREE with the spade (the cell and
 *                                 the ones above and below)
 *   Weapon1..3 (a bullet)      -> DESTROY_ONE   with the gun
 * The 0.75 server keeps no block health: as in 0.75, the client counts the
 * damage and sends the hit that breaks the block. The server then applies its
 * usual rules (tool, distance, BLOCK_DELAY or fire rate, indestructible
 * layers, game mode). */
static void _on_hit_terrain(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_hit_terrain h;
    if (!p10_decode_hit_terrain(data, size, &h)) {
        *ok = 0;
        return;
    }
    if (!_world_can_act(player, "dig")) {
        return;
    }
    p10_blockcoord c = {h.x, h.y, h.z};
    uint8_t        action;
    switch (h.damage_type) {
        case P10_DMG_MELEE:
        case P10_DMG_WEAPON1:
        case P10_DMG_WEAPON2:
        case P10_DMG_WEAPON3:
            action = BLOCKACTION_DESTROY_ONE;
            break;
        case P10_DMG_DIG:
            action = BLOCKACTION_DESTROY_THREE;
            break;
        default:
            evt_log(player, "proto10", "dig_refused cell=%d,%d,%d why=damage-type type=%hhu", h.x, h.y, h.z, h.damage_type);
            return;
    }
    if (!_world_in_map(server, c)) {
        evt_log(player, "proto10", "dig_refused cell=%d,%d,%d why=outside-map", h.x, h.y, h.z);
        return;
    }
    /* same plausibility check as HitEntity: the hit starts at the player */
    float dx = h.fire_position.x - player->movement.eye_pos.x;
    float dy = h.fire_position.y - player->movement.eye_pos.y;
    float dz = h.fire_position.z - player->movement.eye_pos.z;
    if (dx * dx + dy * dy + dz * dz > 4.0f * 4.0f) {
        evt_log(player, "proto10", "dig_refused cell=%d,%d,%d why=fire-position-far", h.x, h.y, h.z);
        return;
    }
    if (!_world_solid(server, h.x, h.y, h.z)) {
        evt_log(player, "proto10", "dig_refused cell=%d,%d,%d why=already-air", h.x, h.y, h.z);
        proto10_world_cell(server, h.x, h.y, h.z, P10_WORLD_CAUSE_NONE);
        return;
    }
    /* a gun sends every pellet that meets a block (block health,
       BlockHealth.c): one that only cracked it leaves it solid, and is no
       refusal (nor a resend, which would also take the crack back) */
    uint32_t damaged = block_health_serial();
    _world_replay_block_action(server, player, action, c);
    if (block_health_serial() != damaged && _world_solid(server, h.x, h.y, h.z)) {
        evt_log(player, "block", "action=damage cell=%d,%d,%d accepted=1 kind=gun", h.x, h.y, h.z);
    } else if (_world_solid(server, h.x, h.y, h.z)) {
        evt_log(player,
                "proto10",
                "dig_refused cell=%d,%d,%d action=%hhu why=%s item=%hhu",
                h.x,
                h.y,
                h.z,
                action,
                _world_why(server, player, action, c),
                player->item);
        proto10_world_cell(server, h.x, h.y, h.z, P10_WORLD_CAUSE_NONE);
    } else {
        evt_log(player, "block", "action=destroy cell=%d,%d,%d accepted=1 kind=%s", h.x, h.y, h.z,
                action == BLOCKACTION_DESTROY_THREE ? "dig" : (player->item == TOOL_GUN ? "gun" : "spade"));
    }
}

/* ---- chat ---- */

static void _on_chat_message(server_t* server, player_t* player, const uint8_t* data, size_t size, int* ok)
{
    p10_chat_message m;
    if (!p10_decode_chat_message(data, size, &m)) {
        *ok = 0;
        return;
    }
    if (player->state == STATE_PROTO10) {
        /* the 0.75 handler reads the team; before "join" there is none */
        evt_log(player, "proto10", "chat_refused why=not-joined");
        return;
    }
    if (m.text.len == 0) {
        evt_log(player, "proto10", "chat_refused why=empty");
        return;
    }
    /* the text becomes the server's CP437, then the one chat handler judges
       it: mute, spam, team and command (/kill, /pm...) rules are shared by
       both wires */
    char buf[P10_CHAT_MAX_TEXT + 1];
    p10_utf8_to_cp437(m.text, buf, sizeof buf);
    game_player_chat(server, player, m.channel, buf);
}

static void _world_broadcast_chat(player_t* receiver, uint32_t from, uint8_t channel, const char* cp437)
{
    char               text[P10_CHAT_MAX_TEXT + 1];
    uint8_t            buf[P10_CHAT_MAX_TEXT + 16];
    p10_writer         w;
    p10_chat_broadcast b;
    /* CP437 grows when converted: cut to 255 bytes on a character boundary */
    size_t len    = p10_cp437_to_utf8((const uint8_t*) cp437, strlen(cp437), text, sizeof text);
    b.from_player = from;
    b.channel     = channel;
    b.text.data   = (const uint8_t*) text;
    b.text.len    = len;
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_chat_broadcast(&w, &b);
    _world_send(receiver, &w);
}

void proto10_world_chat(server_t* server, player_t* sender, uint8_t meant_for, const char* message)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        /* the recipients the 0.75 chat broadcast had */
        if (_world_in_game(p) && is_past_join_screen(p) &&
            ((meant_for == 1 && p->team == sender->team) || meant_for == 0))
        {
            _world_broadcast_chat(p, sender->id, meant_for == 1 ? P10_CHAT_TEAM : P10_CHAT_ALL, message);
        }
    }
}

void proto10_world_notice(player_t* player, const char* message)
{
    if (_world_in_game(player)) {
        _world_broadcast_chat(player, P10_CHAT_FROM_SERVER, P10_CHAT_SYSTEM, message);
    }
}

void proto10_world_broadcast_notice(server_t* server, const char* message)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_world_in_game(p) && is_past_join_screen(p)) {
            _world_broadcast_chat(p, P10_CHAT_FROM_SERVER, P10_CHAT_SYSTEM, message);
        }
    }
}

int proto10_world_on_packet(server_t* server, player_t* player, uint8_t type, const uint8_t* data, size_t size)
{
    int ok = 1;
    switch (type) {
        case P10_PKT_BUILD_BLOCK:
            _on_build_block(server, player, data, size, &ok);
            break;
        case P10_PKT_BUILD_LINE:
            _on_build_line(server, player, data, size, &ok);
            break;
        case P10_PKT_HIT_TERRAIN:
            _on_hit_terrain(server, player, data, size, &ok);
            break;
        case P10_PKT_CHAT_MESSAGE:
            _on_chat_message(server, player, data, size, &ok);
            break;
        default:
            break;
    }
    return ok;
}
