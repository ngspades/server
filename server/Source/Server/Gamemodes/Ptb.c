// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Plant the Bomb: Counter-Strike's bomb defusal on the arena's maps (Ptb.h).
 * The rounds borrow the arena's gates, spawn points and countdown leash
 * (Arena.c's arena_kit_*); everything about the bomb is here.
 */
#include <Server/Bots.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Arena.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Ptb.h>
#include <Server/Grenade.h>
#include <Server/MapStore.h>
#include <Server/Player.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Explosives.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/World.h>
#include <Server/Server.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <math.h>
#include <mersenne-twister/MT.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>
#include <uthash/uthash.h>
/* default_ptb_sites: Resources/ptb_sites.toml, built in (CMakeLists.txt) */
#include <server_default_configs.h>

#define PTB_CT TEAM_A /* team 1 defends the sites */
#define PTB_T  TEAM_B /* team 2 carries the bomb */

#define PTB_SITES_FILE "Resources/ptb_sites.toml"
#define PTB_END_SECONDS 5
/* a terrorist this close to the dropped bomb (horizontally; and within a few
   blocks of its height) picks it up, as one walks over the intel */
#define PTB_PICKUP_REACH 1.6f
/* a counter-terrorist defuses from this close (CS: about an arm's length) */
#define PTB_DEFUSE_REACH 2.2f
/* how far the one planting or defusing may drift before it starts over: a
   body standing still still slides a little on a slope, and a bot holding
   its spot shuffles; a real step away is more than this */
#define PTB_STILL_DRIFT 0.75f
/* a plant or a defuse starts only once the body has stopped (a walk is
   ~0.3 a physics step): held while walking, USE waits for the stop */
#define PTB_START_SPEED 0.06f
/* a client crouches as its hold starts: its first input packets may still
   say standing */
#define PTB_CROUCH_GRACE (NANO_IN_SECOND / 2)
/* the bomb's blast (BattleSpades' rocket curve, (R^2 - d^2) / R^2, much
   larger): kills in the open up to ~24 blocks, nothing past 30 */
#define PTB_BLAST_PEAK   600.0f
#define PTB_BLAST_RADIUS 30.0f
/* its crater, a ball of this radius: the next round puts it back */
#define PTB_CRATER_R     4
#define PTB_CRATER_MAX   1024
/* the progress a planter or defuser is told, at most this often */
#define PTB_PROGRESS_EVERY (NANO_IN_SECOND / 10)
/* the planted bomb is heard (its position told) this far: its beep is
   loud, a client hears it to ~90 blocks (sound.c) */
#define PTB_BOMB_HEAR 90.0f
/* the halftime's pause on top of a round's end, before the swapped freeze */
#define PTB_HALFTIME_SECONDS 5

typedef enum
{
    PTB_WAITING,
    PTB_FREEZE, /* the arena's countdown: gates closed */
    PTB_FIGHT,
    PTB_END,
} ptb_state_t;

/* the wire's arena-state words: the client plays the rounds as the arena's */
static const char* ptb_state_names[] = {"waiting", "countdown", "fight", "end"};

typedef enum
{
    PTB_BOMB_NONE,
    PTB_BOMB_CARRIED,
    PTB_BOMB_DROPPED,
    PTB_BOMB_PLANTED,
    PTB_BOMB_DEFUSED,
    PTB_BOMB_EXPLODED,
} ptb_bomb_t;

static const char* ptb_bomb_names[] = {"none", "carried", "dropped", "planted", "defused", "exploded"};

typedef struct
{
    char  name[2];
    float x, y;   /* the site's middle, map columns */
    int   z;      /* its floor (the solid cell under it) */
    float radius; /* a planter inside this (horizontally) is on the site */
} ptb_site_t;

typedef struct
{
    uint16_t x, y, z;
    uint32_t color;
} ptb_cell_t;

static struct
{
    ptb_state_t state;
    uint64_t    since;
    int         round;
    int         winner; /* TEAM_A, TEAM_B, 2 draw, -1 none yet */
    const char* why;    /* how the last round ended */
    uint8_t     match_over;

    /* [ptb] in config.toml, then the map's [ptb] */
    int   freeze_seconds;
    int   round_seconds;
    int   bomb_seconds;
    int   halftime_round; /* the sides swap after this round, 0 never */
    float plant_seconds;
    float defuse_seconds;
    int   rounds_to_win;

    int        site_count;
    ptb_site_t sites[2];
    const char* sites_from; /* "map", "table", "auto": the log says it */

    ptb_bomb_t bomb;
    uint8_t    carrier;    /* while carried */
    vector3f_t bomb_at;    /* dropped or planted: the cell's middle on its floor block */
    int        bomb_site;  /* planted: 0 A, 1 B */
    uint8_t    planter;    /* who planted it: the blast is credited to them */
    uint64_t   planted_at;
    uint8_t    swapped;  /* the sides swapped this match (once) */
    uint8_t    halftime; /* the round just ended swaps them */
    int        target_site; /* the round's site for the terrorists' bots */

    /* USE held, by player id (the client's "ptb-use", a bot's goal) */
    uint8_t use[SERVER_MAX_PLAYERS];

    /* the plant or the defuse going on (one at a time: the carrier plants,
       one counter-terrorist defuses) */
    uint8_t    act_player; /* 255: none */
    uint8_t    act_defuse;
    uint64_t   act_since;
    vector3f_t act_at;
    uint64_t   act_sent;

    int last_alive[2];
    int said_countdown;
    int said_clock;

    int        crater_count;
    ptb_cell_t crater[PTB_CRATER_MAX];

    /* the match's figures, logged at its end (bench.py reads the EVT lines) */
    int plants, explosions, defuses, drops, pickups;
} ptb;

static uint8_t ptb_bot_roll[SERVER_MAX_PLAYERS]; /* a bot's draw for its round's job */

static int _ptb_env_int(const char* name, int fallback, int lo, int hi)
{
    const char* v = getenv(name);
    if (v == NULL || *v == 0) {
        return fallback;
    }
    int n = atoi(v);
    return n >= lo && n <= hi ? n : fallback;
}

static float _ptb_env_float(const char* name, float fallback, float lo, float hi)
{
    const char* v = getenv(name);
    if (v == NULL || *v == 0) {
        return fallback;
    }
    float f = (float) atof(v);
    return f >= lo && f <= hi ? f : fallback;
}

static float _ptb_num(toml_table_t* t, const char* key, float fallback, float lo, float hi)
{
    toml_datum_t d = toml_double_in(t, key);
    float        f = fallback;
    if (d.ok) {
        f = (float) d.u.d;
    } else {
        d = toml_int_in(t, key);
        if (d.ok) {
            f = (float) d.u.i;
        }
    }
    return f >= lo && f <= hi ? f : fallback;
}

/* [ptb] of config.toml or of the map's toml: the rules' figures */
static void _ptb_rules_from(toml_table_t* t)
{
    if (t == NULL) {
        return;
    }
    ptb.freeze_seconds = (int) _ptb_num(t, "freeze_seconds", (float) ptb.freeze_seconds, 1, 60);
    ptb.round_seconds  = (int) _ptb_num(t, "round_seconds", (float) ptb.round_seconds, 20, 3600);
    ptb.bomb_seconds   = (int) _ptb_num(t, "bomb_seconds", (float) ptb.bomb_seconds, 5, 300);
    ptb.plant_seconds  = _ptb_num(t, "plant_seconds", ptb.plant_seconds, 0.5f, 30.0f);
    ptb.defuse_seconds = _ptb_num(t, "defuse_seconds", ptb.defuse_seconds, 0.5f, 60.0f);
    ptb.rounds_to_win  = (int) _ptb_num(t, "rounds_to_win", (float) ptb.rounds_to_win, 1, 30);
    ptb.halftime_round = (int) _ptb_num(t, "halftime_round", (float) ptb.halftime_round, 0, 59);
}

/* the sites of one [ptb] table (the map's, or the catalog's for this map) */
static int _ptb_sites_from(server_t* server, toml_table_t* t)
{
    toml_array_t* list = t != NULL ? toml_array_in(t, "sites") : NULL;
    int           n    = 0;
    for (int k = 0; list != NULL && k < toml_array_nelem(list) && n < 2; k++) {
        toml_table_t* s = toml_table_at(list, k);
        if (s == NULL) {
            continue;
        }
        ptb_site_t site;
        memset(&site, 0, sizeof site);
        toml_datum_t name = toml_string_in(s, "name");
        site.name[0]      = name.ok && name.u.s[0] ? name.u.s[0] : (char) ('A' + n);
        if (name.ok) {
            free(name.u.s);
        }
        site.x      = _ptb_num(s, "x", -1.0f, 0.0f, 4096.0f);
        site.y      = _ptb_num(s, "y", -1.0f, 0.0f, 4096.0f);
        site.z      = (int) _ptb_num(s, "z", -1.0f, 0.0f, 63.0f);
        site.radius = _ptb_num(s, "radius", 8.0f, 2.0f, 40.0f);
        map_store_t* map = &server->s_map.map;
        if (site.x < 0.0f || site.y < 0.0f || site.x >= map->size_x || site.y >= map->size_y) {
            LOG_WARNING("Plant the Bomb: site %d of the table is off the map: ignored", k);
            continue;
        }
        /* the table names a column: its middle */
        site.x += 0.5f;
        site.y += 0.5f;
        if (site.z <= 0) {
            site.z = map_top(map, (int) site.x, (int) site.y);
        }
        ptb.sites[n++] = site;
    }
    return n;
}

/* the catalog's table: Resources/ptb_sites.toml when the root has one (a
   host's own maps), else the copy built into the binary; NULL when neither
   parses. [<map name>] sites = [..] */
static toml_table_t* _ptb_table(void)
{
    char          err[200];
    toml_table_t* conf = NULL;
    FILE*         fp   = fopen(PTB_SITES_FILE, "r");
    if (fp != NULL) {
        conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        if (conf == NULL) {
            LOG_WARNING("Plant the Bomb: %s could not be read (%s): the built-in table applies", PTB_SITES_FILE,
                        err);
        }
    }
    if (conf == NULL) {
        char* copy = strdup(default_ptb_sites);
        if (copy != NULL) {
            conf = toml_parse(copy, err, sizeof err);
            free(copy);
        }
    }
    return conf;
}

static int _ptb_sites_from_table(server_t* server)
{
    toml_table_t* conf = _ptb_table();
    if (conf == NULL) {
        return 0;
    }
    int n = _ptb_sites_from(server, toml_table_in(conf, server->map_name));
    toml_free(conf);
    return n;
}

/* dry ground near (x, y): the nearest column in rings of up to 24 whose top
   is land (not the water layer, not the sky's edge) */
static int _ptb_land(server_t* server, float x, float y, ptb_site_t* out)
{
    map_store_t* map = &server->s_map.map;
    for (int r = 0; r <= 24; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) {
                    continue;
                }
                int cx = (int) x + dx, cy = (int) y + dy;
                if (cx < 2 || cy < 2 || cx >= map->size_x - 2 || cy >= map->size_y - 2) {
                    continue;
                }
                int top = map_top(map, cx, cy);
                if (top <= 4 || top >= map->size_z - 2) {
                    continue;
                }
                out->x = cx + 0.5f;
                out->y = cy + 0.5f;
                out->z = top;
                return 1;
            }
        }
    }
    return 0;
}

