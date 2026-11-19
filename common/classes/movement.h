// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What a class's figures mean, shared by the server's physics
 * (server/Source/Server/Classes.c, which the movers and the bots call) and
 * the client's prediction (the client's classes.c, player_move): the walk, the
 * sprint, the water's drag, the gun's raise, the spade's swing, the right
 * button's pace and the fall damage.
 *
 * The figures themselves are one table (common/classes/classes.h), the
 * server's; these are the arithmetic on them and the numbers a player with
 * no class falls back to. Both programs used to spell every one of them
 * twice, and a rule changed on one side only made the client predict a jump
 * or a fall the server did not play (a position_corrected every tick, or
 * fall damage taken on the client that the server never sent).
 *
 * Header only, std C99, no dependency on either program. A file that
 * includes it and uses nothing gets no warning (the server builds -Werror).
 */
#ifndef SHARED_CLASSES_MOVEMENT_H
#define SHARED_CLASSES_MOVEMENT_H

#include <classes/classes.h>
#include <math.h>

/* a player with no class: the classic game's walk, water drag and paces */
#define GAME_CLASS_WALK           1.0f
#define GAME_CLASS_WATER_FRICTION 6.0f

/* the ground acceleration a class walks with (the classic 1) */
static inline float game_class_speed(const struct game_class_info* c) {
    return c != NULL ? c->speed : GAME_CLASS_WALK;
}

/* the sprint's acceleration, which REPLACES the walk (0: the classic 1.3) */
static inline float game_class_sprint(const struct game_class_info* c) {
    return c != NULL && c->sprint > 0.0f ? c->sprint : 0.0f;
}

/* the water's drag; the classic game's 6 without a class */
static inline float game_class_water_friction(const struct game_class_info* c) {
    return c != NULL && c->water_friction > 0.0f ? c->water_friction : GAME_CLASS_WATER_FRICTION;
}

/* the jump's velocity (negative is up); no_class is the caller's own figure,
   the server's [gameplay] jump_velocity or the one the server sent it */
static inline float game_class_jump(const struct game_class_info* c, float no_class) {
    return c != NULL ? c->jump : no_class;
}

/* seconds between two right button digs; the classic 1 with no class */
static inline float game_class_dig_delay(const struct game_class_info* c) {
    return c != NULL && c->dig_delay > 0.01f ? c->dig_delay : GAME_DIG_DELAY;
}

/* seconds a spade swing takes; the classic GAME_SWING with no class */
static inline float game_class_swing(const struct game_class_info* c) {
    return c != NULL && c->swing > 0.01f ? c->swing : GAME_SWING;
}

/* A classed player's fall damage is the distance flown down, not the landing
   speed: linear from fall_min to fall_max, fall_dmg there and beyond, times
   fall_water on a water landing. 0 when the class takes none; -1 when the
   player has no class, whose fall damage is the classic quadratic in the
   landing velocity (the caller's). */
static inline long game_class_fall_damage(const struct game_class_info* c, float dist, int wade) {
    if (c == NULL) {
        return -1;
    }
    if (c->fall_dmg <= 0.0f || dist < c->fall_min) {
        return 0;
    }
    float t = c->fall_max > c->fall_min ? (dist - c->fall_min) / (c->fall_max - c->fall_min) : 1.0f;
    if (t > 1.0f) {
        t = 1.0f;
    }
    float damage = c->fall_dmg * t;
    if (wade) {
        damage *= c->fall_water;
    }
    long out = (long) ceilf(damage);
    return out > 0 ? out : 0;
}

#endif
