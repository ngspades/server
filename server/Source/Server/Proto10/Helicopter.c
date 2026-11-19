// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Packets/Packets.h>
#include <Server/Profile.h>
#include <Server/Proto10/Explosives.h>
#include <Server/Proto10/Helicopter.h>
#include <Server/Proto10/Mount.h>
#include <Server/Proto10/Objects.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <math.h>
#include <proto10/orient.h>
#include <string.h>
#include <vehicles/heli.h>

#define HELI_DEG       (3.14159265f / 180.0f)
#define HELI_EYE_UP    2.3f /* the cabin's eye over the skids' bottom */
/* the fall of a wreck keeps going this long at most, in case it comes to
   rest somewhere the box test cannot see (it is then blown up anyway) */
#define HELI_WRECK_LIFE 15.0f

/* one base a team's helicopter comes back to (GAME_HELI_RESPAWN after a
   wreck). `used` is set on a 1024 map with a site found; `entity` 0 while it
   waits to come back */
typedef struct
{
    uint8_t    used;
    uint8_t    team;
    uint8_t    ever; /* one stood here already: a respawn, for the log */
    uint32_t   entity;
    uint64_t   respawn_at;
    vector3f_t base;
    float      yaw;
} heli_slot_t;

/* two helicopters a team, side by side at its base */
#define HELI_PER_TEAM 2
#define HELI_SLOTS    (2 * HELI_PER_TEAM)
/* the two pads of a base stand at least this far apart (the hull is 11 long) */
#define HELI_PAD_GAP  14.0f

static heli_slot_t heli_slots[HELI_SLOTS];

static float _wrap360(float a)
{
    a = fmodf(a, 360.0f);
    return a < 0.0f ? a + 360.0f : a;
}

/* The wire's rotation: the sentry's Rz(yaw) * Rx(pitch) applied to (0, 1, 0),
   with the roll about the body's own forward axis. heli.h's yaw is 0 at +x
   while the euler angles' is 0 at +y (proto10/orient.h), so the heading is
   turned by a quarter first. */
static p10_vec3 _heli_rotation(const heli_state* s)
{
    float er = (s->yaw - 90.0f) * HELI_DEG;
    float p  = s->tilt_fwd * HELI_DEG;
    float r  = s->tilt_side * HELI_DEG;
    float cz = cosf(er * 0.5f), sz = sinf(er * 0.5f);
    float cx = cosf(p * 0.5f), sx = sinf(p * 0.5f);
    float cy = cosf(r * 0.5f), sy = sinf(r * 0.5f);
    float w1 = cz * cx, x1 = cz * sx, y1 = sz * sx, z1 = sz * cx;
    float w = w1 * cy - y1 * sy;
    float x = x1 * cy - z1 * sy;
    float y = y1 * cy + w1 * sy;
    float z = z1 * cy + x1 * sy;
    return p10_rotation_compact(x, y, z, w);
}

/* 1 when the body's box would cross the map at that place and heading: the
   long side along the nose, sampled down the hull and across the skids, and
   the map's edge is a wall (off the map every cell reads as air) */
