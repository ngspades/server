#include <Util/Enums.h>
#include <devices/devices.h>
#include <modes/modes.h>
#include <physics/box.h>
#include <physics/solid.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Profile.h>
#include <math.h>
#include <stddef.h>

// SpadesX
#include <Server/Classes.h>
#include <Server/Packets/Packets.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Physics.h>
#include <Util/Nanos.h>
#include <Util/Types.h>
#include <Server/MapStore.h>

#define SQRT                 0.70710678f
#define FALL_SLOW_DOWN       0.24f
#define FALL_DAMAGE_VELOCITY physics_fall_damage_velocity
#define FALL_DAMAGE_SCALAR   PHYSICS_FALL_DAMAGE_SCALAR

float physics_jump_velocity        = PHYSICS_JUMP_VELOCITY;
float physics_fall_damage_velocity = PHYSICS_FALL_DAMAGE_VELOCITY;

/* ------------------------------------------------------------------ */
/* Orientation and rays                                                */
/* ------------------------------------------------------------------ */

/* The right (strafe) and up (height) vectors of a look direction, so a body
   strafes and aims along its own frame. f == 0 for a straight up/down look
   divides by zero: physics_validate_hit's shooter always has a horizontal
   look, and _phys_set_orientation_vectors guards the other path first. */
static inline void _phys_orientation_side(float x, float y, float z, vector3f_t* strafe, vector3f_t* height)
{
    float f = sqrtf(x * x + y * y);
    strafe->x = -y / f;
    strafe->y = x / f;
    strafe->z = 0.0f;
    height->x = -z * strafe->y;
    height->y = z * strafe->x;
    height->z = x * strafe->y - y * strafe->x;
}

float physics_distance_3d(float x1, float y1, float z1, float x2, float y2, float z2)
{
    return sqrtf(pow(x2 - x1, 2) + pow(y2 - y1, 2) + pow(z2 - z1, 2));
}

int physics_validate_hit(vector3f_t shooter, vector3f_t orientation, vector3f_t otherPos, float tolerance)
{
    float         cx, cy, cz, r, x, y;
    orientation_t o;

    o.forward = orientation;
    _phys_orientation_side(orientation.x, orientation.y, orientation.z, &o.strafe, &o.height);

    otherPos.x -= shooter.x;
    otherPos.y -= shooter.y;
    otherPos.z -= shooter.z;

    cz = otherPos.x * o.forward.x + otherPos.y * o.forward.y + otherPos.z * o.forward.z;
    r  = 1.f / cz;
    cx = otherPos.x * o.strafe.x + otherPos.y * o.strafe.y + otherPos.z * o.strafe.z;
    x  = cx * r;
    cy = otherPos.x * o.height.x + otherPos.y * o.height.y + otherPos.z * o.height.z;
    y  = cy * r;
    r *= tolerance;

    return x - r < 0 && x + r > 0 && y - r < 0 && y + r > 0;
}

/* float to long, toward zero (the voxel walk's cell index) */
static inline void _phys_ftol(float f, long* a)
{
    *a = (long) f;
}

/* Is the cell at (x, y, z) solid for a player's box? Unlike a plain solid
   query: the water layer reads as empty (its slot stands for the block above
   it), everything outside the horizontal map is solid, above the world is
   open and below it is solid. */
static inline int _phys_cell_blocks_box(server_t* server, float x, float y, float z)
{
    int sz;

    if (x < 0 || x >= server->s_map.map.size_x || y < 0 || y >= server->s_map.map.size_y)
        return 1;
    if (z < 0)
        return 0;
    sz = (int) z;
    if (sz == server->s_map.map.size_z - 1)
        sz = server->s_map.map.size_z - 2;
    else if (sz >= server->s_map.map.size_z)
        return 1;
    return map_solid(&server->s_map.map, (int) x, (int) y, sz);
}

/* The same as _phys_cell_blocks_box but with the edges open (a grenade can
   fly out of the world) and no wrapping. */
