// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Powered flight, shared by the client prediction (the client's player.c) and
 * the server simulation (server/Source/Util/Physics.c), so both integrate the
 * very same thrust. Movement is client-authoritative but the server runs its
 * own copy of the player physics between position reports; if the two
 * disagreed, the server would pull a flying player down and hand out fall
 * damage for a landing that never happened.
 *
 * Plain C99 (math.h), header only, no dependency on either program.
 *
 * Coordinates are the map's: z grows downwards, so climbing makes velocity z
 * negative. Velocities are AoS physics units (blocks per 1/32 s) and the
 * player physics adds 1 unit per second of gravity (32 blocks/s^2); the
 * thrust below is in the same units, so thrust > 1 climbs.
 *
 * The jetpack is the first user (docs/protocol-1.0.md 12.4); a vehicle can
 * reuse flight_step with fuel == NULL (unlimited) and its own rules.
 */
#ifndef FLIGHT_FLIGHT_H
#define FLIGHT_FLIGHT_H

#include <math.h>

typedef struct {
    float fuel;        /* s of thrust from a full tank, ignition included; 0 disables flight */
    float regen;       /* s from empty to full; 0 never refills */
    float thrust;      /* upward acceleration, units per second (gravity is 1) */
    float max_rise;    /* fastest climb the thrust produces, units (positive) */
    float air_control;    /* horizontal acceleration factor while thrusting (plain airborne is 0.1) */
    float ceiling;        /* no thrust while z is above (smaller than) this */
    float glide_control;  /* horizontal acceleration factor while gliding: equipped, fuel left, not thrusting */
    float ignition;       /* s paid once at each rising edge, before the sustained drain; 0 disables the bite */
    float refill_delay;   /* s after standing on damage before regen resumes */
} flight_rules;

/* The numbers of the retail release's jump pack, converted here
   (2026 port; see docs/protocol-1.0.md 12.4):
   - the lever pays a one-time bite of 0.133 s (its 10 of 100 tank points),
     then drains at 75 points/s from the rest, so thrust lasts 1.2 s after
     ignition and the whole tank, 1.333 s, is what rules.fuel holds;
   - thrust 86.4 blocks/s^2, tank refilled at 10 points/s (a full tank in
     10 s) while not thrusting, in the air as well as on the ground, but
     never for the 2.0 s after standing on a hit;
   - climbing at most 28 blocks/s, steering at half a walk while thrusting
     (16 blocks/s top speed, twice a run) and a fifth of one between pulses,
     so the flyer keeps its momentum in the air.
   The retail mover shaped its climb with a drag toward the previous speed
   each frame instead of a cap, and under it the climb finds some ceiling
   over 50 blocks/s because the drag and the thrust balance there. Our
   integrator is continuous (a * dt at the frame's length), and a cap at
   half the retail drag-limited ceiling keeps the jump of a fresh hop, the
   reachable height and the "twice a run" feel of the retail pack without
   the wire's max_rise differing with the length of the frame.
   fall_distance: the retail reset (thrusting sets it to 0) has no
   counterpart to copy: our fall damage is the vertical speed at landing
   (Physics.c), no accumulator exists, and holding the thrust during a fall
   already cancels that velocity, so a thrusting player never lands hard. */
#define FLIGHT_JETPACK_DEFAULTS \
    {1.333f, 10.0f, 2.7f, 0.875f, 0.5f, -2.0f, 0.2f, 0.133f, 2.0f}

/* blocks/s and blocks/s^2 (config, wire) <-> physics units */
#define FLIGHT_UNITS_PER_BLOCK_S (1.0f / 32.0f)

static inline int flight_enabled(const flight_rules* r)
{
    return r->fuel > 0.0f && r->thrust > 0.0f && r->max_rise > 0.0f;
}

/*
 * One physics step, called before gravity and friction (where the jump is
 * applied). want: the pilot holds the thrust input. airborne: the physics'
 * own flag from the previous step, so thrust never lifts a player off the
 * ground by itself (the jump does that). fuel: NULL means unlimited, used
 * for players whose thrust another program already gated on its fuel.
 * since_damage: seconds since the last hit (a huge number when none) —
 * regen waits refill_delay. rising: the input's rising edge (want and not
 * thrusting last step); the ignition bite is paid on the first assigned
 * frame (held but blocked by the ceiling, it stays rising and pays when
 * the flight finally starts, like the retail press cost).
 * Returns 1 when thrust was applied this step.
 */
static inline int flight_step(const flight_rules* r, float* fuel, int want, int airborne, float z, float* vz, float dt,
                              float since_damage, int rising)
{
    if (!flight_enabled(r)) {
        return 0;
    }
    /* the bite must fit the tank: a rising edge needs fuel over ignition,
       a held one over 0 (need is 0 then) */
    float need = rising ? r->ignition : 0.0f;
    int   thrust = want && airborne && z > r->ceiling && (fuel == NULL || *fuel > need);
    if (thrust) {
        /* never faster than max_rise, but never brake a faster motion either
           (a fresh jump starts at 0.36, above the default cap) */
        float limit = *vz < -r->max_rise ? *vz : -r->max_rise;
        *vz -= r->thrust * dt;
        if (*vz < limit) {
            *vz = limit;
        }
        if (fuel != NULL) {
            *fuel -= dt + need;
            if (*fuel < 0.0f) {
                *fuel = 0.0f;
            }
        }
    } else if (fuel != NULL && r->regen > 0.0f && since_damage >= r->refill_delay) {
        /* no "on the ground only" gate: the retail pack drips in the air too */
        *fuel += dt * r->fuel / r->regen;
        if (*fuel > r->fuel) {
            *fuel = r->fuel;
        }
    }
    return thrust;
}

/*
 * Free flight, the creative mode's: like the spectator's camera, but with a
 * body that blocks stop. No gravity: forward and back go the look's way
 * (its pitch too), sideways along the strafe, up while jump is held, down
 * while crouching; let go and it stops. Sprint doubles it. The velocity it
 * gives is the step's whole velocity (no friction, no momentum), in physics
 * units; both movers then clip it against the map as they clip a walk.
 * f: the look (a unit vector, z down), s: the strafe (unit, horizontal).
 */
#define FLIGHT_FREE_SPEED  0.25f /* 8 blocks a second */
#define FLIGHT_FREE_SPRINT 2.0f

static inline void flight_free(const float f[3], const float s[2], int forward, int back, int left, int right, int up,
                               int down, int sprint, float v[3])
{
    float k = FLIGHT_FREE_SPEED * (sprint ? FLIGHT_FREE_SPRINT : 1.0f);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (forward) {
        x += f[0], y += f[1], z += f[2];
    } else if (back) {
        x -= f[0], y -= f[1], z -= f[2];
    }
    if (left) {
        x -= s[0], y -= s[1];
    } else if (right) {
        x += s[0], y += s[1];
    }
    if (up) {
        z -= 1.0f;
    } else if (down) {
        z += 1.0f;
    }
    float l = sqrtf(x * x + y * y + z * z);
    if (l > 1.0f) {
        /* never faster on a diagonal */
        x /= l, y /= l, z /= l;
    }
    v[0] = x * k;
    v[1] = y * k;
    v[2] = z * k;
}

#endif