/* no site anywhere: two places across the way between the spawns, nearer the
   counter-terrorists' (in Counter-Strike the sites are closer to their spawn,
   the terrorists come to them). tools/find_bombsites.py does better, from the
   map's colours: its result goes into the table */
static int _ptb_sites_auto(server_t* server)
{
    float tx, ty, tz, cx, cy, cz;
    arena_kit_side_middle(server, PTB_T, &tx, &ty, &tz);
    arena_kit_side_middle(server, PTB_CT, &cx, &cy, &cz);
    float dx = cx - tx, dy = cy - ty, d = sqrtf(dx * dx + dy * dy);
    if (d < 8.0f) {
        return 0;
    }
    float ax = dx / d, ay = dy / d, lx = -ay, ly = ax;
    int   n  = 0;
    for (int side = -1; side <= 1; side += 2) {
        float      along = 0.65f * d, across = 0.3f * d * (float) side;
        ptb_site_t site;
        memset(&site, 0, sizeof site);
        if (!_ptb_land(server, tx + ax * along + lx * across, ty + ay * along + ly * across, &site)) {
            continue;
        }
        site.name[0] = (char) ('A' + n);
        site.radius  = 7.0f;
        ptb.sites[n++] = site;
    }
    return n;
}

static void _ptb_read_map(server_t* server, toml_table_t* map_toml)
{
    arena_kit_read_map(server, map_toml);
    ptb.freeze_seconds = 8;
    ptb.round_seconds  = 115;
    ptb.bomb_seconds   = 40;
    ptb.plant_seconds  = 3.2f;
    ptb.defuse_seconds = 10.0f;
    ptb.rounds_to_win  = 8;
    ptb.halftime_round = -1; /* -1: rounds_to_win - 1 */
    FILE* fp           = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        if (conf != NULL) {
            _ptb_rules_from(toml_table_in(conf, "ptb"));
            toml_free(conf);
        }
    }
    toml_table_t* own = map_toml != NULL ? toml_table_in(map_toml, "ptb") : NULL;
    _ptb_rules_from(own);
    /* the tests' shortcuts win over both */
    ptb.freeze_seconds = _ptb_env_int("SP_PTB_FREEZE_SECONDS", ptb.freeze_seconds, 1, 60);
    ptb.round_seconds  = _ptb_env_int("SP_PTB_ROUND_SECONDS", ptb.round_seconds, 20, 3600);
    ptb.bomb_seconds   = _ptb_env_int("SP_PTB_BOMB_SECONDS", ptb.bomb_seconds, 5, 300);
    ptb.plant_seconds  = _ptb_env_float("SP_PTB_PLANT_SECONDS", ptb.plant_seconds, 0.5f, 30.0f);
    ptb.defuse_seconds = _ptb_env_float("SP_PTB_DEFUSE_SECONDS", ptb.defuse_seconds, 0.5f, 60.0f);
    ptb.rounds_to_win  = _ptb_env_int("SP_PTB_ROUNDS_TO_WIN", ptb.rounds_to_win, 1, 30);
    ptb.halftime_round = _ptb_env_int("SP_PTB_HALFTIME_ROUND", ptb.halftime_round, 0, 59);
    /* Counter-Strike's halftime: half of the longest match (rounds_to_win
       8 is 15 rounds at most: the sides swap after round 7) */
    if (ptb.halftime_round < 0) {
        ptb.halftime_round = ptb.rounds_to_win - 1;
    }
    /* the sites come with the map's blocks (read_map runs after the load) */
    ptb.site_count = _ptb_sites_from(server, own);
    ptb.sites_from = "map";
    if (ptb.site_count < 2) {
        ptb.site_count = _ptb_sites_from_table(server);
        ptb.sites_from = "table";
    }
    if (ptb.site_count < 2) {
        ptb.site_count = 0;
        ptb.sites_from = "auto";
    }
}

static void _ptb_set_state(ptb_state_t s)
{
    ptb.state          = s;
    ptb.since          = get_nanos();
    ptb.said_countdown = -1;
    ptb.said_clock     = -1;
}

static int _ptb_in_team(const player_t* p)
{
    return (p->team == TEAM_A || p->team == TEAM_B) &&
           (p->state == STATE_READY || p->state == STATE_WAITING_FOR_RESPAWN || p->state == STATE_SPAWNING);
}

static int _ptb_alive(const player_t* p)
{
    return (p->team == TEAM_A || p->team == TEAM_B) && p->state == STATE_READY && p->alive;
}

static player_t* _ptb_player(server_t* server, uint8_t id)
{
    player_t* p = NULL;
    HASH_FIND(hh, server->players, &id, sizeof(id), p);
    return p;
}

/* the floor block under a body or a point: the first solid cell from z down */
static int _ptb_floor(server_t* server, float x, float y, float z)
{
    map_store_t* map = &server->s_map.map;
    int          cx = (int) floorf(x), cy = (int) floorf(y);
    for (int k = (int) floorf(z); k < map->size_z - 1; k++) {
        if (k >= 0 && map_solid(map, cx, cy, k)) {
            return k;
        }
    }
    return map->size_z - 1;
}