static inline long _phys_cell_solid_wade(server_t* server, long x, long y, long z)
{
    int sz;

    if (x < 0 || x >= server->s_map.map.size_x || y < 0 || y >= server->s_map.map.size_y)
        return 0;
    if (z < 0)
        return 0;
    sz = (int) z;
    if (sz == server->s_map.map.size_z - 1)
        sz = server->s_map.map.size_z - 2;
    else if (sz >= server->s_map.map.size_z - 1)
        return 1;
    return map_solid(&server->s_map.map, (int) x, (int) y, sz);
}

/* A solid query that wraps the map's horizontal edges (the world repeats) */
static inline long _phys_cell_solid_wrap(server_t* server, long x, long y, long z)
{
    if (z < 0)
        return 0;
    if (z >= server->s_map.map.size_z)
        return 1;
    return map_solid(&server->s_map.map, (int) x & (server->s_map.map.size_x - 1),
                     (int) y & (server->s_map.map.size_y - 1), z);
}

/* The state of a voxel walk (Amanatides & Woo): the current and end cell,
   the step, the distances to the first and to each border, the accumulated
   crossing and its per-cell increment. */
typedef struct
{
    vector3l_t a; /* current cell */
    vector3l_t c; /* end cell */
    vector3l_t d; /* step, -1 / 0 / 1 */
    vector3f_t f; /* distance to the first border */
    vector3f_t g; /* distance across one whole cell */
    vector3l_t p; /* accumulated crossing */
    vector3l_t i; /* per-cell increment */
    long       steps;
} _phys_ray_t;

/* One axis of the walk's setup: the step, the two distances, and how many
   borders the walk crosses on this axis (its share of the cell count). */
static inline long _phys_ray_axis(long* a, long* c, long* d, float x0, float x1, float* f, float* g)
{
    if (*c < *a) {
        *d = -1;
        *f = x0 - *a;
        *g = (x0 - x1) * 1024;
        return *a - *c;
    } else if (*c != *a) {
        *d = 1;
        *f = *a + 1 - x0;
        *g = (x1 - x0) * 1024;
        return *c - *a;
    }
    *f = *g = 0;
    return 0;
}

/* Set up a voxel walk from (x0,y0,z0) to (x1,y1,z1) */
static inline void _phys_ray_begin(_phys_ray_t* r, float x0, float y0, float z0, float x1, float y1, float z1)
{
    r->d.x   = 0;
    r->d.y   = 0;
    r->d.z   = 0;
    r->steps = 0;

    _phys_ftol(x0 - .5f, &r->a.x);
    _phys_ftol(y0 - .5f, &r->a.y);
    _phys_ftol(z0 - .5f, &r->a.z);
    _phys_ftol(x1 - .5f, &r->c.x);
    _phys_ftol(y1 - .5f, &r->c.y);
    _phys_ftol(z1 - .5f, &r->c.z);

    r->steps += _phys_ray_axis(&r->a.x, &r->c.x, &r->d.x, x0, x1, &r->f.x, &r->g.x);
    r->steps += _phys_ray_axis(&r->a.y, &r->c.y, &r->d.y, y0, y1, &r->f.y, &r->g.y);
    r->steps += _phys_ray_axis(&r->a.z, &r->c.z, &r->d.z, z0, z1, &r->f.z, &r->g.z);

    _phys_ftol(r->f.x * r->g.z - r->f.z * r->g.x, &r->p.x);
    _phys_ftol(r->g.x, &r->i.x);
    _phys_ftol(r->f.y * r->g.z - r->f.z * r->g.y, &r->p.y);
    _phys_ftol(r->g.y, &r->i.y);
    _phys_ftol(r->f.y * r->g.x - r->f.x * r->g.y, &r->p.z);
    _phys_ftol(r->g.z, &r->i.z);
}

/* Advance the walk one cell along the axis whose border it crosses next */
static inline void _phys_ray_step(_phys_ray_t* r)
{
    if (((r->p.x | r->p.y) >= 0) && (r->a.z != r->c.z)) {
        r->a.z += r->d.z;
        r->p.x -= r->i.x;
        r->p.y -= r->i.y;
    } else if ((r->p.z >= 0) && (r->a.x != r->c.x)) {
        r->a.x += r->d.x;
        r->p.x += r->i.z;
        r->p.z -= r->i.y;
    } else {
        r->a.y += r->d.y;
        r->p.y += r->i.z;
        r->p.z += r->i.x;
    }
}

