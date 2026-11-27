// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/BotForts.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Koth.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Player.h>
#include <proto10/codec.h>
#include <Server/Server.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <Server/Proto10/Flags.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* A match is rounds: a team that holds the hill alone for
   KOTH_ROUND_POINTS seconds wins the round, the hill moves and the next
   round starts after KOTH_BREAK seconds, nobody respawning; the first to
   KOTH_ROUNDS rounds wins the map (best of five). The hill used to move
   every 90 s inside one long count to 90, told as coordinates nobody could
   read, and the match ended at once. */
#define KOTH_ROUND_POINTS 60
#define KOTH_ROUNDS       3
#define KOTH_BREAK        5
#define KOTH_SECONDS      900
#define KOTH_RADIUS       12.0f
/* on the hill: a cylinder over its ring, from up to this far below the
   lowest floor inside it (koth.floor) to the sky. The hill may stand on a
   roof or a tree, and the ground round it, the roof and a jetpack over the
   ring all count: it held only 8.5 over the hill's own block, so a hill on
   a town's roof was held only by standing up there. z grows downwards */
#define KOTH_HEIGHT       4.5f
#define KOTH_FLOOR_DEPTH  24 /* a floor deeper than this under the hill is a cave's */
#define KOTH_TOWER        4 /* blocks the tower bot raises */
#define KOTH_FORTS        4 /* posts of each team round the ring, two holders each */
#define KOTH_OVERWATCH    2 /* Marksmen a team that watch the hill from high ground */
/* a squad walks to the hill behind its leader until one of them is this
   near the hill's middle, then each bot takes its own job on it */
#define KOTH_SQUAD_SPLIT  (KOTH_RADIUS * 3.0f)

enum
{
    KOTH_FIGHT,
    KOTH_BREAKING,
};

/* who holds the hill this second */
enum
{
    KOTH_NOBODY    = -1,
    KOTH_CONTESTED = 2, /* TEAM_A 0 and TEAM_B 1 are the teams */
};

static struct
{
    int      hx, hy, hz;
    int      floor;      /* the cylinder's floor: the deepest floor in the ring */
    int      round;      /* from 1 */
    int      state;
    int      holder;     /* KOTH_NOBODY, TEAM_A, TEAM_B or KOTH_CONTESTED */
    int      on[2];      /* players of each team on the hill */
    int      points[2];  /* this round's */
    int      last_winner; /* of the last round, -1 before the first */
    uint16_t hill_secs[256]; /* each player's seconds on the hill this match */
    uint64_t deadline;
    uint64_t next_score;
    uint64_t break_until;
    uint64_t next_fort; /* the next look for a structure on the hill */
    uint8_t  ended;
} koth;

/* the host's live choice for the classes (the console's /classes): -1 the
   mode's own rule (config.toml, the command line) */
static int koth_classes_host = -1;

/* where round k's hill goes along the line across the middle of the two
   spawns, in parts of the distance between them: every point of that line
   is as far from one spawn as from the other. A hill picked round the map's
   middle could sit by one team's spawn and the round was won at the start. */
static const float koth_offsets[] = {0.0f, 0.3f, -0.3f, 0.15f, -0.15f, 0.45f, -0.45f};
#define KOTH_SITES ((int) (sizeof koth_offsets / sizeof koth_offsets[0]))

static void _koth_spawn_middle(server_t* server, int team, float* x, float* y)
{
    quad3d_t* s = &server->protocol.spawns[team];
    *x          = (s->from.x + s->to.x) / 2.0f;
    *y          = (s->from.y + s->to.y) / 2.0f;
}

/* a dry column whose ground is within 2 of z: somewhere to stand on the hill */
/* the cylinder's floor: the deepest cell a player stands on inside the
   ring (solid, air over it, not the water layer), down to KOTH_FLOOR_DEPTH
   under the hill: under a roof or a tree it is the street or the grass */
