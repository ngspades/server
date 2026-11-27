// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/BotForts.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/Gamemodes/Tc.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Player.h>
#include <Server/Proto10/Flags.h>
#include <Server/Server.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <proto10/codec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Territory control, pyspades' rules (pyspades/entities.py Territory,
   pyspades/server.py get_cp_entities, piqueserver's TC mode):
   - 3 to 7 territories by the map's land (MIN/MAX_TERRITORY_COUNT), one in
     each strip across the line from one team's spawn to the other's, at a
     random dry spot of the strip's middle half; the first half belong to
     the team on that side, the last half to the other, an odd middle one
     to nobody;
   - a territory's progress runs from team 1's end (0) to team 2's (1):
     every player in it (TC_CAPTURE_DISTANCE, a cube) pushes it toward its
     team at TC_CAPTURE_RATE a second, the two teams cancelling out; at an
     end it is that team's (a team's score is the territories it holds);
   - holding them all wins the map; a respawn lands round a territory of
     the team (SPAWN_RADIUS; ours: the front one, _tc_spawn_point); standing
     on one refills every 20 s.
   Ours adds a time limit (the team holding more wins) and the classes as
   the host's option, off by default, as in the zombie mode and KotH. */
#define TC_MIN_TERRITORIES 3
#define TC_MAX_TERRITORIES 7 /* pyspades' MAX_TERRITORY_COUNT; P10_TERRITORIES_MAX holds it */
#define TC_CAPTURE_DISTANCE 16.0f
#define TC_CAPTURE_RATE     0.05f
#define TC_SPAWN_RADIUS     32.0f
#define TC_SPAWN_THREAT     96.0f /* an enemy this near a territory: spawn one back */
#define TC_REFILL_DISTANCE  3.0f
#define TC_REFILL_SECONDS   20
#define TC_SECONDS          1200 /* gamemode_match_seconds */
/* a squad walks to its territory behind its leader until one of them is
   this near it, then each bot takes its own place in it */
#define TC_SQUAD_SPLIT      (TC_CAPTURE_DISTANCE * 2.5f)

typedef struct
{
    int   x, y, z;
    int   owner;    /* TEAM_A, TEAM_B, or -1 nobody */
    float progress; /* 0 team 1's end, 1 team 2's */
    int   on[2];    /* players of each team in it */
    /* what the clients were last told: a capture runs on at its rate on
       their side (0.75's ProgressBar did the same), so the key changes only
       when the rate or the holder does, and every 2 s while it moves (a
       joiner's snapshot is never more than that behind) */
    int      sent_owner, sent_cap, sent_share, sent_per_s;
    uint64_t sent_at;
} tc_territory_t;

static struct
{
    tc_territory_t t[TC_MAX_TERRITORIES];
    int            count;
    int            axis; /* 0: the teams face along x, 1: along y */
    uint64_t       deadline;
    uint64_t       last_step;
    uint64_t       status_at;
    uint64_t       refill_at[256];
    uint16_t       captures[256]; /* territories each player helped take */
    uint64_t       next_fort[TC_MAX_TERRITORIES];
    uint64_t       next_bunker[TC_MAX_TERRITORIES];
    uint8_t        ended;
} tc;

static int tc_classes_host = -1;

static void _tc_spawn_middle(server_t* server, int team, float* x, float* y)
{
    quad3d_t* s = &server->protocol.spawns[team];
    *x          = (s->from.x + s->to.x) / 2.0f;
    *y          = (s->from.y + s->to.y) / 2.0f;
}

static int _tc_dry(map_store_t* map, int x, int y, int* z)
{
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    int top = map_top(map, (uint16_t) x, (uint16_t) y);
    *z      = top;
    return top >= 4 && top < map->size_z - 2;
}

/* the share of dry columns, every 4th one looked at */
static float _tc_land(server_t* server, int x0, int y0, int x1, int y1)
{
    map_store_t* map = &server->s_map.map;
    int       dry = 0, n = 0, z;
    for (int x = x0; x < x1; x += 4) {
        for (int y = y0; y < y1; y += 4) {
            dry += _tc_dry(map, x, y, &z);
            n++;
        }
    }
    return n > 0 ? (float) dry / (float) n : 0.0f;
}