static int _ptb_on_site(server_t* server, const player_t* p)
{
    float x = p->movement.position.x, y = p->movement.position.y;
    int   floor = _ptb_floor(server, x, y, p->movement.position.z + 1.0f);
    for (int k = 0; k < ptb.site_count; k++) {
        float dx = x - ptb.sites[k].x, dy = y - ptb.sites[k].y;
        if (dx * dx + dy * dy <= ptb.sites[k].radius * ptb.sites[k].radius && abs(floor - ptb.sites[k].z) <= 4) {
            return k;
        }
    }
    return -1;
}

/* ---- the crater: put back at the next round, so every round plays the
   map as it was made (the arena's rule) ---- */

static void _ptb_crater(server_t* server, vector3f_t at)
{
    map_store_t* map = &server->s_map.map;
    int          cx = (int) floorf(at.x), cy = (int) floorf(at.y), cz = (int) floorf(at.z);
    for (int z = cz - PTB_CRATER_R; z <= cz + PTB_CRATER_R; z++) {
        for (int y = cy - PTB_CRATER_R; y <= cy + PTB_CRATER_R; y++) {
            for (int x = cx - PTB_CRATER_R; x <= cx + PTB_CRATER_R; x++) {
                int ddx = x - cx, ddy = y - cy, ddz = z - cz;
                if (ddx * ddx + ddy * ddy + ddz * ddz > PTB_CRATER_R * PTB_CRATER_R || z < 0 || z >= 62 ||
                    x < 0 || y < 0 || x >= map->size_x || y >= map->size_y || !map_solid(map, x, y, z) ||
                    arena_kit_gate_cell(x, y, z) || ptb.crater_count >= PTB_CRATER_MAX)
                {
                    continue;
                }
                ptb.crater[ptb.crater_count++] = (ptb_cell_t) {x, y, z, map_color(map, x, y, z)};
                map_set_air(map, x, y, z);
                proto10_world_cell(server, x, y, z, P10_WORLD_CAUSE_PLAYER);
            }
        }
    }
    if (ptb.crater_count > 0) {
        gamemode_terrain_changed(server, 0);
    }
}

static void _ptb_crater_restore(server_t* server)
{
    if (ptb.crater_count == 0) {
        return;
    }
    map_store_t* map = &server->s_map.map;
    for (int k = 0; k < ptb.crater_count; k++) {
        ptb_cell_t* c = &ptb.crater[k];
        map_set_color(map, c->x, c->y, c->z, c->color | 0xFF000000);
        proto10_world_cell(server, c->x, c->y, c->z, P10_WORLD_CAUSE_NONE);
    }
    evt_log(NULL, "ptb", "action=crater_restore cells=%d", ptb.crater_count);
    ptb.crater_count = 0;
    gamemode_terrain_changed(server, 1);
}

/* ---- the bomb ---- */

/* Where the planted bomb is reaches only a client within PTB_BOMB_HEAR of
   it: nothing on the wire says which site holds it (the owner's rule, as in
   Counter-Strike: the defenders find it by its beeps). A client in range is
   told "ptb-bomb-near x y z" (it draws the bomb and beeps it there: it hears
   it anyway), out of range "ptb-bomb-far"; at the blast the ones in range get
   "ptb-bomb-blast x y z". There is no entity: every client receives every
   entity, with its position */
static uint8_t ptb_told[SERVER_MAX_PLAYERS];

static void _ptb_tell(server_t* server, player_t* p, const char* what)
{
    char        v[3][12];
    const char* parts[4] = {what, v[0], v[1], v[2]};
    snprintf(v[0], sizeof v[0], "%d", (int) ptb.bomb_at.x);
    snprintf(v[1], sizeof v[1], "%d", (int) ptb.bomb_at.y);
    snprintf(v[2], sizeof v[2], "%d", (int) ptb.bomb_at.z);
    proto10_bridge_generic(server, p, strcmp(what, "ptb-bomb-far") ? 4 : 1, parts);
}

static float _ptb_bomb_dist(const player_t* p)
{
    float dx = p->movement.position.x - ptb.bomb_at.x, dy = p->movement.position.y - ptb.bomb_at.y;
    float dz = p->movement.position.z + 2.0f - ptb.bomb_at.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* every tick while it ticks: who comes into range, who leaves it */
static void _ptb_hear_tick(server_t* server)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot || p->id >= SERVER_MAX_PLAYERS || p->state == STATE_DISCONNECTED) {
            continue;
        }
        int near = p->state == STATE_READY && _ptb_bomb_dist(p) <= PTB_BOMB_HEAR;
        if (near != ptb_told[p->id]) {
            ptb_told[p->id] = (uint8_t) near;
            _ptb_tell(server, p, near ? "ptb-bomb-near" : "ptb-bomb-far");
            evt_log(p, "ptb", "action=hear near=%d dist=%.0f", near, _ptb_bomb_dist(p));
        }
    }
}

/* the round is over or the bomb defused: nobody keeps it; blast: the ones
   in range see it go off where it was */
static void _ptb_bomb_forget(server_t* server, int blast)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->id < SERVER_MAX_PLAYERS && ptb_told[p->id] && p->state != STATE_DISCONNECTED) {
            _ptb_tell(server, p, blast ? "ptb-bomb-blast" : "ptb-bomb-far");
        }
    }
    memset(ptb_told, 0, sizeof ptb_told);
}

static void _ptb_progress(server_t* server, player_t* p, int defuse, int permille, int active)
{
    char        v[2][12];
    const char* parts[3] = {defuse ? "ptb-defuse" : "ptb-plant", v[0], v[1]};
    snprintf(v[0], sizeof v[0], "%d", permille);
    snprintf(v[1], sizeof v[1], "%d", active);
    if (p != NULL && !p->is_bot) {
        proto10_bridge_generic(server, p, 3, parts);
    }
}

static void _ptb_refused(server_t* server, player_t* p, const char* why)
{
    const char* parts[2] = {"ptb-refused", why};
    if (p != NULL && !p->is_bot) {
        proto10_bridge_generic(server, p, 2, parts);
    }
}

static void _ptb_act_cancel(server_t* server, const char* why)
{
    if (ptb.act_player == 255) {
        return;
    }
    player_t* p = _ptb_player(server, ptb.act_player);
    evt_log(p, "ptb", "action=%s_cancel why=%s", ptb.act_defuse ? "defuse" : "plant", why);
    _ptb_progress(server, p, ptb.act_defuse, 0, 0);
    ptb.act_player = 255;
}

static void _ptb_drop(server_t* server, player_t* carrier, const char* why)
{
    if (ptb.bomb != PTB_BOMB_CARRIED || carrier == NULL || ptb.carrier != carrier->id) {
        return;
    }
    if (ptb.act_player == carrier->id) {
        _ptb_act_cancel(server, why);
    }
    vector3f_t pos = carrier->movement.position;
    int        floor = _ptb_floor(server, pos.x, pos.y, pos.z + 1.0f);
    ptb.bomb_at        = (vector3f_t) {floorf(pos.x) + 0.5f, floorf(pos.y) + 0.5f, (float) floor};
    ptb.bomb           = PTB_BOMB_DROPPED;
    ptb.carrier        = 255;
    ptb.drops++;
    evt_log(carrier, "ptb", "action=drop why=%s at=%.1f,%.1f,%.0f", why, ptb.bomb_at.x, ptb.bomb_at.y, ptb.bomb_at.z);
}

static void _ptb_give(server_t* server, player_t* p, const char* why)
{
    (void) server;
    ptb.bomb    = PTB_BOMB_CARRIED;
    ptb.carrier = p->id;
    evt_log(p, "ptb", "action=carry why=%s", why);
}

static void _ptb_plant(server_t* server, player_t* p, int site, vector3f_t base)
{
    memset(ptb_told, 0, sizeof ptb_told);
    ptb.bomb       = PTB_BOMB_PLANTED;
    ptb.carrier    = 255;
    ptb.bomb_at    = base;
    ptb.bomb_site  = site;
    ptb.planter    = p->id;
    ptb.planted_at = get_nanos();
    ptb.plants++;
    evt_log(p, "ptb", "action=plant site=%s at=%.1f,%.1f,%.0f fuse=%d", ptb.sites[site].name, base.x, base.y,
            base.z, ptb.bomb_seconds);
    _ptb_hear_tick(server);
}