/* 1 when no solid cell lies on the segment from (x0,y0,z0) to (x1,y1,z1).
   Only the first 32 cells are walked (a long line is "clear"). */
long physics_can_see(server_t* server, float x0, float y0, float z0, float x1, float y1, float z1)
{
    _phys_ray_t r;
    _phys_ray_begin(&r, x0, y0, z0, x1, y1, z1);
    if (r.steps > 32)
        r.steps = 32;

    while (r.steps) {
        _phys_ray_step(&r);
        if (_phys_cell_solid_wrap(server, r.a.x, r.a.y, r.a.z))
            return 0;
        r.steps--;
    }
    return 1;
}

/* Walk the segment along (x1,y1,z1) for `length` blocks from (x0,y0,z0) and
   report the first solid cell in (x,y,z); 0 when the segment stays clear. */
long physics_cast_ray(server_t* server,
                      float     x0,
                      float     y0,
                      float     z0,
                      float     x1,
                      float     y1,
                      float     z1,
                      float     length,
                      long*     x,
                      long*     y,
                      long*     z)
{
    x1 = x0 + x1 * length;
    y1 = y0 + y1 * length;
    z1 = z0 + z1 * length;

    _phys_ray_t r;
    _phys_ray_begin(&r, x0, y0, z0, x1, y1, z1);
    if (r.steps > length)
        r.steps = (long) length;

    while (r.steps) {
        _phys_ray_step(&r);
        if (_phys_cell_solid_wrap(server, r.a.x, r.a.y, r.a.z)) {
            *x = r.a.x;
            *y = r.a.y;
            *z = r.a.z;
            return 1;
        }
        r.steps--;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Player movement                                                     */
/* ------------------------------------------------------------------ */

/* The eye sits at the body's position, raised a little just after a climb
   (the smoothed step up) */
static inline void _phys_reposition(player_t* player, const vector3f_t* position, physics_t* physics)
{
    float since_climb;

    player->movement.eye_pos = player->movement.position = *position;
    since_climb = player->lastclimb - physics->ftotclk;
    if (since_climb > -0.25f)
        player->movement.eye_pos.z += (since_climb + 0.25f) / 0.25f;
}

/* Store the new look and re-derive the strafe and up vectors from it */
static inline void _phys_set_orientation_vectors(vector3f_t* o, vector3f_t* s, vector3f_t* h)
{
    float f = sqrtf(o->x * o->x + o->y * o->y);
    if (f == 0) {
        return;
    }
    _phys_orientation_side(o->x, o->y, o->z, s, h);
}

void physics_reorient_player(player_t* player, vector3f_t* orientation)
{
    player->movement.forward_orientation = *orientation;
    _phys_set_orientation_vectors(orientation, &player->movement.strafe_orientation,
                                  &player->movement.height_orientation);
}

/* Any of the body's four corners at height z is inside a solid cell */
static inline int _phys_corners_blocked(server_t* server, float x1, float x2, float y1, float y2, float z)
{
    return _phys_cell_blocks_box(server, x1, y1, z) || _phys_cell_blocks_box(server, x1, y2, z) ||
           _phys_cell_blocks_box(server, x2, y1, z) || _phys_cell_blocks_box(server, x2, y2, z);
}

int physics_try_uncrouch(server_t* server, player_t* player)
{
    float x1 = player->movement.position.x + GAME_BOX_HALF_WIDTH;
    float x2 = player->movement.position.x - GAME_BOX_HALF_WIDTH;
    float y1 = player->movement.position.y + GAME_BOX_HALF_WIDTH;
    float y2 = player->movement.position.y - GAME_BOX_HALF_WIDTH;
    float z1 = player->movement.position.z + 2.25f;
    float z2 = player->movement.position.z - 1.35f;

    /* in midair, first check the feet can lower */
    if (player->airborne && !_phys_corners_blocked(server, x1, x2, y1, y2, z1))
        return 1;
    /* then check the head can rise */
    if (!_phys_corners_blocked(server, x1, x2, y1, y2, z2)) {
        player->movement.position.z -= 0.9f; /* up from the crouch to the stand */
        player->movement.eye_pos.z -= 0.9f;
        return 1;
    }
    return 0;
}

/* A sprinting 1.0 player climbs a one block step on any device (common/
   physics/box.h): the server's copy of the client's 1.0 device; a bot or a
   0.75 player has none (0) and never climbs while sprinting. */
static inline int player_sprint_climbs(const player_t* player)
{
    return player->protocol == PROTOCOL_10 && game_device_sprint_climbs(player->device_input);
}

/* Is the cell under the body's leading face solid: `lead` is the coordinate
   along the axis being moved and `fixed` the other horizontal one, so on y
   the pair is (fixed, lead), not (lead, fixed). */
static inline int _phys_face_blocked(server_t* server, int along_y, float lead, float fixed, float z)
{
    return along_y ? _phys_cell_blocks_box(server, fixed, lead, z) : _phys_cell_blocks_box(server, lead, fixed, z);
}

/* Move the body's leading face along one horizontal axis. The face scans
   down from the box's top for the first height where its two front corners
   both fit; a clear column advances the axis, a one-block step advances it
   with *climbed set, and a taller wall zeroes the axis's velocity. */
static inline void _phys_axis_move(server_t* server,
                                   float     nx,
                                   float     fixed_lo,
                                   float     fixed_hi,
                                   float     lift,
                                   int       along_y,
                                   int       can_climb,
                                   float     span,
                                   float*    position_axis,
                                   float*    velocity_axis,
                                   long*     climbed,
                                   int       block_zeroes)
{
    float f = *velocity_axis < 0 ? -GAME_BOX_HALF_WIDTH : GAME_BOX_HALF_WIDTH;
    float z = span;

    while (z >= GAME_BOX_SCAN_END && !_phys_face_blocked(server, along_y, nx + f, fixed_lo, lift + z) &&
           !_phys_face_blocked(server, along_y, nx + f, fixed_hi, lift + z))
        z -= GAME_BOX_SCAN_STEP;

    if (z < GAME_BOX_SCAN_END) {
        *position_axis = nx;
        return;
    }

    if (can_climb) {
        z = GAME_BOX_CLIMB_START;
        while (z >= GAME_BOX_CLIMB_END && !_phys_face_blocked(server, along_y, nx + f, fixed_lo, lift + z) &&
               !_phys_face_blocked(server, along_y, nx + f, fixed_hi, lift + z))
            z -= GAME_BOX_SCAN_STEP;
        if (z < GAME_BOX_CLIMB_END) {
            *position_axis = nx;
            *climbed       = 1;
            return;
        }
        *velocity_axis = 0;
        return;
    }

    if (block_zeroes)
        *velocity_axis = 0;
}

/* A living body another one may not walk into (GM_RULE_SOLID_PLAYERS) */
static inline int _phys_is_solid_body(const player_t* other)
{
    return other->alive && other->state == STATE_READY && (other->team == TEAM_A || other->team == TEAM_B);
}

/* Solid players (common/physics/solid.h, the client predicts the same): the
   step that ended at the body's position slides along every living body it
   would walk into. When the slid spot is inside a wall the body stays where
   it was before the step, which is always a legal place. */
static void _phys_solid_players(server_t* server, player_t* player, float ox, float oy, float lift, float span)
{
    float nx = player->movement.position.x, ny = player->movement.position.y;
    int   changed = 0;

    if (profile_is_075() || !(gamemode_player_rules(server, player) & GM_RULE_SOLID_PLAYERS)) {
        return;
    }
    player_t *other, *tmp;
    HASH_ITER(hh, server->players, other, tmp)
    {
        if (other == player || !_phys_is_solid_body(other) ||
            !game_solid_height(player->movement.position.z, other->movement.position.z)) {
            continue;
        }
        changed |= game_solid_clip(ox, oy, &nx, &ny, other->movement.position.x, other->movement.position.y,
                                   GAME_SOLID_DIST, GAME_SOLID_NUDGE, player->id < other->id ? 1.0f : -1.0f);
    }
    if (!changed) {
        return;
    }
    /* the slid spot must fit the walls at every height the body is scanned */
    for (float z = span; z >= GAME_BOX_SCAN_END; z -= GAME_BOX_SCAN_STEP) {
        if (_phys_corners_blocked(server, nx - GAME_BOX_HALF_WIDTH, nx + GAME_BOX_HALF_WIDTH,
                                  ny - GAME_BOX_HALF_WIDTH, ny + GAME_BOX_HALF_WIDTH,
                                  player->movement.position.z + lift + z)) {
            nx = ox;
            ny = oy;
            break;
        }
    }
    player->movement.position.x = nx;
    player->movement.position.y = ny;
}

/* player movement with autoclimb */
void physics_box_clip_move(server_t* server, player_t* player, physics_t* physics)
{
    float step, span, nx, ny, nz, offset;
    long  climb = 0;
    int   can_climb;

    float start_x = player->movement.position.x, start_y = player->movement.position.y;

    step = physics->fsynctics * 32.f;
    nx   = step * player->movement.velocity.x + player->movement.position.x;
    ny   = step * player->movement.velocity.y + player->movement.position.y;

    if (player->crouching) {
        offset = GAME_BOX_LIFT_CROUCH;
        span   = GAME_BOX_SPAN_CROUCH;
    } else {
        offset = GAME_BOX_LIFT_STAND;
        span   = GAME_BOX_SPAN_STAND;
    }

    nz        = player->movement.position.z + offset;
    can_climb = !player->crouching && player->movement.forward_orientation.z < 0.5f &&
                (!player->sprinting || player_sprint_climbs(player));

    _phys_axis_move(server, nx, player->movement.position.y - GAME_BOX_HALF_WIDTH,
                    player->movement.position.y + GAME_BOX_HALF_WIDTH, nz, 0, can_climb, span,
                    &player->movement.position.x, &player->movement.velocity.x, &climb, 1);

    /* a climb on x already moved the body up: the y axis then only slides
       along the step's face (a second climb is refused, and a wall does not
       kill the leftover y velocity) */
    _phys_axis_move(server, ny, player->movement.position.x - GAME_BOX_HALF_WIDTH,
                    player->movement.position.x + GAME_BOX_HALF_WIDTH, nz, 1, can_climb && !climb, span,
                    &player->movement.position.y, &player->movement.velocity.y, &climb, !climb);

    if (climb) {
        player->movement.velocity.x *= 0.5f;
        player->movement.velocity.y *= 0.5f;
        player->lastclimb = physics->ftotclk;
        nz -= 1.0f;
        span = GAME_BOX_CLIMB_LAND;
    } else {
        if (player->movement.velocity.z < 0)
            span = -span;
        nz += player->movement.velocity.z * physics->fsynctics * 32.f;
    }

    player->airborne = 1;
    player->wade     = player->movement.position.z > GAME_WADE_Z;

    if (_phys_corners_blocked(server, player->movement.position.x - GAME_BOX_HALF_WIDTH,
                              player->movement.position.x + GAME_BOX_HALF_WIDTH,
                              player->movement.position.y - GAME_BOX_HALF_WIDTH,
                              player->movement.position.y + GAME_BOX_HALF_WIDTH, nz + span)) {
        if (player->movement.velocity.z >= 0)
            player->airborne = 0;
        player->movement.velocity.z = 0;
    } else
        player->movement.position.z = nz - offset;

    _phys_solid_players(server, player, start_x, start_y, offset, player->crouching ? GAME_BOX_SPAN_CROUCH : GAME_BOX_SPAN_STAND);
    _phys_reposition(player, &player->movement.position, physics);
}

/* Free flight (creative): the velocity of the keys, no gravity, clipped like
   a walk; the client predicts the very same step. */
static inline void _phys_move_free(server_t* server, player_t* player, physics_t* physics)
{
    float look[3] = {player->movement.forward_orientation.x, player->movement.forward_orientation.y,
                     player->movement.forward_orientation.z};
    float ll      = sqrtf(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    float side[2] = {player->movement.strafe_orientation.x, player->movement.strafe_orientation.y};
    float sl      = sqrtf(side[0] * side[0] + side[1] * side[1]);
    float v[3];

    if (ll > 0.001f) {
        look[0] /= ll, look[1] /= ll, look[2] /= ll;
    }
    if (sl > 0.001f) {
        side[0] /= sl, side[1] /= sl;
    }

    flight_free(look, side, player->move_forward, player->move_backwards, player->move_left, player->move_right,
                player->fly_up, player->crouching, player->sprinting, v);

    player->jumping             = 0;
    player->movement.velocity.x = v[0];
    player->movement.velocity.y = v[1];
    player->movement.velocity.z = v[2];
    physics_box_clip_move(server, player, physics);
}

/* The jump at the start of a tick: a plain jump, or the zombie's charged
   leap (up and ahead, as the client predicts it). */
static inline void _phys_apply_jump(player_t* player)
{
    if (player->jumping) {
        player->jumping = 0;
        if (player->high_jump && player->leap_charge) {
            float c   = game_zombie_leap_charge(player->leap_charge);
            float fx  = player->movement.forward_orientation.x, fy = player->movement.forward_orientation.y;
            float len = sqrtf(fx * fx + fy * fy);
            player->movement.velocity.z = game_zombie_leap_up(c);
            if (len > 0.001f) {
                player->movement.velocity.x += fx / len * game_zombie_leap_forward(c);
                player->movement.velocity.y += fy / len * game_zombie_leap_forward(c);
            }
        } else {
            /* the class's jump (Server/Classes.c sets it every tick) */
            player->movement.velocity.z = player->jump_velocity < 0.0f ? player->jump_velocity : -physics_jump_velocity;
        }
        player->leap_charge = 0;
    }
}

/* One step of the jetpack, integrated exactly like the client predicts it
   (flight.h). rising: last step's applied thrust still stands in
   jetpack_thrusting here (the assignment happens after the call), so a held
   input blocked by the ceiling stays rising and pays the ignition bite on
   the first frame the flight actually starts. */
static inline void _phys_step_jetpack(server_t* server, player_t* player, physics_t* physics)
{
    uint8_t was_thrusting = player->jetpack_thrusting;
    float   since_damage  = 1.0e9f;

    if (player->hurt_at != 0) {
        uint64_t n = get_nanos() - player->hurt_at; /* hurt_at is stamped for every send_set_hp, falls and water too */
        if ((int64_t) n < 0) {
            n = 0;
        }
        since_damage = (float) ((double) n / 1.0e9);
    }

    player->jetpack_thrusting = (uint8_t) (player->jetpack_equipped &&
                                           flight_step(&server->jetpack,
                                                       &player->jetpack_fuel,
                                                       player->jetpack_input,
                                                       player->airborne,
                                                       player->movement.position.z,
                                                       &player->movement.velocity.z,
                                                       physics->fsynctics,
                                                       since_damage,
                                                       !was_thrusting));
}

/* The ground acceleration this tick: the base step, scaled by stance, aim,
   sneak, sprint, class speed, zombie speed and how far a stick is pushed. */
static inline float _phys_walk_scale(server_t* server, player_t* player, physics_t* physics)
{
    float f = physics->fsynctics; // player acceleration scalar

    if (player->airborne)
        f *= player->jetpack_thrusting
               ? server->jetpack.air_control
               : (player->jetpack_equipped && player->jetpack_fuel > 0.0f ? server->jetpack.glide_control : 0.1f);
    else if (player->crouching)
        f *= 0.3f;
    else if (player->secondary_fire && player->item == TOOL_GUN)
        f *= 0.5f;
    else if (player->sneaking)
        f *= 0.5f;
    else if (player->sprinting)
        /* a class's sprint REPLACES its walk speed (retail); the classic
           x1.3 stays for a player without one */
        f *= player->class_sprint > 0.0f ? player->class_sprint : 1.3f;

    if (player->fast && !player->airborne)
        f *= player->slowed ? GAME_ZOMBIE_SLOWED : GAME_ZOMBIE_SPEED;

    /* the class's ground speed, on top of crouch and aim (and of nothing
       when its sprint already stood in for the walk: the client predicts
       the same) */
    if (player->class_speed > 0.0f && !player->airborne &&
        !(player->sprinting && player->class_sprint > 0.0f))
        f *= player->class_speed;

    /* a stick pushed part way (protocol 1.0's analogue input) */
    if (player->move_scale > 0.0f && player->move_scale < 1.0f && !player->airborne)
        f *= player->move_scale;

    return f;
}

long physics_move_player(server_t* server, player_t* player, physics_t* physics)
{
    float f, f2;

    if (player->flying) {
        _phys_move_free(server, player, physics);
        return 0;
    }

    /* move the player and apply simple physics (gravity, momentum, friction) */
    _phys_apply_jump(player);
    _phys_step_jetpack(server, player, physics);

    f = _phys_walk_scale(server, player, physics);

    if ((player->move_forward || player->move_backwards) && (player->move_left || player->move_right))
        f *= SQRT; // if strafe + forward/backwards then limit diagonal velocity

    if (player->move_forward) {
        player->movement.velocity.x += player->movement.forward_orientation.x * f;
        player->movement.velocity.y += player->movement.forward_orientation.y * f;
    } else if (player->move_backwards) {
        player->movement.velocity.x -= player->movement.forward_orientation.x * f;
        player->movement.velocity.y -= player->movement.forward_orientation.y * f;
    }
    if (player->move_left) {
        player->movement.velocity.x -= player->movement.strafe_orientation.x * f;
        player->movement.velocity.y -= player->movement.strafe_orientation.y * f;
    } else if (player->move_right) {
        player->movement.velocity.x += player->movement.strafe_orientation.x * f;
        player->movement.velocity.y += player->movement.strafe_orientation.y * f;
    }

    f = physics->fsynctics + 1;
    player->movement.velocity.z += physics->fsynctics;
    player->movement.velocity.z /= f; // air friction
    if (player->wade)
        /* the class's own drag (the Miner's 12 holds it slower in the water),
           the classic 6 for everyone else */
        f = physics->fsynctics * (player->class_water > 0.0f ? player->class_water : 6.f) + 1;
    else if (!player->airborne)
        f = physics->fsynctics * 4.f + 1; // ground friction
    player->movement.velocity.x /= f;
    player->movement.velocity.y /= f;

    f2 = player->movement.velocity.z;
    float fall_start_z = player->movement.position.z;
    physics_box_clip_move(server, player, physics);

    /* the fall flown down, in blocks (the classes' fall damage, retail):
       z grows downwards, so a move down adds to it and a move up past 0.1
       clears it (a step or a glide, not a fall) */
    {
        float dz = player->movement.position.z - fall_start_z;
        if (dz > 0.0f) {
            player->fall_dist += dz;
        } else if (dz < -0.1f) {
            player->fall_dist = 0.0f;
        }
    }

    // hit ground... check if hurt
    if (!player->movement.velocity.z) {
        long class_fall = classes_fall_damage(server, player);
        if (class_fall >= 0) {
            /* a class: the distance decides, whatever the landing speed */
            if (f2 > FALL_SLOW_DOWN) {
                player->movement.velocity.x *= 0.5f;
                player->movement.velocity.y *= 0.5f;
                return class_fall > 0 ? class_fall : -1;
            }
            return class_fall;
        }
        if (f2 > FALL_SLOW_DOWN) {
            // slow down on landing
            player->movement.velocity.x *= 0.5f;
            player->movement.velocity.y *= 0.5f;

            // return fall damage
            if (f2 > FALL_DAMAGE_VELOCITY) {
                f2 -= FALL_DAMAGE_VELOCITY;
                return ((long) (f2 * f2 * FALL_DAMAGE_SCALAR));
            }

            return (-1); // no fall damage but play fall sound
        }
    }

    return (0); // no fall damage
}

int physics_move_grenade(server_t* server, grenade_t* grenade, physics_t* physics)
{
    vector3f_t fpos = grenade->position; // old position

    /* velocity and gravity (friction on a grenade is negligible) */
    float f = physics->fsynctics * 32;
    grenade->velocity.z += physics->fsynctics;
    grenade->position.x += grenade->velocity.x * f;
    grenade->position.y += grenade->velocity.y * f;
    grenade->position.z += grenade->velocity.z * f;

    vector3l_t lp;
    lp.x = (long) floor(grenade->position.x);
    lp.y = (long) floor(grenade->position.y);
    lp.z = (long) floor(grenade->position.z);

    if (!_phys_cell_solid_wade(server, lp.x, lp.y, lp.z)) {
        return 0; // we didn't hit anything, no collision
    }

    static const float BOUNCE_SOUND_THRESHOLD = 1.1f;
    int               ret                     = 1;
    if (fabsf(grenade->velocity.x) > BOUNCE_SOUND_THRESHOLD ||
        fabsf(grenade->velocity.y) > BOUNCE_SOUND_THRESHOLD ||
        fabsf(grenade->velocity.z) > BOUNCE_SOUND_THRESHOLD)
        ret = 2; // play sound

    /* the cell the grenade came from decides which face it bounced off */
    vector3l_t lp2;
    lp2.x = (long) floor(fpos.x);
    lp2.y = (long) floor(fpos.y);
    lp2.z = (long) floor(fpos.z);

    if (lp.z != lp2.z && ((lp.x == lp2.x && lp.y == lp2.y) || !_phys_cell_solid_wade(server, lp.x, lp.y, lp2.z)))
        grenade->velocity.z = -grenade->velocity.z;
    else if (lp.x != lp2.x && ((lp.y == lp2.y && lp.z == lp2.z) || !_phys_cell_solid_wade(server, lp2.x, lp.y, lp.z)))
        grenade->velocity.x = -grenade->velocity.x;
    else if (lp.y != lp2.y && ((lp.x == lp2.x && lp.z == lp2.z) || !_phys_cell_solid_wade(server, lp.x, lp2.y, lp.z)))
        grenade->velocity.y = -grenade->velocity.y;

    grenade->position = fpos; // set back to old position
    grenade->velocity.x *= 0.36f;
    grenade->velocity.y *= 0.36f;
    grenade->velocity.z *= 0.36f;
    return ret;
}

/* a cell outside the map is no cell, and neither is the water layer */
static uint8_t _phys_line_cell_solid(server_t* server, int x, int y, int z)
{
    map_store_t* map = &server->s_map.map;
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z - 1)
        return 0;
    return map_solid(map, x, y, z);
}

/* One axis of the segment walk: the direction step, the distance to the
   first border and the distance across one cell (2.0, far past the end, when
   the axis does not move, so the others decide). */
static inline void _phys_line_axis(float d, int cell, float from, int* step, float* first, float* unit)
{
    *step = d > 0.0f ? 1 : -1;
    if (d != 0.0f) {
        *first = (d > 0.0f ? (float) (cell + 1) - from : from - (float) cell) / fabsf(d);
        *unit  = 1.0f / fabsf(d);
    } else {
        *first = 2.0f;
        *unit  = 2.0f;
    }
}

/* every cell the segment crosses is free (a voxel walk, Amanatides & Woo),
   all of them: physics_can_see stops after 32 cells. */
uint8_t physics_line_clear(server_t* server, float x0, float y0, float z0, float x1, float y1, float z1)
{
    int   x = (int) floorf(x0), y = (int) floorf(y0), z = (int) floorf(z0);
    int   ex = (int) floorf(x1), ey = (int) floorf(y1), ez = (int) floorf(z1);
    float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    int   sx, sy, sz;
    float tx, ty, tz, ux, uy, uz;

    _phys_line_axis(dx, x, x0, &sx, &tx, &ux);
    _phys_line_axis(dy, y, y0, &sy, &ty, &uy);
    _phys_line_axis(dz, z, z0, &sz, &tz, &uz);

    int n = abs(ex - x) + abs(ey - y) + abs(ez - z);
    for (int k = 0; k < n && k < 2048; k++) {
        if (tx <= ty && tx <= tz) {
            x += sx, tx += ux;
        } else if (ty <= tz) {
            y += sy, ty += uy;
        } else {
            z += sz, tz += uz;
        }
        if (_phys_line_cell_solid(server, x, y, z))
            return 0;
    }
    return 1;
}
