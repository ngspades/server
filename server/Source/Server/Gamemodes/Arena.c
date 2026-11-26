// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Grenade.h>
#include <Server/Bots.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Arena.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Player.h>
#include <Server/Profile.h>
#include <mersenne-twister/MT.h>
#include <proto10/codec.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/World.h>
#include <Server/Server.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <Server/MapStore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

#define ARENA_MAX_SEEDS 32
/* a gate is a door, not a building: a seed that floods further than this
   sits on the wrong block (a wall of the same colour) and would eat the map */
#define ARENA_MAX_GATE_CELLS 4096
#define ARENA_END_SECONDS    3
/* the community script's arena_max_spawn_distance: in the countdown, farther
   than this from a spawn of its side and a player is brought back (a step
   through the gate, a jump onto the wall, a spawn in the ground) */
#define ARENA_MAX_SPAWN_DISTANCE 35
/* the winner's colour in the fog for a moment after the round */
#define ARENA_TEAM_COLOR_SECONDS 5
/* one against one in the last minute: each survivor is marked for the enemy,
   refreshed this often (the intel trick of the community script, on our
   spotting marks) */
#define ARENA_MARK_SECONDS 60
#define ARENA_MARK_CYCLE   3
/* the countdown's last seconds are spoken, and the clock's last ones */
#define ARENA_SAY_COUNTDOWN 5
#define ARENA_SAY_CLOCK     10

typedef struct
{
    uint16_t x, y, z;
    uint32_t color;
} arena_cell_t;

typedef enum
{
    ARENA_WAITING,   /* a team is empty: free play, gates open */
    ARENA_COUNTDOWN, /* gates closed, everyone on their spawn */
    ARENA_FIGHT,     /* gates open, the dead stay dead */
    ARENA_END,       /* a short pause showing the winner */
} arena_state_t;

static struct
{
    arena_state_t state;
    uint64_t      since; /* when the state began, nanoseconds */
    int           round;
    int           winner;
    uint8_t       match_over;
    int           countdown_seconds;
    int           round_seconds; /* the round's clock, 10..3600 */
    int           seed_count;
    int           seeds[ARENA_MAX_SEEDS][3];
    int           cell_count;
    arena_cell_t  cells[ARENA_MAX_GATE_CELLS];
    uint8_t       gates_closed;

    /* the community script's options, a map extension or [arena] */
    int   spawn_change;        /* 0 none, 1 the last round, 2 every round, 3 rotate, 4 random rotate */
    float max_spawn_distance;  /* in the countdown: farther off and the player is brought back */
    int   team_color_seconds;  /* the winner's colour in the fog, 0 off */
    int   spawn_rotate;        /* rule 4's slot, drawn once a round */
    int   last_alive[2];       /* the last tick's counts, for "n players remain" */
    int   said_countdown;      /* the last countdown second spoken */
    int   said_clock;          /* the last clock second spoken */
    uint64_t mark_at;          /* the survivors' last mark, nanoseconds */
    uint64_t color_until;      /* the winner's fog until then */
    color_t  color_saved;      /* the fog's colour before it */
    uint8_t  color_tinted;
} arena;

static const char* arena_state_names[] = {"waiting", "countdown", "fight", "end"};

static int _arena_cfg_int(toml_table_t* table, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(table, key);
    return d.ok ? (int) d.u.i : fallback;
}

static float _arena_cfg_float(toml_table_t* table, const char* key, float fallback)
{
    toml_datum_t d = toml_double_in(table, key);
    if (d.ok) {
        return (float) d.u.d;
    }
    d = toml_int_in(table, key);
    return d.ok ? (float) d.u.i : fallback;
}

/* [arena] in config.toml: the server's defaults, which the map's own table
   and its .txt then narrow down. The table is optional. */
static void _arena_read_config(void)
{
    arena.countdown_seconds  = 5;
    arena.round_seconds      = 180;
    arena.spawn_change       = 0;
    arena.max_spawn_distance = (float) ARENA_MAX_SPAWN_DISTANCE;
    arena.team_color_seconds = ARENA_TEAM_COLOR_SECONDS;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp == NULL) {
        return;
    }
    char          err[200];
    toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
    fclose(fp);
    if (conf == NULL) {
        LOG_WARNING("Arena: config.toml could not be read (%s): the defaults apply", err);
        return;
    }
    toml_table_t* table = toml_table_in(conf, "arena");
    if (table != NULL) {
        int   n = _arena_cfg_int(table, "countdown_seconds", arena.countdown_seconds);
        if (n >= 1 && n <= 60) {
            arena.countdown_seconds = n;
        }
        n = _arena_cfg_int(table, "round_seconds", arena.round_seconds);
        if (n >= 10 && n <= 3600) {
            arena.round_seconds = n;
        }
        n = _arena_cfg_int(table, "spawn_change", arena.spawn_change);
        if (n >= 0 && n <= 4) {
            arena.spawn_change = n;
        }
        float f = _arena_cfg_float(table, "max_spawn_distance", arena.max_spawn_distance);
        if (f > 0.0f && f <= 512.0f) {
            arena.max_spawn_distance = f;
        }
        n = _arena_cfg_int(table, "team_color_seconds", arena.team_color_seconds);
        if (n >= 0 && n <= 60) {
            arena.team_color_seconds = n;
        }
    }
    toml_free(conf);
}