/* the blast's author: whoever planted it, else a terrorist still here (a
   blast needs someone to credit), else nobody: the blocks still go */
static player_t* _ptb_blast_author(server_t* server)
{
    player_t* p = _ptb_player(server, ptb.planter);
    if (p != NULL && p->state != STATE_DISCONNECTED) {
        return p;
    }
    player_t *q, *tmp;
    HASH_ITER(hh, server->players, q, tmp)
    {
        if (q->team == PTB_T && q->state != STATE_DISCONNECTED) {
            return q;
        }
    }
    return NULL;
}

static void _ptb_end_round(server_t* server, int winner, const char* why);

static void _ptb_explode(server_t* server)
{
    vector3f_t at = {ptb.bomb_at.x, ptb.bomb_at.y, ptb.bomb_at.z - 0.5f};
    _ptb_bomb_forget(server, 1);
    ptb.bomb = PTB_BOMB_EXPLODED;
    ptb.explosions++;
    player_t* author = _ptb_blast_author(server);
    if (author != NULL) {
        explosion_blast_retail(server, author, at, 0, PTB_BLAST_PEAK, PTB_BLAST_RADIUS, 1.0f, KILL_REASON_EXPLOSIVE,
                               "ptb_bomb");
    }
    _ptb_crater(server, at);
    evt_log(author, "ptb", "action=explode site=%s at=%.1f,%.1f,%.0f crater=%d", ptb.sites[ptb.bomb_site].name, at.x,
            at.y, at.z, ptb.crater_count);
    _ptb_end_round(server, PTB_T, "bomb");
}

static void _ptb_defused(server_t* server, player_t* p)
{
    _ptb_bomb_forget(server, 0);
    ptb.bomb = PTB_BOMB_DEFUSED;
    ptb.defuses++;
    float left = (float) ptb.bomb_seconds - (float) (get_nanos() - ptb.planted_at) / (float) NANO_IN_SECOND;
    evt_log(p, "ptb", "action=defuse site=%s left=%.1f", ptb.sites[ptb.bomb_site].name, left);
    _ptb_end_round(server, PTB_CT, "defuse");
}

/* USE held or let go: the client's "ptb-use" and a bot's goal both come here */
static void _ptb_set_use(server_t* server, player_t* p, int held)
{
    if (p->id >= SERVER_MAX_PLAYERS) {
        return;
    }
    held = held ? 1 : 0;
    if (ptb.use[p->id] == held) {
        return;
    }
    ptb.use[p->id] = (uint8_t) held;
    if (!held && ptb.act_player == p->id) {
        _ptb_act_cancel(server, "released");
    }
    if (!held || ptb.state != PTB_FIGHT || !_ptb_alive(p)) {
        return;
    }
    /* a press that cannot start anything says why (once a press) */
    if (p->team == PTB_T) {
        if (ptb.bomb != PTB_BOMB_CARRIED || ptb.carrier != p->id) {
            _ptb_refused(server, p, ptb.bomb == PTB_BOMB_PLANTED ? "planted" : "no_bomb");
        } else if (_ptb_on_site(server, p) < 0) {
            _ptb_refused(server, p, "site");
        }
    } else if (p->team == PTB_CT && ptb.bomb != PTB_BOMB_PLANTED) {
        _ptb_refused(server, p, "not_planted");
    }
}

/* the plant or the defuse, every tick: who may start one, whether the one
   going on still stands, and its end */
static void _ptb_act_tick(server_t* server)
{
    uint64_t now = get_nanos();
    if (ptb.act_player != 255) {
        player_t* p = _ptb_player(server, ptb.act_player);
        if (p == NULL || !_ptb_alive(p) || !ptb.use[p->id] || ptb.state != PTB_FIGHT) {
            _ptb_act_cancel(server, p == NULL || !_ptb_alive(p) ? "dead" : "released");
            return;
        }
        float dx = p->movement.position.x - ptb.act_at.x, dy = p->movement.position.y - ptb.act_at.y;
        if (dx * dx + dy * dy > PTB_STILL_DRIFT * PTB_STILL_DRIFT) {
            _ptb_refused(server, p, "moved");
            _ptb_act_cancel(server, "moved");
            return;
        }
        /* crouched the whole hold, as in Counter-Strike: a bot is held down
           here (after its think, before the physics), a client crouches
           itself while USE is held (ptb.c) and standing up gives it up */
        if (p->is_bot) {
            p->crouching = 1;
        } else if (!p->crouching && now - ptb.act_since > PTB_CROUCH_GRACE) {
            _ptb_refused(server, p, "stood");
            _ptb_act_cancel(server, "stood");
            return;
        }
        if (p->hurt_at > ptb.act_since) {
            _ptb_refused(server, p, "hit");
            _ptb_act_cancel(server, "hit");
            return;
        }
        /* not "airborne": crouching down for the hold lifts the body's box
           off the ground for a tick, and every plant was cancelled. A jump
           stands the body up (the stood rule) or moves it (the drift) */
        if (!ptb.act_defuse && (ptb.bomb != PTB_BOMB_CARRIED || ptb.carrier != p->id)) {
            _ptb_act_cancel(server, "gone");
            return;
        }
        if (ptb.act_defuse && ptb.bomb != PTB_BOMB_PLANTED) {
            _ptb_act_cancel(server, "gone");
            return;
        }
        float need = ptb.act_defuse ? ptb.defuse_seconds : ptb.plant_seconds;
        float done = (float) (now - ptb.act_since) / (float) NANO_IN_SECOND / need;
        if (done >= 1.0f) {
            ptb.act_player = 255;
            _ptb_progress(server, p, ptb.act_defuse, 1000, 0);
            if (ptb.act_defuse) {
                _ptb_defused(server, p);
            } else {
                int        site  = _ptb_on_site(server, p);
                vector3f_t pos   = p->movement.position;
                int        floor = _ptb_floor(server, pos.x, pos.y, pos.z + 1.0f);
                _ptb_plant(server, p, site >= 0 ? site : 0,
                           (vector3f_t) {floorf(pos.x) + 0.5f, floorf(pos.y) + 0.5f, (float) floor});
            }
            return;
        }
        if (now - ptb.act_sent >= PTB_PROGRESS_EVERY) {
            ptb.act_sent = now;
            _ptb_progress(server, p, ptb.act_defuse, (int) (done * 1000.0f), 1);
        }
        return;
    }
    if (ptb.state != PTB_FIGHT) {
        return;
    }
    /* who may start: the carrier on a site, standing; a counter-terrorist
       by the planted bomb */
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->id >= SERVER_MAX_PLAYERS || !ptb.use[p->id] || !_ptb_alive(p) ||
            hypotf(p->movement.velocity.x, p->movement.velocity.y) > PTB_START_SPEED)
        {
            continue;
        }
        int defuse = 0;
        if (p->team == PTB_T && ptb.bomb == PTB_BOMB_CARRIED && ptb.carrier == p->id && !p->airborne &&
            _ptb_on_site(server, p) >= 0)
        {
            defuse = 0;
        } else if (p->team == PTB_CT && ptb.bomb == PTB_BOMB_PLANTED) {
            float dx = p->movement.position.x - ptb.bomb_at.x, dy = p->movement.position.y - ptb.bomb_at.y;
            float dz = p->movement.position.z + 2.0f - ptb.bomb_at.z;
            if (dx * dx + dy * dy > PTB_DEFUSE_REACH * PTB_DEFUSE_REACH || fabsf(dz) > 3.0f) {
                continue;
            }
            defuse = 1;
        } else {
            continue;
        }
        ptb.act_player = p->id;
        ptb.act_defuse = (uint8_t) defuse;
        ptb.act_since  = now;
        ptb.act_sent   = now;
        ptb.act_at     = p->movement.position;
        _ptb_progress(server, p, defuse, 0, 1);
        evt_log(p, "ptb", "action=%s_start speed=%.3f", defuse ? "defuse" : "plant",
                hypotf(p->movement.velocity.x, p->movement.velocity.y));
        return;
    }
}