/* pyspades' "cool algorithm number 1", turned to the line between the two
   spawns: on a map whose teams face north and south the strips run across
   y, so every territory still lies between them */
static void _tc_place(server_t* server)
{
    map_store_t* map = &server->s_map.map;
    int       x0, y0, x1, y1;
    map_meta_play_area(server, &x0, &y0, &x1, &y1);
    float ax, ay, bx, by;
    _tc_spawn_middle(server, TEAM_A, &ax, &ay);
    _tc_spawn_middle(server, TEAM_B, &bx, &by);
    tc.axis   = fabsf(bx - ax) >= fabsf(by - ay) ? 0 : 1;
    /* team 1 on the low side of the axis, or the strips go the other way */
    int flip  = tc.axis == 0 ? ax > bx : ay > by;
    float land = _tc_land(server, x0, y0, x1, y1);
    tc.count  = (int) (land * (TC_MAX_TERRITORIES - TC_MIN_TERRITORIES)) + TC_MIN_TERRITORIES;
    if (tc.count > TC_MAX_TERRITORIES) {
        tc.count = TC_MAX_TERRITORIES;
    }
    int   along0 = tc.axis == 0 ? x0 : y0, along1 = tc.axis == 0 ? x1 : y1;
    int   cross0 = tc.axis == 0 ? y0 : x0, cross1 = tc.axis == 0 ? y1 : x1;
    float strip  = (float) (along1 - along0) / (float) tc.count;
    int   quarter = (cross1 - cross0) / 4;
    for (int i = 0; i < tc.count; i++) {
        tc_territory_t* t = &tc.t[i];
        int             k = flip ? tc.count - 1 - i : i; /* strip k from team 1's side */
        int             a0 = along0 + (int) (strip * k), a1 = along0 + (int) (strip * (k + 1));
        int             found = 0, z = 0;
        /* a random dry column of the strip's middle half, as pyspades;
           its middle when 400 draws find none (a strip of sea) */
        for (int tries = 0; tries < 400 && !found; tries++) {
            int a = a0 + (int) (gen_rand(&server->rand) * (float) (a1 - a0));
            int c = cross0 + quarter + (int) (gen_rand(&server->rand) * (float) (2 * quarter));
            t->x  = tc.axis == 0 ? a : c;
            t->y  = tc.axis == 0 ? c : a;
            found = _tc_dry(map, t->x, t->y, &z);
        }
        if (!found) {
            t->x = tc.axis == 0 ? (a0 + a1) / 2 : (cross0 + cross1) / 2;
            t->y = tc.axis == 0 ? (cross0 + cross1) / 2 : (a0 + a1) / 2;
            _tc_dry(map, t->x, t->y, &z);
        }
        t->z          = z;
        t->on[0]      = t->on[1] = 0;
        t->sent_owner = -1; /* none sent: the first tick fills it */
        if (k < tc.count / 2) {
            t->owner    = TEAM_A;
            t->progress = 0.0f;
        } else if (k > (tc.count - 1) / 2) {
            t->owner    = TEAM_B;
            t->progress = 1.0f;
        } else {
            t->owner    = -1; /* an odd count's middle one */
            t->progress = 0.5f;
        }
        evt_log(NULL, "tc", "action=territory index=%d at=%d,%d,%d owner=%d dry=%d", i, t->x, t->y, t->z, t->owner,
                found);
    }
}

/* what the clients get of each territory (see sent_*) */
static void _tc_sync(server_t* server, uint64_t now)
{
    for (int i = 0; i < tc.count; i++) {
        tc_territory_t* t    = &tc.t[i];
        int             rate = t->on[TEAM_B] - t->on[TEAM_A];
        int             cap  = t->owner == TEAM_A ? TEAM_B
                               : t->owner == TEAM_B ? TEAM_A
                               : rate < 0           ? TEAM_A
                                                    : TEAM_B;
        int per_s = rate * (int) (TC_CAPTURE_RATE * 1000.0f) * (cap == TEAM_B ? 1 : -1);
        int owner = t->owner < 0 ? 2 : t->owner;
        /* at an end, pushed on: nothing moves */
        if ((t->progress >= 1.0f && per_s * (cap == TEAM_B ? 1 : -1) > 0) ||
            (t->progress <= 0.0f && per_s * (cap == TEAM_B ? 1 : -1) < 0)) {
            per_s = 0;
        }
        if (owner != t->sent_owner || cap != t->sent_cap || per_s != t->sent_per_s ||
            (per_s != 0 && now - t->sent_at >= 2ull * NANO_IN_SECOND))
        {
            float share   = cap == TEAM_B ? t->progress : 1.0f - t->progress;
            t->sent_owner = owner;
            t->sent_cap   = cap;
            t->sent_share = (int) lroundf(share * 1000.0f);
            t->sent_per_s = per_s;
            t->sent_at    = now;
            if (profile_is_075() && server != NULL) {
                /* 0.75's ProgressBar: the client runs the capture on at
                   players * 0.05 a second toward the capturing team */
                legacy_tc_progress(server, i, cap, abs(rate), share);
            }
        }
    }
}

