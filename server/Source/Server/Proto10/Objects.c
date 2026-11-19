// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <classes/classes.h>
#include <Server/Proto10/NetStats.h>
#include <Server/EventLog.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Session.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <uthash/uthash.h>
#include <Util/Weapon.h>
#include <enet/enet.h>
#include <math.h>
#include <proto10/orient.h>
#include <string.h>

/* ids wrap inside this range and skip the ones still in use */
#define OBJ_ID_SPAN (1u << 20)
/* a turning sentry changes its rotation every tick; 20 Hz is smooth enough
   and keeps a sentry at a few hundred bytes per second */
#define OBJ_TRAJECTORY_INTERVAL (NANO_IN_SECOND / 20)

static p10_object_t        objects[P10_MAX_OBJECTS];
static uint32_t            next_object_id = P10_FIRST_OBJECT_ID;
static const p10_object_t* damage_source;
static uint8_t             obj_buf[16384];

/* per player rate limit of hits on objects (pellets of one shotgun shot
   arrive as several HitEntity packets) */
static uint64_t obj_hit_window_start[256];
static uint8_t  obj_hit_count[256];

static const char* _obj_type_name(uint8_t type)
{
    switch (type) {
        case P10_ENT_SENTRY:
            return "sentry";
        case P10_ENT_LANDMINE:
            return "landmine";
        case P10_ENT_DYNAMITE:
            return "dynamite";
        case P10_ENT_MG:
            return "mg";
        case P10_ENT_HELICOPTER:
            return "heli";
        default:
            return "object";
    }
}

static uint8_t _obj_team_to_p10(uint8_t team)
{
    return team == TEAM_A ? P10_TEAM_1 : (team == TEAM_B ? P10_TEAM_2 : P10_TEAM_SPECTATOR);
}

static int _obj_peer_in_game(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME;
}

static void _obj_broadcast(server_t* server, const p10_writer* w)
{
    if (w->error) {
        LOG_ERROR("proto10: object packet did not fit its buffer");
        return;
    }
    player_t *  p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_obj_peer_in_game(p)) {
            continue;
        }
        conn_send_reliable(p->conn, w->data, w->size);
    }
}

static void _obj_full_item(const p10_object_t* o, p10_entity_item* it)
{
    memset(it, 0, sizeof *it);
    it->entity_id           = o->id;
    it->mask                = P10_EU_CREATE | P10_EU_FLAGS | P10_EU_TRAJECTORY | P10_EU_HEALTH;
    it->entity_type         = o->type;
    it->owner               = o->no_owner ? P10_NO_OWNER : o->owner_id;
    it->team                = _obj_team_to_p10(o->team);
    it->flags               = P10_EF_WEAPON_CLIP; /* bullets hit it, players walk through (not simulated) */
    it->trajectory.type     = P10_TRAJ_CONSTANT;
    it->trajectory.origin.x = o->base.x;
    it->trajectory.origin.y = o->base.y;
    it->trajectory.origin.z = o->base.z;
    it->trajectory.rotation = o->has_rotation ? o->rotation : p10_rotation_from_euler(o->euler);
    if (o->linear) {
        it->trajectory.type       = P10_TRAJ_LINEAR;
        it->trajectory.velocity.x = o->vel.x;
        it->trajectory.velocity.y = o->vel.y;
        it->trajectory.velocity.z = o->vel.z;
    }
    int h = o->health < 0 ? 0 : o->health;
    if (o->max_health > 0) {
        /* a percent, never 0 while it has any health left: 0 is the wreck */
        h = (h * 100 + o->max_health - 1) / o->max_health;
        h = h > 100 ? 100 : h;
    }
    it->health = (uint8_t) (h > 255 ? 255 : h);
}

static int _obj_vec_eq(p10_vec3 a, p10_vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

p10_object_t* proto10_objects_find(uint32_t id)
{
    if (id == 0) {
        return NULL;
    }
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        if (objects[i].id == id) {
            return &objects[i];
        }
    }
    return NULL;
}

