// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Crates.h>
#include <Server/Proto10/Entities.h>
#include <Server/Proto10/Rockets.h>
#include <Server/Profile.h>
#include <Util/Log.h>
#include <crates/drop.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <Util/Weapon.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CRATE_MAX        16
#define CRATE_CROWD_R    12.0f /* blocks */
#define CRATE_DROP_ABOVE 30    /* blocks over the ground it starts falling from */
/* seconds on the ground: then it goes, so a long match never piles them up
   (CRATE_MAX at once all the same) */
#define CRATE_LIFETIME   120
#define CRATE_REACH      1.5f  /* a player's middle this near takes it */
/* the heat map: where players died lately, in cells of HEAT_CELL x
   HEAT_CELL columns (64 x 64 of them cover a 1536 map), each death 1,
   halved every HEAT_HALF seconds; a hot spot's crate lands round the cell's
   mean death within HEAT_SCATTER */
#define HEAT_CELL        24
#define HEAT_GRID        64
#define HEAT_HALF        60.0f
#define HEAT_SCATTER     6.0f

typedef struct
{
    uint32_t id; /* 0: a free slot */
    uint8_t  type; /* P10_ENT_AMMO_CRATE or P10_ENT_HEALTH_CRATE */
    float    x, y, z;   /* map coordinates (z down): where it is now */
    float    ground;    /* the z it lands on (the top of the column) */
    float    fall_from; /* the z its current fall began at (a drop's, or where it lost its support) */
    game_crate_drop_t drop;  /* the fall, common/crates/drop.h (the clients run it too) */
    long              drop_steps; /* the 1/60 s steps of that fall already run */
    uint8_t  landed;
    uint8_t  from_kill; /* counts against the rule's max_out */
    uint8_t  from_heat; /* counts against the rule's hot_max */
    uint64_t born, landed_at;
} crate_t;

static crate_t  crates[CRATE_MAX];
static uint64_t crate_kill_at; /* the last drop of a kill: the rule's cooldown */
static uint64_t crate_hot_at;  /* the last hot spot's drop */
static uint64_t heat_decay_at; /* the heat map's last halving step */

typedef struct
{
    float heat;   /* recent deaths, decaying */
    float sx, sy; /* their positions, weighted the same: the mean is sx / heat */
} heat_cell_t;

static heat_cell_t heat[HEAT_GRID][HEAT_GRID];

/* the default, where the classes play: a kill in a crowd of "more than 3"
   (the killer counted) drops one 15% of the time, within 8 blocks */
/* the default, where the classes play: a kill in a crowd of "more than 3"
   (the killer counted) drops one 15% of the time, within 8 blocks; and
   every 40 s one where 2 or more died lately (a fight that goes on keeps
   being fed), at most 4 of those out */
static const gm_crate_rule_t crate_default = {.chance    = 0.15f,
                                              .crowd     = 4,
                                              .team      = 255,
                                              .near_dist = 0.0f,
                                              .far_dist  = 8.0f,
                                              .cooldown  = 0,
                                              .max_out   = CRATE_MAX,
                                              .hot_every = 40,
                                              .hot_min   = 2.0f,
                                              .hot_max   = 4};

/* the mode's rule, or the default where the classes play; NULL: no crate
   comes by itself (the admins' and the plugins' still do) */
static const gm_crate_rule_t* _crate_rule(server_t* server)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops != NULL && ops->crate_rule != NULL) {
        return ops->crate_rule;
    }
    return gamemode_has(server, GM_FEAT_CLASSES) ? &crate_default : NULL;
}

/* a death warms its cell (any death: a fall in a fight is the fight's) */
static void _crate_heat_add(float x, float y)
{
    int cx = (int) floorf(x / HEAT_CELL), cy = (int) floorf(y / HEAT_CELL);
    if (cx < 0 || cy < 0 || cx >= HEAT_GRID || cy >= HEAT_GRID) {
        return;
    }
    heat[cx][cy].heat += 1.0f;
    heat[cx][cy].sx += x;
    heat[cx][cy].sy += y;
}

