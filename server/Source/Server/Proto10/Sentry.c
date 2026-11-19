// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Packets/Packets.h>
#include <Server/Perks.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Sentry.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <uthash/uthash.h>
#include <math.h>
#include <proto10/orient.h>
#include <stdio.h>
#include <stdlib.h>
#include <tomlc99/toml.h>

#define SENTRY_RANGE         48.0f
#define SENTRY_MUZZLE        0.95f /* above the ground */
#define SENTRY_FIRE_INTERVAL (NANO_IN_SECOND / 2)
#define SENTRY_TURN_RATE     3.0f  /* rad/s */
#define SENTRY_SCAN_RATE     0.8f  /* rad/s, the slow turn of a sentry with nothing to shoot */
/* a sentry covers 180 degrees ahead of it (TF2's sentry does not turn round
   on its own): an enemy behind it is safe, so where it is put matters */
#define SENTRY_ARC_HALF      ((float) M_PI * 0.5f)
#define SENTRY_AIM_TOLERANCE 0.12f /* rad: fires once the barrel is this close */
#define SENTRY_SPREAD        0.04f /* rad at short range, grows with distance */
#define SENTRY_DAMAGE_BODY   20
#define SENTRY_DAMAGE_HEAD   40
/* the same respawn time every 0.75 hit path passes */
#define SENTRY_RESPAWN_TIME 5

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float _sentry_wrap(float a)
{
    while (a > (float) M_PI) {
        a -= 2.0f * (float) M_PI;
    }
    while (a < -(float) M_PI) {
        a += 2.0f * (float) M_PI;
    }
    return a;
}

static vector3f_t _sentry_muzzle(const p10_object_t* o)
{
    vector3f_t m = {o->base.x, o->base.y, o->base.z - SENTRY_MUZZLE};
    return m;
}

/* the player's collision box (move_player): x/y +-0.45, from 0.45 above the
   position to its feet, 2.25 below standing or 1.35 crouching */
static float _sentry_player_bottom(const player_t* p)
{
    return p->movement.position.z + (p->crouching ? 1.35f : 2.25f);
}

static int _sentry_ray_player(const player_t* t, vector3f_t a, vector3f_t b, float* t_out)
{
    p10_object_t box;
    memset(&box, 0, sizeof box);
    box.base.x     = t->movement.position.x;
    box.base.y     = t->movement.position.y;
    box.base.z     = _sentry_player_bottom(t);
    box.half_width = 0.45f;
    box.height     = box.base.z - (t->movement.position.z - 0.45f);
    return proto10_objects_segment_hits(&box, a, b, t_out);
}

/* the point to aim at on a target: the chest, or the head when only the head
   shows over a wall; 0 when neither is visible from the muzzle */
static int _sentry_aim_point(server_t* server, const p10_object_t* o, const player_t* t, vector3f_t* out)
{
    vector3f_t m = _sentry_muzzle(o);
    float      dx = t->movement.position.x - m.x, dy = t->movement.position.y - m.y;
    float      dz = t->movement.position.z - m.z;
    if (dx * dx + dy * dy + dz * dz > SENTRY_RANGE * SENTRY_RANGE) {
        return 0;
    }
    float zs[2] = {t->movement.position.z + 0.6f, t->movement.position.z - 0.15f};
    for (int i = 0; i < 2; i++) {
        if (physics_can_see(server, m.x, m.y, m.z, t->movement.position.x, t->movement.position.y, zs[i])) {
            out->x = t->movement.position.x;
            out->y = t->movement.position.y;
            out->z = zs[i];
            return 1;
        }
    }
    return 0;
}

/* inside the half circle in front of the sentry, seen from its column */
static int _sentry_in_arc(const p10_object_t* o, const player_t* p)
{
    float dx = p->movement.position.x - o->base.x, dy = p->movement.position.y - o->base.y;
    if (dx * dx + dy * dy < 0.0001f) {
        return 1;
    }
    p10_vec3 e = p10_euler_from_front(dx, dy, 0.0f);
    return fabsf(_sentry_wrap(e.z - o->home_yaw)) <= SENTRY_ARC_HALF;
}

static int _sentry_valid_target(const p10_object_t* o, const player_t* p)
{
    return p->state == STATE_READY && p->alive && p->team != TEAM_SPECTATOR && p->team != o->team &&
        !p->is_invisible && _sentry_in_arc(o, p);
}