static void _tc_score(server_t* server)
{
    uint8_t* score = server->protocol.gamemode.score;
    score[0] = score[1] = 0;
    for (int i = 0; i < tc.count; i++) {
        if (tc.t[i].owner == TEAM_A || tc.t[i].owner == TEAM_B) {
            score[tc.t[i].owner]++;
        }
    }
}

static void _tc_init(server_t* server)
{
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    memset(&tc, 0, sizeof tc);
    _tc_place(server);
    _tc_sync(server, get_nanos());
    /* the team scores count territories held, the limit is all of them */
    server->protocol.gamemode.score_limit = (uint8_t) tc.count;
    _tc_score(server);
    uint64_t now  = get_nanos();
    tc.deadline   = now + (uint64_t) gamemode_match_seconds("tc_seconds", TC_SECONDS) * NANO_IN_SECOND;
    tc.last_step  = now;
    /* the classes are the host's choice, off unless asked: the command line
       (--classes 0|1) first, then the console (/classes) */
    int classes = server->classes_flag == 1;
    if (tc_classes_host >= 0) {
        classes = tc_classes_host;
    }
    if (classes) {
        server->features_on |= GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS;
    }
    LOG_INFO("Territory control: %d territories, %u-%u held, %d s at most, classes %s", tc.count,
             server->protocol.gamemode.score[0], server->protocol.gamemode.score[1], (int) ((tc.deadline - now) / NANO_IN_SECOND), classes ? "on" : "off");
    evt_log(NULL, "tc", "action=start territories=%d held=%u-%u axis=%c classes=%d", tc.count,
            server->protocol.gamemode.score[0], server->protocol.gamemode.score[1], tc.axis ? 'y' : 'x', classes);
}

static int _tc_in(const tc_territory_t* t, const player_t* p, float reach)
{
    /* pyspades' vector_collision: a cube round the territory's cell */
    return fabsf(p->movement.position.x - (t->x + 0.5f)) < reach &&
           fabsf(p->movement.position.y - (t->y + 0.5f)) < reach &&
           fabsf(p->movement.position.z - (float) t->z) < reach;
}

static void _tc_end(server_t* server, const char* why)
{
    uint8_t* score = server->protocol.gamemode.score;
    tc.ended       = 1;
    evt_log(NULL, "tc", "action=end held=%u-%u of=%d why=%s", score[0], score[1], tc.count, why);
    gamemode_match_won(server);
}

static void _tc_capture(server_t* server, int i, int team)
{
    tc_territory_t* t   = &tc.t[i];
    int             was = t->owner;
    t->owner            = team;
    _tc_score(server);
    int       helpers = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state == STATE_READY && p->alive && (int) p->team == team && _tc_in(t, p, TC_CAPTURE_DISTANCE)) {
            if (tc.captures[p->id] < UINT16_MAX) {
                tc.captures[p->id]++;
            }
            helpers++;
        }
    }
    uint8_t* score = server->protocol.gamemode.score;
    if (profile_is_075()) {
        legacy_tc_capture(server, i, team, score[team] >= tc.count);
    }
    evt_log(NULL, "tc", "action=capture index=%d team=%d was=%d players=%d held=%u-%u", i, team, was, helpers,
            score[0], score[1]);
    /* the territory changed hands: its fortification is the old owner's no
       longer, and the new owner may raise one at once */
    if (was == TEAM_A || was == TEAM_B) {
        bot_fort_discard((uint8_t) was, t->x, t->y, 12);
    }
    tc.next_fort[i] = 0;
    tc.next_bunker[i] = 0;
    if (score[team] >= tc.count) {
        _tc_end(server, "all");
    }
}