static const char* _crate_kind(const crate_t* c)
{
    return c->type == P10_ENT_AMMO_CRATE ? "ammo" : "health";
}

static void _crate_item(const crate_t* c, int create, p10_entity_item* it)
{
    memset(it, 0, sizeof *it);
    it->entity_id           = c->id;
    it->mask                = (uint8_t) (P10_EU_TRAJECTORY | (create ? P10_EU_CREATE : 0));
    it->entity_type         = c->type;
    it->owner               = P10_NO_OWNER;
    it->team                = P10_TEAM_SPECTATOR;
    /* the crate is never moved by the wire: a client falls it itself, the
       retail client's way (common/crates/drop.h), from where it is created;
       the server's copy is for the pickup only */
    it->trajectory.type     = P10_TRAJ_CONSTANT;
    it->trajectory.origin.x = c->x;
    it->trajectory.origin.y = c->y;
    it->trajectory.origin.z = c->z;
}

static void _crate_send(server_t* server, const crate_t* c, int create)
{
    uint8_t         buf[128];
    p10_writer      w;
    p10_entity_item it;
    _crate_item(c, create, &it);
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    rockets_broadcast(server, &w);
}

static void _crate_remove(server_t* server, crate_t* c, const char* action)
{
    evt_log(NULL, "crate", "action=%s kind=%s entity=%u at=%.0f,%.0f,%.0f", action, _crate_kind(c), c->id, c->x, c->y,
            c->z);
    uint8_t           buf[16];
    p10_writer        w;
    p10_entity_remove rm = {c->id};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_remove(&w, &rm);
    rockets_broadcast(server, &w);
    c->id = 0;
}

/* a dry column between near and far of (x, y), its ground in *z; 0 when
   24 draws find none */
static int _crate_spot(server_t* server, float cx, float cy, float near_dist, float far_dist, float* x, float* y, float* z)
{
    map_store_t* map = &server->s_map.map;
    for (int tries = 0; tries < 24; tries++) {
        float a  = gen_rand(&server->rand) * 6.2831853f;
        /* right there first when asked for no scatter (a reward on a player) */
        float r  = tries == 0 && far_dist <= 0.0f ? 0.0f
                                             : near_dist + gen_rand(&server->rand) * (far_dist - near_dist > 1.0f ? far_dist - near_dist : 1.0f);
        int   px = (int) floorf(cx + cosf(a) * r), py = (int) floorf(cy + sinf(a) * r);
        if (px < 1 || py < 1 || px >= map->size_x - 1 || py >= map->size_y - 1) {
            continue;
        }
        int top = map_top(map, (uint16_t) px, (uint16_t) py);
        if (top < 2 || top >= map->size_z - 2) {
            continue; /* the sky's edge, or the water */
        }
        *x = (float) px + 0.5f;
        *y = (float) py + 0.5f;
        *z = (float) top;
        return 1;
    }
    return 0;
}

static crate_t* _crate_drop(server_t* server, float x, float y, float near_dist, float far_dist, uint8_t type, const char* why)
{
    if (profile_is_075() || (type != P10_ENT_AMMO_CRATE && type != P10_ENT_HEALTH_CRATE)) {
        return NULL;
    }
    crate_t* c = NULL;
    for (int k = 0; k < CRATE_MAX; k++) {
        if (crates[k].id == 0) {
            c = &crates[k];
            break;
        }
    }
    float cx, cy, cz;
    if (c == NULL || !_crate_spot(server, x, y, near_dist, far_dist, &cx, &cy, &cz)) {
        evt_log(NULL, "crate", "action=drop_skipped why=%s by=%s at=%.0f,%.0f", c == NULL ? "full" : "no_ground", why,
                x, y);
        return NULL;
    }
    memset(c, 0, sizeof *c);
    c->id     = proto10_entities_alloc_id();
    c->type   = type;
    c->x      = cx;
    c->y      = cy;
    c->ground = cz;
    c->fall_from = cz - CRATE_DROP_ABOVE < 0.0f ? 0.0f : cz - CRATE_DROP_ABOVE;
    c->z      = c->fall_from;
    c->drop   = (game_crate_drop_t) {0};
    c->drop_steps = 0;
    c->born   = get_nanos();
    evt_log(NULL, "crate", "action=drop kind=%s entity=%u by=%s at=%.0f,%.0f,%.0f away=%.0f", _crate_kind(c), c->id, why,
            cx, cy, cz, sqrtf((cx - x) * (cx - x) + (cy - y) * (cy - y)));
    _crate_send(server, c, 1);
    return c;
}

