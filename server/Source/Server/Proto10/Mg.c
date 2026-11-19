// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/BlockHealth.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Nodes.h>
#include <Server/Packets/Packets.h>
#include <Server/Perks.h>
#include <Server/Plugins.h>
#include <Server/Proto10/Mg.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Sentry.h>
#include <Server/Proto10/World.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <uthash/uthash.h>
#include <classes/classes.h>
#include <math.h>
#include <proto10/orient.h>
#include <string.h>

/* how far a round goes: past the fog's usual 128, as the SMG's tracer */
#define MG_RANGE      128.0f
/* the swivel over the floor's top: the barrel turns there */
#define MG_SWIVEL     1.1f
/* the recoil eases back at this rate (1/s, exponential) */
#define MG_RECOIL_EASE 6.0f
#define MG_DEG         (3.14159265f / 180.0f)

static float _mg_wrap(float a)
{
    while (a > 3.14159265f) {
        a -= 6.2831853f;
    }
    while (a < -3.14159265f) {
        a += 6.2831853f;
    }
    return a;
}

static vector3f_t _mg_swivel(const p10_object_t* o)
{
    vector3f_t s = {o->base.x, o->base.y, o->base.z - MG_SWIVEL};
    return s;
}

vector3f_t mg_gunner_feet(const p10_object_t* mg)
{
    /* behind the barrel along the way it was put down facing: the gunner
       stays put while the barrel swings, as on a tripod */
    p10_vec3   home = {0.0f, 0.0f, mg->home_yaw};
    p10_vec3   f    = p10_front_from_euler(home);
    float      back = GAME_MG_HALF_WIDTH + 0.5f;
    vector3f_t feet = {mg->base.x - f.x * back, mg->base.y - f.y * back, mg->base.z};
    return feet;
}

/* the first solid cell along the ray, within range; 1 and its cell and the
   distance to it */
static int _mg_ray_block(server_t* server, vector3f_t o, p10_vec3 d, float range, int* bx, int* by, int* bz, float* t_out)
{
    map_store_t* map = &server->s_map.map;
    int          x = (int) floorf(o.x), y = (int) floorf(o.y), z = (int) floorf(o.z);
    int          sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
    float        tdx = fabsf(d.x) > 1e-6f ? fabsf(1.0f / d.x) : 1e30f;
    float        tdy = fabsf(d.y) > 1e-6f ? fabsf(1.0f / d.y) : 1e30f;
    float        tdz = fabsf(d.z) > 1e-6f ? fabsf(1.0f / d.z) : 1e30f;
    float        tx  = fabsf(d.x) > 1e-6f ? ((sx > 0 ? (float) (x + 1) - o.x : o.x - (float) x) * tdx) : 1e30f;
    float        ty  = fabsf(d.y) > 1e-6f ? ((sy > 0 ? (float) (y + 1) - o.y : o.y - (float) y) * tdy) : 1e30f;
    float        tz  = fabsf(d.z) > 1e-6f ? ((sz > 0 ? (float) (z + 1) - o.z : o.z - (float) z) * tdz) : 1e30f;
    float        t   = 0.0f;
    while (t <= range) {
        if (z >= 0 && map_solid(map, x, y, z)) {
            *bx    = x;
            *by    = y;
            *bz    = z;
            *t_out = t;
            return 1;
        }
        if (tx < ty && tx < tz) {
            t = tx;
            tx += tdx;
            x += sx;
        } else if (ty < tz) {
            t = ty;
            ty += tdy;
            y += sy;
        } else {
            t = tz;
            tz += tdz;
            z += sz;
        }
        if (x < 0 || y < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z) {
            return 0;
        }
    }
    return 0;
}

/* a round on a block: the SMG's block damage through block health, broken
   at GAME_BLOCK_BREAK like a gun's hit (Block.c's rules: the mode, the
   map's protected cells, the water layer) */
static void _mg_hit_block(server_t* server, player_t* gunner, int x, int y, int z)
{
    if (z >= 62 || (gamemode_player_rules(server, gunner) & GM_RULE_NO_DESTROY) ||
        map_meta_cell_protected(server, x, y, z) || !gamemode_cell_editable(server, x, y, z))
    {
        return;
    }
    int damage = game_weapon_block_damage(WEAPON_SMG);
    int taken  = block_health_peek(server, x, y, z, damage);
    if (taken < GAME_BLOCK_BREAK) {
        block_health_hit(server, x, y, z, damage);
        proto10_world_cell(server, x, y, z, P10_WORLD_CAUSE_NONE);
        return;
    }
    if (!plugins_on_destroy(server, gunner, x, y, z, "mg")) {
        return;
    }
    vector3i_t  cell  = {x, y, z};
    vector3i_t* neigh = get_neighbours(cell);
    map_set_air(&server->s_map.map, x, y, z);
    proto10_world_cell(server, x, y, z, P10_WORLD_CAUSE_PLAYER);
    for (int i = 0; i < 6; ++i) {
        if (neigh[i].z < 62) {
            check_node(server, neigh[i]);
        }
    }
    gamemode_terrain_changed(server, 0);
}