/* the way the other team comes from, for the structures' facing */
static void _tc_face_enemy(server_t* server, int team, int x, int y, int* fx, int* fy)
{
    float ex, ey;
    _tc_spawn_middle(server, team == TEAM_A ? TEAM_B : TEAM_A, &ex, &ey);
    float dx = ex - ((float) x + 0.5f), dy = ey - ((float) y + 0.5f);
    if (fabsf(dx) >= fabsf(dy)) {
        *fx = dx >= 0.0f ? 1 : -1;
        *fy = 0;
    } else {
        *fx = 0;
        *fy = dy >= 0.0f ? 1 : -1;
    }
}

/* Three bots of one team in a territory start fortifying it: a ring around
   it first (a battlemented wall to stand behind while it is taken), then,
   once the team holds it and the ring is whole, a roofed bunker at its
   middle. One structure of each kind a territory, a request every 10 s (the
   request says no while one stands); the teams' counts decide who builds
   it, so two teams crossing there do not both send builders at the same
   cells. */
/* Lookouts over a territory the team fights for: a tall watchtower on the
   highest ground that sees its middle, and, once it holds the territory, a
   two floor barricade facing the enemy; both watched from their tops. The
   search walks a ray a column: once every 15 s a territory. */
static uint64_t tc_next_lookout[TC_MAX_TERRITORIES];

static void _tc_lookouts(server_t* server, uint64_t now, int i, int present, int holder)
{
    tc_territory_t* t = &tc.t[i];
    if (now < tc_next_lookout[i]) {
        return;
    }
    tc_next_lookout[i] = now + 15ull * NANO_IN_SECOND;
    int fx, fy, x, y, z;
    if (present >= 0 && !bot_fort_exists(BOT_FORT_WATCHTOWER, t->x, t->y, 32)) {
        _tc_face_enemy(server, present, t->x, t->y, &fx, &fy);
        if (bot_fort_overlook(server, (uint8_t) present, BOT_FORT_WATCHTOWER, t->x, t->y, t->z, 6, 28, fx, fy, &x, &y,
                              &z))
        {
            bot_fort_request_watch(server, (uint8_t) present, BOT_FORT_WATCHTOWER, x, y, z, fx, fy, t->x, t->y, t->z);
        }
    }
    if (holder >= 0 && (present < 0 || present == holder) && !bot_fort_exists(BOT_FORT_BARRICADE, t->x, t->y, 18)) {
        _tc_face_enemy(server, holder, t->x, t->y, &fx, &fy);
        if (bot_fort_overlook(server, (uint8_t) holder, BOT_FORT_BARRICADE, t->x, t->y, t->z, 5, 14, fx, fy, &x, &y,
                              &z))
        {
            bot_fort_request_watch(server, (uint8_t) holder, BOT_FORT_BARRICADE, x, y, z, fx, fy, t->x, t->y, t->z);
        }
    }
}

static void _tc_forts(server_t* server, uint64_t now)
{
    for (int i = 0; i < tc.count; i++) {
        tc_territory_t* t = &tc.t[i];
        int             a = t->on[TEAM_A], b = t->on[TEAM_B];
        int             present = a >= 3 && a > b ? TEAM_A : b >= 3 && b > a ? TEAM_B : -1;
        int             holder = t->owner == TEAM_A || t->owner == TEAM_B ? t->owner : -1;
        if (present < 0 && holder < 0) {
            continue;
        }
        _tc_lookouts(server, now, i, present, holder);
        int fx, fy;
        if (!bot_fort_exists(BOT_FORT_RING, t->x, t->y, 8)) {
            if (present < 0 || now < tc.next_fort[i]) {
                continue;
            }
            tc.next_fort[i] = now + 10ull * NANO_IN_SECOND;
            _tc_face_enemy(server, present, t->x, t->y, &fx, &fy);
            bot_fort_request(server, (uint8_t) present, BOT_FORT_RING, t->x, t->y, t->z, fx, fy);
            continue;
        }
        /* the ring stands and the territory is held: the holder roofs the
           middle (the builders are the holder's nearest bots) */
        if (holder >= 0 && bot_fort_complete(BOT_FORT_RING, t->x, t->y, 8) &&
            !bot_fort_exists(BOT_FORT_BUNKER, t->x, t->y, 8))
        {
            if (now < tc.next_bunker[i]) {
                continue;
            }
            tc.next_bunker[i] = now + 10ull * NANO_IN_SECOND;
            _tc_face_enemy(server, holder, t->x, t->y, &fx, &fy);
            bot_fort_request(server, (uint8_t) holder, BOT_FORT_BUNKER, t->x, t->y, t->z, fx, fy);
        }
    }
}