static void _ptb_pickup_tick(server_t* server)
{
    if (ptb.bomb != PTB_BOMB_DROPPED) {
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team != PTB_T || !_ptb_alive(p)) {
            continue;
        }
        float dx = p->movement.position.x - ptb.bomb_at.x, dy = p->movement.position.y - ptb.bomb_at.y;
        float dz = p->movement.position.z + 2.0f - ptb.bomb_at.z;
        if (dx * dx + dy * dy <= PTB_PICKUP_REACH * PTB_PICKUP_REACH && fabsf(dz) <= 3.5f) {
            ptb.pickups++;
            _ptb_give(server, p, "pickup");
            return;
        }
    }
}

/* ---- the rounds ---- */

static void _ptb_init(server_t* server)
{
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    /* the sides are what they are in this mode, whatever config.toml calls
       them (ten characters at most: the 0.75 wire's size, which the client's
       copy keeps); the popups say the long names */
    snprintf(server->protocol.name_team[PTB_CT], sizeof(server->protocol.name_team[0]), "CT Forces");
    snprintf(server->protocol.name_team[PTB_T], sizeof(server->protocol.name_team[0]), "Terrorists");
    server->protocol.color_team[PTB_CT].r = 62;
    server->protocol.color_team[PTB_CT].g = 153;
    server->protocol.color_team[PTB_CT].b = 234;
    server->protocol.color_team[PTB_T].r  = 232;
    server->protocol.color_team[PTB_T].g  = 164;
    server->protocol.color_team[PTB_T].b  = 40;
    server->protocol.gamemode.score_limit = (uint8_t) ptb.rounds_to_win;
    arena_kit_gates_init(server);
    if (ptb.site_count < 2) {
        ptb.site_count = _ptb_sites_auto(server);
        ptb.sites_from = "auto";
    }
    ptb.round        = 0;
    ptb.swapped      = 0;
    ptb.halftime     = 0;
    ptb.winner       = -1;
    ptb.why          = "";
    ptb.match_over   = 0;
    ptb.bomb         = PTB_BOMB_NONE;
    ptb.carrier      = 255;
    memset(ptb_told, 0, sizeof ptb_told);
    ptb.act_player   = 255;
    ptb.crater_count = 0;
    ptb.plants = ptb.explosions = ptb.defuses = ptb.drops = ptb.pickups = 0;
    memset(ptb.use, 0, sizeof ptb.use);
    _ptb_set_state(PTB_WAITING);
    LOG_INFO("Plant the Bomb: freeze %d s, rounds of %d s, bomb %d s, plant %.1f s, defuse %.1f s, %d rounds to "
             "win, sides swap after round %d, %d sites (%s)",
             ptb.freeze_seconds, ptb.round_seconds, ptb.bomb_seconds, ptb.plant_seconds, ptb.defuse_seconds,
             ptb.rounds_to_win, ptb.halftime_round, ptb.site_count, ptb.sites_from);
    for (int k = 0; k < ptb.site_count; k++) {
        evt_log(NULL, "ptb", "action=site name=%s at=%.1f,%.1f,%d radius=%.1f from=%s", ptb.sites[k].name,
                ptb.sites[k].x, ptb.sites[k].y, ptb.sites[k].z, ptb.sites[k].radius, ptb.sites_from);
    }
    evt_log(NULL, "ptb", "action=rules freeze=%d round=%d bomb=%d plant=%.1f defuse=%.1f rounds=%d gates=%d sites=%d",
            ptb.freeze_seconds, ptb.round_seconds, ptb.bomb_seconds, ptb.plant_seconds, ptb.defuse_seconds,
            ptb.rounds_to_win, arena_kit_gate_count(), ptb.site_count);
}

static void _ptb_start_freeze(server_t* server)
{
    ptb.round++;
    _ptb_bomb_forget(server, 0);
    _ptb_crater_restore(server);
    _ptb_set_state(PTB_FREEZE);
    arena_kit_set_gates(server, 1);
    grenades_clear_all(server); /* none thrown last round explodes in this one */
    ptb.winner     = -1;
    ptb.why        = "";
    ptb.bomb       = PTB_BOMB_NONE;
    ptb.carrier    = 255;
    ptb.act_player = 255;
    memset(ptb.use, 0, sizeof ptb.use);
    /* the round's site for the terrorists' bots, and each bot's draw */
    /* the nearer site to the terrorists' spawn two rounds of three: a far
       site the bots took every other round ran out the clock on the long
       maps (Titanic_Arena's bow: 2 plants a match) */
    ptb.target_site = 0;
    if (ptb.site_count > 1) {
        float tx, ty, tz;
        arena_kit_side_middle(server, PTB_T, &tx, &ty, &tz);
        float d0     = hypotf(ptb.sites[0].x - tx, ptb.sites[0].y - ty);
        float d1     = hypotf(ptb.sites[1].x - tx, ptb.sites[1].y - ty);
        int   nearer = d1 < d0 ? 1 : 0;
        ptb.target_site = gen_rand_long(&server->rand) % 3UL < 2UL ? nearer : 1 - nearer;
    }
    for (int k = 0; k < SERVER_MAX_PLAYERS; k++) {
        ptb_bot_roll[k] = (uint8_t) (gen_rand_long(&server->rand) % 256UL);
    }
    /* everyone, alive or not, starts the round fresh behind their gate; one
       terrorist gets the bomb */
    int       count = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_ptb_in_team(p)) {
            p->state = STATE_SPAWNING;
            count += p->team == PTB_T;
        }
    }
    /* a test that plays the carrier (the harness): the first human
       terrorist gets the bomb */
    const char* test_carrier = getenv("SP_PTB_TEST_CARRIER");
    if (count > 0 && test_carrier != NULL && !strcmp(test_carrier, "human")) {
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (_ptb_in_team(p) && p->team == PTB_T && !p->is_bot) {
                _ptb_give(server, p, "test");
                count = 0;
                break;
            }
        }
    }
    if (count > 0) {
        int pick = (int) (gen_rand_long(&server->rand) % (unsigned long) count);
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (_ptb_in_team(p) && p->team == PTB_T && pick-- == 0) {
                _ptb_give(server, p, "round");
                break;
            }
        }
    }
    evt_log(NULL, "ptb", "action=freeze round=%d score=%u-%u seconds=%d target=%s", ptb.round,
            server->protocol.gamemode.score[0], server->protocol.gamemode.score[1], ptb.freeze_seconds,
            ptb.site_count > 0 ? ptb.sites[ptb.target_site].name : "-");
}

static void _ptb_end_round(server_t* server, int winner, const char* why)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    _ptb_act_cancel(server, "round_end");
    grenades_clear_all(server); /* a round ended: nothing in flight carries over */
    ptb.winner = winner;
    ptb.why    = why;
    if (winner == TEAM_A || winner == TEAM_B) {
        gm->score[winner]++;
        ptb.match_over = gm->score[winner] >= gm->score_limit;
    }
    ptb.halftime = !ptb.match_over && !ptb.swapped && ptb.halftime_round > 0 && ptb.round == ptb.halftime_round;
    /* a bomb still ticking when the last counter-terrorist fell goes with
       the round: the next one starts clean */
    if (ptb.bomb == PTB_BOMB_PLANTED) {
        _ptb_bomb_forget(server, 0);
    }
    _ptb_set_state(PTB_END);
    evt_log(NULL, "ptb", "action=round_end round=%d winner=%s why=%s bomb=%s score=%u-%u match_over=%u", ptb.round,
            winner == PTB_CT ? "ct" : winner == PTB_T ? "t" : "draw", why, ptb_bomb_names[ptb.bomb], gm->score[0],
            gm->score[1], ptb.match_over);
}

/* Halftime: every player of a side goes to the other, alive or not, with
   no death (the zombie mode's mover), and the rounds won go with the
   people: team 1 is always the defenders (the spawns, the name, the blue),
   so the two scores swap places. The freeze that follows respawns
   everyone on its new side's spawns and gives the bomb to a new terrorist.
   The clients see each player's new team in its PlayerUpdate and are told
   "ptb-swap" for the popup */