int crates_drop(server_t* server, float x, float y, float scatter, uint8_t type, const char* why)
{
    return _crate_drop(server, x, y, 0.0f, scatter, type, why) != NULL;
}

/* the first solid cell in a column at or under z (z grows down): where a
   crate whose support was taken falls to, the water layer included so that
   it never drops out of the world (the drop's own ground search counts it
   the same) */
static float _crate_floor_below(server_t* server, int x, int y, int z)
{
    map_store_t* map = &server->s_map.map;
    int       cz  = z < 0 ? 0 : z;
    for (; cz < map->size_z; cz++) {
        if (map_solid(map, (uint16_t) x, (uint16_t) y, (uint16_t) cz)) {
            return (float) cz;
        }
    }
    return (float) (map->size_z - 1);
}

/* a cell of the map changed (proto10_world_cell, once the map itself has):
   a crate resting on the cell that went falls again, on the Linear fall of
   its drop from where it is; a block built back under a falling one stops
   it there. No mode question: a crate is no objective */
void crates_cell_changed(server_t* server, int x, int y, int z, int solid)
{
    for (int k = 0; k < CRATE_MAX; k++) {
        crate_t* c = &crates[k];
        if (c->id == 0 || (int) floorf(c->x) != x || (int) floorf(c->y) != y) {
            continue;
        }
        if (!solid) {
            if (c->landed && (int) c->ground == z) {
                float ground = _crate_floor_below(server, x, y, z + 1);
                c->landed     = 0;
                c->born       = get_nanos();
                c->fall_from  = c->z;
                c->ground     = ground;
                c->drop       = (game_crate_drop_t) {0};
                c->drop_steps = 0;
                evt_log(NULL, "crate", "action=fall kind=%s entity=%u from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f",
                        _crate_kind(c), c->id, c->x, c->y, c->z, c->x, c->y, ground);
                _crate_send(server, c, 0);
            } else if (!c->landed && (int) c->ground == z) {
                /* the floor it was falling to is gone too */
                c->ground = _crate_floor_below(server, x, y, z + 1);
            }
        } else if (!c->landed && z >= (int) floorf(c->z) && (float) z < c->ground) {
            /* a block built under it: it lands there, not on the old ground */
            c->ground = (float) z;
        }
    }
}

/* tests only: SP_CRATE_CHANCE (0..1) and SP_CRATE_CROWD (players) */
static float _crate_env(const char* name, float fallback)
{
    const char* v = getenv(name);
    return v != NULL && *v != '\0' ? (float) atof(v) : fallback;
}