static player_t* _sentry_pick_target(server_t* server, p10_object_t* o, vector3f_t* aim)
{
    /* keep the current target while it stays visible: switching every tick
       between two enemies would never let the barrel settle */
    if (o->has_target) {
        player_t* cur;
        uint8_t   id = o->target_id;
        HASH_FIND(hh, server->players, &id, sizeof(id), cur);
        if (cur != NULL && _sentry_valid_target(o, cur) && _sentry_aim_point(server, o, cur, aim)) {
            return cur;
        }
    }
    player_t * best = NULL, *p, *tmp;
    float      best_d2 = SENTRY_RANGE * SENTRY_RANGE + 1.0f;
    vector3f_t m       = _sentry_muzzle(o);
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_sentry_valid_target(o, p)) {
            continue;
        }
        float dx = p->movement.position.x - m.x, dy = p->movement.position.y - m.y, dz = p->movement.position.z - m.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        vector3f_t a;
        if (d2 < best_d2 && _sentry_aim_point(server, o, p, &a)) {
            best    = p;
            best_d2 = d2;
            *aim    = a;
        }
    }
    return best;
}

static void _sentry_fire(server_t* server, p10_object_t* o, player_t* owner, player_t* target, uint64_t now)
{
    vector3f_t m    = _sentry_muzzle(o);
    p10_vec3   f    = p10_front_from_euler(o->euler);
    float      dx   = target->movement.position.x - m.x, dy = target->movement.position.y - m.y;
    float      dist = sqrtf(dx * dx + dy * dy);
    /* honest aim error: the shot really travels off the barrel's line, and
       only what that ray meets is hit */
    float spread = SENTRY_SPREAD * (1.0f + dist / 40.0f);
    f.x += (float) (gen_rand(&server->rand) - 0.5) * spread;
    f.y += (float) (gen_rand(&server->rand) - 0.5) * spread;
    f.z += (float) (gen_rand(&server->rand) - 0.5) * spread;
    float len = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
    if (len < 1e-6f) {
        return;
    }
    f.x /= len;
    f.y /= len;
    f.z /= len;
    vector3f_t end = {m.x + f.x * SENTRY_RANGE, m.y + f.y * SENTRY_RANGE, m.z + f.z * SENTRY_RANGE};
    float      t;
    int        hit = 0, head = 0;
    if (_sentry_ray_player(target, m, end, &t)) {
        /* stop just short of the box, or the test ends inside the target's
           own cell; walls in between block the shot */
        float      tt = t - 0.02f > 0.0f ? t - 0.02f : 0.0f;
        vector3f_t p  = {m.x + (end.x - m.x) * tt, m.y + (end.y - m.y) * tt, m.z + (end.z - m.z) * tt};
        hit           = physics_can_see(server, m.x, m.y, m.z, p.x, p.y, p.z) != 0;
        head          = hit && p.z < target->movement.position.z + 0.15f;
    }
    char tname[EVT_ACTOR_LEN];
    evt_actor(tname, sizeof tname, target);
    evt_log(owner,
            "sentry",
            "action=fire entity=%u target=%s hit=%d head=%d dist=%.1f",
            o->id,
            tname,
            hit,
            head,
            dist);

    /* the client draws the tracer along the rotation it is sent with the
       Fire event, so the barrel shows the real (erroneous) shot direction */
    o->euler             = p10_euler_from_front(f.x, f.y, f.z);
    o->force_trajectory  = 1;
    o->has_pending_event = 1;
    o->pending_event     = P10_EV_FIRE;
    o->pending_param     = (uint64_t) hit;
    o->next_shot         = now + SENTRY_FIRE_INTERVAL;

    if (hit) {
        /* the owner is the attacker: the kill, the score and the kill feed
           are theirs; the bridge reads the damage source so the victim's hit
           indicator points at the sentry */
        proto10_objects_set_damage_source(o);
        send_set_hp(server,
                    owner,
                    target,
                    head ? SENTRY_DAMAGE_HEAD : SENTRY_DAMAGE_BODY,
                    1,
                    KILL_REASON_SENTRY,
                    SENTRY_RESPAWN_TIME,
                    0,
                    m);
        proto10_objects_set_damage_source(NULL);
    }
}

/* a number in the [sentry] table; integers are accepted too */
static float _sentry_number(toml_table_t* table, const char* key, float fallback)
{
    toml_datum_t d = toml_double_in(table, key);
    if (d.ok) {
        return (float) d.u.d;
    }
    d = toml_int_in(table, key);
    if (d.ok) {
        return (float) d.u.i;
    }
    return fallback;
}

void sentry_init(server_t* server)
{
    float lifetime = SENTRY_LIFETIME_DEFAULT;

    /* like the jetpack, this subsystem owns its table and re-reads the file;
       the table is optional and the key has a default */
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = conf != NULL ? toml_table_in(conf, "sentry") : NULL;
        if (t != NULL) {
            lifetime = _sentry_number(t, "lifetime_seconds", lifetime);
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }
    const char* env = getenv("SP_SENTRY_LIFETIME");
    if (env != NULL && *env != 0) {
        float v = (float) atof(env);
        if (v >= 0.0f) {
            lifetime = v;
        }
    }
    if (lifetime < 0.0f) {
        lifetime = 0.0f;
    }
    server->sentry_lifetime = lifetime;
    if (lifetime > 0.0f) {
        LOG_INFO("Sentry perk: wears out after %.0f s", lifetime);
    } else {
        LOG_INFO("Sentry perk: lasts forever (lifetime_seconds = 0)");
    }
}