/* the player's box (Sentry.c's): x/y +-0.45, from 0.45 over the position to
   the feet, 2.25 under it standing, 1.35 crouched */
static int _mg_ray_player(const player_t* t, vector3f_t a, vector3f_t b, float* t_out)
{
    p10_object_t box;
    memset(&box, 0, sizeof box);
    box.base.x     = t->movement.position.x;
    box.base.y     = t->movement.position.y;
    box.base.z     = t->movement.position.z + (t->crouching ? 1.35f : 2.25f);
    box.half_width = 0.45f;
    box.height     = box.base.z - (t->movement.position.z - 0.45f);
    return proto10_objects_segment_hits(&box, a, b, t_out);
}

static void _mg_fire(server_t* server, p10_object_t* o, player_t* gunner, uint64_t now)
{
    p10_vec3   f   = p10_front_from_euler(o->euler);
    vector3f_t s   = _mg_swivel(o);
    vector3f_t m   = {s.x + f.x * GAME_MG_MUZZLE, s.y + f.y * GAME_MG_MUZZLE, s.z + f.z * GAME_MG_MUZZLE};
    vector3f_t end = {m.x + f.x * MG_RANGE, m.y + f.y * MG_RANGE, m.z + f.z * MG_RANGE};

    /* the nearest of: a block, an enemy's body, another object */
    float     best = 2.0f; /* a fraction of the range */
    int       bx = 0, by = 0, bz = 0, kind = 0; /* 1 block, 2 player, 3 object */
    float     tb;
    player_t* victim = NULL;
    p10_object_t* thing = NULL;
    if (_mg_ray_block(server, m, f, MG_RANGE, &bx, &by, &bz, &tb)) {
        best = tb / MG_RANGE;
        kind = 1;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == gunner || p->state != STATE_READY || !p->alive || p->team == TEAM_SPECTATOR ||
            p->team == gunner->team || p->is_invisible || mount_in_vehicle(p))
        {
            continue;
        }
        float t;
        if (_mg_ray_player(p, m, end, &t) && t < best) {
            best   = t;
            kind   = 2;
            victim = p;
        }
    }
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* q = proto10_objects_at(i);
        float         t;
        if (q == NULL || q == o || q->team == o->team) {
            continue;
        }
        if (proto10_objects_segment_hits(q, m, end, &t) && t < best) {
            best  = t;
            kind  = 3;
            thing = q;
        }
    }

    int head = 0, limb = 0, damage = 0;
    if (kind == 2) {
        float hz = m.z + (end.z - m.z) * best;
        head     = hz < victim->movement.position.z + 0.15f;
        limb     = !head && hz > victim->movement.position.z + (victim->crouching ? 0.6f : 0.9f);
        damage   = head ? GAME_MG_DAMAGE_HEAD : (limb ? GAME_MG_DAMAGE_LIMB : GAME_MG_DAMAGE_BODY);
    }
    char what[EVT_ACTOR_LEN + 16] = "none";
    if (kind == 1) {
        snprintf(what, sizeof what, "block:%d,%d,%d", bx, by, bz);
    } else if (kind == 2) {
        evt_actor(what, sizeof what, victim);
    } else if (kind == 3) {
        snprintf(what, sizeof what, "entity:%u", thing->id);
    }
    evt_log(gunner, "mg", "action=fire entity=%u hit=%s part=%s damage=%d dist=%.1f", o->id, what,
            kind == 2 ? (head ? "head" : (limb ? "limb" : "body")) : "-", damage,
            kind ? best * MG_RANGE : MG_RANGE);

    /* the Fire event goes with the rotation it was fired at (Objects.c):
       bit 0 the round met something, bit 1 it was a body */
    o->force_trajectory  = 1;
    o->has_pending_event = 1;
    o->pending_event     = P10_EV_FIRE;
    o->pending_param     = (uint64_t) (kind != 0) | (uint64_t) (kind == 2) << 1;
    o->next_shot         = now + (uint64_t) (GAME_MG_FIRE_INTERVAL * NANO_IN_SECOND);
    gunner->shots++;

    if (kind == 1) {
        _mg_hit_block(server, gunner, bx, by, bz);
    } else if (kind == 2) {
        /* the gunner is the attacker (its kill, its score); the damage's
           source is the MG, for the victim's hit indicator */
        proto10_objects_set_damage_source(o);
        send_set_hp(server, gunner, victim, damage, 1, KILL_REASON_MG, 5, 0, m);
        proto10_objects_set_damage_source(NULL);
    } else if (kind == 3) {
        proto10_objects_damage(server, thing, gunner, GAME_MG_DAMAGE_BODY, "mg");
    }

    /* the kick: up, and to a side at random; the barrel eases back */
    o->recoil_x -= GAME_MG_RECOIL_UP * MG_DEG;
    o->recoil_z += (float) (gen_rand(&server->rand) - 0.5) * 2.0f * GAME_MG_RECOIL_SIDE * MG_DEG;
}

