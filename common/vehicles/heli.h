// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The helicopter: its size, health and flight, shared by the server (it
 * simulates every helicopter from its pilot's input and is authoritative:
 * server/Source/Server/Proto10/Helicopter.c) and the client (it predicts its
 * own helicopter while piloting and draws them all: the client's heli.c), so
 * both integrate the very same flight.
 *
 * After the AoS 1.0 beta client's (aosogl, aoslib/scenes/main.py
 * Helicopter: tilt with the keys, lift with jump, the rotor's pitch in its
 * sound, models chopper1/chopper2), rewritten as a continuous integrator in
 * blocks and seconds; credited in README.md and CREDITS.md.
 *
 * Map coordinates: z grows downwards, so climbing is a negative vz.
 * Collision is the caller's (each program reads its own map): heli_step only
 * integrates; the caller clips the move against the box below and calls
 * heli_landed / heli_impact.
 *
 * Header only: std C99 + math.h, no dependency on either program.
 */
#ifndef SHARED_HELI_H
#define SHARED_HELI_H

#include <math.h>
#include <stdint.h>

#define GAME_HELI_HEALTH         400
/* the body's box round its position (the middle of the skids' bottom) */
#define GAME_HELI_HALF_LENGTH    5.5f
#define GAME_HELI_HALF_WIDTH     1.8f
#define GAME_HELI_HEIGHT         4.5f
#define GAME_HELI_ROTOR_RADIUS   6.0f
/* the mast (the models' pivot, where the rotor turns) stands this far ahead
   of the box's middle: chopper1.kv6's pivot is 34 voxels into its 102, so
   drawn at the position the tail boom stood 1.9 blocks out of the box, and
   into hills and walls the box had stopped at */
#define GAME_HELI_MAST_FORWARD   1.8f
/* model scale, a voxel in blocks (aosogl drew it at 0.15; ours is smaller
   so it fits a base and a street) */
#define GAME_HELI_VOXEL          0.11f
/* seat 0 is the pilot, the others passengers; nobody fires from it */
#define GAME_HELI_SEATS          3
/* a player boards from this far (eye to the body's box) */
#define GAME_HELI_USE_RANGE      3.0f
/* s after its wreck before a new one stands at its team's base */
#define GAME_HELI_RESPAWN        10.0f

#define GAME_HELI_MAX_SPEED      24.0f /* blocks/s, horizontal */
#define GAME_HELI_ACCEL          12.0f /* blocks/s^2 at full tilt */
#define GAME_HELI_DRAG           0.9f  /* 1/s, horizontal, always */
#define GAME_HELI_CLIMB          9.0f  /* blocks/s up with lift held */
#define GAME_HELI_SINK           7.0f  /* blocks/s down with descend held */
#define GAME_HELI_VERT_ACCEL     10.0f /* blocks/s^2 toward the wanted climb */
#define GAME_HELI_GRAVITY        20.0f /* blocks/s^2 with the rotor stopped or wrecked */
#define GAME_HELI_YAW_RATE       75.0f /* degrees/s toward the pilot's look */
#define GAME_HELI_TILT           16.0f /* degrees of nose and roll at full input */
#define GAME_HELI_TILT_RATE      40.0f /* degrees/s */
#define GAME_HELI_SPOOL          2.0f  /* s for the rotor from still to full */
#define GAME_HELI_CEILING        3.0f  /* lowest z (highest point) it climbs to */
/* a landing faster than this (blocks/s, down) hurts it, and */
#define GAME_HELI_SAFE_LANDING   6.0f
/* every block/s over it costs this much health */
#define GAME_HELI_CRASH_DAMAGE   25.0f

typedef struct {
    float x, y, z;       /* the skids' bottom middle, map coordinates */
    float vx, vy, vz;    /* blocks/s */
    float yaw;           /* degrees, the nose's heading (0 = +x, 90 = +y) */
    float tilt_fwd;      /* degrees, nose down positive (drawn) */
    float tilt_side;     /* degrees, right side down positive (drawn) */
    float rotor;         /* 0..1, the rotor's speed */
    uint8_t wrecked;     /* health gone: no lift, it falls */
} heli_state;