static void _arena_read_map(server_t* server, toml_table_t* map_toml)
{
    _arena_read_config();
    arena.seed_count = 0;
    toml_table_t* table = map_toml != NULL ? toml_table_in(map_toml, "arena") : NULL;
    /* the author's arena_gates (the .txt) win over the toml's copy */
    int from_txt = map_meta_arena_gates(server, arena.seeds, ARENA_MAX_SEEDS);
    if (from_txt > 0) {
        arena.seed_count = from_txt;
    }
    /* the script's own rules win over the server's */
    int change = map_meta_arena_spawn_change(server);
    if (change >= 0) {
        arena.spawn_change = change;
    }
    float distance = map_meta_arena_max_spawn_distance(server);
    if (distance > 0.0f) {
        arena.max_spawn_distance = distance;
    }
    if (table == NULL) {
        return;
    }
    int n = _arena_cfg_int(table, "countdown_seconds", arena.countdown_seconds);
    if (n >= 1 && n <= 60) {
        arena.countdown_seconds = n;
    }
    n = _arena_cfg_int(table, "round_seconds", arena.round_seconds);
    if (n >= 10 && n <= 3600) {
        arena.round_seconds = n;
    }
    n = _arena_cfg_int(table, "spawn_change", arena.spawn_change);
    if (n >= 0 && n <= 4) {
        arena.spawn_change = n;
    }
    float f = _arena_cfg_float(table, "max_spawn_distance", arena.max_spawn_distance);
    if (f > 0.0f && f <= 512.0f) {
        arena.max_spawn_distance = f;
    }
    n = _arena_cfg_int(table, "team_color_seconds", arena.team_color_seconds);
    if (n >= 0 && n <= 60) {
        arena.team_color_seconds = n;
    }
    toml_array_t* gates = from_txt > 0 ? NULL : toml_array_in(table, "gates");
    for (int k = 0; gates != NULL && k < toml_array_nelem(gates) && arena.seed_count < ARENA_MAX_SEEDS; k++) {
        toml_array_t* cell = toml_array_at(gates, k);
        if (cell == NULL || toml_array_nelem(cell) != 3) {
            continue;
        }
        for (int c = 0; c < 3; c++) {
            arena.seeds[arena.seed_count][c] = (int) toml_int_at(cell, c).u.i;
        }
        arena.seed_count++;
    }
}

static int _cell_known(int x, int y, int z)
{
    for (int k = 0; k < arena.cell_count; k++) {
        if (arena.cells[k].x == x && arena.cells[k].y == y && arena.cells[k].z == z) {
            return 1;
        }
    }
    return 0;
}

/* pyspades' Gate: the solid blocks of the seed's colour connected to it */
static void _flood_gate(server_t* server, const int seed[3])
{
    map_store_t* map = &server->s_map.map;
    if (seed[0] < 0 || seed[1] < 0 || seed[2] < 0 || seed[0] >= map->size_x || seed[1] >= map->size_y ||
        seed[2] >= map->size_z || !map_solid(map, seed[0], seed[1], seed[2]))
    {
        evt_log(NULL, "arena", "action=gate_seed cell=%d,%d,%d why=not_solid", seed[0], seed[1], seed[2]);
        return;
    }
    uint32_t color = map_color(map, seed[0], seed[1], seed[2]) & 0xFFFFFF;
    int      first = arena.cell_count;
    if (_cell_known(seed[0], seed[1], seed[2])) {
        return;
    }
    arena.cells[arena.cell_count++] = (arena_cell_t) {seed[0], seed[1], seed[2], color};
    /* the cells list is the queue: everything after `next` is unexpanded */
    for (int next = first; next < arena.cell_count; next++) {
        static const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int d = 0; d < 6; d++) {
            int x = arena.cells[next].x + dirs[d][0];
            int y = arena.cells[next].y + dirs[d][1];
            int z = arena.cells[next].z + dirs[d][2];
            if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z - 1) {
                continue;
            }
            if (!map_solid(map, x, y, z) || (map_color(map, x, y, z) & 0xFFFFFF) != color ||
                _cell_known(x, y, z))
            {
                continue;
            }
            if (arena.cell_count >= ARENA_MAX_GATE_CELLS) {
                LOG_WARNING("Arena gate at %d,%d,%d floods past %d blocks: ignored", seed[0], seed[1], seed[2],
                            ARENA_MAX_GATE_CELLS);
                arena.cell_count = first;
                return;
            }
            arena.cells[arena.cell_count++] = (arena_cell_t) {x, y, z, color};
        }
    }
    evt_log(NULL, "arena", "action=gate_seed cell=%d,%d,%d blocks=%d", seed[0], seed[1], seed[2],
            arena.cell_count - first);
}

/* every gate change goes through proto10_world_cell, so 1.0 clients (and
   anyone still downloading the map) see the door open and close */
static void _set_gates(server_t* server, uint8_t closed)
{
    if (arena.gates_closed == closed) {
        return;
    }
    for (int k = 0; k < arena.cell_count; k++) {
        arena_cell_t* c = &arena.cells[k];
        if (closed) {
            map_set_color(&server->s_map.map, c->x, c->y, c->z, c->color | 0xFF000000);
        } else {
            map_set_air(&server->s_map.map, c->x, c->y, c->z);
        }
        proto10_world_cell(server, c->x, c->y, c->z, P10_WORLD_CAUSE_NONE);
    }
    arena.gates_closed = closed;
    evt_log(NULL, "arena", "action=gates open=%d cells=%d", !closed, arena.cell_count);
}

static void _set_state(arena_state_t state)
{
    arena.state          = state;
    arena.since          = get_nanos();
    arena.said_countdown = -1;
    arena.said_clock     = -1;
    arena.mark_at        = 0;
}

static void _arena_lanes_reset(void);

/* Which side's spawn points a team uses this round (arena_spawn_change):
   its own, or the other side's when the rule moves the teams. The bots'
   lanes start on the same side, or they would walk back at the spawn they
   stand on. */