p10_object_t* proto10_objects_at(int index)
{
    if (index < 0 || index >= P10_MAX_OBJECTS || objects[index].id == 0) {
        return NULL;
    }
    return &objects[index];
}

p10_object_t* proto10_objects_owned(uint8_t owner_id, uint8_t type)
{
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        if (objects[i].id != 0 && !objects[i].no_owner && objects[i].type == type && objects[i].owner_id == owner_id) {
            return &objects[i];
        }
    }
    return NULL;
}

player_t* proto10_objects_owner(server_t* server, const p10_object_t* obj)
{
    if (obj->no_owner) {
        return NULL;
    }
    player_t* p;
    uint8_t   id = obj->owner_id;
    HASH_FIND(hh, server->players, &id, sizeof(id), p);
    return p;
}

static uint32_t _obj_alloc_id(void)
{
    for (uint32_t tries = 0; tries < OBJ_ID_SPAN; tries++) {
        uint32_t id    = next_object_id;
        next_object_id = P10_FIRST_OBJECT_ID + (next_object_id + 1 - P10_FIRST_OBJECT_ID) % OBJ_ID_SPAN;
        if (proto10_objects_find(id) == NULL) {
            return id;
        }
    }
    return 0;
}

p10_object_t* proto10_objects_spawn(server_t*  server,
                                    uint8_t    type,
                                    player_t*  owner,
                                    vector3f_t base,
                                    int        health,
                                    float      half_width,
                                    float      height)
{
    (void) server;
    p10_object_t* o = NULL;
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        if (objects[i].id == 0) {
            o = &objects[i];
            break;
        }
    }
    if (o == NULL) {
        return NULL;
    }
    memset(o, 0, sizeof *o);
    o->id = _obj_alloc_id();
    if (o->id == 0) {
        return NULL;
    }
    o->type       = type;
    o->owner_id   = owner->id;
    o->team       = owner->team;
    o->health     = health;
    o->base       = base;
    o->half_width = half_width;
    o->height     = height;
    o->created    = get_nanos();
    return o;
}

p10_object_t* proto10_objects_spawn_team(server_t*  server,
                                         uint8_t    type,
                                         uint8_t    team,
                                         vector3f_t base,
                                         int        health,
                                         float      half_width,
                                         float      height)
{
    player_t nobody;
    memset(&nobody, 0, sizeof nobody);
    nobody.team     = team;
    p10_object_t* o = proto10_objects_spawn(server, type, &nobody, base, health, half_width, height);
    if (o != NULL) {
        o->no_owner = 1;
    }
    return o;
}

void proto10_objects_map_start(server_t* server)
{
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        if (objects[i].id != 0) {
            proto10_objects_destroy(server, &objects[i], 0, "map");
        }
    }
}

void proto10_objects_send_to(player_t* player, const p10_object_t* obj)
{
    if (obj == NULL || obj->id == 0 || !obj->announced || !_obj_peer_in_game(player)) {
        return;
    }
    p10_entity_item it;
    _obj_full_item(obj, &it);
    it.mask = P10_EU_TRAJECTORY | P10_EU_HEALTH;
    uint8_t    buf[128];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_entity_update(&w, 0);
    p10_put_entity_item(&w, &it);
    if (!w.error) {
        conn_send_reliable(player->conn, w.data, w.size);
    }
}

static void _obj_send_event(server_t* server, const p10_object_t* o, uint8_t type, uint64_t param)
{
    uint8_t          buf[32];
    p10_writer       w;
    p10_entity_event ev = {o->id, type, param};
    p10_writer_init(&w, buf, sizeof buf);
    p10_encode_entity_event(&w, &ev);
    _obj_broadcast(server, &w);
}