typedef struct {
    int8_t forward;      /* -1 back, 0, 1 forward */
    int8_t strafe;       /* -1 left, 0, 1 right */
    int8_t lift;         /* -1 descend, 0 hold, 1 climb */
    uint8_t piloted;     /* someone is in seat 0 */
    float yaw;           /* degrees, the pilot's look */
} heli_input;

static inline float heli__approach(float v, float target, float step)
{
    if (v < target)
        return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

static inline float heli__angle_diff(float a, float b)
{
    float d = fmodf(b - a, 360.0f);
    if (d > 180.0f)
        d -= 360.0f;
    if (d < -180.0f)
        d += 360.0f;
    return d;
}

/* one step of dt seconds; moves state->x/y/z by the velocity (the caller
   clips the move against the map before or after, its choice, and must do
   the same on both sides) */
static inline void heli_step(heli_state* s, const heli_input* in, float dt)
{
    int powered = in->piloted && !s->wrecked;
    s->rotor = heli__approach(s->rotor, powered ? 1.0f : 0.0f, dt / GAME_HELI_SPOOL);
    int fwd = powered ? in->forward : 0, side = powered ? in->strafe : 0, lift = powered ? in->lift : 0;
    float want_f = GAME_HELI_TILT * (float) fwd, want_s = GAME_HELI_TILT * (float) side;
    s->tilt_fwd  = heli__approach(s->tilt_fwd, want_f, GAME_HELI_TILT_RATE * dt);
    s->tilt_side = heli__approach(s->tilt_side, want_s, GAME_HELI_TILT_RATE * dt);
    if (powered) {
        float d = heli__angle_diff(s->yaw, in->yaw), max = GAME_HELI_YAW_RATE * dt;
        s->yaw += d > max ? max : (d < -max ? -max : d);
        s->yaw = fmodf(s->yaw + 360.0f, 360.0f);
    }
    /* thrust along the tilt, scaled by the rotor */
    float r  = s->yaw * 3.14159265f / 180.0f;
    float ax = cosf(r), ay = sinf(r); /* nose */
    float bx = -ay, by = ax;          /* right */
    float tf = s->tilt_fwd / GAME_HELI_TILT, ts = s->tilt_side / GAME_HELI_TILT;
    s->vx += (ax * tf + bx * ts) * GAME_HELI_ACCEL * s->rotor * dt;
    s->vy += (ay * tf + by * ts) * GAME_HELI_ACCEL * s->rotor * dt;
    float drag = 1.0f - GAME_HELI_DRAG * dt;
    if (drag < 0.0f)
        drag = 0.0f;
    s->vx *= drag;
    s->vy *= drag;
    float h = sqrtf(s->vx * s->vx + s->vy * s->vy);
    if (h > GAME_HELI_MAX_SPEED) {
        s->vx *= GAME_HELI_MAX_SPEED / h;
        s->vy *= GAME_HELI_MAX_SPEED / h;
    }
    /* vertical: the rotor holds what the pilot asks; without it, gravity */
    float want_vz = lift > 0 ? -GAME_HELI_CLIMB : (lift < 0 ? GAME_HELI_SINK : 0.0f);
    float held    = heli__approach(s->vz, want_vz, GAME_HELI_VERT_ACCEL * dt);
    float fallen  = s->vz + GAME_HELI_GRAVITY * dt;
    s->vz = s->rotor * held + (1.0f - s->rotor) * fallen;
    s->x += s->vx * dt;
    s->y += s->vy * dt;
    s->z += s->vz * dt;
    if (s->z < GAME_HELI_CEILING) {
        s->z = GAME_HELI_CEILING;
        if (s->vz < 0.0f)
            s->vz = 0.0f;
    }
}

/* the health a touchdown at vertical speed vz (blocks/s, down positive) costs */
static inline int heli_impact(float vz)
{
    if (vz <= GAME_HELI_SAFE_LANDING)
        return 0;
    return (int) ((vz - GAME_HELI_SAFE_LANDING) * GAME_HELI_CRASH_DAMAGE);
}

#endif