static int _arena_spawn_list(server_t* server, int team)
{
    gamemode_vars_t* gm    = &server->protocol.gamemode;
    int              other = team == 0 ? 1 : 0;
    switch (arena.spawn_change) {
        case 1: /* the last round: one more win takes the match */
            return (gm->score_limit > 1 && (gm->score[0] + 1 >= gm->score_limit || gm->score[1] + 1 >= gm->score_limit))
                       ? other
                       : team;
        case 2:
            /* every round: round 1 on the sides the map made, round 2 on
               the other ones, and so on. (The script swaps the odd rounds;
               a match here starts where the author put its teams) */
            return (arena.round % 2 == 0) ? other : team;
        default:
            return team;
    }
}

/* Which spawn of the side's list a round starts on: -1 draws one at every
   spawn (the script's plain rule), 3 keeps one slot a round by the score's
   sum and 4 one drawn at the round's start */
static int _arena_spawn_slot(server_t* server, int count)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    if (count <= 0) {
        return -1;
    }
    switch (arena.spawn_change) {
        case 3:
            return (int) ((gm->score[0] + gm->score[1]) % (unsigned) count);
        case 4:
            return arena.spawn_rotate % count;
        default:
            return -1;
    }
}

/* the middle of a side's spawns (its rectangle when the map lists no
   points): where its bots' lanes start and the countdown's leash begins */
static void _arena_side_middle(server_t* server, int side, float* x, float* y, float* z)
{
    int count = map_meta_spawn_count(server, side);
    int used  = 0;
    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    for (int k = 0; k < count; k++) {
        vector3f_t pt;
        if (map_meta_spawn_from(server, side, k, &pt)) {
            mx += pt.x;
            my += pt.y;
            mz += pt.z;
            used++;
        }
    }
    if (used > 0) {
        *x = mx / (float) used;
        *y = my / (float) used;
        *z = mz / (float) used;
        return;
    }
    quad3d_t* rect = &server->protocol.spawns[side];
    /* a rectangle names cells: its middle is half a cell in, as a point's */
    *x             = (rect->from.x + rect->to.x) * 0.5f + 0.5f;
    *y             = (rect->from.y + rect->to.y) * 0.5f + 0.5f;
    *z             = (rect->from.z + rect->to.z) * 0.5f;
}

/* how far a body stands from its side's nearest spawn: the countdown's leash
   (arena_max_spawn_distance). The rectangle's middle when the map lists no
   points, a spawn rectangle being a room rather than a point */
static float _arena_spawn_distance(server_t* server, int side, const vector3f_t* at)
{
    int   count = map_meta_spawn_count(server, side);
    float best  = -1.0f;
    for (int k = 0; k < count; k++) {
        vector3f_t pt;
        if (!map_meta_spawn_from(server, side, k, &pt)) {
            continue;
        }
        float dx = pt.x - at->x, dy = pt.y - at->y, dz = pt.z - at->z;
        float d  = sqrtf(dx * dx + dy * dy + dz * dz);
        if (best < 0.0f || d < best) {
            best = d;
        }
    }
    if (best >= 0.0f) {
        return best;
    }
    float cx, cy, cz;
    _arena_side_middle(server, side, &cx, &cy, &cz);
    float dx = cx - at->x, dy = cy - at->y, dz = cz - at->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* the mode places the spawn: one of the side's points (several on the maps
   that list them), the round's slot when the rule keeps one, and the next
   point when that one's ground was dug away */
static int _arena_spawn_point(server_t* server, player_t* player)
{
    if (player->team != TEAM_A && player->team != TEAM_B) {
        return 0;
    }
    /* the --bots-test subject and the tutorial's target meet the other
       team's spawn, as the rectangle path does */
    int side  = _arena_spawn_list(server, player->spawn_rect_override ? (player->team == TEAM_A ? TEAM_B : TEAM_A) : player->team);
    int count = map_meta_spawn_count(server, side);
    if (count <= 0) {
        return 0; /* the rectangle, as before */
    }
    int slot = _arena_spawn_slot(server, count);
    int first = slot >= 0 ? slot : (int) (gen_rand_long(&server->rand) % (unsigned long) count);
    for (int k = 0; k < count; k++) {
        vector3f_t pt, at;
        int        index = (first + k) % count;
        if (!map_meta_spawn_from(server, side, index, &pt) || !player_spawn_on_point(server, pt, &at)) {
            continue;
        }
        player->movement.position = at;
        evt_log(player, "arena", "action=spawn team=%d side=%d slot=%d of=%d at=%.1f,%.1f,%.1f", player->team, side,
                index, count, at.x, at.y, at.z);
        return 1;
    }
    evt_log(player, "arena", "action=spawn_none team=%d side=%d of=%d", player->team, side, count);
    return 0;
}

static void _arena_init(server_t* server)
{
    /* no intel and no tents: the score counts rounds won */
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    arena.round           = 0;
    arena.winner          = -1;
    arena.match_over      = 0;
    arena.cell_count      = 0;
    arena.spawn_rotate    = 0;
    arena.last_alive[0]   = arena.last_alive[1] = 0;
    arena.color_tinted    = 0;
    /* the map was just loaded: its gates stand closed */
    arena.gates_closed = 1;
    _arena_lanes_reset();
    for (int k = 0; k < arena.seed_count; k++) {
        _flood_gate(server, arena.seeds[k]);
    }
    _set_state(ARENA_WAITING);
    /* waiting is free play: nobody may be shut in until a round starts */
    _set_gates(server, 0);
    LOG_INFO("Arena: %d gate blocks, countdown %d s, rounds of %d s, %u rounds to win, "
             "spawn change %d, spawns %d/%d",
             arena.cell_count, arena.countdown_seconds, arena.round_seconds,
             (unsigned) server->protocol.gamemode.score_limit, arena.spawn_change,
             map_meta_spawn_count(server, 0), map_meta_spawn_count(server, 1));
    evt_log(NULL, "arena", "action=rules countdown=%d round=%d spawn_change=%d max_spawn_distance=%.0f "
                           "team_color=%d spawns=%d,%d",
            arena.countdown_seconds, arena.round_seconds, arena.spawn_change, arena.max_spawn_distance,
            arena.team_color_seconds, map_meta_spawn_count(server, 0), map_meta_spawn_count(server, 1));
}