static void _ptb_swap(server_t* server)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    int              moved = 0;
    player_t *       p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team != TEAM_A && p->team != TEAM_B) {
            continue;
        }
        uint8_t to = p->team == TEAM_A ? TEAM_B : TEAM_A;
        if (server->protocol.num_team_users[p->team] > 0) {
            server->protocol.num_team_users[p->team]--;
        }
        p->team = to;
        server->protocol.num_team_users[to]++;
        moved++;
    }
    uint8_t keep = gm->score[0];
    gm->score[0] = gm->score[1];
    gm->score[1] = keep;
    ptb.swapped  = 1;
    ptb.halftime = 0;
    const char* parts[1] = {"ptb-swap"};
    proto10_bridge_generic(server, NULL, 1, parts);
    evt_log(NULL, "ptb", "action=swap round=%d moved=%d score=%u-%u", ptb.round, moved, gm->score[0], gm->score[1]);
}

static void _ptb_tick(server_t* server)
{
    int       present[2] = {0, 0}, alive[2] = {0, 0}, humans = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_ptb_in_team(p)) {
            continue;
        }
        present[p->team]++;
        humans += !p->is_bot;
        if (p->state == STATE_SPAWNING || (p->state == STATE_READY && p->alive)) {
            alive[p->team]++;
        }
    }
    int   go      = humans > 0 || bots_play_alone();
    int   both    = present[0] > 0 && present[1] > 0;
    float elapsed = (float) (get_nanos() - ptb.since) / (float) NANO_IN_SECOND;

    /* the carrier left the team or the game without a death (player_out
       handles a death): the bomb lies where it was */
    if (ptb.bomb == PTB_BOMB_CARRIED) {
        player_t* c = _ptb_player(server, ptb.carrier);
        if (c == NULL || c->team != PTB_T || c->state == STATE_DISCONNECTED) {
            if (c != NULL) {
                _ptb_drop(server, c, "left");
            } else {
                ptb.bomb    = PTB_BOMB_DROPPED;
                ptb.carrier = 255;
            }
        }
    }

    switch (ptb.state) {
        case PTB_WAITING:
            if (both && go) {
                _ptb_start_freeze(server);
            }
            break;
        case PTB_FREEZE:
            if (!both || !go) {
                _ptb_set_state(PTB_WAITING);
                arena_kit_set_gates(server, 0);
                evt_log(NULL, "ptb", "action=waiting why=%s", both ? "no_human" : "team_empty");
            } else if (elapsed >= (float) ptb.freeze_seconds) {
                _ptb_set_state(PTB_FIGHT);
                arena_kit_set_gates(server, 0);
                ptb.last_alive[0] = alive[0];
                ptb.last_alive[1] = alive[1];
                evt_log(NULL, "ptb", "action=fight round=%d alive=%d-%d", ptb.round, alive[0], alive[1]);
                /* a test of the defuse (the harness): the bomb planted on a
                   site's middle as the fight starts, by its carrier */
                const char* test_plant = getenv("SP_PTB_TEST_PLANT");
                player_t*   c          = ptb.bomb == PTB_BOMB_CARRIED ? _ptb_player(server, ptb.carrier) : NULL;
                if (test_plant != NULL && c != NULL && ptb.site_count > 0) {
                    int k = test_plant[0] == 'B' && ptb.site_count > 1 ? 1 : 0;
                    _ptb_plant(server, c, k,
                               (vector3f_t) {floorf(ptb.sites[k].x) + 0.5f, floorf(ptb.sites[k].y) + 0.5f,
                                             (float) ptb.sites[k].z});
                }
            } else {
                int left = ptb.freeze_seconds - (int) elapsed;
                if (left != ptb.said_countdown) {
                    ptb.said_countdown = left;
                    if (left >= 1 && left <= 5 && left < ptb.freeze_seconds) {
                        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "%d", left);
                    }
                }
                arena_kit_keep_in_spawn(server);
            }
            break;
        case PTB_FIGHT: {
            if (!go) {
                _ptb_set_state(PTB_WAITING);
                arena_kit_set_gates(server, 0);
                evt_log(NULL, "ptb", "action=waiting why=no_human");
                break;
            }
            _ptb_pickup_tick(server);
            _ptb_act_tick(server);
            if (ptb.state != PTB_FIGHT) {
                break; /* the defuse ended the round */
            }
            if ((alive[0] < ptb.last_alive[0] || alive[1] < ptb.last_alive[1]) && alive[0] > 0 && alive[1] > 0) {
                broadcast_server_notice_kind(server, 0, NOTICE_INFO, "%d vs %d remain.", alive[0], alive[1]);
            }
            ptb.last_alive[0] = alive[0];
            ptb.last_alive[1] = alive[1];
            if (ptb.bomb == PTB_BOMB_PLANTED) {
                _ptb_hear_tick(server);
                float fuse = (float) (get_nanos() - ptb.planted_at) / (float) NANO_IN_SECOND;
                if (fuse >= (float) ptb.bomb_seconds) {
                    _ptb_explode(server);
                } else if (alive[PTB_CT] == 0) {
                    /* Counter-Strike: the last defender down after the plant
                       is the terrorists' round already */
                    _ptb_end_round(server, PTB_T, "eliminated");
                }
                /* every terrorist down after the plant: the round goes on, the
                   defenders still have to defuse */
                break;
            }
            if (alive[PTB_CT] == 0 && alive[PTB_T] == 0) {
                _ptb_end_round(server, 2, "draw");
            } else if (alive[PTB_T] == 0) {
                _ptb_end_round(server, PTB_CT, "eliminated");
            } else if (alive[PTB_CT] == 0) {
                _ptb_end_round(server, PTB_T, "eliminated");
            } else if (elapsed >= (float) ptb.round_seconds) {
                _ptb_end_round(server, PTB_CT, "time");
            } else {
                int left = ptb.round_seconds - (int) elapsed;
                if (left != ptb.said_clock) {
                    ptb.said_clock = left;
                    if (left == 30) {
                        broadcast_server_notice_kind(server, 0, NOTICE_BIG, "%d seconds left in the round.", left);
                    }
                }
            }
            break;
        }
        case PTB_END:
            if (elapsed < (float) (PTB_END_SECONDS + (ptb.halftime ? PTB_HALFTIME_SECONDS : 0))) {
                break;
            }
            if (ptb.halftime) {
                _ptb_swap(server);
            }
            if (ptb.match_over) {
                evt_log(NULL, "ptb", "action=match_end score=%u-%u rounds=%d plants=%d explosions=%d defuses=%d "
                                     "drops=%d pickups=%d",
                        server->protocol.gamemode.score[0], server->protocol.gamemode.score[1], ptb.round, ptb.plants,
                        ptb.explosions, ptb.defuses, ptb.drops, ptb.pickups);
                gamemode_match_won(server);
            } else if (both && go) {
                _ptb_start_freeze(server);
            } else {
                _ptb_set_state(PTB_WAITING);
                arena_kit_set_gates(server, 0);
            }
            break;
    }
}

static void _ptb_player_out(server_t* server, player_t* player)
{
    if (player->id < SERVER_MAX_PLAYERS) {
        ptb.use[player->id] = 0;
    }
    if (ptb.act_player == player->id) {
        _ptb_act_cancel(server, "out");
    }
    _ptb_drop(server, player, "death");
}

static int _ptb_allows_respawn(server_t* server, player_t* player)
{
    (void) server;
    (void) player;
    return ptb.state == PTB_WAITING || ptb.state == PTB_FREEZE;
}

static int _ptb_cell_protected(server_t* server, int x, int y, int z)
{
    (void) server;
    return arena_kit_gate_cell(x, y, z);
}

static unsigned _ptb_player_rules(server_t* server, player_t* player)
{
    (void) server;
    (void) player;
    return GM_RULE_NO_BLOCK | GM_RULE_NO_DESTROY;
}

static int _ptb_weapon_change_instant(server_t* server, player_t* player)
{
    (void) server;
    return ptb.state == PTB_FREEZE && arena_kit_gates_closed() && player->alive &&
           (player->team == TEAM_A || player->team == TEAM_B);
}

static int _ptb_seconds_left(void)
{
    int total = ptb.state == PTB_FREEZE ? ptb.freeze_seconds : ptb.state == PTB_FIGHT ? ptb.round_seconds : 0;
    int left  = total - (int) ((get_nanos() - ptb.since) / NANO_IN_SECOND);
    return left > 0 ? left : 0;
}