void proto10_objects_destroy(server_t* server, p10_object_t* obj, int explode, const char* why)
{
    if (obj == NULL || obj->id == 0) {
        return;
    }
    player_t* owner = proto10_objects_owner(server, obj);
    evt_log(owner,
            _obj_type_name(obj->type),
            "action=%s entity=%u why=%s health=%d cell=%d,%d,%d",
            explode ? "explode" : "remove",
            obj->id,
            why,
            obj->health,
            (int) floorf(obj->base.x),
            (int) floorf(obj->base.y),
            (int) floorf(obj->base.z));
    if (obj->announced) {
        if (explode) {
            _obj_send_event(server, obj, P10_EV_EXPLODE, 0);
        }
        uint8_t           buf[16];
        p10_writer        w;
        p10_entity_remove rm = {obj->id};
        p10_writer_init(&w, buf, sizeof buf);
        p10_encode_entity_remove(&w, &rm);
        _obj_broadcast(server, &w);
    }
    if (damage_source == obj) {
        damage_source = NULL;
    }
    memset(obj, 0, sizeof *obj);
}

/* a player changed class: the perks the class places go with it, so a
   Marksman's landmine or a Miner's dynamite is not left behind by the class
   the player is leaving (the sentry stays: it is a place of its own and it
   wears out by itself) */
void proto10_objects_owner_class_changed(server_t* server, player_t* player)
{
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = &objects[i];
        if (o->id == 0 || o->owner_id != player->id) {
            continue;
        }
        if (o->type == P10_ENT_LANDMINE || o->type == P10_ENT_DYNAMITE) {
            proto10_objects_destroy(server, o, 0, "owner_class");
        }
    }
}

/* the owner left, went to the other team or to the spectators */
static const char* _obj_owner_problem(server_t* server, const p10_object_t* o)
{
    if (o->no_owner) {
        return NULL;
    }
    player_t* owner = proto10_objects_owner(server, o);
    if (owner == NULL || owner->state == STATE_DISCONNECTED) {
        return "owner_left";
    }
    /* the MG stays where it was put whatever its owner does (the user's
       rule): only its owner leaving the server takes it */
    if (owner->team != o->team && !o->keep_on_team_change) {
        return "owner_changed_team";
    }
    return NULL;
}

void proto10_objects_tick(server_t* server, int anyone_listening)
{
    uint64_t now = get_nanos();
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = &objects[i];
        if (o->id == 0) {
            continue;
        }
        const char* problem = _obj_owner_problem(server, o);
        if (problem != NULL) {
            proto10_objects_destroy(server, o, 0, problem);
            continue;
        }
        if (o->think != NULL) {
            o->think(server, o, now);
        }
    }

    if (!anyone_listening) {
        /* same rule as the players' delta state: it advances with nobody to
           send to, and a joiner starts from its GameStateFinal */
        for (int i = 0; i < P10_MAX_OBJECTS; i++) {
            objects[i].announced         = 0;
            objects[i].has_pending_event = 0;
        }
        return;
    }

    p10_writer w;
    int        items = 0;
    p10_writer_init(&w, obj_buf, sizeof obj_buf);
    p10_start_entity_update(&w, 0);
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = &objects[i];
        if (o->id == 0) {
            continue;
        }
        p10_entity_item cur;
        _obj_full_item(o, &cur);
        if (!o->announced) {
            p10_put_entity_item(&w, &cur);
            items++;
            o->announced          = 1;
            o->last               = cur;
            o->trajectory_sent_at = now;
            o->force_trajectory   = 0;
            continue;
        }
        p10_entity_item d = cur;
        d.mask            = 0;
        if (cur.flags != o->last.flags) {
            d.mask |= P10_EU_FLAGS;
        }
        if (cur.health != o->last.health) {
            d.mask |= P10_EU_HEALTH;
        }
        if ((!_obj_vec_eq(cur.trajectory.origin, o->last.trajectory.origin) ||
             !_obj_vec_eq(cur.trajectory.rotation, o->last.trajectory.rotation) ||
             !_obj_vec_eq(cur.trajectory.velocity, o->last.trajectory.velocity)) &&
            (o->force_trajectory || now - o->trajectory_sent_at >= OBJ_TRAJECTORY_INTERVAL))
        {
            d.mask |= P10_EU_TRAJECTORY;
            o->trajectory_sent_at = now;
            o->force_trajectory   = 0;
        }
        if (d.mask) {
            p10_put_entity_item(&w, &d);
            items++;
            if (!(d.mask & P10_EU_TRAJECTORY)) {
                cur.trajectory = o->last.trajectory; /* still owed, compare again next tick */
            }
            o->last = cur;
        }
    }
    if (items) {
        _obj_broadcast(server, &w);
    }
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = &objects[i];
        if (o->id != 0 && o->has_pending_event) {
            _obj_send_event(server, o, o->pending_event, o->pending_param);
            o->has_pending_event = 0;
        }
    }
}