static int _in_team(const player_t* p)
{
    return (p->team == TEAM_A || p->team == TEAM_B) &&
           (p->state == STATE_READY || p->state == STATE_WAITING_FOR_RESPAWN || p->state == STATE_SPAWNING);
}

static void _arena_send_fog(server_t* server, color_t c)
{
    char        v[3][8];
    const char* parts[4] = {"fog", v[0], v[1], v[2]};
    snprintf(v[0], sizeof v[0], "%u", (unsigned) c.r);
    snprintf(v[1], sizeof v[1], "%u", (unsigned) c.g);
    snprintf(v[2], sizeof v[2], "%u", (unsigned) c.b);
    server->protocol.color_fog = c;
    proto10_bridge_generic(server, NULL, 4, parts);
}

/* the winner's colour for a moment, then the map's own again (a tint that
   stayed would read as the next round's weather) */
static void _arena_color_restore(server_t* server)
{
    if (!arena.color_tinted) {
        return;
    }
    arena.color_tinted = 0;
    _arena_send_fog(server, arena.color_saved);
    evt_log(NULL, "arena", "action=fog_color off=%u,%u,%u", (unsigned) arena.color_saved.r,
            (unsigned) arena.color_saved.g, (unsigned) arena.color_saved.b);
}

static void _start_countdown(server_t* server)
{
    arena.round++;
    /* the slot a random rotation starts on, drawn here and only when the
       rule uses it: a draw the match does not need moved every later random
       pick (spawns, bots) and played another match */
    arena.spawn_rotate = arena.spawn_change == 4 ? (int) (gen_rand_long(&server->rand) % 65536UL) : 0;
    _arena_color_restore(server);
    _set_state(ARENA_COUNTDOWN);
    _set_gates(server, 1);
    grenades_clear_all(server); /* none thrown last round explodes in this one */
    /* everyone, alive or not, starts the round fresh behind their gate */
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_in_team(p)) {
            p->state = STATE_SPAWNING;
        }
    }
    /* a lane is walked from the gate the round starts at */
    _arena_lanes_reset();
    broadcast_server_notice_kind(server, 0, NOTICE_INFO, "The gates open in %d seconds.",
                                 arena.countdown_seconds);
    evt_log(NULL, "arena", "action=countdown round=%d score=%u-%u seconds=%d spawns=%d,%d spawn_change=%d",
            arena.round, server->protocol.gamemode.score[0], server->protocol.gamemode.score[1],
            arena.countdown_seconds, _arena_spawn_list(server, TEAM_A), _arena_spawn_list(server, TEAM_B),
            arena.spawn_change);
}

static void _end_round(server_t* server, int winner, const char* why)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    arena.winner        = winner;
    grenades_clear_all(server);
    if (winner == TEAM_A || winner == TEAM_B) {
        gm->score[winner]++;
        arena.match_over = gm->score[winner] >= gm->score_limit;
    }
    _set_state(ARENA_END);
    /* the script's fog in the winner's colour, a draw keeps the map's */
    if ((winner == TEAM_A || winner == TEAM_B) && arena.team_color_seconds > 0) {
        _arena_color_restore(server);
        arena.color_saved  = server->protocol.color_fog;
        arena.color_until  = get_nanos() + (uint64_t) arena.team_color_seconds * NANO_IN_SECOND;
        arena.color_tinted = 1;
        _arena_send_fog(server, server->protocol.color_team[winner]);
        evt_log(NULL, "arena", "action=fog_color on=%u,%u,%u seconds=%d",
                (unsigned) server->protocol.color_team[winner].r, (unsigned) server->protocol.color_team[winner].g,
                (unsigned) server->protocol.color_team[winner].b, arena.team_color_seconds);
    }
    evt_log(NULL, "arena", "action=round_end round=%d winner=%s why=%s score=%u-%u match_over=%u", arena.round,
            winner == TEAM_A ? "team1" : winner == TEAM_B ? "team2" : "draw", why, gm->score[0], gm->score[1],
            arena.match_over);
}

/* the script's intel trick, on our spotting marks: one survivor a side in the
   last minute, each marked for the enemy and refreshed, so a round that comes
   down to one against one is found rather than hunted for */
static void _arena_mark_survivors(server_t* server, const int alive[2], float elapsed)
{
    if (alive[0] != 1 || alive[1] != 1) {
        return;
    }
    if (arena.round_seconds > 0 && (float) arena.round_seconds - elapsed > (float) ARENA_MARK_SECONDS) {
        return;
    }
    uint64_t now = get_nanos();
    if (arena.mark_at != 0 && now - arena.mark_at < (uint64_t) ARENA_MARK_CYCLE * NANO_IN_SECOND) {
        return;
    }
    player_t* last[2] = {NULL, NULL};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_in_team(p) || (p->team != TEAM_A && p->team != TEAM_B)) {
            continue;
        }
        if (p->state == STATE_READY && p->alive) {
            last[p->team] = p;
        }
    }
    if (last[0] == NULL || last[1] == NULL) {
        return;
    }
    arena.mark_at = now;
    proto10_bridge_spot_mark(server, last[1], TEAM_A);
    proto10_bridge_spot_mark(server, last[0], TEAM_B);
    evt_log(NULL, "arena", "action=mark team1=%hhu team2=%hhu", last[0]->id, last[1]->id);
}