static int _koth_floor(map_store_t* map, int hx, int hy, int hz)
{
    int floor = hz, r = (int) KOTH_RADIUS;
    for (int ix = -r; ix <= r; ix++) {
        for (int iy = -r; iy <= r; iy++) {
            if (ix * ix + iy * iy > r * r) {
                continue;
            }
            int x = hx + ix, y = hy + iy;
            for (int z = hz + KOTH_FLOOR_DEPTH; z > floor; z--) {
                if (z < map->size_z - 1 && map_solid(map, x, y, z) && !map_solid(map, x, y, z - 1)) {
                    floor = z;
                    break;
                }
            }
        }
    }
    return floor;
}

static int _koth_level(map_store_t* map, int x, int y, int z)
{
    if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y) {
        return 0;
    }
    int top = map_top(map, (uint16_t) x, (uint16_t) y);
    return top >= 4 && top < map->size_z - 2 && abs(top - z) <= 2;
}

/* most of the 9x9 round the column stands at its level: a hill on 2Fort's
   one-block bridge over the channel had its holders' places in the water
   under it, out of the hill's height, and no round ever ended */
static int _koth_flat(map_store_t* map, int x, int y, int z)
{
    int n = 0;
    for (int ix = -4; ix <= 4; ix++) {
        for (int iy = -4; iy <= 4; iy++) {
            n += _koth_level(map, x + ix, y + iy, z);
        }
    }
    return n >= 56;
}

/* round's hill: the dry column nearest its point on that line (2Fort's
   middle is the water channel), flat round it (_koth_flat; a second pass
   takes any dry one), the fairest one of the nearest ring (the smallest
   difference of its distances to the two spawns), inside the play area.
   0 when the whole neighbourhood is water. */
static int _koth_site(server_t* server, int round, int* ox, int* oy, int* oz)
{
    map_store_t* map = &server->s_map.map;
    float     ax, ay, bx, by;
    _koth_spawn_middle(server, TEAM_A, &ax, &ay);
    _koth_spawn_middle(server, TEAM_B, &bx, &by);
    float dx = bx - ax, dy = by - ay;
    float off = koth_offsets[(round - 1) % KOTH_SITES];
    /* (-dy, dx): the perpendicular to the spawns' line, as long as it */
    float tx = (ax + bx) / 2.0f - dy * off;
    float ty = (ay + by) / 2.0f + dx * off;
    int   x0, y0, x1, y1;
    map_meta_play_area(server, &x0, &y0, &x1, &y1);
    x0 += 4;
    y0 += 4;
    x1 -= 4;
    y1 -= 4;
    int cx = (int) fminf(fmaxf(tx, (float) x0), (float) x1);
    int cy = (int) fminf(fmaxf(ty, (float) y0), (float) y1);
    for (int k = 0; k < 2 * 49; k++) {
        int   flat = k < 49, r = k % 49;
        float best = 1e9f;
        int   bxc = -1, byc = -1, bz = 0;
        for (int ix = -r; ix <= r; ix++) {
            for (int iy = -r; iy <= r; iy++) {
                if (abs(ix) != r && abs(iy) != r) {
                    continue;
                }
                int x = cx + ix, y = cy + iy;
                if (x < x0 || y < y0 || x > x1 || y > y1) {
                    continue;
                }
                int top = map_top(map, (uint16_t) x, (uint16_t) y);
                if (top < 4 || top >= map->size_z - 2) {
                    continue; /* the sky's edge, or open water */
                }
                if (flat && !_koth_flat(map, x, y, top)) {
                    continue;
                }
                float da = sqrtf((x - ax) * (x - ax) + (y - ay) * (y - ay));
                float db = sqrtf((x - bx) * (x - bx) + (y - by) * (y - by));
                if (fabsf(da - db) < best) {
                    best = fabsf(da - db);
                    bxc  = x;
                    byc  = y;
                    bz   = top;
                }
            }
        }
        if (bxc >= 0) {
            *ox = bxc;
            *oy = byc;
            *oz = bz;
            return 1;
        }
    }
    return 0;
}