uint32_t proto10_objects_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        n += objects[i].id != 0;
    }
    return n;
}

void proto10_objects_put(p10_writer* w)
{
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        if (objects[i].id != 0) {
            p10_entity_item it;
            _obj_full_item(&objects[i], &it);
            p10_put_entity_item(w, &it);
        }
    }
}

void proto10_objects_damage(server_t* server, p10_object_t* obj, player_t* attacker, int amount, const char* cause)
{
    if (obj == NULL || obj->id == 0 || amount <= 0 || obj->detonate) {
        return;
    }
    /* your own side's sentry is not shot down by a stray bullet (it is your
       defence, and its blast would hit the line it guards); a landmine or a
       dynamite is cleared by any bullet, its own team's included: a trap you
       cannot shoot away would be a hole with no answer */
    if (attacker != NULL && attacker->team == obj->team &&
        (obj->type == P10_ENT_SENTRY || obj->type == P10_ENT_MG || obj->type == P10_ENT_HELICOPTER))
    {
        evt_log(attacker, "entity_hit", "refused why=friendly entity=%u type=%s cause=%s", obj->id,
                _obj_type_name(obj->type), cause);
        return;
    }
    if (attacker != NULL) {
        obj->last_attacker     = attacker->id;
        obj->has_last_attacker = 1;
    }
    obj->health -= amount;
    if (obj->health < 0) {
        obj->health = 0;
    }
    evt_log(attacker,
            "entity_damage",
            "entity=%u type=%s owner=%u amount=%d health=%d cause=%s",
            obj->id,
            _obj_type_name(obj->type),
            obj->owner_id,
            amount,
            obj->health,
            cause);
    if (obj->type == P10_ENT_HELICOPTER) {
        /* the helicopter's own line: its health is a wreck's clock (12.8) */
        evt_log(attacker, "heli", "action=damage entity=%u amount=%d health=%d cause=%s", obj->id, amount,
                obj->health, cause);
    }
    if (obj->health == 0 && obj->keeps_wreck) {
        return; /* its think falls with it and blows it up on the ground */
    }
    if (obj->health == 0 && obj->blast_on_death) {
        obj->detonate = 1; /* its think blows it up next tick */
    } else if (obj->health == 0) {
        char why[64];
        char a[EVT_ACTOR_LEN];
        evt_actor(a, sizeof a, attacker);
        snprintf(why, sizeof why, "destroyed_by:%s", a);
        proto10_objects_destroy(server, obj, 1, why);
    }
}

/* a point into the object's own frame: the long side along x (the
   helicopter's), the base at the origin; a square box keeps map axes */
static void _obj_local(const p10_object_t* obj, float x, float y, float* lx, float* ly)
{
    x -= obj->base.x;
    y -= obj->base.y;
    if (obj->half_len > 0.0f) {
        float c = cosf(obj->box_yaw), s = sinf(obj->box_yaw);
        *lx = x * c + y * s;
        *ly = -x * s + y * c;
    } else {
        *lx = x;
        *ly = y;
    }
}