/* the countdown's leash: a player who left its spawn room (through the gate
   before it closes, onto the wall, into the ground) is put back on one of its
   side's spawns and refilled, so a round does not start with an enemy behind
   the other team */
static void _arena_keep_in_spawn(server_t* server)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        /* the spawned only: one still in STATE_SPAWNING holds last round's
           position and would be placed twice (and draw the spawn's slot) */
        if (p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
            continue;
        }
        /* the side its body was put on: a test's bot spawning on the human's
           side (spawn_rect_override) was pulled back 60 times a second */
        int   side = _arena_spawn_list(server, p->spawn_rect_override ? (p->team == TEAM_A ? TEAM_B : TEAM_A) : p->team);
        float off  = _arena_spawn_distance(server, side, &p->movement.position);
        if (off <= arena.max_spawn_distance) {
            continue;
        }
        set_player_respawn_point(server, p);
        p->movement.velocity.x = p->movement.velocity.y = p->movement.velocity.z = 0.0f;
        player_restock(server, p);
        proto10_bridge_teleport(p);
        evt_log(p, "arena", "action=spawn_teleport off=%.0f side=%d max=%.0f", off, side, arena.max_spawn_distance);
    }
}

static void _arena_tick(server_t* server)
{
    int       present[2] = {0, 0}, alive[2] = {0, 0}, humans = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_in_team(p)) {
            continue;
        }
        present[p->team]++;
        humans += !p->is_bot;
        if (p->state == STATE_SPAWNING || (p->state == STATE_READY && p->alive)) {
            alive[p->team]++;
        }
    }
    /* As in Zombie: the listen server has its bots before the player, and a
       countdown that starts without one is already a fight when the player
       arrives. SP_BOTS_PLAY still lets a benchmark run a bot-only round (a
       spectating host is no human in a team: bots_playing() counted it) */
    int   humans_or_benchmark = humans > 0 || bots_play_alone();
    float elapsed = (float) (get_nanos() - arena.since) / (float) NANO_IN_SECOND;
    int   both    = present[0] > 0 && present[1] > 0;

    /* the winner's colour lasts its seconds whatever happens next */
    if (arena.color_tinted && get_nanos() >= arena.color_until) {
        _arena_color_restore(server);
    }

    switch (arena.state) {
        case ARENA_WAITING:
            if (both && humans_or_benchmark) {
                _start_countdown(server);
            }
            break;
        case ARENA_COUNTDOWN:
            if (!both || !humans_or_benchmark) {
                _set_state(ARENA_WAITING);
                _set_gates(server, 0);
                evt_log(NULL, "arena", "action=waiting why=%s", both ? "no_human" : "team_empty");
            } else if (elapsed >= (float) arena.countdown_seconds) {
                _set_state(ARENA_FIGHT);
                _set_gates(server, 0);
                arena.last_alive[0] = alive[0];
                arena.last_alive[1] = alive[1];
                evt_log(NULL, "arena", "action=fight round=%d alive=%d-%d", arena.round, alive[0], alive[1]);
            } else {
                /* the last seconds, spoken (the client's clock shows them
                   too: a player reading the chat sees the same) */
                int left = arena.countdown_seconds - (int) elapsed;
                if (left != arena.said_countdown) {
                    arena.said_countdown = left;
                    /* not the countdown's first second: the notice that
                       opened it already said that number */
                    if (left >= 1 && left <= ARENA_SAY_COUNTDOWN && left < arena.countdown_seconds) {
                        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "%d", left);
                        evt_log(NULL, "arena", "action=countdown_tick left=%d", left);
                    }
                }
                _arena_keep_in_spawn(server);
            }
            break;
        case ARENA_FIGHT:
            if (!humans_or_benchmark) {
                /* nobody is watching the bots: back to free play */
                _set_state(ARENA_WAITING);
                _set_gates(server, 0);
                evt_log(NULL, "arena", "action=waiting why=no_human");
                break;
            }
            /* a team that lost someone hears how many are left. The teams'
               names cannot be in the text: the client translates a notice by
               its key, in which a number is a placeholder and a name is not,
               so the two counts read team 1 first, as the scoreboard does */
            if ((alive[0] < arena.last_alive[0] || alive[1] < arena.last_alive[1]) && alive[0] > 0 && alive[1] > 0) {
                broadcast_server_notice_kind(server, 0, NOTICE_INFO, "%d vs %d remain.", alive[0], alive[1]);
                evt_log(NULL, "arena", "action=remaining alive=%d-%d teams=%s,%s", alive[0], alive[1],
                        server->protocol.name_team[0], server->protocol.name_team[1]);
            }
            arena.last_alive[0] = alive[0];
            arena.last_alive[1] = alive[1];
            /* the clock's warnings, then the survivors' marks */
            if (arena.round_seconds > 0) {
                int left = arena.round_seconds - (int) elapsed;
                if (left != arena.said_clock) {
                    arena.said_clock = left;
                    if (left == 60) {
                        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "%d minute left in the round.", 1);
                        evt_log(NULL, "arena", "action=clock_warn left=%d", left);
                    } else if (left == 30) {
                        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "%d seconds left in the round.", left);
                        evt_log(NULL, "arena", "action=clock_warn left=%d", left);
                    } else if (left >= 1 && left <= ARENA_SAY_CLOCK) {
                        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "%d", left);
                        evt_log(NULL, "arena", "action=clock_warn left=%d", left);
                    }
                }
            }
            _arena_mark_survivors(server, alive, elapsed);
            if (alive[0] == 0 || alive[1] == 0) {
                /* a team that left the match lost it too */
                _end_round(server, alive[0] ? TEAM_A : alive[1] ? TEAM_B : 2, "eliminated");
            } else if (elapsed >= (float) arena.round_seconds) {
                /* the clock: the side with more players alive wins the
                   round, equal numbers draw */
                _end_round(server, alive[0] > alive[1] ? TEAM_A : alive[1] > alive[0] ? TEAM_B : 2, "timeout");
            }
            break;
        case ARENA_END:
            if (elapsed < (float) ARENA_END_SECONDS) {
                break;
            }
            if (arena.match_over) {
                evt_log(NULL, "arena", "action=match_end score=%u-%u", server->protocol.gamemode.score[0],
                        server->protocol.gamemode.score[1]);
                /* like a CTF win: everyone reloads the map, the arena restarts
                   from gamemode_init with the scores at zero */
                gamemode_match_won(server);
            } else if (both && humans_or_benchmark) {
                _start_countdown(server);
            } else {
                _set_state(ARENA_WAITING);
                _set_gates(server, 0);
            }
            break;
    }
}

