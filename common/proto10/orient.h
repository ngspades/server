// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Orientation of a Player trajectory (docs/protocol-1.0.md 8.5).
 *
 * The wire carries euler angles; both of our programs think in a forward
 * vector (0.75 OrientationData). In map coordinates (x, y horizontal, z grows
 * downwards):
 *
 *   euler.x = pitch (positive looks down), euler.z = yaw, euler.y = roll (0)
 *   front   = (-cos(pitch) * sin(yaw), cos(pitch) * cos(yaw), sin(pitch))
 *
 * which is what the reference's Rz(yaw) * Rx(pitch) applied to (0, 1, 0)
 * gives with right-handed rotations. Header only so neither build needs a
 * new source file.
 */
#ifndef PROTO10_ORIENT_H
#define PROTO10_ORIENT_H

#include "buf.h"

#include <math.h>

static inline p10_vec3 p10_euler_from_front(float fx, float fy, float fz)
{
    p10_vec3 e;
    float len = sqrtf(fx * fx + fy * fy + fz * fz);
    if (!(len > 1e-6f)) {
        /* degenerate input: look along +y, level */
        e.x = e.y = e.z = 0.0f;
        return e;
    }
    fx /= len;
    fy /= len;
    fz /= len;
    if (fz > 1.0f)
        fz = 1.0f;
    if (fz < -1.0f)
        fz = -1.0f;
    e.x = asinf(fz);
    e.y = 0.0f;
    e.z = atan2f(-fx, fy);
    return e;
}

static inline p10_vec3 p10_front_from_euler(p10_vec3 e)
{
    p10_vec3 f;
    float cp = cosf(e.x);
    f.x      = -cp * sinf(e.z);
    f.y      = cp * cosf(e.z);
    f.z      = sinf(e.x);
    return f;
}

/*
 * Orientation of a non-Player trajectory (a sentry's barrel): the compact
 * quaternion of the same Rz(yaw) * Rx(pitch), so a rotation maps (0, 1, 0)
 * to the front vector exactly like the Player euler angles above.
 */
static inline p10_vec3 p10_rotation_from_euler(p10_vec3 e)
{
    float cz = cosf(e.z * 0.5f), sz = sinf(e.z * 0.5f);
    float cx = cosf(e.x * 0.5f), sx = sinf(e.x * 0.5f);
    /* w = cz * cx is never negative for |yaw|, |pitch| <= pi */
    return p10_rotation_compact(cz * sx, sz * sx, sz * cx, cz * cx);
}

static inline p10_vec3 p10_front_from_rotation(p10_vec3 q)
{
    float n2 = q.x * q.x + q.y * q.y + q.z * q.z;
    float w  = n2 < 1.0f ? sqrtf(1.0f - n2) : 0.0f;
    p10_vec3 f;
    /* the second column of the rotation matrix: R * (0, 1, 0) */
    f.x = 2.0f * (q.x * q.y - w * q.z);
    f.y = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    f.z = 2.0f * (q.y * q.z + w * q.x);
    return f;
}

#endif
