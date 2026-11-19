// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The supply crates' air drop, after the retail game's Crate.update: the
 * figures BattleSpades recovered (include/battlespades/world/retail_effects.hpp:
 * CRATE_PARACHUTE_*, constants.py 3043-3049, and a live trace of a 214 block
 * drop at about 5.1 s) and the same order of operations (src/world/
 * retail_effects.cpp, step_crate_drop).
 *
 *   - the crate falls at GAME_CRATE_GRAVITY (the retail GenericMovement
 *     gravity, 30 blocks a second squared);
 *   - its parachute opens GAME_CRATE_CHUTE_OPEN blocks above the ground and
 *     multiplies the speed by GAME_CRATE_CHUTE_DRAG on every 1/60 s step, so
 *     it settles at 2 blocks a second;
 *   - the parachute is released GAME_CRATE_CHUTE_RELEASE blocks above the
 *     ground and the crate falls the rest freely;
 *   - it bounces at GAME_CRATE_BOUNCE of the impact speed when it meets the
 *     ground faster than GAME_CRATE_BOUNCE_MIN, and rests otherwise.
 *
 * The client runs this to draw the drop, as the retail client does (the
 * server announces the crate in the air and sends nothing while it falls);
 * the server runs the same steps once per tick, only to know where the crate
 * is for the pickup. One definition, so the two never disagree.
 *
 * Header only: std C99, no dependency on either program. z is the map's:
 * grows downwards, so a falling crate has a positive GAME_CRATE velocity.
 */
#ifndef SHARED_CRATE_DROP_H
#define SHARED_CRATE_DROP_H

#define GAME_CRATE_GRAVITY        30.0f
#define GAME_CRATE_CHUTE_OPEN     10.0f
#define GAME_CRATE_CHUTE_RELEASE  2.0f
#define GAME_CRATE_CHUTE_DRAG     0.75f
#define GAME_CRATE_BOUNCE         0.5f
#define GAME_CRATE_BOUNCE_MIN     2.0f
#define GAME_CRATE_STEP           (1.0f / 60.0f)

typedef struct {
    float         vz;       /* blocks a second, positive is down */
    unsigned char chute;    /* deployed */
    unsigned char released; /* deployed and gone: it falls freely again */
} game_crate_drop_t;

/* One 1/60 s step towards a ground at `support` (the map z of the block's
   top). 1 when the crate met the ground, *impact holding the speed it did,
   and *z put on the support; 0 while it still falls. A caller that bounces
   sets vz = -impact * GAME_CRATE_BOUNCE and steps again. */
static inline int game_crate_drop_step(float* z, game_crate_drop_t* d, float support, float* impact) {
    d->vz += GAME_CRATE_GRAVITY * GAME_CRATE_STEP;
    *z += d->vz * GAME_CRATE_STEP;
    if (*z >= support) {
        *z      = support;
        *impact = d->vz;
        return 1;
    }
    float above = support - *z;
    if (!d->chute && above < GAME_CRATE_CHUTE_OPEN) {
        d->chute = 1;
    }
    if (d->chute && !d->released && above < GAME_CRATE_CHUTE_RELEASE) {
        d->released = 1;
    }
    if (d->chute && !d->released) {
        d->vz *= GAME_CRATE_CHUTE_DRAG;
    }
    *impact = 0.0f;
    return 0;
}

/* seconds from `from` to the first touch of `support`, the chute included */
static inline float game_crate_fall_seconds(float from, float support) {
    game_crate_drop_t d = {0};
    float             z = from, impact = 0.0f, t = 0.0f;
    while (t < 120.0f) {
        if (game_crate_drop_step(&z, &d, support, &impact)) {
            return t + GAME_CRATE_STEP;
        }
        t += GAME_CRATE_STEP;
    }
    return t;
}

#endif