static int _arena_allows_respawn(server_t* server, player_t* player)
{
    (void) server;
    (void) player;
    return arena.state == ARENA_WAITING || arena.state == ARENA_COUNTDOWN;
}

static int _arena_cell_protected(server_t* server, int x, int y, int z)
{
    (void) server;
    return arena.gates_closed && _cell_known(x, y, z);
}

static int _arena_seconds_left(void)
{
    int total = arena.state == ARENA_COUNTDOWN ? arena.countdown_seconds
                : arena.state == ARENA_FIGHT   ? arena.round_seconds
                                               : 0;
    int left  = total - (int) ((get_nanos() - arena.since) / NANO_IN_SECOND);
    return left > 0 ? left : 0;
}

/* the rounds won are the generic team scores; capture-limit, the rounds
   that win the map */
static void _arena_state_keys(server_t* server, gm_keys_t* out)
{
    (void) server;
    gm_keys_put(out, P10_GM_ARENA_STATE, "%s", arena_state_names[arena.state]);
    gm_keys_put(out, P10_GM_ARENA_ROUND, "%d", arena.round);
    gm_keys_put(out, P10_GM_ARENA_TIME, "%d", _arena_seconds_left());
    gm_keys_put(out, P10_GM_ARENA_WINNER, "%s",
                arena.winner == TEAM_A   ? "team1"
                : arena.winner == TEAM_B ? "team2"
                : arena.winner == 2      ? "draw"
                                         : "");
}

/* an Arena round keeps the map as it was made: the gun and the grenade still
   hurt players, but nothing of the map may be opened and nothing may be
   built on it (no blocks in the inventory either) */
static unsigned _arena_player_rules(server_t* server, player_t* player)
{
    (void) server;
    (void) player;
    return GM_RULE_NO_BLOCK | GM_RULE_NO_DESTROY;
}

/* Where a bot goes in a round. A mode with no objective of its own sends
   every bot at the nearest enemy (gamemode_hunt_nearest) and both teams do
   it at once, so every fight of a round happened in one cell of the middle
   of the map (de_dust: 100% of the damage in 3 cells of 24 blocks, half of
   it in one). Each bot walks a lane of its own instead, the team spread
   across the map toward the enemy's spawn, one point of 30 blocks at a
   time; an enemy within reach is still the fight, and a bot past its lane's
   end hunts like before, so a round still ends with a team wiped out. */
#define ARENA_LANES      9
#define ARENA_LANE_WIDTH 90.0f
#define ARENA_LANE_STEP  30.0f
#define ARENA_WP_REACH   6.0f /* close enough to a lane point to take the next one */
#define ARENA_ENGAGE     80.0f /* an enemy this close is the fight, not the lane */
#define ARENA_DISENGAGE  110.0f /* and it goes back to its lane once past this */

typedef struct
{
    float   x, y, z;
    uint8_t set;
    uint8_t engaged; /* it left its lane for an enemy along the way */
} arena_lane_t;

/* the point of its lane each bot walks to, by team and player id */
static arena_lane_t arena_lane[2][SERVER_MAX_PLAYERS];

static void _arena_lanes_reset(void)
{
    memset(arena_lane, 0, sizeof(arena_lane));
}

/* dry land near (x, y) at the level the bot walks on: the nearest column in
   rings up to 20 cells away whose top block is within two cells of the
   bot's own floor, kept inside the area the map's author lets players be. A
   column whose top block is a rooftop six cells up is a goal no bot can
   reach: the arena lets nothing be dug or built, so bots sent to one stood
   under it turning in a circle for the rest of the round (de_dust, 30 s of
   `bot_nav action=plan result=none`, `dig_refused why=mode`). With no
   column at its level, the closest one by level. 0 when there is no ground
   at all round the point (a lane pushed sideways off de_dust's land into
   the void): the old answer was that column's floor 63, the water layer, a
   goal no plan reaches, and the bot walked into the wall in front of it
   for the rest of the round (`bot_path action=stuck goal_floor=63`) */