static void _tc_tick(server_t* server)
{
    if (tc.ended) {
        return;
    }
    uint64_t now = get_nanos();
    float    dt  = (float) (now - tc.last_step) / (float) NANO_IN_SECOND;
    tc.last_step = now;
    for (int i = 0; i < tc.count; i++) {
        tc.t[i].on[0] = tc.t[i].on[1] = 0;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
            continue;
        }
        for (int i = 0; i < tc.count; i++) {
            if (_tc_in(&tc.t[i], p, TC_CAPTURE_DISTANCE)) {
                tc.t[i].on[p->team]++;
            }
            /* on the territory itself: ammo, blocks and grenades back */
            if (_tc_in(&tc.t[i], p, TC_REFILL_DISTANCE) &&
                now - tc.refill_at[p->id] >= (uint64_t) TC_REFILL_SECONDS * NANO_IN_SECOND)
            {
                tc.refill_at[p->id] = now;
                player_restock(server, p);
                evt_log(p, "tc", "action=refill index=%d", i);
            }
        }
    }
    for (int i = 0; i < tc.count && !tc.ended; i++) {
        tc_territory_t* t    = &tc.t[i];
        int             rate = t->on[TEAM_B] - t->on[TEAM_A];
        if (rate == 0) {
            continue;
        }
        t->progress += (float) rate * TC_CAPTURE_RATE * dt;
        if (t->progress >= 1.0f) {
            t->progress = 1.0f;
            if (t->owner != TEAM_B) {
                _tc_capture(server, i, TEAM_B);
            }
        } else if (t->progress <= 0.0f) {
            t->progress = 0.0f;
            if (t->owner != TEAM_A) {
                _tc_capture(server, i, TEAM_A);
            }
        }
    }
    _tc_forts(server, now);
    /* every 30 s: each territory's holder, progress and players in it */
    if (now >= tc.status_at) {
        tc.status_at = now + 30ull * NANO_IN_SECOND;
        char   line[256];
        size_t used = 0;
        line[0]     = 0;
        for (int i = 0; i < tc.count && used < sizeof line; i++) {
            used += (size_t) snprintf(line + used, sizeof line - used, " t%d=%d/%.2f/%d:%d", i, tc.t[i].owner,
                                      tc.t[i].progress, tc.t[i].on[0], tc.t[i].on[1]);
        }
        evt_log(NULL, "tc", "action=status held=%u-%u%s", server->protocol.gamemode.score[0],
                server->protocol.gamemode.score[1], line);
    }
    _tc_sync(server, now);
    if (!tc.ended && now >= tc.deadline) {
        _tc_end(server, "time");
    }
}

/* each territory's place is its Checkpoint entity (Proto10/Flags.c); the key
   says what it cannot: who holds it, the team that would take it, that
   team's share of the progress (permille) and its change a second, so a
   client draws the bar of the territory it stands in and runs it on
   between two states */
static void _tc_state_keys(server_t* server, gm_keys_t* out)
{
    (void) server;
    gm_keys_put(out, P10_GM_TERRITORY_COUNT, "%d", tc.count);
    for (int i = 0; i < tc.count; i++) {
        tc_territory_t* t = &tc.t[i];
        char            key[24];
        snprintf(key, sizeof key, "%s%d", P10_GM_TERRITORY_PREFIX, i);
        gm_keys_put(out, key, "%d,%d,%d,%d", t->sent_owner, t->sent_cap, t->sent_share, t->sent_per_s);
        proto10_flags_set(P10_FLAG_TERRITORY0 + i, (float) t->x, (float) t->y, (float) t->z, -1);
    }
}

static int _tc_time_left(server_t* server)
{
    (void) server;
    return tc.ended ? 0 : gamemode_seconds_until(tc.deadline);
}