static void _koth_place(server_t* server)
{
    if (!_koth_site(server, koth.round, &koth.hx, &koth.hy, &koth.hz)) {
        koth.hx = server->s_map.map.size_x / 2;
        koth.hy = server->s_map.map.size_y / 2;
        koth.hz = map_top(&server->s_map.map, (uint16_t) koth.hx, (uint16_t) koth.hy);
    }
    koth.floor = _koth_floor(&server->s_map.map, koth.hx, koth.hy, koth.hz);
    koth.next_fort = get_nanos(); /* the team that takes it may fortify at once */
    float ax, ay, bx, by;
    _koth_spawn_middle(server, TEAM_A, &ax, &ay);
    _koth_spawn_middle(server, TEAM_B, &bx, &by);
    evt_log(NULL, "koth", "action=hill round=%d at=%d,%d,%d floor=%d to_team1=%.0f to_team2=%.0f", koth.round,
            koth.hx, koth.hy, koth.hz, koth.floor, hypotf(koth.hx - ax, koth.hy - ay),
            hypotf(koth.hx - bx, koth.hy - by));
}

static void _koth_init(server_t* server)
{
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    /* the team scores count rounds won, as in the arena */
    server->protocol.gamemode.score_limit   = KOTH_ROUNDS;
    server->protocol.gamemode.score[0]      = 0;
    server->protocol.gamemode.score[1]      = 0;
    uint64_t now     = get_nanos();
    koth.deadline    = now + (uint64_t) gamemode_match_seconds("koth", KOTH_SECONDS) * NANO_IN_SECOND;
    koth.next_score  = now;
    koth.next_fort   = now;
    koth.ended       = 0;
    koth.round       = 1;
    koth.state       = KOTH_FIGHT;
    koth.holder      = KOTH_NOBODY;
    koth.on[0]       = koth.on[1] = 0;
    koth.points[0]   = koth.points[1] = 0;
    koth.last_winner = -1;
    memset(koth.hill_secs, 0, sizeof koth.hill_secs);
    _koth_place(server);
    /* the host's choice for the classes: the command line (--classes 0|1)
       first, then the console (/classes) */
    int classes = 0;
    if (server->classes_flag == 1) {
        classes = 1;
    } else if (server->classes_flag == 2) {
        classes = 0;
    }
    if (koth_classes_host >= 0) {
        classes = koth_classes_host;
    }
    /* king of the hill has the classes option: the host's choice turns them
       on for this map. The classes own the perks then (like ngCTF). */
    if (classes) {
        server->features_on |= GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS;
    }
    LOG_INFO("King of the Hill: hill at %d,%d,%d, %d s held a round, %d rounds win, %d s at most", koth.hx,
             koth.hy, koth.hz, KOTH_ROUND_POINTS, KOTH_ROUNDS, (int) ((koth.deadline - now) / NANO_IN_SECOND));
    evt_log(NULL, "koth", "action=start at=%d,%d,%d round_points=%d rounds=%d classes=%d", koth.hx, koth.hy,
            koth.hz, KOTH_ROUND_POINTS, KOTH_ROUNDS, classes);
}

static void _koth_end(server_t* server, const char* why)
{
    uint8_t* score  = server->protocol.gamemode.score;
    int      winner = score[0] != score[1]                   ? (score[0] > score[1] ? TEAM_A : TEAM_B)
                      : koth.points[0] != koth.points[1] ? (koth.points[0] > koth.points[1] ? TEAM_A : TEAM_B)
                                                         : -1;
    koth.ended      = 1;
    evt_log(NULL, "koth", "action=end winner=%d rounds=%u-%u why=%s", winner, score[0], score[1], why);
    gamemode_match_won(server);
}

/* on the hill: in its cylinder, from under its floor up to the sky */
static int _koth_on(const player_t* p)
{
    float dx = p->movement.position.x - (koth.hx + 0.5f);
    float dy = p->movement.position.y - (koth.hy + 0.5f);
    float dz = p->movement.position.z - (float) koth.floor;
    return dx * dx + dy * dy <= KOTH_RADIUS * KOTH_RADIUS && dz <= KOTH_HEIGHT;
}

/* once a second: who is on the hill, who holds it, and its points. Only a
   team alone on it scores; one enemy inside stops the count. */