static int _arena_land_near(server_t* server, player_t* bot, float x, float y, vector3f_t* out)
{
    map_store_t* map = &server->s_map.map;
    int       ax0, ay0, ax1, ay1;
    if (map_meta_play_area(server, &ax0, &ay0, &ax1, &ay1)) {
        x = fminf(fmaxf(x, (float) ax0 + 3.0f), (float) ax1 - 3.0f);
        y = fminf(fmaxf(y, (float) ay0 + 3.0f), (float) ay1 - 3.0f);
    }
    /* the point can be off the map (a lane pushed sideways at its edge) and
       the blocks are read by cell: keep both inside */
    x = fminf(fmaxf(x, 2.0f), (float) map->size_x - 3.0f);
    y = fminf(fmaxf(y, 2.0f), (float) map->size_y - 3.0f);
    int     bx = (int) fminf(fmaxf(bot->movement.position.x, 0.0f), (float) map->size_x - 1.0f);
    int     by = (int) fminf(fmaxf(bot->movement.position.y, 0.0f), (float) map->size_y - 1.0f);
    int     ref = map_top(map, (uint16_t) bx, (uint16_t) by);
    uint8_t level = ref > 4 && ref < map->size_z - 2;
    /* the closest column with ground of any level, in case none is at this one */
    int fb_x = -1, fb_y = -1, fb_top = 0, fb_off = 0;
    for (int r = 0; r <= 20; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) {
                    continue; /* the ring only */
                }
                int cx = (int) x + dx, cy = (int) y + dy;
                if (cx < 2 || cy < 2 || cx >= map->size_x - 2 || cy >= map->size_y - 2) {
                    continue;
                }
                int top = map_top(map, (uint16_t) cx, (uint16_t) cy);
                if (top <= 4 || top >= map->size_z - 2) {
                    continue;
                }
                int off = abs(top - ref);
                if (!level || off <= 2) {
                    *out = (vector3f_t) {cx + 0.5f, cy + 0.5f, (float) top};
                    return 1;
                }
                if (fb_x < 0 || off < fb_off) {
                    fb_x   = cx;
                    fb_y   = cy;
                    fb_top = top;
                    fb_off = off;
                }
            }
        }
    }
    if (fb_x >= 0) {
        *out = (vector3f_t) {fb_x + 0.5f, fb_y + 0.5f, (float) fb_top};
        return 1;
    }
    return 0;
}

/* the nearest living enemy closer than range, or NULL */
static player_t* _arena_nearest_enemy(server_t* server, player_t* bot, float range)
{
    player_t *p, *tmp, *best = NULL;
    float     best_d = range * range;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || (p->team != TEAM_A && p->team != TEAM_B) || p->state != STATE_READY ||
            !p->alive)
        {
            continue;
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        if (dx * dx + dy * dy < best_d) {
            best_d = dx * dx + dy * dy;
            best   = p;
        }
    }
    return best;
}

static int _arena_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    if ((bot->team != TEAM_A && bot->team != TEAM_B) || !bot->is_bot || bot->id >= SERVER_MAX_PLAYERS) {
        return 0;
    }
    arena_lane_t* lane_wp = &arena_lane[bot->team == TEAM_A ? 0 : 1][bot->id];

    /* behind a closed gate there is nowhere to go: every plan from the
       spawn room came back empty (`plan result=none expanded=42`) and the
       bots pushed into the gate and the room's walls until it opened (half
       of csitaly's `bot_path action=stuck goal=push`). They wait at their
       spawn facing the enemy's, as players do */
    if (arena.state == ARENA_COUNTDOWN && arena.gates_closed) {
        float watch_z = 0.0f;
        memset(out, 0, sizeof(*out));
        gm_goal_name(out, "gate", 0);
        out->x = bot->movement.position.x;
        out->y = bot->movement.position.y;
        /* the other side's spawns are where the enemy comes from: the maps
           that list spawn points have no rectangle to watch */
        _arena_side_middle(server, _arena_spawn_list(server, bot->team == TEAM_A ? TEAM_B : TEAM_A), &out->watch_x,
                           &out->watch_y, &watch_z);
        out->hold = 2.0f; /* its own spot: it stays there */
        out->role = GM_BOT_ATTACK;
        return 1;
    }

    /* an enemy close by is the fight: walking the lane past it left the bots
       of both teams standing on their own side of de_dust's walls, ten of
       them a match on a round that ran out on the clock. Its place on the
       lane is kept, and it goes back to it once the fight is over */
    if (_arena_nearest_enemy(server, bot, lane_wp->engaged ? ARENA_DISENGAGE : ARENA_ENGAGE) != NULL) {
        lane_wp->engaged = 1;
        return gamemode_hunt_nearest(server, bot, out);
    }
    lane_wp->engaged = 0;

    /* the lane runs between the two sides' spawns: the points a round starts
       on when the map lists them, its rectangle otherwise, and the other
       way round on a round the spawn change swaps the sides */
    float sz, ez;
    float sx = 0.0f, sy = 0.0f, ex = 0.0f, ey = 0.0f;
    _arena_side_middle(server, _arena_spawn_list(server, bot->team), &sx, &sy, &sz);
    _arena_side_middle(server, _arena_spawn_list(server, bot->team == TEAM_A ? TEAM_B : TEAM_A), &ex, &ey, &ez);
    float dx = ex - sx, dy = ey - sy, d = sqrtf(dx * dx + dy * dy);
    if (d < 1.0f) {
        return gamemode_hunt_nearest(server, bot, out);
    }
    float ax = dx / d, ay = dy / d; /* toward the enemy's spawn */
    float lx = -ay, ly = ax;        /* across it */
    float along = (bot->movement.position.x - sx) * ax + (bot->movement.position.y - sy) * ay;

    float wdx = lane_wp->x - bot->movement.position.x;
    float wdy = lane_wp->y - bot->movement.position.y;
    if (lane_wp->set && wdx * wdx + wdy * wdy < ARENA_WP_REACH * ARENA_WP_REACH) {
        lane_wp->set = 0; /* there: the next point of the lane */
    }
    if (!lane_wp->set) {
        /* past the enemy's spawn there is nothing left to walk to: hunt (a
           bot kept at the end of its lane with the last enemy hiding would
           sit there until the round ran out on the clock) */
        if (along + ARENA_LANE_STEP > d - 6.0f) {
            return gamemode_hunt_nearest(server, bot, out);
        }
        /* the bot's lane: its rank among the team's bots, spread over
           ARENA_LANE_WIDTH and centred on the line between the two spawns */
        int       rank = 0, n = 0;
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (!p->is_bot || p->state == STATE_DISCONNECTED || p->team != bot->team) {
                continue;
            }
            n++;
            if (p->id < bot->id) {
                rank++;
            }
        }
        int   lanes = n > 1 ? (n < ARENA_LANES ? n : ARENA_LANES) : 1;
        float lane  = lanes > 1 ? ((float) (rank % lanes) / (float) (lanes - 1) - 0.5f) * ARENA_LANE_WIDTH : 0.0f;
        /* the point is taken from where it stands, not from a grid: a goal
           that snapped back onto its own cell left the bot standing on it
           (`bot_path action=stuck dist=0.3`, 130 times a match) */
        vector3f_t wp;
        if (!_arena_land_near(server, bot, sx + ax * (along + ARENA_LANE_STEP) + lx * lane,
                              sy + ay * (along + ARENA_LANE_STEP) + ly * lane, &wp))
        {
            evt_log(bot, "arena", "action=lane_none lane=%.0f of=%d", lane, n);
            return gamemode_hunt_nearest(server, bot, out);
        }
        lane_wp->x   = wp.x;
        lane_wp->y   = wp.y;
        lane_wp->z   = wp.z;
        lane_wp->set = 1;
        evt_log(bot, "arena", "action=lane dest=%.1f,%.1f,%.1f lane=%.0f of=%d", wp.x, wp.y, wp.z, lane, n);
    }
    memset(out, 0, sizeof(*out));
    gm_goal_name(out, "push", 0);
    out->x    = lane_wp->x;
    out->y    = lane_wp->y;
    out->z    = lane_wp->z;
    out->role = GM_BOT_ATTACK;
    return 1;
}