void crates_on_kill(server_t* server, player_t* killer, player_t* victim)
{
    if (profile_is_075()) {
        return;
    }
    _crate_heat_add(victim->movement.position.x, victim->movement.position.y);
    if (killer == NULL || killer == victim || killer->team == victim->team) {
        return;
    }
    const gm_crate_rule_t* rule = _crate_rule(server);
    if (rule == NULL) {
        return;
    }
    if (rule->team != 255 && killer->team != rule->team) {
        return;
    }
    uint64_t now = get_nanos();
    if (rule->cooldown > 0 && crate_kill_at != 0 && now - crate_kill_at < (uint64_t) rule->cooldown * NANO_IN_SECOND) {
        return;
    }
    int out = 0;
    for (int k = 0; k < CRATE_MAX; k++) {
        out += crates[k].id != 0 && crates[k].from_kill;
    }
    if (out >= rule->max_out) {
        return;
    }
    /* the crowd: everyone alive in a team round the killer, it included */
    int       crowd = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
            continue;
        }
        float dx = p->movement.position.x - killer->movement.position.x;
        float dy = p->movement.position.y - killer->movement.position.y;
        float dz = p->movement.position.z - killer->movement.position.z;
        crowd += dx * dx + dy * dy + dz * dz <= CRATE_CROWD_R * CRATE_CROWD_R;
    }
    if (crowd < (int) _crate_env("SP_CRATE_CROWD", rule->crowd) ||
        gen_rand(&server->rand) >= _crate_env("SP_CRATE_CHANCE", rule->chance)) {
        return;
    }
    crate_t* c = _crate_drop(server, killer->movement.position.x, killer->movement.position.y, rule->near_dist, rule->far_dist,
                             gen_rand(&server->rand) < 0.5f ? P10_ENT_AMMO_CRATE : P10_ENT_HEALTH_CRATE, "kill");
    if (c != NULL) {
        c->from_kill  = 1;
        crate_kill_at = now;
    }
}

/* what the crate gives, only to a player who needs it (a full one walks
   over it and leaves it for a mate) */
static int _crate_take(server_t* server, crate_t* c, player_t* p)
{
    if (c->type == P10_ENT_HEALTH_CRATE) {
        if (p->hp >= 100) {
            return 0;
        }
        p->hp = 100;
    } else {
        uint8_t reserve = p->weapon_reserve, alt = p->alt_reserve, drill = p->drill_charges;
        set_default_player_ammo_reserve(p);
        classes_restock(server, p);
        if (p->weapon_reserve == reserve && p->alt_reserve == alt && p->drill_charges == drill) {
            return 0;
        }
    }
    const char* parts[] = {"crate", _crate_kind(c)};
    proto10_bridge_generic(server, p, 2, parts);
    evt_log(p, "crate", "action=take kind=%s entity=%u hp=%d reserve=%u", _crate_kind(c), c->id, (int) p->hp,
            (unsigned) p->weapon_reserve);
    return 1;
}

/* once a second: the heat cools; every hot_every seconds a crate where it
   is hottest, drawn by heat among the cells over hot_min (always the
   hottest would feed one fight all match) */
static void _crate_hot_tick(server_t* server, uint64_t now)
{
    if (heat_decay_at == 0) {
        heat_decay_at = now;
        crate_hot_at  = now;
    }
    if (now - heat_decay_at < NANO_IN_SECOND) {
        return;
    }
    float f       = exp2f(-(float) (now - heat_decay_at) / (float) NANO_IN_SECOND / HEAT_HALF);
    heat_decay_at = now;
    for (int i = 0; i < HEAT_GRID; i++) {
        for (int j = 0; j < HEAT_GRID; j++) {
            heat[i][j].heat *= f;
            heat[i][j].sx *= f;
            heat[i][j].sy *= f;
        }
    }
    const gm_crate_rule_t* rule = _crate_rule(server);
    if (rule == NULL || rule->hot_every == 0 || now - crate_hot_at < (uint64_t) rule->hot_every * NANO_IN_SECOND) {
        return;
    }
    crate_hot_at = now;
    int out      = 0;
    for (int k = 0; k < CRATE_MAX; k++) {
        out += crates[k].id != 0 && crates[k].from_heat;
    }
    float total = 0.0f, top = 0.0f;
    for (int i = 0; i < HEAT_GRID; i++) {
        for (int j = 0; j < HEAT_GRID; j++) {
            total += heat[i][j].heat >= rule->hot_min ? heat[i][j].heat : 0.0f;
            top = fmaxf(top, heat[i][j].heat);
        }
    }
    if (out >= rule->hot_max || total <= 0.0f) {
        evt_log(NULL, "crate", "action=hot_skipped why=%s out=%d hottest=%.1f", out >= rule->hot_max ? "full" : "cold",
                out, top);
        return;
    }
    float pick = gen_rand(&server->rand) * total;
    for (int i = 0; i < HEAT_GRID; i++) {
        for (int j = 0; j < HEAT_GRID; j++) {
            heat_cell_t* h = &heat[i][j];
            if (h->heat < rule->hot_min || (pick -= h->heat) > 0.0f) {
                continue;
            }
            crate_t* c = _crate_drop(server, h->sx / h->heat, h->sy / h->heat, 0.0f, HEAT_SCATTER,
                                     gen_rand(&server->rand) < 0.5f ? P10_ENT_AMMO_CRATE : P10_ENT_HEALTH_CRATE, "hotspot");
            if (c != NULL) {
                c->from_heat = 1;
                evt_log(NULL, "crate", "action=hot entity=%u heat=%.1f hottest=%.1f", c->id, h->heat, top);
            }
            return;
        }
    }
}