static void _koth_count(server_t* server)
{
    int       on[2] = {0, 0};
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
            continue;
        }
        if (_koth_on(p)) {
            on[p->team]++;
            if (koth.state == KOTH_FIGHT && koth.hill_secs[p->id] < UINT16_MAX) {
                koth.hill_secs[p->id]++;
            }
        }
    }
    koth.on[0] = on[0];
    koth.on[1] = on[1];
    int holder = on[0] > 0 && on[1] > 0 ? KOTH_CONTESTED : on[0] > 0 ? TEAM_A : on[1] > 0 ? TEAM_B : KOTH_NOBODY;
    if (holder != koth.holder) {
        evt_log(NULL, "koth", "action=holder holder=%d on=%d,%d points=%d,%d", holder, on[0], on[1], koth.points[0],
                koth.points[1]);
        koth.holder = holder;
    }
    if (koth.state != KOTH_FIGHT || (holder != TEAM_A && holder != TEAM_B)) {
        return;
    }
    if (++koth.points[holder] < KOTH_ROUND_POINTS) {
        return;
    }
    uint8_t* score = server->protocol.gamemode.score;
    score[holder]++;
    koth.last_winner = holder;
    /* the hill's reward: the tent's restock (health, blocks, grenades, the
       reserves) for everyone of the winning team on it, for the run to the
       next one */
    int restocked = 0;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state == STATE_READY && p->alive && (int) p->team == holder && _koth_on(p)) {
            player_restock(server, p);
            restocked++;
        }
    }
    evt_log(NULL, "koth", "action=round_won round=%d team=%d rounds=%u-%u points=%d,%d restocked=%d", koth.round,
            holder, score[0], score[1], koth.points[0], koth.points[1], restocked);
    if (score[holder] >= KOTH_ROUNDS) {
        _koth_end(server, "rounds");
        return;
    }
    /* the next round's hill at once, so the break is the run to it */
    koth.round++;
    koth.points[0] = koth.points[1] = 0;
    koth.state       = KOTH_BREAKING;
    koth.break_until = get_nanos() + (uint64_t) KOTH_BREAK * NANO_IN_SECOND;
    _koth_place(server);
}

/* the way the other team comes from, for the structures' facing */
static void _koth_face_enemy(server_t* server, int team, int* fx, int* fy)
{
    float ex, ey;
    _koth_spawn_middle(server, team == TEAM_A ? TEAM_B : TEAM_A, &ex, &ey);
    float dx = ex - ((float) koth.hx + 0.5f), dy = ey - ((float) koth.hy + 0.5f);
    if (fabsf(dx) >= fabsf(dy)) {
        *fx = dx >= 0.0f ? 1 : -1;
        *fy = 0;
    } else {
        *fx = 0;
        *fy = dy >= 0.0f ? 1 : -1;
    }
}

/* When a team is on the hill together it fortifies it: from three bots a
   watchtower at its middle (the rank 0 bot's old pillar, now raised by the
   structure's builders), from four a battlemented ring with a gate each
   side. The structures are shared cover; the bots building them are the
   nearest of that team, up to four. A structure is asked for every five
   seconds; the request itself says no while one of the kind stands there. */
/* Lookouts over the hill: a tall watchtower on the highest ground round it
   that sees the hill's floor (inside the ring it counts as on the hill), and
   once the team holds it a two floor barricade facing the enemy, both raised
   by the bots that take the chance and watched from their tops. The search
   reads the map along a ray a column: every 15 s a team, not every request. */
static uint64_t koth_next_lookout[TEAM_B + 1];