void sentry_put_world_params(server_t* server, p10_writer* w)
{
    char v[32];
    snprintf(v, sizeof v, "%g", server->sentry_lifetime);
    p10_put_map_entry(w, "sentry-lifetime", v);
}

/* no target: the head sweeps its half circle from side to side (~4 s a
   way) and levels out, so a sentry visibly watches instead of freezing. The
   aim change goes to clients like any other (Objects.c), and a target found
   mid-sweep is tracked from wherever the head is */
static void _sentry_scan(p10_object_t* o, float dt)
{
    float level = SENTRY_TURN_RATE * dt;
    float off   = _sentry_wrap(o->euler.z - o->home_yaw);
    if (off >= SENTRY_ARC_HALF) {
        o->scan_dir = -1;
    } else if (off <= -SENTRY_ARC_HALF) {
        o->scan_dir = 1;
    }
    float way = o->scan_dir < 0 ? -1.0f : 1.0f;
    off += way * SENTRY_SCAN_RATE * dt;
    off = off > SENTRY_ARC_HALF ? SENTRY_ARC_HALF : (off < -SENTRY_ARC_HALF ? -SENTRY_ARC_HALF : off);
    o->euler.z  = _sentry_wrap(o->home_yaw + off);
    o->euler.x += o->euler.x > level ? -level : (o->euler.x < -level ? level : -o->euler.x);
    o->euler.y  = 0.0f;
}

static void _sentry_think(server_t* server, p10_object_t* o, uint64_t now)
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

    /* [sentry] lifetime_seconds: an untouched sentry wears out and destroys
       itself. Health only ever goes down here, so the bar drains visibly and
       shooting it just makes it die faster. Deliberately before the owner
       check: it still expires while its owner waits to respawn. */
    if (server->sentry_lifetime > 0.0f) {
        float elapsed = (float) (now - o->created) / (float) NANO_IN_SECOND;
        int   left    = GAME_SENTRY_HEALTH - (int) (elapsed / server->sentry_lifetime * (float) GAME_SENTRY_HEALTH);
        if (left < o->health) {
            o->health = left;
        }
        if (o->health <= 0) {
            /* the destroy log carries why=expired; the client shows it explode */
            proto10_objects_destroy(server, o, 1, "expired");
            return;
        }
    }

    player_t* owner = proto10_objects_owner(server, o);
    if (owner == NULL) {
        return;
    }
    /* send_set_hp refuses damage from a dead attacker; rather than a special
       case there, the sentry rests while its owner waits to respawn */
    if (!owner->alive || owner->state != STATE_READY) {
        if (!o->idle_logged) {
            evt_log(owner, "sentry", "action=idle entity=%u why=owner_dead", o->id);
            o->idle_logged = 1;
        }
        o->has_target = 0;
        _sentry_scan(o, dt);
        return;
    }
    o->idle_logged = 0;

    vector3f_t aim;
    player_t*  target = _sentry_pick_target(server, o, &aim);
    if (target == NULL) {
        if (o->has_target) {
            evt_log(owner, "sentry", "action=lost_target entity=%u", o->id);
        }
        o->has_target = 0;
        _sentry_scan(o, dt);
        return;
    }
    if (!o->has_target || o->target_id != target->id) {
        char tname[EVT_ACTOR_LEN];
        evt_actor(tname, sizeof tname, target);
        /* off: degrees from the sentry's front, never past 90 (_sentry_in_arc) */
        p10_vec3 bearing = p10_euler_from_front(target->movement.position.x - o->base.x,
                                                target->movement.position.y - o->base.y, 0.0f);
        evt_log(owner, "sentry", "action=target entity=%u target=%s off=%.0f", o->id, tname,
                _sentry_wrap(bearing.z - o->home_yaw) * 180.0f / (float) M_PI);
    }
    if (!o->has_target && now - o->spot_at >= 2 * NANO_IN_SECOND) {
        /* a new spot, not a switch between targets: the clients play the
           warning beep; a flickering target would beep without end otherwise */
        o->spot_at = now;
        char    idbuf[16];
        const char* parts[2] = {"sentry-spot", idbuf};
        snprintf(idbuf, sizeof idbuf, "%u", o->id);
        proto10_bridge_generic(server, NULL, 2, parts);
        evt_log(owner, "sentry", "action=spot entity=%u", o->id);
    }
    o->has_target = 1;
    o->target_id  = target->id;

    vector3f_t m    = _sentry_muzzle(o);
    p10_vec3   want = p10_euler_from_front(aim.x - m.x, aim.y - m.y, aim.z - m.z);
    float      step = SENTRY_TURN_RATE * dt;
    float      dyaw = _sentry_wrap(want.z - o->euler.z);
    float      dpit = want.x - o->euler.x;
    o->euler.z      = _sentry_wrap(o->euler.z + (dyaw > step ? step : (dyaw < -step ? -step : dyaw)));
    o->euler.x += dpit > step ? step : (dpit < -step ? -step : dpit);
    o->euler.y = 0.0f;

    if (fabsf(_sentry_wrap(want.z - o->euler.z)) < SENTRY_AIM_TOLERANCE &&
        fabsf(want.x - o->euler.x) < SENTRY_AIM_TOLERANCE && now >= o->next_shot)
    {
        _sentry_fire(server, o, owner, target, now);
    }
}