void crates_tick(server_t* server)
{
    uint64_t now = get_nanos();
    _crate_hot_tick(server, now);
    for (int k = 0; k < CRATE_MAX; k++) {
        crate_t* c = &crates[k];
        if (c->id == 0) {
            continue;
        }
        if (!c->landed) {
            /* the drop, common/crates/drop.h: every client runs these steps to
               draw it, and nothing about the fall is sent; here it is only
               where the crate is for the pickup */
            long steps = (long) ((now - c->born) / (uint64_t) (GAME_CRATE_STEP * (float) NANO_IN_SECOND));
            while (c->drop_steps < steps) {
                float impact = 0.0f;
                c->drop_steps++;
                if (game_crate_drop_step(&c->z, &c->drop, c->ground, &impact)) {
                    c->z      = c->ground;
                    c->landed = 1;
                    /* its life counts from the first landing: digging under a
                       crate again and again must not keep it forever */
                    if (c->landed_at == 0) {
                        c->landed_at = now;
                    }
                    evt_log(NULL, "crate", "action=land kind=%s entity=%u at=%.0f,%.0f,%.0f impact=%.1f", _crate_kind(c),
                            c->id, c->x, c->y, c->z, impact);
                    break;
                }
            }
            continue;
        }
        if (now - c->landed_at >= (uint64_t) CRATE_LIFETIME * NANO_IN_SECOND) {
            _crate_remove(server, c, "expire");
            continue;
        }
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (p->state != STATE_READY || !p->alive || (p->team != TEAM_A && p->team != TEAM_B)) {
                continue;
            }
            /* standing at it: the body's middle over its cell, its feet on
               the crate's ground (the body is 2.4 over its feet) */
            float dx = p->movement.position.x - c->x, dy = p->movement.position.y - c->y;
            float dz = c->z - p->movement.position.z;
            if (dx * dx + dy * dy <= CRATE_REACH * CRATE_REACH && dz >= 0.0f && dz <= 3.5f && _crate_take(server, c, p)) {
                _crate_remove(server, c, "taken");
                break;
            }
        }
    }
}

uint8_t crates_type(const char* word)
{
    return word == NULL ? 0
           : !strcmp(word, "ammo")   ? P10_ENT_AMMO_CRATE
           : !strcmp(word, "health") ? P10_ENT_HEALTH_CRATE
                                     : 0;
}

void crates_reset(void)
{
    memset(crates, 0, sizeof crates);
    crate_kill_at = 0;
    crate_hot_at  = 0;
    heat_decay_at = 0;
    memset(heat, 0, sizeof heat);
}

uint32_t crates_count(void)
{
    uint32_t n = 0;
    for (int k = 0; k < CRATE_MAX; k++) {
        n += crates[k].id != 0;
    }
    return n;
}

void crates_put(p10_writer* w)
{
    for (int k = 0; k < CRATE_MAX; k++) {
        if (crates[k].id != 0) {
            p10_entity_item it;
            _crate_item(&crates[k], 1, &it);
            p10_put_entity_item(w, &it);
        }
    }
}