static void _koth_lookouts(server_t* server, int team, int n, int fx, int fy)
{
    uint64_t now = get_nanos();
    if (now < koth_next_lookout[team]) {
        return;
    }
    koth_next_lookout[team] = now + 15ull * NANO_IN_SECOND;
    int x, y, z;
    if (!bot_fort_exists(BOT_FORT_WATCHTOWER, koth.hx, koth.hy, 30) &&
        bot_fort_overlook(server, (uint8_t) team, BOT_FORT_WATCHTOWER, koth.hx, koth.hy, koth.hz, 6, 26, fx, fy, &x,
                          &y, &z))
    {
        bot_fort_request_watch(server, (uint8_t) team, BOT_FORT_WATCHTOWER, x, y, z, fx, fy, koth.hx, koth.hy, koth.hz);
    }
    if (koth.holder == team && n >= 4 && !bot_fort_exists(BOT_FORT_BARRICADE, koth.hx, koth.hy, 20) &&
        bot_fort_overlook(server, (uint8_t) team, BOT_FORT_BARRICADE, koth.hx, koth.hy, koth.hz, 6, 16, fx, fy, &x,
                          &y, &z))
    {
        bot_fort_request_watch(server, (uint8_t) team, BOT_FORT_BARRICADE, x, y, z, fx, fy, koth.hx, koth.hy, koth.hz);
    }
}

static void _koth_forts(server_t* server)
{
    for (int team = TEAM_A; team <= TEAM_B; team++) {
        int n = koth.on[team];
        if (n < 3) {
            continue;
        }
        int fx, fy;
        _koth_face_enemy(server, team, &fx, &fy);
        /* the tower first, while the team is on the hill together: it is the
           rank 0 bot's lookout and it has to go up before the fight settles
           into a hold. The ring needs the hill actually held, or two teams
           asking for the same spots left both structures half done. */
        int made = bot_fort_request(server, (uint8_t) team, BOT_FORT_TOWER, koth.hx, koth.hy, koth.hz, fx, fy);
        if (koth.holder == team && n >= 4 &&
            (bot_fort_complete(BOT_FORT_TOWER, koth.hx, koth.hy, 6) ||
             (!made && !bot_fort_exists(BOT_FORT_TOWER, koth.hx, koth.hy, 6))))
        {
            bot_fort_request(server, (uint8_t) team, BOT_FORT_RING, koth.hx, koth.hy, koth.hz, fx, fy);
        }
        _koth_lookouts(server, team, n, fx, fy);
    }
}

static void _koth_tick(server_t* server)
{
    if (koth.ended) {
        return;
    }
    uint64_t now = get_nanos();
    if (koth.state == KOTH_BREAKING && now >= koth.break_until) {
        koth.state = KOTH_FIGHT;
        evt_log(NULL, "koth", "action=fight round=%d", koth.round);
    }
    if (now >= koth.next_score) {
        koth.next_score = now + NANO_IN_SECOND;
        _koth_count(server);
        if (koth.ended) {
            return;
        }
    }
    if (now >= koth.next_fort) {
        koth.next_fort = now + 5ull * NANO_IN_SECOND;
        _koth_forts(server);
    }
    if (now >= koth.deadline) {
        _koth_end(server, "time");
    }
}

static const char* _koth_team_word(int team)
{
    return team == TEAM_A ? "team1" : team == TEAM_B ? "team2" : team == KOTH_CONTESTED ? "contested" : "none";
}

static void _koth_state_keys(server_t* server, gm_keys_t* out)
{
    (void) server;
    /* the hill is a Checkpoint entity (Proto10/Flags.c), sent before these
       keys; they keep what it cannot say */
    proto10_flags_set(P10_FLAG_HILL, (float) koth.hx, (float) koth.hy, (float) koth.hz, -1);
    gm_keys_put(out, P10_GM_KOTH_RADIUS, "%d", (int) KOTH_RADIUS);
    gm_keys_put(out, P10_GM_KOTH_FLOOR, "%d", koth.floor);
    gm_keys_put(out, P10_GM_KOTH_STATE, "%s", koth.state == KOTH_FIGHT ? "fight" : "break");
    gm_keys_put(out, P10_GM_KOTH_ROUND, "%d", koth.round);
    gm_keys_put(out, P10_GM_KOTH_HOLDER, "%s", _koth_team_word(koth.holder));
    gm_keys_put(out, P10_GM_KOTH_ON, "%d,%d", koth.on[0], koth.on[1]);
    gm_keys_put(out, P10_GM_KOTH_POINTS, "%d,%d,%d", koth.points[0], koth.points[1], KOTH_ROUND_POINTS);
    if (koth.last_winner >= 0) {
        gm_keys_put(out, P10_GM_KOTH_WINNER, "%s", _koth_team_word(koth.last_winner));
    }
}