static int _heli_blocked(server_t* server, float x, float y, float z, float yaw)
{
    map_store_t* map = &server->s_map.map;
    float        c = cosf(yaw * HELI_DEG), s = sinf(yaw * HELI_DEG);
    /* every cell the box covers is looked at (steps of at most a block, the
       top and the sides included): the old 1 x 1.8 x 2.1 lattice let a
       beam, a pole or a one block wall slip between its samples */
    for (float u = -GAME_HELI_HALF_LENGTH; u <= GAME_HELI_HALF_LENGTH + 0.01f; u += 0.9f) {
        for (int k = -2; k <= 2; k++) {
            float v  = (float) k * 0.5f * GAME_HELI_HALF_WIDTH;
            float px = x + u * c - v * s;
            float py = y + u * s + v * c;
            if (px < 0.0f || py < 0.0f || px >= (float) map->size_x || py >= (float) map->size_y) {
                return 1;
            }
            for (float dz = 0.1f; dz <= GAME_HELI_HEIGHT + 0.01f; dz += 0.9f) {
                if (map_solid(map, (int) floorf(px), (int) floorf(py), (int) floorf(z - dz))) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int heli_boardable(const p10_object_t* heli)
{
    return heli != NULL && heli->type == P10_ENT_HELICOPTER && heli->health > 0 && !heli->heli.wrecked;
}

void heli_seat_eye(const p10_object_t* heli, int seat, vector3f_t* out)
{
    /* the pilot ahead of the mast, the passengers behind it, one each side */
    static const float fwd[GAME_HELI_SEATS]  = {GAME_HELI_MAST_FORWARD + 1.6f, GAME_HELI_MAST_FORWARD - 1.0f,
                                                GAME_HELI_MAST_FORWARD - 1.0f};
    static const float side[GAME_HELI_SEATS] = {0.0f, 0.8f, -0.8f};
    int                k                    = seat >= 0 && seat < GAME_HELI_SEATS ? seat : 0;
    float              r                    = heli->heli.yaw * HELI_DEG;
    float              c = cosf(r), s = sinf(r);
    out->x = heli->heli.x + c * fwd[k] - s * side[k];
    out->y = heli->heli.y + s * fwd[k] + c * side[k];
    out->z = heli->heli.z - HELI_EYE_UP;
}

/* the wreck's blast kills whoever is still aboard: the last enemy who hit it
   is the killer (a crash credits nobody and is the occupant's own death) */
static void _heli_wreck_occupants(server_t* server, const p10_object_t* o)
{
    player_t* attacker = NULL;
    if (o->has_last_attacker) {
        uint8_t id = o->last_attacker;
        HASH_FIND(hh, server->players, &id, sizeof id, attacker);
    }
    for (int seat = 0; seat < GAME_HELI_SEATS; seat++) {
        player_t* p = mount_occupant(server, o->id, seat);
        if (p == NULL) {
            continue;
        }
        mount_dismount(server, p, "wreck");
        if (p->alive) {
            send_kill_action_packet(server, attacker != NULL ? attacker : p, p, KILL_REASON_EXPLOSIVE, 5, 0);
        }
    }
}

/* it is gone: its base gets a new one in GAME_HELI_RESPAWN */
static void _heli_gone(server_t* server, uint32_t entity, const char* why)
{
    (void) server;
    for (int i = 0; i < HELI_SLOTS; i++) {
        heli_slot_t* slot = &heli_slots[i];
        if (!slot->used || slot->entity != entity) {
            continue;
        }
        slot->entity     = 0;
        slot->respawn_at = get_nanos() + (uint64_t) (GAME_HELI_RESPAWN * NANO_IN_SECOND);
        evt_log(NULL, "heli", "action=respawn team=%d pad=%d why=%s in=%.0f", (int) slot->team, i % HELI_PER_TEAM, why,
                GAME_HELI_RESPAWN);
    }
}

static void _heli_think(server_t* server, p10_object_t* o, uint64_t now);

/* the object itself, wherever it is: a base slot or a plugin's spawn */
static p10_object_t* _heli_create(server_t* server, int team, const vector3f_t* base, float yaw)
{
    p10_object_t* o = proto10_objects_spawn_team(server, P10_ENT_HELICOPTER, (uint8_t) team, *base,
                                                 GAME_HELI_HEALTH, GAME_HELI_HALF_WIDTH, GAME_HELI_HEIGHT);
    if (o == NULL) {
        return NULL;
    }
    memset(&o->heli, 0, sizeof o->heli);
    o->heli.x   = base->x;
    o->heli.y   = base->y;
    o->heli.z   = base->z;
    o->heli.yaw = yaw;
    o->max_health   = GAME_HELI_HEALTH;
    o->half_len     = GAME_HELI_HALF_LENGTH;
    o->box_yaw      = yaw * HELI_DEG;
    o->linear       = 1;
    o->has_rotation = 1;
    o->rotation     = _heli_rotation(&o->heli);
    o->keeps_wreck  = 1;
    o->think        = _heli_think;
    o->created      = get_nanos();
    o->last_think   = 0;
    o->vel          = (vector3f_t) {0, 0, 0};
    return o;
}

static p10_object_t* _heli_spawn(server_t* server, int index)
{
    heli_slot_t* slot = &heli_slots[index];
    int          team = slot->team;
    p10_object_t* o   = _heli_create(server, team, &slot->base, slot->yaw);
    if (o == NULL) {
        evt_log(NULL, "heli", "action=spawn_failed team=%d why=too_many_objects", team);
        slot->respawn_at = get_nanos() + (uint64_t) NANO_IN_SECOND;
        return NULL;
    }
    slot->entity    = o->id;
    slot->respawn_at = 0;
    evt_log(NULL,
            "heli",
            "action=%s team=%d entity=%u at=%.1f,%.1f,%.1f yaw=%.0f health=%d",
            slot->ever ? "respawn" : "spawn",
            team,
            o->id,
            o->heli.x,
            o->heli.y,
            o->heli.z,
            o->heli.yaw,
            o->health);
    slot->ever = 1;
    return o;
}

/* a plugin's helicopter, on a spot of its own: no base slot, so nothing
   puts a new one there after a wreck (the plugin asks again) */
p10_object_t* heli_spawn_in_front(server_t* server, player_t* player, const char** why)
{
    *why = NULL;
    if (profile_is_075()) {
        *why = "profile";
        return NULL;
    }
    map_store_t* map = &server->s_map.map;
    float        ox  = player->movement.forward_orientation.x;
    float        oy  = player->movement.forward_orientation.y;
    float        len = sqrtf(ox * ox + oy * oy);
    if (len < 0.01f) {
        /* looking straight up or down: put it where the map's +x goes */
        ox  = 1.0f;
        oy  = 0.0f;
        len = 1.0f;
    }
    ox /= len;
    oy /= len;
    float yaw = _wrap360(atan2f(oy, ox) * 180.0f / 3.14159265f);
    /* in front, a few distances: the first ground with room for the hull */
    static const float ahead[] = {9.0f, 12.0f, 15.0f};
    int                team    = player->team == TEAM_B ? TEAM_B : TEAM_A;
    for (size_t k = 0; k < sizeof ahead / sizeof ahead[0]; k++) {
        float cx = player->movement.position.x + ox * ahead[k];
        float cy = player->movement.position.y + oy * ahead[k];
        for (int r = 0; r <= 6; r += 2) {
            int steps = r == 0 ? 1 : 2 * r;
            for (int s = 0; s < steps; s++) {
                float a  = 2.0f * 3.14159265f * (float) s / (float) steps;
                int   px = (int) floorf(cx + cosf(a) * (float) r);
                int   py = (int) floorf(cy + sinf(a) * (float) r);
                if (px < 1 || py < 1 || px >= map->size_x - 1 || py >= map->size_y - 1) {
                    continue;
                }
                int top = map_top(map, (uint16_t) px, (uint16_t) py);
                if (top < 2 || top >= map->size_z - 2) {
                    continue; /* the sky's edge, or the water */
                }
                vector3f_t base = {(float) px + 0.5f, (float) py + 0.5f, (float) top};
                if (_heli_blocked(server, base.x, base.y, base.z, yaw)) {
                    continue;
                }
                p10_object_t* o = _heli_create(server, team, &base, yaw);
                if (o == NULL) {
                    *why = "too_many_objects";
                    return NULL;
                }
                evt_log(player, "heli",
                        "action=spawn team=%d entity=%u at=%.1f,%.1f,%.1f yaw=%.0f look=%.2f,%.2f health=%d by=plugin",
                        team, o->id, o->heli.x, o->heli.y, o->heli.z, o->heli.yaw, ox, oy, o->health);
                return o;
            }
        }
    }
    *why = "no_ground";
    return NULL;
}

static void _heli_think(server_t* server, p10_object_t* o, uint64_t now)
{
    float dt = o->last_think ? (float) (now - o->last_think) / (float) NANO_IN_SECOND : 0.0f;
    o->last_think = now;
    if (dt <= 0.0f) {
        return;
    }
    if (dt > 0.25f) {
        dt = 0.25f;
    }

    heli_state* s = &o->heli;
    if (!s->wrecked && o->health <= 0) {
        /* down, but not out: it falls and blows up on the ground. A wreck is
           a kind of object no remove takes, see objects_damage's keeps_wreck */
        s->wrecked  = 1;
        o->wreck_at = now;
        evt_log(NULL, "heli", "action=wreck entity=%u team=%u at=%.1f,%.1f,%.1f", o->id, (unsigned) o->team, s->x,
                s->y, s->z);
        _heli_wreck_occupants(server, o);
    }

    heli_input in;
    memset(&in, 0, sizeof in);
    player_t* pilot = s->wrecked ? NULL : mount_occupant(server, o->id, 0);
    if (pilot != NULL) {
        p10_vec3 e;
        in.piloted = 1;
        in.forward = (int8_t) ((pilot->move_forward ? 1 : 0) - (pilot->move_backwards ? 1 : 0));
        in.strafe  = (int8_t) ((pilot->move_right ? 1 : 0) - (pilot->move_left ? 1 : 0));
        in.lift    = (int8_t) mount_lift_input(pilot);
        e          = p10_euler_from_front(pilot->movement.forward_orientation.x,
                                          pilot->movement.forward_orientation.y,
                                          pilot->movement.forward_orientation.z);
        in.yaw     = _wrap360(e.z * 180.0f / 3.14159265f + 90.0f);
    }

    heli_state next = *s;
    heli_step(&next, &in, dt);
    /* a hull already inside something (a wreck's last frame, a block built
       round it) may move out any way: stopping every move there would pin it */
    int inside = _heli_blocked(server, s->x, s->y, s->z, s->yaw);
    int hit    = 0; /* the speed it struck something sideways or overhead at */
    if (!inside) {
        /* the heading turns only where the hull still fits: a tail swung into
           a wall is held, not pushed into it */
        if (_heli_blocked(server, s->x, s->y, s->z, next.yaw)) {
            next.yaw = s->yaw;
        }
        /* the hull against the map, one axis at a time, so a wall stops the
           way it goes and not the whole move; what it struck at it keeps a
           little of, backwards */
        if (_heli_blocked(server, next.x, s->y, s->z, next.yaw)) {
            if (fabsf(next.vx) > (float) hit) {
                hit = (int) fabsf(next.vx);
            }
            next.x  = s->x;
            next.vx = -0.2f * next.vx;
        }
        if (_heli_blocked(server, next.x, next.y, s->z, next.yaw)) {
            if (fabsf(next.vy) > (float) hit) {
                hit = (int) fabsf(next.vy);
            }
            next.y  = s->y;
            next.vy = -0.2f * next.vy;
        }
    }
    int landed = 0;
    if (!inside && _heli_blocked(server, next.x, next.y, next.z, next.yaw)) {
        next.z = s->z;
        if (next.vz > 0.0f) {
            landed = 1;
            int impact = heli_impact(next.vz);
            if (impact > 0) {
                evt_log(NULL, "heli", "action=crash entity=%u at=%.1f,%.1f,%.1f vz=%.1f damage=%d", o->id, next.x,
                        next.y, next.z, next.vz, impact);
                proto10_objects_damage(server, o, NULL, impact, "crash");
                if (pilot == NULL && !s->wrecked && impact > 0 && o->health > 0) {
                    /* nobody at the stick: it came down from the sky and
                       blows up whatever the rotor left of its health */
                    o->health = 0;
                    evt_log(NULL, "heli", "action=abandoned_crash entity=%u vz=%.1f", o->id, next.vz);
                }
            }
        } else if (next.vz < 0.0f) {
            /* a ceiling, a bridge, a beam overhead */
            if ((int) -next.vz > hit) {
                hit = (int) -next.vz;
            }
        }
        next.vz = next.vz > 0.0f ? 0.0f : -0.2f * next.vz;
    }
    if (hit > 0 && heli_impact((float) hit) > 0) {
        int impact = heli_impact((float) hit);
        evt_log(NULL, "heli", "action=crash entity=%u at=%.1f,%.1f,%.1f speed=%d damage=%d why=object", o->id,
                next.x, next.y, next.z, hit, impact);
        proto10_objects_damage(server, o, NULL, impact, "crash");
    }
    if (landed && (s->wrecked || (o->health <= 0 && pilot == NULL))) {
        /* the wreck (or a helicopter dropped from the sky) came down: blow it
           up, killing whoever is still aboard, then its base gets a new one */
        uint32_t id = o->id;
        if (!s->wrecked) {
            _heli_wreck_occupants(server, o);
        }
        proto10_objects_destroy(server, o, 1, "wreck");
        _heli_gone(server, id, "wreck");
        return;
    }
    if (s->wrecked && (float) (now - o->wreck_at) / (float) NANO_IN_SECOND > HELI_WRECK_LIFE) {
        uint32_t id = o->id;
        evt_log(NULL, "heli", "action=crash entity=%u why=lifetime", o->id);
        proto10_objects_destroy(server, o, 1, "wreck");
        _heli_gone(server, id, "wreck");
        return;
    }

    *s              = next;
    o->base         = (vector3f_t) {s->x, s->y, s->z};
    o->vel          = (vector3f_t) {s->vx, s->vy, s->vz};
    o->linear       = 1;
    o->box_yaw      = s->yaw * HELI_DEG;
    o->rotation     = _heli_rotation(s);
    o->has_rotation = 1;
}

/* a column the skids can rest on: its own ground, five free cells over it,
   and the same ground under the hull's footprint (a step under a skid is a
   crash waiting) */
static int _heli_ground(server_t* server, int x, int y, int* top)
{
    map_store_t* m = &server->s_map.map;
    if (x < 1 || y < 1 || x >= m->size_x - 1 || y >= m->size_y - 1) {
        return 0;
    }
    int g = map_top(m, x, y);
    if (g <= 1 || g >= m->size_z - 2) {
        return 0; /* no ground, or the water layer */
    }
    for (int dz = 1; dz <= 5; dz++) {
        if (map_solid(m, x, y, g - dz)) {
            return 0;
        }
    }
    for (int dx = -5; dx <= 5; dx += 2) {
        for (int dy = -1; dy <= 1; dy++) {
            int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= m->size_x || yy >= m->size_y) {
                return 0;
            }
            if (map_top(m, xx, yy) != g) {
                return 0;
            }
        }
    }
    *top = g;
    return 1;
}

/* a site near the team's spawn, facing the other side: the nearest column
   whose ground is flat all over the hull */
static int _heli_site(server_t* server, int team, const vector3f_t* avoid, vector3f_t* out, float* yaw)
{
    quad3d_t* sp = &server->protocol.spawns[team];
    float     cx = (sp->from.x + sp->to.x) * 0.5f, cy = (sp->from.y + sp->to.y) * 0.5f;
    quad3d_t* other = &server->protocol.spawns[team == TEAM_A ? TEAM_B : TEAM_A];
    float     ox    = (other->from.x + other->to.x) * 0.5f - cx;
    float     oy    = (other->from.y + other->to.y) * 0.5f - cy;
    float     face  = _wrap360(atan2f(oy, ox) * 180.0f / 3.14159265f);
    /* the ground the players spawn on: a flat pit under the base is not a
       helipad (Villages_1024's base sits over a 15 block drop, and the
       helicopter was put on its floor, under the players' feet), and a
       roof over it is no better. The spawn rectangle's z is often 0 (only
       x/y are given), so the level is the most common map_top over the
       rectangle: the ground the team stands on, not its pits or roofs */
    int level = 0;
    /* the mode's own base (the tent's ground): the team stands there, so the
       helicopter does too. A mode without one falls back to the most common
       map_top over the spawn rectangle */
    vector3f_t base = server->protocol.gamemode.base[team];
    if (base.z > 1.0f && base.z < (float) server->s_map.map.size_z - 2.0f) {
        level = (int) floorf(base.z);
    } else {
        int hist[64] = {0};
        int x0 = (int) floorf(fminf(sp->from.x, sp->to.x)), x1 = (int) floorf(fmaxf(sp->from.x, sp->to.x));
        int y0 = (int) floorf(fminf(sp->from.y, sp->to.y)), y1 = (int) floorf(fmaxf(sp->from.y, sp->to.y));
        for (int x = x0; x <= x1; x += 2) {
            for (int y = y0; y <= y1; y += 2) {
                int t = map_top(&server->s_map.map, x, y);
                if (t > 1 && t < server->s_map.map.size_z - 2 && t < (int) (sizeof hist / sizeof *hist)) {
                    hist[t]++;
                }
            }
        }
        int best = 0;
        for (int z = 0; z < (int) (sizeof hist / sizeof *hist); z++) {
            if (hist[z] > best) {
                best  = hist[z];
                level = z;
            }
        }
    }
    int check_level = level > 1;
    for (int r = 0; r <= 160; r += 4) {
        int steps = r == 0 ? 1 : 2 * r;
        for (int k = 0; k < steps; k++) {
            float a = 2.0f * 3.14159265f * (float) k / (float) steps;
            int   x = (int) floorf(cx + cosf(a) * (float) r);
            int   y = (int) floorf(cy + sinf(a) * (float) r);
            int   g;
            if (!_heli_ground(server, x, y, &g)) {
                continue;
            }
            if (check_level && fabsf((float) g - level) > 4.0f) {
                continue;
            }
            vector3f_t b = {x + 0.5f, y + 0.5f, (float) g};
            if (avoid != NULL && hypotf(b.x - avoid->x, b.y - avoid->y) < HELI_PAD_GAP) {
                continue;
            }
            if (_heli_blocked(server, b.x, b.y, b.z, face)) {
                continue;
            }
            *out = b;
            *yaw = face;
            return 1;
        }
    }
    return 0;
}

void heli_map_start(server_t* server)
{
    memset(heli_slots, 0, sizeof heli_slots);
    /* protocol 1.0 only, and the maps that have room for a helicopter */
    if (profile_is_075() || server->s_map.map.size_x != 1024 || server->s_map.map.size_y != 1024) {
        return;
    }
    for (int i = 0; i < HELI_SLOTS; i++) {
        int          team = i / HELI_PER_TEAM;
        heli_slot_t* slot = &heli_slots[i];
        slot->team        = (uint8_t) team;
        /* the second pad is the nearest flat ground a gap from the first */
        const vector3f_t* first = i % HELI_PER_TEAM ? (heli_slots[i - 1].used ? &heli_slots[i - 1].base : NULL) : NULL;
        if (i % HELI_PER_TEAM && first == NULL) {
            continue;
        }
        if (!_heli_site(server, team, first, &slot->base, &slot->yaw)) {
            evt_log(NULL, "heli", "action=base team=%d pad=%d why=no_site", team, i % HELI_PER_TEAM);
            continue;
        }
        slot->used = 1;
        evt_log(NULL, "heli", "action=base team=%d pad=%d at=%.1f,%.1f,%.1f yaw=%.0f", team, i % HELI_PER_TEAM,
                slot->base.x, slot->base.y, slot->base.z, slot->yaw);
        _heli_spawn(server, i);
    }
}

void heli_tick(server_t* server)
{
    if (profile_is_075()) {
        return;
    }
    uint64_t now = get_nanos();
    for (int i = 0; i < HELI_SLOTS; i++) {
        heli_slot_t* slot = &heli_slots[i];
        if (!slot->used) {
            continue;
        }
        if (slot->entity == 0) {
            if (slot->respawn_at != 0 && now >= slot->respawn_at) {
                _heli_spawn(server, i);
            }
            continue;
        }
        p10_object_t* o = proto10_objects_find(slot->entity);
        if (o == NULL) {
            /* taken away by something else (a map reset destroys them all
               before heli_map_start resets these slots) */
            slot->entity     = 0;
            slot->respawn_at = now + (uint64_t) (GAME_HELI_RESPAWN * NANO_IN_SECOND);
            continue;
        }
        /* its occupants see it 60 times a second while it moves: the seat
           they sit on is the thing they steer by */
        for (int seat = 0; seat < GAME_HELI_SEATS; seat++) {
            player_t* p = mount_occupant(server, o->id, seat);
            if (p != NULL && p->conn != NULL) {
                proto10_objects_send_to(p, o);
            }
        }
    }
}