/* In the countdown everyone stands behind the closed gates where nothing can
   happen to them, so picking another gun is free: pyspades' arena lets a
   player choose its gun before the round, and a death here would only send it
   back to the same cell after the wait. Outside the countdown (waiting, the
   fight, the end) a change still costs the normal death and respawn */
static int _arena_weapon_change_instant(server_t* server, player_t* player)
{
    (void) server;
    return arena.state == ARENA_COUNTDOWN && arena.gates_closed && player->alive &&
           (player->team == TEAM_A || player->team == TEAM_B);
}

/* no perks: an Arena round is about the gun. The bots walk lanes across the
   map (_arena_bot_goal), and patrol toward the enemy's spawn cell when they
   have none */
/* a spawn faces the other side's spawns, where the enemy comes from (the
   maps that list spawn points have no rectangle) */
static int _arena_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    float z = 0.0f;
    _arena_side_middle(server, _arena_spawn_list(server, player->team == TEAM_A ? TEAM_B : TEAM_A), x, y, &z);
    return 1;
}

const gamemode_ops_t arena_ops = {
.id             = GAME_MODE_ARENA,
.name           = "arena",
.wire_name      = "arena",
/* no squads: a round is a team's, nobody respawns beside a mate there */
.squad_teams    = 0,
.read_map       = _arena_read_map,
.init           = _arena_init,
.tick           = _arena_tick,
.allows_respawn = _arena_allows_respawn,
.cell_protected = _arena_cell_protected,
.player_rules   = _arena_player_rules,
.weapon_change_instant = _arena_weapon_change_instant,
.state_keys     = _arena_state_keys,
.bot_goal       = _arena_bot_goal,
.spawn_point    = _arena_spawn_point,
.spawn_facing   = _arena_spawn_facing,
};

/* The arena's machinery Plant the Bomb (Ptb.c) plays its rounds with: the
   same maps, gates and spawn points. Thin wrappers over the statics above,
   so the arena itself runs exactly the code it ran before (bench.py check).
   Only one mode runs at a time, so the two share the arena's state. */
void arena_kit_read_map(server_t* server, toml_table_t* map_toml)
{
    _arena_read_map(server, map_toml);
    /* the bomb's sides stay put: the sites are defended by the side that
       starts by them */
    arena.spawn_change = 0;
}

void arena_kit_gates_init(server_t* server)
{
    arena.round        = 0;
    arena.cell_count   = 0;
    arena.spawn_rotate = 0;
    arena.gates_closed = 1;
    _arena_lanes_reset();
    for (int k = 0; k < arena.seed_count; k++) {
        _flood_gate(server, arena.seeds[k]);
    }
    _set_gates(server, 0);
}

void arena_kit_set_gates(server_t* server, int closed)
{
    _set_gates(server, closed ? 1 : 0);
}

int arena_kit_gates_closed(void)
{
    return arena.gates_closed;
}

int arena_kit_gate_cell(int x, int y, int z)
{
    return arena.gates_closed && _cell_known(x, y, z);
}

int arena_kit_gate_count(void)
{
    return arena.cell_count;
}

int arena_kit_countdown_seconds(void)
{
    return arena.countdown_seconds;
}

int arena_kit_spawn_point(server_t* server, player_t* player)
{
    return _arena_spawn_point(server, player);
}

void arena_kit_side_middle(server_t* server, int side, float* x, float* y, float* z)
{
    _arena_side_middle(server, side, x, y, z);
}

void arena_kit_keep_in_spawn(server_t* server)
{
    _arena_keep_in_spawn(server);
}

int arena_kit_land_near(server_t* server, player_t* bot, float x, float y, vector3f_t* out)
{
    return _arena_land_near(server, bot, x, y, out);
}