static int _koth_time_left(server_t* server)
{
    (void) server;
    return koth.ended ? 0 : gamemode_seconds_until(koth.deadline);
}

static int _koth_extend(server_t* server, int seconds)
{
    koth.deadline += (uint64_t) seconds * NANO_IN_SECOND;
    server->match_extra_seconds += (uint32_t) seconds;
    evt_log(NULL, "koth", "action=extend seconds=%d", seconds);
    return 1;
}

/* a place on the hill along the spoke at angle a, r out: in along it until
   the ground is at the hill's level (a place in a ditch or on a roof beside
   it is out of the hill's height) */
static void _koth_spoke(server_t* server, float a, float r, float* x, float* y)
{
    while (r > 0.0f && !_koth_level(&server->s_map.map, (int) (koth.hx + 0.5f + cosf(a) * r),
                                    (int) (koth.hy + 0.5f + sinf(a) * r), koth.hz)) {
        r -= 1.0f;
    }
    if (r < 0.0f) {
        r = 0.0f;
    }
    *x = koth.hx + 0.5f + cosf(a) * r;
    *y = koth.hy + 0.5f + sinf(a) * r;
}

/* the bot's place among its team's bots, by id: the same jobs every life */
/* the team's Marksmen by id, the first KOTH_OVERWATCH of them watch the
   hill from high ground (-1: not one of them) */
static int _koth_marksman_rank(server_t* server, player_t* bot)
{
    if (!bot->is_bot || bot->klass != GAME_CLASS_MARKSMAN || !classes_on(server)) {
        return -1;
    }
    int       rank = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        rank += p->is_bot && p->team == bot->team && p->klass == GAME_CLASS_MARKSMAN && p->id < bot->id;
    }
    return rank;
}

/* a Rocketeer: it holds the hill's air (gm_bot_goal_t.perch) instead of a
   bunker on the ring, which would keep its jetpack on the ground */
static int _koth_rocketeer(server_t* server, player_t* bot)
{
    return bot->is_bot && classes_on(server) && bot->klass == GAME_CLASS_ROCKETEER;
}

/* the bots of the bot's team */
static int _koth_team_bots(server_t* server, player_t* bot)
{
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        n += p->is_bot && p->team == bot->team;
    }
    return n;
}

static int _koth_rank(server_t* server, player_t* bot)
{
    int       rank = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        rank += p->is_bot && p->team == bot->team && p->id < bot->id;
    }
    return rank;
}

/* The hill's jobs, by the bot's rank in its team: the first raises a tower
   in the hill's middle (Babel's pillar, KOTH_TOWER blocks: a lookout over
   the hill that still counts on it), the first KOTH_OVERWATCH Marksmen
   watch it from high ground round it, the next hold posts round the ring
   facing out in pairs, each pair putting up one bunker (GM_INTENT_POST:
   the bots fight from it as from a camper's spot), so the team that holds
   the hill walls it in; the rest go onto it and fight. Off the
   hill every one of them runs onto it shooting on the move (GM_BOT_RUSH): a
   bot that stopped for every enemy in sight never got there on 2Fort, where
   both teams see each other from their spawns. */