static int _tc_extend(server_t* server, int seconds)
{
    tc.deadline += (uint64_t) seconds * NANO_IN_SECOND;
    server->match_extra_seconds += (uint32_t) seconds;
    evt_log(NULL, "tc", "action=extend seconds=%d", seconds);
    return 1;
}

/* The territory a team's player is sent toward: the nearest one its team
   does not hold, from (x, y); none (the team holds them all) gives the
   enemy's spawn. 1 when it is a territory */
static int _tc_front_target(server_t* server, int team, float x, float y, float* tx, float* ty)
{
    float best = 1e30f;
    for (int i = 0; i < tc.count; i++) {
        if (tc.t[i].owner == team) {
            continue;
        }
        float dx = (float) tc.t[i].x + 0.5f - x, dy = (float) tc.t[i].y + 0.5f - y;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            *tx  = (float) tc.t[i].x + 0.5f;
            *ty  = (float) tc.t[i].y + 0.5f;
        }
    }
    if (best < 1e30f) {
        return 1;
    }
    _tc_spawn_middle(server, team == TEAM_A ? TEAM_B : TEAM_A, tx, ty);
    return 0;
}

/* an enemy alive within r of the territory */
static int _tc_enemy_near(server_t* server, int enemy, const tc_territory_t* t, float r)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if ((int) p->team == enemy && p->alive && p->state == STATE_READY &&
            hypotf(p->movement.position.x - ((float) t->x + 0.5f), p->movement.position.y - ((float) t->y + 0.5f)) < r)
        {
            return 1;
        }
    }
    return 0;
}

/* Toward the front: the team's territory nearest one it does not hold, so
   nobody walks the whole map back (pyspades drew a random territory of the
   team; a 1024 map's far one was minutes away), unless an enemy is within
   TC_SPAWN_THREAT of it: then the next one back that is quiet, and with
   none the one with the fewest enemies in it. Measured, 16 bots, 1200 s,
   6 seeds, against the random draw: always the front one made both teams
   pour out face to face between their fronts, kills up half to double and
   captures down to a quarter (deaths 80+ blocks from any territory 37% to
   70% on Villages_1024); the second one back walked as far as the draw;
   a threat radius of 48 still lost captures, 160 walked as far. With 96:
   the walk to the front 403 to 300 blocks on Villages_1024, captures
   hiesville 16 to 29, Raindance 19 to 32, Villages_1024 25 to 23.
   Behind the territory, on the side away from that target: round it on
   every side, a player was born half inside the enemy's range */
static int _tc_spawn_point(server_t* server, player_t* player)
{
    if (player->team != TEAM_A && player->team != TEAM_B) {
        return 0;
    }
    int   enemy = player->team == TEAM_A ? TEAM_B : TEAM_A;
    int   order[TC_MAX_TERRITORIES], n = 0;
    float dist[TC_MAX_TERRITORIES], fx[TC_MAX_TERRITORIES], fy[TC_MAX_TERRITORIES];
    for (int i = 0; i < tc.count; i++) {
        if (tc.t[i].owner != (int) player->team) {
            continue;
        }
        _tc_front_target(server, player->team, (float) tc.t[i].x + 0.5f, (float) tc.t[i].y + 0.5f, &fx[i], &fy[i]);
        dist[i] = hypotf(fx[i] - ((float) tc.t[i].x + 0.5f), fy[i] - ((float) tc.t[i].y + 0.5f));
        int k   = n++;
        while (k > 0 && dist[order[k - 1]] > dist[i]) { /* nearest the front first */
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }
    if (n == 0) {
        return 0;
    }
    int pick = -1;
    for (int k = 0; k < n && pick < 0; k++) {
        const tc_territory_t* t = &tc.t[order[k]];
        if (t->on[enemy] == 0 && !_tc_enemy_near(server, enemy, t, TC_SPAWN_THREAT)) {
            pick = order[k];
        }
    }
    if (pick < 0) {
        pick = order[0];
        for (int k = 1; k < n; k++) {
            if (tc.t[order[k]].on[enemy] < tc.t[pick].on[enemy]) {
                pick = order[k];
            }
        }
    }
    int   pick_on = tc.t[pick].on[enemy];
    float ftx = fx[pick], fty = fy[pick];
    const tc_territory_t* t   = &tc.t[pick];
    map_store_t*          map = &server->s_map.map;
    float                 cx = (float) t->x + 0.5f, cy = (float) t->y + 0.5f;
    float                 away = atan2f(cy - fty, cx - ftx);
    for (int tries = 0; tries < 16; tries++) {
        float x, y;
        if (tries < 8) { /* behind it, away from the front */
            float a = away + (gen_rand(&server->rand) - 0.5f) * (float) M_PI;
            float r = 6.0f + gen_rand(&server->rand) * (TC_SPAWN_RADIUS - 6.0f);
            x       = cx + cosf(a) * r;
            y       = cy + sinf(a) * r;
        } else { /* anywhere round it */
            x = cx + (gen_rand(&server->rand) * 2.0f - 1.0f) * TC_SPAWN_RADIUS;
            y = cy + (gen_rand(&server->rand) * 2.0f - 1.0f) * TC_SPAWN_RADIUS;
        }
        int        z;
        vector3f_t at;
        if (!_tc_dry(map, (int) x, (int) y, &z)) {
            continue;
        }
        if (player_spawn_near(server, x, y, (float) z - 2.4f, &at)) {
            player->movement.position = at;
            float wx = 0.0f, wy = 0.0f;
            _tc_front_target(server, player->team, at.x, at.y, &wx, &wy);
            evt_log(player, "tc", "action=spawn near=%d at=%.0f,%.0f,%.0f behind=%d contested=%d front=%.0f", pick,
                    at.x, at.y, at.z, tries < 8, pick_on, hypotf(wx - at.x, wy - at.y));
            return 1;
        }
    }
    return 0;
}

/* a spawn faces the nearest territory its team does not hold */
static int _tc_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    _tc_front_target(server, player->team, player->movement.position.x, player->movement.position.y, x, y);
    return 1;
}