static void _mg_think(server_t* server, p10_object_t* o, uint64_t now)
{
    float dt      = o->last_think ? (float) (now - o->last_think) / (float) NANO_IN_SECOND : 0.0f;
    o->last_think = now;
    if (dt > 0.25f) {
        dt = 0.25f;
    }
    int fx = (int) floorf(o->base.x), fy = (int) floorf(o->base.y), fz = (int) floorf(o->base.z + 0.5f);
    if (!map_solid(&server->s_map.map, fx, fy, fz)) {
        proto10_objects_destroy(server, o, 1, "floor_gone");
        return;
    }
    float ease = expf(-MG_RECOIL_EASE * dt);
    o->recoil_x *= ease;
    o->recoil_z *= ease;

    player_t* gunner = mount_occupant(server, o->id, 0);
    if (gunner == NULL) {
        /* unmanned, the barrel sinks level */
        float step = 1.0f * dt;
        o->euler.x += o->euler.x > step ? -step : (o->euler.x < -step ? step : -o->euler.x);
        return;
    }
    vector3f_t look = gunner->movement.forward_orientation;
    p10_vec3   want = p10_euler_from_front(look.x, look.y, look.z);
    float      off  = _mg_wrap(want.z + o->recoil_z - o->home_yaw);
    float      arc  = GAME_MG_YAW_ARC * MG_DEG;
    off             = off > arc ? arc : (off < -arc ? -arc : off);
    float pitch     = want.x + o->recoil_x;
    float up = GAME_MG_PITCH_UP * MG_DEG, down = GAME_MG_PITCH_DOWN * MG_DEG;
    pitch      = pitch < -up ? -up : (pitch > down ? down : pitch);
    o->euler.z = _mg_wrap(o->home_yaw + off);
    o->euler.x = pitch;
    o->euler.y = 0.0f;
    if (gunner->primary_fire && now >= o->next_shot) {
        _mg_fire(server, o, gunner, now);
    }
}

p10_object_t* mg_place(server_t* server, player_t* owner, int x, int y, int z, const char** why)
{
    if (!perks_enabled(server, P10_PERK_MG)) {
        *why = "mode";
    } else if (owner->state != STATE_READY || !owner->alive || owner->team == TEAM_SPECTATOR) {
        *why = "not_alive";
    } else if (proto10_objects_owned(owner->id, P10_ENT_MG) != NULL) {
        *why = "already_have_one";
    } else if (sentry_cell_ok(server, owner, x, y, z, why)) {
        /* the sentry's cell rules; its reach is the MG's too (both 5) */
        *why = "ok";
    }
    if (strcmp(*why, "ok") != 0) {
        evt_log(owner, "mg", "action=refused why=%s cell=%d,%d,%d", *why, x, y, z);
        return NULL;
    }
    vector3f_t    base = {x + 0.5f, y + 0.5f, (float) z};
    p10_object_t* o =
    proto10_objects_spawn(server, P10_ENT_MG, owner, base, GAME_MG_HEALTH, GAME_MG_HALF_WIDTH, GAME_MG_HEIGHT);
    if (o == NULL) {
        *why = "too_many_objects";
        evt_log(owner, "mg", "action=refused why=%s cell=%d,%d,%d", *why, x, y, z);
        return NULL;
    }
    p10_vec3 e = p10_euler_from_front(owner->movement.forward_orientation.x, owner->movement.forward_orientation.y, 0.0f);
    o->euler.z             = e.z;
    o->home_yaw            = e.z;
    o->max_health          = GAME_MG_HEALTH;
    o->keep_on_team_change = 1;
    o->think               = _mg_think;
    p10_vec3 home          = p10_front_from_euler(o->euler);
    evt_log(owner, "mg", "action=place entity=%u cell=%d,%d,%d health=%d front=%.2f,%.2f", o->id, x, y, z, o->health,
            home.x, home.y);
    return o;
}