static int _ptb_bomb_left(void)
{
    if (ptb.bomb != PTB_BOMB_PLANTED) {
        return 0;
    }
    int left = ptb.bomb_seconds - (int) ((get_nanos() - ptb.planted_at) / NANO_IN_SECOND);
    return left > 0 ? left : 0;
}

static void _ptb_state_keys(server_t* server, gm_keys_t* out)
{
    (void) server;
    /* the arena's round keys: the client draws the clock and the rounds */
    gm_keys_put(out, P10_GM_ARENA_STATE, "%s", ptb_state_names[ptb.state]);
    gm_keys_put(out, P10_GM_ARENA_ROUND, "%d", ptb.round);
    gm_keys_put(out, P10_GM_ARENA_TIME, "%d", _ptb_seconds_left());
    gm_keys_put(out, P10_GM_ARENA_WINNER, "%s",
                ptb.winner == TEAM_A   ? "team1"
                : ptb.winner == TEAM_B ? "team2"
                : ptb.winner == 2      ? "draw"
                                       : "");
    /* never an empty value: a key whose value turns empty is not sent, and
       the clients would keep the old one (a defuser of the last round) */
    gm_keys_put(out, P10_GM_PTB_WHY, "%s", ptb.state == PTB_END && ptb.why[0] ? ptb.why : "-");
    gm_keys_put(out, P10_GM_PTB_BOMB, "%s", ptb_bomb_names[ptb.bomb]);
    if (ptb.bomb == PTB_BOMB_CARRIED) {
        gm_keys_put(out, P10_GM_PTB_CARRIER, "%u", (unsigned) ptb.carrier);
    } else {
        gm_keys_put(out, P10_GM_PTB_CARRIER, "%s", "-1");
    }
    /* the dropped bomb only: where the planted one is never goes to
       everyone (_ptb_hear_tick) */
    if (ptb.bomb == PTB_BOMB_DROPPED) {
        gm_keys_put(out, P10_GM_PTB_BOMB_AT, "%d,%d,%d", (int) ptb.bomb_at.x, (int) ptb.bomb_at.y,
                    (int) ptb.bomb_at.z);
    } else {
        gm_keys_put(out, P10_GM_PTB_BOMB_AT, "%s", "-1,-1,-1");
    }
    gm_keys_put(out, P10_GM_PTB_BOMB_TIME, "%d", _ptb_bomb_left());
    gm_keys_put(out, P10_GM_PTB_BOMB_TOTAL, "%d", ptb.bomb_seconds);
    if (ptb.act_player != 255 && ptb.act_defuse) {
        gm_keys_put(out, P10_GM_PTB_DEFUSER, "%u", (unsigned) ptb.act_player);
    } else {
        gm_keys_put(out, P10_GM_PTB_DEFUSER, "%s", "-1");
    }
    gm_keys_put(out, P10_GM_PTB_PLANTER, "%d", ptb.act_player != 255 && !ptb.act_defuse ? (int) ptb.act_player : -1);
    for (int k = 0; k < ptb.site_count; k++) {
        gm_keys_put(out, k == 0 ? P10_GM_PTB_SITE_A : P10_GM_PTB_SITE_B, "%d,%d,%d,%d", (int) ptb.sites[k].x,
                    (int) ptb.sites[k].y, ptb.sites[k].z, (int) (ptb.sites[k].radius + 0.5f));
    }
}

static int _ptb_player_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    if (strcmp(command, "ptb-use") != 0) {
        return 0;
    }
    _ptb_set_use(server, player, arg != NULL && atoi(arg) != 0);
    return 1;
}

static int _ptb_player_carries(server_t* server, player_t* player)
{
    (void) server;
    /* the carrier, and the one defusing: neither stops for a fight nor
       steps aside */
    return (ptb.bomb == PTB_BOMB_CARRIED && ptb.carrier == player->id) ||
           (ptb.act_player == player->id && ptb.act_defuse);
}

static int _ptb_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    float z = 0.0f;
    arena_kit_side_middle(server, player->team == TEAM_A ? TEAM_B : TEAM_A, x, y, &z);
    return 1;
}

/* ---- the bots ---- */

/* the bot's rank among its team's living bots (by id), and their count */
static int _ptb_rank(server_t* server, player_t* bot, int* count)
{
    int       rank = 0, n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!p->is_bot || p->team != bot->team || !_ptb_alive(p)) {
            continue;
        }
        n++;
        rank += p->id < bot->id;
    }
    *count = n;
    return rank;
}

/* living players of a team within r of (x, y) */
static int _ptb_near(server_t* server, int team, float x, float y, float r)
{
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if ((int) p->team != team || !_ptb_alive(p)) {
            continue;
        }
        float dx = p->movement.position.x - x, dy = p->movement.position.y - y;
        n += dx * dx + dy * dy <= r * r;
    }
    return n;
}

/* a spot of a ring round (x, y), the k-th of n, on land at the bot's level */
static void _ptb_ring_spot(server_t* server, player_t* bot, float x, float y, float r, int k, int n, gm_bot_goal_t* out)
{
    float      a = 6.2831853f * (float) k / (float) (n > 0 ? n : 1) + 0.7f * (float) (ptb.round % 5);
    vector3f_t at;
    if (arena_kit_land_near(server, bot, x + cosf(a) * r, y + sinf(a) * r, &at)) {
        out->x = at.x;
        out->y = at.y;
        out->z = at.z;
    } else {
        out->x = x;
        out->y = y;
    }
}

/* the bot's distance rank to (x, y) among its team's living bots: the
   nearest are the ones that go */
static int _ptb_rank_near(server_t* server, player_t* bot, float x, float y)
{
    float bdx = bot->movement.position.x - x, bdy = bot->movement.position.y - y, bd = bdx * bdx + bdy * bdy;
    int   rank = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == bot || !p->is_bot || p->team != bot->team || !_ptb_alive(p)) {
            continue;
        }
        float dx = p->movement.position.x - x, dy = p->movement.position.y - y, d = dx * dx + dy * dy;
        rank += d < bd || (d == bd && p->id < bot->id);
    }
    return rank;
}