/* The bot's territory: the ones its team does not hold, and its own under
   attack (an enemy in it), shared out by the bot's rank in its team so the
   team spreads over them rather than all running to the nearest; with
   nothing to take or save, the nearest one of its own to hold. */
static int _tc_rank(server_t* server, player_t* bot)
{
    int       rank = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        rank += p->is_bot && p->team == bot->team && p->id < bot->id;
    }
    return rank;
}

/* the territory a player of a team goes for and the goal's name: the
   nearest it does not hold (or its own under attack), every third bot the
   next one on; with all held, the nearest */
static int _tc_pick(server_t* server, player_t* bot, const char** name)
{
    int   enemy = bot->team == TEAM_A ? TEAM_B : TEAM_A;
    int   want[TC_MAX_TERRITORIES], n = 0;
    float bx = bot->movement.position.x, by = bot->movement.position.y;
    for (int i = 0; i < tc.count; i++) {
        if (tc.t[i].owner != (int) bot->team || tc.t[i].on[enemy] > 0) {
            want[n++] = i;
        }
    }
    int pick = -1;
    if (n > 0) {
        /* the nearest two get most of the team: the rank picks among them,
           every third bot the next one on (a team spread over all of them
           took none) */
        for (int a = 0; a < n; a++) {
            for (int b = a + 1; b < n; b++) {
                float da = hypotf(tc.t[want[a]].x - bx, tc.t[want[a]].y - by);
                float db = hypotf(tc.t[want[b]].x - bx, tc.t[want[b]].y - by);
                if (db < da) {
                    int s   = want[a];
                    want[a] = want[b];
                    want[b] = s;
                }
            }
        }
        int rank = _tc_rank(server, bot);
        pick     = want[(rank % 3 == 2 && n > 1) ? 1 : 0];
        *name    = tc.t[pick].owner == (int) bot->team ? "tc-defend" : "tc-take";
    } else {
        float best = 1e9f;
        for (int i = 0; i < tc.count; i++) {
            float d = hypotf(tc.t[i].x - bx, tc.t[i].y - by);
            if (d < best) {
                best = d;
                pick = i;
            }
        }
        *name = "tc-hold";
    }
    return pick;
}