int proto10_objects_segment_hits(const p10_object_t* obj, vector3f_t a, vector3f_t b, float* t_out)
{
    float hl    = obj->half_len > 0.0f ? obj->half_len : obj->half_width;
    float lo[3] = {-hl, -obj->half_width, obj->base.z - obj->height};
    float hi[3] = {hl, obj->half_width, obj->base.z};
    float ax, ay, bx, by;
    _obj_local(obj, a.x, a.y, &ax, &ay);
    _obj_local(obj, b.x, b.y, &bx, &by);
    float o[3]  = {ax, ay, a.z};
    float d[3]  = {bx - ax, by - ay, b.z - a.z};
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-9f) {
            if (o[k] < lo[k] || o[k] > hi[k]) {
                return 0;
            }
            continue;
        }
        float ta = (lo[k] - o[k]) / d[k];
        float tb = (hi[k] - o[k]) / d[k];
        if (ta > tb) {
            float s = ta;
            ta      = tb;
            tb      = s;
        }
        if (ta > t0) {
            t0 = ta;
        }
        if (tb < t1) {
            t1 = tb;
        }
        if (t0 > t1) {
            return 0;
        }
    }
    if (t_out != NULL) {
        *t_out = t0;
    }
    return 1;
}

static float _obj_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* the 0.75 torso damage of each weapon (Hit.c), so a sentry is as hard to
   kill with each gun as the shots it takes to kill a player suggest */
static int _obj_weapon_damage(uint8_t weapon)
{
    switch (weapon) {
        case WEAPON_SMG:
            return 29;
        case WEAPON_SHOTGUN:
            return 27;
        case GAME_WEAPON_REVOLVER:
            return GAME_REVOLVER_TORSO;
        case WEAPON_RIFLE:
        default:
            return 49;
    }
}

#define OBJ_MELEE_DAMAGE 50
#define OBJ_MELEE_RANGE  4.0f