static int _ptb_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    if ((bot->team != TEAM_A && bot->team != TEAM_B) || !bot->is_bot || bot->id >= SERVER_MAX_PLAYERS ||
        ptb.site_count == 0)
    {
        return 0;
    }
    int   want_use = 0;
    float ex, ey, ez;
    arena_kit_side_middle(server, bot->team == TEAM_A ? TEAM_B : TEAM_A, &ex, &ey, &ez);
    memset(out, 0, sizeof(*out));

    /* the freeze: everyone waits behind its gate facing the enemy's side */
    if (ptb.state == PTB_FREEZE || ptb.state == PTB_WAITING || ptb.state == PTB_END) {
        gm_goal_name(out, "gate", 0);
        out->x       = bot->movement.position.x;
        out->y       = bot->movement.position.y;
        out->watch_x = ex;
        out->watch_y = ey;
        out->hold    = 2.0f;
        out->role    = GM_BOT_ATTACK;
        _ptb_set_use(server, bot, 0);
        return 1;
    }

    int n    = 0;
    int rank = _ptb_rank(server, bot, &n);
    const ptb_site_t* target = &ptb.sites[ptb.target_site < ptb.site_count ? ptb.target_site : 0];

    if (bot->team == PTB_T) {
        if (ptb.bomb == PTB_BOMB_CARRIED && ptb.carrier == bot->id) {
            /* the carrier: to the site, then still on it, USE held */
            gm_goal_name(out, "ptb-plant", GM_INTENT_PLANT | GM_INTENT_TAKE);
            out->x       = target->x;
            out->y       = target->y;
            out->z       = (float) target->z;
            out->hold    = 1.2f;
            out->watch_x = ex;
            out->watch_y = ey;
            out->rush    = 1;
            out->role    = GM_BOT_RUSH;
            out->walk_only = 1;
            float dx = bot->movement.position.x - target->x, dy = bot->movement.position.y - target->y;
            want_use = dx * dx + dy * dy <= (target->radius * 0.7f) * (target->radius * 0.7f) &&
                       _ptb_on_site(server, bot) >= 0;
        } else if (ptb.bomb == PTB_BOMB_DROPPED && _ptb_rank_near(server, bot, ptb.bomb_at.x, ptb.bomb_at.y) == 0) {
            /* the nearest terrorist fetches the dropped bomb */
            gm_goal_name(out, "ptb-fetch", GM_INTENT_TAKE);
            out->x     = ptb.bomb_at.x;
            out->y     = ptb.bomb_at.y;
            out->z     = ptb.bomb_at.z;
            out->exact = 1;
            out->rush  = 1;
            out->role  = GM_BOT_RUSH;
            out->walk_only = 1;
        } else if (ptb.bomb == PTB_BOMB_PLANTED) {
            /* after the plant: hold the angles round the bomb */
            gm_goal_name(out, "ptb-hold-bomb", 0);
            _ptb_ring_spot(server, bot, ptb.bomb_at.x, ptb.bomb_at.y, 5.0f + (float) (ptb_bot_roll[bot->id] % 6),
                           rank, n, out);
            out->hold    = 2.5f;
            out->watch_x = ex;
            out->watch_y = ey;
            out->role    = GM_BOT_DEFEND;
            out->walk_only = 1;
        } else {
            player_t* c = ptb.bomb == PTB_BOMB_CARRIED ? _ptb_player(server, ptb.carrier) : NULL;
            /* two escort the carrier, the rest go ahead to the site and
               clear it (a third of the time one lurks at the other site) */
            if (c != NULL && _ptb_alive(c) && rank < 2) {
                gm_goal_name(out, "ptb-escort", GM_INTENT_WITH);
                out->x    = c->movement.position.x;
                out->y    = c->movement.position.y;
                out->z    = c->movement.position.z + 2.4f;
                out->hold = 4.0f;
                out->watch_x = target->x;
                out->watch_y = target->y;
                out->role = GM_BOT_ATTACK;
            } else {
                const ptb_site_t* s = target;
                if (ptb.site_count > 1 && ptb_bot_roll[bot->id] % 3 == 0 && rank == n - 1) {
                    s = &ptb.sites[1 - ptb.target_site];
                }
                gm_goal_name(out, "ptb-take-site", GM_INTENT_PUSH);
                _ptb_ring_spot(server, bot, s->x, s->y, s->radius * 0.6f, rank, n, out);
                out->hold = 2.0f;
                out->watch_x = ex;
                out->watch_y = ey;
                out->role = GM_BOT_ATTACK;
            }
            out->walk_only = 1;
        }
    } else {
        if (ptb.bomb == PTB_BOMB_PLANTED) {
            float left = (float) ptb.bomb_seconds - (float) (get_nanos() - ptb.planted_at) / (float) NANO_IN_SECOND;
            int   near = _ptb_rank_near(server, bot, ptb.bomb_at.x, ptb.bomb_at.y);
            /* nobody told them where: a defender that has not heard it yet
               goes to its site and listens, as a player would; one that
               hears it (PTB_BOMB_HEAR) goes for it */
            if (_ptb_bomb_dist(bot) > PTB_BOMB_HEAR) {
                /* the nearer site first; there and still nothing heard, the
                   other one */
                int k = 0;
                if (ptb.site_count > 1) {
                    float d0 = hypotf(bot->movement.position.x - ptb.sites[0].x, bot->movement.position.y - ptb.sites[0].y);
                    float d1 = hypotf(bot->movement.position.x - ptb.sites[1].x, bot->movement.position.y - ptb.sites[1].y);
                    k        = d1 < d0 ? 1 : 0;
                    if ((k ? d1 : d0) < ptb.sites[k].radius + 4.0f) {
                        k = 1 - k;
                    }
                }
                const ptb_site_t* site = &ptb.sites[k];
                gm_goal_name(out, "ptb-search", GM_INTENT_PUSH);
                _ptb_ring_spot(server, bot, site->x, site->y, site->radius * 0.5f, rank, n, out);
                out->hold      = 2.0f;
                out->role      = GM_BOT_ATTACK;
                out->walk_only = 1;
                _ptb_set_use(server, bot, 0);
                return 1;
            }
            int   threat = _ptb_near(server, PTB_T, ptb.bomb_at.x, ptb.bomb_at.y, 18.0f);
            /* the nearest defends the bomb's defuse: alone with it, or no time
               left to clear the site first */
            if (near == 0 && (threat == 0 || left < ptb.defuse_seconds + 6.0f)) {
                gm_goal_name(out, "ptb-defuse", GM_INTENT_TAKE);
                out->x     = ptb.bomb_at.x;
                out->y     = ptb.bomb_at.y;
                out->z     = ptb.bomb_at.z;
                out->hold  = 1.0f;
                out->rush  = 1;
                out->role  = GM_BOT_RUSH;
                out->walk_only = 1;
                float dx = bot->movement.position.x - ptb.bomb_at.x, dy = bot->movement.position.y - ptb.bomb_at.y;
                want_use  = dx * dx + dy * dy <= (PTB_DEFUSE_REACH - 0.4f) * (PTB_DEFUSE_REACH - 0.4f);
            } else {
                /* the rest retake the site: the terrorists holding it */
                gm_goal_name(out, "ptb-retake", GM_INTENT_PUSH);
                _ptb_ring_spot(server, bot, ptb.bomb_at.x, ptb.bomb_at.y, 4.0f, rank, n, out);
                out->hold = 2.0f;
                out->role = GM_BOT_ATTACK;
                out->assault = 1;
                out->walk_only = 1;
            }
        } else {
            /* hold the sites: half each, by rank; one of each four rotates
               to the round's other site midway (a guess at the attack) */
            int               s   = ptb.site_count > 1 ? rank % 2 : 0;
            float             el  = (float) (get_nanos() - ptb.since) / (float) NANO_IN_SECOND;
            if (ptb.site_count > 1 && rank % 4 == 3 && el > 35.0f) {
                s = 1 - s;
            }
            const ptb_site_t* site = &ptb.sites[s];
            gm_goal_name(out, "ptb-guard-site", GM_INTENT_GUARD);
            _ptb_ring_spot(server, bot, site->x, site->y, site->radius * 0.8f, rank / 2, (n + 1) / 2, out);
            out->hold     = 2.0f;
            out->watch_x  = ex;
            out->watch_y  = ey;
            out->role     = GM_BOT_DEFEND;
            out->has_home = 1;
            out->home_x   = site->x;
            out->home_y   = site->y;
            out->home_z   = (float) site->z;
            out->walk_only = 1;
        }
    }
    if (want_use) {
        /* there: its own spot is the goal, so it stops dead and stays (the
           creep onto the exact cell, a hop or the step off a mate's body
           cancelled every other plant and defuse) */
        out->x            = bot->movement.position.x;
        out->y            = bot->movement.position.y;
        out->hold         = 3.0f;
        out->keep_terrain = 1;
        out->exact        = 0;
    }
    _ptb_set_use(server, bot, want_use);
    return 1;
}

static int _ptb_map_fits(const char* map_name)
{
    /* a map of the catalog's table, or another bomb defusal map (de_*, or
       tagged ptb): its sites are then found between the spawns. The arena's
       other maps are no bomb maps (the owner's call) */
    toml_table_t* conf = _ptb_table();
    if (conf != NULL) {
        int in = toml_table_in(conf, map_name) != NULL;
        toml_free(conf);
        if (in) {
            return 1;
        }
    }
    return !strncmp(map_name, "de_", 3) || !strncmp(map_name, "De_", 3) || gamemode_map_tagged(map_name, "ptb");
}

const gamemode_ops_t ptb_ops = {
.id                    = GAME_MODE_PTB,
.name                  = "ptb",
.wire_name             = "ptb",
/* no squads (the owner's call: rounds of a few players each) */
.read_map              = _ptb_read_map,
.init                  = _ptb_init,
.tick                  = _ptb_tick,
.player_out            = _ptb_player_out,
.allows_respawn        = _ptb_allows_respawn,
.cell_protected        = _ptb_cell_protected,
.player_rules          = _ptb_player_rules,
.weapon_change_instant = _ptb_weapon_change_instant,
.state_keys            = _ptb_state_keys,
.player_command        = _ptb_player_command,
.player_carries        = _ptb_player_carries,
.bot_goal              = _ptb_bot_goal,
.spawn_point           = arena_kit_spawn_point,
.spawn_facing          = _ptb_spawn_facing,
.map_fits              = _ptb_map_fits,
};