static int _tc_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    memset(out, 0, sizeof(*out));
    if ((bot->team != TEAM_A && bot->team != TEAM_B) || tc.count == 0) {
        return 0;
    }
    const char* name = "tc-hold";
    int         pick = _tc_pick(server, bot, &name);
    /* a squad goes for its leader's territory (a bot leader's pick: a
       human's is wherever it walks, and its bots follow it there), behind
       the leader until one of them is near it */
    player_t* leader = squads_leader(server, bot);
    if (leader != NULL && leader != bot && leader->is_bot && leader->alive) {
        pick = _tc_pick(server, leader, &name);
    }
    const tc_territory_t* t = &tc.t[pick];
    if (squads_follow(server, bot, t->x + 0.5f, t->y + 0.5f, TC_SQUAD_SPLIT, out)) {
        return 1;
    }
    gm_goal_name(out, name, 0);
    /* inside the capture cube, each bot at a place of its own round the cell;
       a whole bunker there gives every third bot the place inside it */
    float a = (float) bot->id * 2.399963f;
    float r = 3.0f + (float) (bot->id % 4) * 2.0f;
    if (bot_fort_exists(BOT_FORT_RING, t->x, t->y, 8)) {
        /* the ring's wall is at four cells: hold inside it or outside, never
           on it (the walk to a spot on its wall read as stuck) */
        r = bot->id % 3 == 0 ? 3.0f : 6.5f + (float) (bot->id % 3) * 1.5f;
    }
    if (bot->id % 3 == 0 && bot_fort_complete(BOT_FORT_BUNKER, t->x, t->y, 8)) {
        r = 0.0f;
    }
    out->x     = t->x + 0.5f + cosf(a) * r;
    out->y     = t->y + 0.5f + sinf(a) * r;
    out->z     = (float) t->z;
    out->hold  = 2.0f;
    int inside = _tc_in(t, bot, TC_CAPTURE_DISTANCE * 0.8f);
    out->role  = inside ? GM_BOT_ATTACK : GM_BOT_RUSH;
    out->watch_x = out->x + cosf(a) * 10.0f;
    out->watch_y = out->y + sinf(a) * 10.0f;
    if (!inside) {
        gamemode_flank(server, bot, out);
    }
    return 1;
}

static void _tc_patrol_anchor(server_t* server, player_t* bot, float* x, float* y)
{
    gm_bot_goal_t g;
    if (_tc_bot_goal(server, bot, &g)) {
        *x = g.x;
        *y = g.y;
    }
}

static int _tc_set_classes(server_t* server, int on)
{
    tc_classes_host = on ? 1 : 0;
    if (tc_classes_host) {
        server->features_on |= GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS;
    } else {
        server->features_on &= ~(GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS);
    }
    classes_tell_all(server, tc_classes_host);
    LOG_INFO("Territory control: the classes are %s", tc_classes_host ? "on" : "off");
    evt_log(NULL, "tc", "action=classes on=%d", tc_classes_host);
    return 1;
}

static void _tc_client_in_game(server_t* server, player_t* player)
{
    classes_client_in_game(server, player);
}

static int _tc_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    return classes_command(server, player, command, arg);
}

static uint32_t _tc_player_captures(server_t* server, player_t* player)
{
    (void) server;
    return tc.captures[player->id];
}

const gamemode_ops_t tc_ops = {
    .id             = GAME_MODE_TC,
    .name           = "tc",
    .wire_name      = "tc",
    /* squads, and a respawn by a mate or at the rally before the front's */
    .features       = GM_FEAT_SQUAD_SPAWN,
    .squad_teams    = SQUAD_TEAMS_BOTH,
    .init           = _tc_init,
    .tick           = _tc_tick,
    .extend         = _tc_extend,
    .time_left      = _tc_time_left,
    .state_keys     = _tc_state_keys,
    .summary_stat   = "captures",
    .player_stat    = _tc_player_captures,
    .bot_goal       = _tc_bot_goal,
    .patrol_anchor  = _tc_patrol_anchor,
    .spawn_point    = _tc_spawn_point,
    .spawn_facing   = _tc_spawn_facing,
    .set_classes    = _tc_set_classes,
    .client_in_game = _tc_client_in_game,
    .player_command = _tc_command,
    .hit_damage     = classes_hit_damage,
};

int tc_count(void)
{
    return tc.count;
}

int tc_territory(int i, float* x, float* y, float* z)
{
    *x = (float) tc.t[i].x + 0.5f;
    *y = (float) tc.t[i].y + 0.5f;
    *z = (float) tc.t[i].z;
    return tc.t[i].owner < 0 ? 2 : tc.t[i].owner;
}