void proto10_objects_on_hit(server_t* server, player_t* player, const p10_hit_entity* hit)
{
    p10_object_t* obj = proto10_objects_find(hit->entity_id);
    if (obj == NULL) {
        evt_log(player, "entity_hit", "refused why=no_such_entity entity=%u", hit->entity_id);
        return;
    }
    vector3f_t eye = player->movement.eye_pos;
    float      dx = hit->fire_position.x - eye.x, dy = hit->fire_position.y - eye.y, dz = hit->fire_position.z - eye.z;
    if (dx * dx + dy * dy + dz * dz > 4.0f * 4.0f) {
        evt_log(player, "entity_hit", "refused why=fire_position_far entity=%u", obj->id);
        return;
    }
    /* the point that was hit, kept inside the box (and off the floor block it
       stands on) so the line of sight test is about the object, not about
       whatever a client claims */
    vector3f_t p;
    float      hl = obj->half_len > 0.0f ? obj->half_len : obj->half_width, lx, ly;
    _obj_local(obj, hit->hit_position.x, hit->hit_position.y, &lx, &ly);
    lx = _obj_clampf(lx, -hl + 0.05f, hl - 0.05f);
    ly = _obj_clampf(ly, -obj->half_width + 0.05f, obj->half_width - 0.05f);
    if (obj->half_len > 0.0f) {
        float c = cosf(obj->box_yaw), s = sinf(obj->box_yaw);
        p.x = obj->base.x + lx * c - ly * s;
        p.y = obj->base.y + lx * s + ly * c;
    } else {
        p.x = obj->base.x + lx;
        p.y = obj->base.y + ly;
    }
    p.z = _obj_clampf(hit->hit_position.z, obj->base.z - obj->height + 0.05f, obj->base.z - 0.1f);
    float ex = p.x - hit->hit_position.x, ey = p.y - hit->hit_position.y, ez = p.z - hit->hit_position.z;
    if (ex * ex + ey * ey + ez * ez > 1.5f * 1.5f) {
        evt_log(player, "entity_hit", "refused why=hit_position_far entity=%u", obj->id);
        return;
    }
    float range2 = (p.x - eye.x) * (p.x - eye.x) + (p.y - eye.y) * (p.y - eye.y) + (p.z - eye.z) * (p.z - eye.z);
    if (!physics_can_see(server, eye.x, eye.y, eye.z, p.x, p.y, p.z)) {
        evt_log(player, "entity_hit", "refused why=no_line_of_sight entity=%u", obj->id);
        return;
    }

    uint64_t now   = get_nanos();
    int      melee = hit->damage_type == P10_DMG_MELEE;
    int      damage;
    uint8_t  max_hits;
    uint64_t window;
    if (melee) {
        if (player->item != TOOL_SPADE || range2 > OBJ_MELEE_RANGE * OBJ_MELEE_RANGE) {
            evt_log(player, "entity_hit", "refused why=melee_tool_or_range entity=%u item=%u", obj->id, player->item);
            return;
        }
        damage   = OBJ_MELEE_DAMAGE;
        max_hits = 1;
        window   = NANO_IN_SECOND / 5; /* the client swings every 0.2 s */
    } else {
        if (player->item != TOOL_GUN || player->sprinting) {
            evt_log(player, "entity_hit", "refused why=gun_not_ready entity=%u item=%u", obj->id, player->item);
            return;
        }
        damage   = _obj_weapon_damage(player->weapon);
        max_hits = player->weapon == WEAPON_SHOTGUN ? 8 : 1;
        /* a little slack: packets do not arrive exactly a fire interval apart */
        window = get_player_weapon_delay_nano(player) * 9 / 10;
    }
    uint8_t id = player->id;
    if (now - obj_hit_window_start[id] >= window) {
        obj_hit_window_start[id] = now;
        obj_hit_count[id]        = 0;
    }
    if (obj_hit_count[id] >= max_hits) {
        evt_log(player, "entity_hit", "refused why=too_fast entity=%u", obj->id);
        return;
    }
    obj_hit_count[id]++;
    /* a hit on a landmine or a dynamite sets it off at once, whatever the gun:
       they are traps, not targets (an SMG's 29 against a landmine's 30 left
       it standing for a second shot). Their health still matters to a blast's
       falloff, which does not go through here */
    if (obj->type == P10_ENT_LANDMINE || obj->type == P10_ENT_DYNAMITE) {
        damage = obj->health > 0 ? obj->health : 1;
    }
    proto10_objects_damage(server, obj, player, damage, melee ? "spade" : "gun");
}

void proto10_objects_grenade(server_t* server, player_t* thrower, vector3f_t g)
{
    for (int i = 0; i < P10_MAX_OBJECTS; i++) {
        p10_object_t* o = &objects[i];
        if (o->id == 0) {
            continue;
        }
        /* the same falloff as a grenade on a player (get_grenade_damage) */
        float cx = o->base.x, cy = o->base.y, cz = o->base.z - o->height * 0.5f;
        float dx = fabsf(cx - g.x), dy = fabsf(cy - g.y), dz = fabsf(cz - g.z);
        if (dx >= 16 || dy >= 16 || dz >= 16 || g.z >= 62) {
            continue;
        }
        if (!physics_can_see(server, cx, cy, cz, g.x, g.y, g.z)) {
            continue;
        }
        float d2     = dx * dx + dy * dy + dz * dz;
        int   damage = d2 <= 0.0f ? 100 : (int) fminf(4096.0f / d2, 100.0f);
        if (damage > 0) {
            proto10_objects_damage(server, o, thrower, damage, "grenade");
        }
    }
}

void proto10_objects_set_damage_source(const p10_object_t* obj)
{
    damage_source = obj;
}

const p10_object_t* proto10_objects_damage_source(void)
{
    return damage_source;
}