static int _koth_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    memset(out, 0, sizeof(*out));
    if (bot->team != TEAM_A && bot->team != TEAM_B) {
        return 0;
    }
    float hdx  = bot->movement.position.x - (koth.hx + 0.5f);
    float hdy  = bot->movement.position.y - (koth.hy + 0.5f);
    int   off  = hdx * hdx + hdy * hdy > (KOTH_RADIUS * 0.8f) * (KOTH_RADIUS * 0.8f);
    int   rank = _koth_rank(server, bot);
    /* the squad goes to the hill together (squads_follow); the overwatch
       Marksmen keep their own way: they watch the hill from off it */
    int marksman = _koth_marksman_rank(server, bot);
    if (!(marksman >= 0 && marksman < KOTH_OVERWATCH) &&
        squads_follow(server, bot, koth.hx + 0.5f, koth.hy + 0.5f, KOTH_SQUAD_SPLIT, out))
    {
        return 1;
    }
    out->z     = (float) koth.hz;
    out->role  = off ? GM_BOT_RUSH : GM_BOT_ATTACK;
    /* each team's tower and posts at angles of its own, so the two teams'
       walls do not fight over the same cells */
    float team_turn = bot->team == TEAM_A ? 0.0f : 3.14159265f / KOTH_FORTS;
    if (rank == 0) {
        /* a watchtower the team raised (or one the enemy did: the cover is
           shared): stand on its top and watch the other side. While it is
           going up, hold beside it and let its builders work */
        float tx, ty;
        int   tf;
        if (bot_fort_top(BOT_FORT_TOWER, koth.hx, koth.hy, 6, &tx, &ty, &tf)) {
            out->x  = tx;
            out->y  = ty;
            out->z  = (float) tf;
            gm_goal_name(out, "hill-tower-top", 0);
            out->hold = 1.2f;
            float ex, ey;
            _koth_spawn_middle(server, bot->team == TEAM_A ? TEAM_B : TEAM_A, &ex, &ey);
            out->watch_x = ex;
            out->watch_y = ey;
            return 1;
        }
        if (bot_fort_exists(BOT_FORT_TOWER, koth.hx, koth.hy, 6)) {
            float a = bot->team == TEAM_A ? 0.0f : 3.14159265f;
            _koth_spoke(server, a, KOTH_RADIUS * 0.55f, &out->x, &out->y);
            gm_goal_name(out, "hill", 0);
            out->hold    = 1.5f;
            out->watch_x = out->x + cosf(a) * 8.0f;
            out->watch_y = out->y + sinf(a) * 8.0f;
            return 1;
        }
        out->x = koth.hx + (bot->team == TEAM_A ? -0.5f : 1.5f);
        out->y = koth.hy + 0.5f;
        if (!_koth_level(&server->s_map.map, (int) out->x, (int) out->y, koth.hz)) {
            out->x = koth.hx + 0.5f;
        }
        out->exact = 1;
        out->perch = KOTH_RADIUS; /* a roof or a tree top over the hill counts */
        if ((int) bot->movement.position.x == (int) out->x && (int) bot->movement.position.y == (int) out->y) {
            gm_goal_name(out, "hill-tower", 0);
            out->climb = 1;
            out->z     = (float) (koth.hz - KOTH_TOWER);
        } else {
            gm_goal_name(out, "hill-tower-foot", 0);
        }
        return 1;
    }
    /* the Marksmen watch the hill from the highest ground round it that
       sees it (the CTF's overwatch, home the hill): standing on the hill
       with the rest, a rifle was wasted at ten blocks */
    if (marksman >= 0 && marksman < KOTH_OVERWATCH) {
        out->x        = koth.hx + 0.5f;
        out->y        = koth.hy + 0.5f;
        out->seated   = 1;
        out->camp     = 2;
        out->camp_ground = 1;
        out->has_home = 1;
        out->home_x   = koth.hx + 0.5f;
        out->home_y   = koth.hy + 0.5f;
        out->home_z   = (float) koth.floor;
        out->role     = GM_BOT_ATTACK;
        gm_goal_name(out, "hill-overwatch", 0);
        return 1;
    }
    /* the posts: two holders each, two apart, building one bunker between
       them (a third of the team at most: the rest take the hill). A
       Rocketeer skips them: it watches the ring from above (perch) */
    int holders = 2 * KOTH_FORTS;
    if (holders > _koth_team_bots(server, bot) / 3 * 2) {
        holders = _koth_team_bots(server, bot) / 3 * 2;
    }
    if (rank <= holders && !_koth_rocketeer(server, bot)) {
        int   post = (rank - 1) / 2, side = (rank - 1) % 2 ? 1 : -1;
        float a    = team_turn + (float) post * 2.0f * 3.14159265f / KOTH_FORTS;
        _koth_spoke(server, a, KOTH_RADIUS * 0.6f, &out->x, &out->y);
        /* across the spoke: each holder a block and a half off its middle */
        out->x += -sinf(a) * 1.5f * side;
        out->y += cosf(a) * 1.5f * side;
        gm_goal_name(out, "post", GM_INTENT_POST);
        out->watch_x    = out->x + cosf(a) * 12.0f;
        out->watch_y    = out->y + sinf(a) * 12.0f;
        out->wall_style = 1;
        out->wall_side  = (int8_t) side;
        return 1;
    }
    float a = (float) bot->id * 2.399963f;
    /* the structures take the middle and a ring at four cells: hold inside
       the ring or outside it, never on its wall (the path there read as
       stuck at the wall's foot) */
    float r = KOTH_RADIUS * 0.55f * (float) ((bot->id % 4) + 1) / 4.0f;
    if (bot_fort_exists(BOT_FORT_RING, koth.hx, koth.hy, 8) || bot_fort_exists(BOT_FORT_TOWER, koth.hx, koth.hy, 8)) {
        r = bot->id % 3 == 0 ? 3.0f : 6.5f + (float) (bot->id % 3) * 1.2f;
    }
    _koth_spoke(server, a, r, &out->x, &out->y);
    gm_goal_name(out, "hill", 0);
    out->perch   = KOTH_RADIUS; /* any height over the ring holds the hill */
    out->hold    = 1.5f;
    out->watch_x = out->x + cosf(a) * 8.0f;
    out->watch_y = out->y + sinf(a) * 8.0f;
    if (off) {
        gamemode_flank(server, bot, out);
    }
    return 1;
}