int sentry_cell_ok(server_t* server, player_t* player, int x, int y, int z, const char** why)
{
    map_store_t* map = &server->s_map.map;
    if (x < 0 || y < 0 || z < 2 || x >= map->size_x || y >= map->size_y || z >= map->size_z) {
        *why = "out_of_map";
        return 0;
    }
    if (z >= map->size_z - 1) {
        *why = "water";
        return 0;
    }
    if (!map_solid(map, x, y, z)) {
        *why = "no_ground";
        return 0;
    }
    if (map_solid(map, x, y, z - 1) || map_solid(map, x, y, z - 2)) {
        *why = "no_room";
        return 0;
    }
    vector3f_t eye = player->movement.eye_pos;
    float      cx = x + 0.5f, cy = y + 0.5f, cz = (float) z;
    float      dx = cx - eye.x, dy = cy - eye.y, dz = cz - eye.z;
    if (dx * dx + dy * dy + dz * dz > GAME_SENTRY_PLACE_RANGE * GAME_SENTRY_PLACE_RANGE) {
        *why = "too_far";
        return 0;
    }
    if (!physics_can_see(server, eye.x, eye.y, eye.z, cx, cy, cz - 0.5f)) {
        *why = "not_visible";
        return 0;
    }
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = proto10_objects_at(i);
        if (o != NULL && (int) floorf(o->base.x) == x && (int) floorf(o->base.y) == y &&
            fabsf(o->base.z - cz) < 2.5f)
        {
            *why = "occupied";
            return 0;
        }
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_READY || !p->alive || p->team == TEAM_SPECTATOR) {
            continue;
        }
        /* a player standing in the cells the sentry would fill */
        if (fabsf(p->movement.position.x - cx) < 0.95f && fabsf(p->movement.position.y - cy) < 0.95f &&
            _sentry_player_bottom(p) > cz - GAME_SENTRY_HEIGHT - 0.1f && p->movement.position.z - 0.45f < cz)
        {
            *why = "player_in_the_way";
            return 0;
        }
    }
    *why = "ok";
    return 1;
}

p10_object_t* sentry_place(server_t* server, player_t* owner, int x, int y, int z, const char** why)
{
    /* a perk: refused in a mode without it, whoever asks */
    if (!perks_enabled(server, P10_PERK_SENTRY)) {
        *why = "mode";
        return NULL;
    }
    if (owner->state != STATE_READY || !owner->alive || owner->team == TEAM_SPECTATOR) {
        *why = "not_alive";
        return NULL;
    }
    if (proto10_objects_owned(owner->id, P10_ENT_SENTRY) != NULL) {
        *why = "already_have_one";
        return NULL;
    }
    if (!sentry_cell_ok(server, owner, x, y, z, why)) {
        return NULL;
    }
    vector3f_t    base = {x + 0.5f, y + 0.5f, (float) z};
    p10_object_t* o    = proto10_objects_spawn(
    server, P10_ENT_SENTRY, owner, base, GAME_SENTRY_HEALTH, GAME_SENTRY_HALF_WIDTH, GAME_SENTRY_HEIGHT);
    if (o == NULL) {
        *why = "too_many_objects";
        return NULL;
    }
    /* start facing where the owner looks */
    p10_vec3 e = p10_euler_from_front(
    owner->movement.forward_orientation.x, owner->movement.forward_orientation.y, 0.0f);
    o->euler.z  = e.z;
    o->home_yaw = e.z;
    o->scan_dir = 1;
    o->think    = _sentry_think;
    p10_vec3 home = p10_front_from_euler(o->euler);
    evt_log(owner, "sentry", "action=place entity=%u cell=%d,%d,%d health=%d front=%.2f,%.2f", o->id, x, y, z,
            o->health, home.x, home.y);
    *why = "ok";
    return o;
}