/* the host's choice for the classes: king of the hill has the classes option,
   the host's choice turns them on for this map */
static int _koth_set_classes(server_t* server, int on)
{
    koth_classes_host = on ? 1 : 0;
    if (koth_classes_host) {
        server->features_on |= GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS;
    } else {
        server->features_on &= ~(GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS);
    }
    /* the clients in the game: the class screen appears (or goes) at once */
    classes_tell_all(server, koth_classes_host);
    LOG_INFO("King of the Hill: the classes are %s", koth_classes_host ? "on" : "off");
    evt_log(NULL, "koth", "action=classes on=%d", koth_classes_host);
    return 1;
}

/* with the classes on, a client entering the game is told so (the class
   screen replaces the weapon screen); off, classes_client_in_game says
   nothing */
static void _koth_client_in_game(server_t* server, player_t* player)
{
    classes_client_in_game(server, player);
}

/* the class screen's requests ("class 2", the drill, the second gun) */
static int _koth_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    return classes_command(server, player, command, arg);
}

/* the match summary's column: seconds on the hill while a round was on */
static uint32_t _koth_player_hill(server_t* server, player_t* player)
{
    (void) server;
    return koth.hill_secs[player->id];
}

/* a spawn faces the hill (it was born facing along x, often away from it) */
static int _koth_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    (void) server;
    (void) player;
    *x = koth.hx + 0.5f;
    *y = koth.hy + 0.5f;
    return 1;
}

const gamemode_ops_t koth_ops = {
    .id             = GAME_MODE_KOTH,
    .name           = "koth",
    .wire_name      = "koth",
    /* squads, and a respawn by a mate out of a fight or at the leader's
       rally: without them every life began at the far spawn and the hill was
       a lone walk across the map */
    .features       = GM_FEAT_SQUAD_SPAWN,
    .squad_teams    = SQUAD_TEAMS_BOTH,
    .init           = _koth_init,
    .tick           = _koth_tick,
    .extend         = _koth_extend,
    .time_left         = _koth_time_left,
    .state_keys     = _koth_state_keys,
    .summary_stat   = "hill",
    .player_stat    = _koth_player_hill,
    .bot_goal       = _koth_bot_goal,
    .spawn_facing   = _koth_spawn_facing,
    .set_classes    = _koth_set_classes,
    .client_in_game = _koth_client_in_game,
    .player_command = _koth_command,
    /* a class's health: every hit scaled by 100 / health (no class: as is) */
    .hit_damage     = classes_hit_damage,
};
