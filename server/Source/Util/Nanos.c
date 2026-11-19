// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Server.h>
#include <Util/Enums.h>
#include <Util/Nanos.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t get_real_nanos(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* SP_TIME_SCALE (tests only): the game's clock runs that many times faster
   than the real one, and the loop ticks as often, so a 240 s bot match is
   measured in a few real seconds. Read once, at the first call (from the main
   thread at start-up, before the planner thread starts) */
static double   time_scale = -1.0;
static uint64_t time_base;

double get_time_scale(void)
{
    if (time_scale < 0.0) {
        const char* env = getenv("SP_TIME_SCALE");
        double      s   = env != NULL ? atof(env) : 1.0;
        time_scale      = s >= 1.0 && s <= 1000.0 ? s : 1.0;
        if (!get_fast_mode()) {
            time_base = get_real_nanos(); /* SP_FAST keeps its whole second */
        }
    }
    return time_scale;
}

/* SP_FAST (tests only): the game's clock is the count of ticks, 1/60 s each,
   and the loop runs them back to back with no sleep: a match takes as long
   as the CPU needs, and the same seed plays the same match (the planner
   answers on the tick too, BotPlanner.c). Read once at start-up */
static int               fast_mode = -1;
static volatile uint64_t fast_ticks;

int get_fast_mode(void)
{
    if (fast_mode < 0) {
        const char* env = getenv("SP_FAST");
        fast_mode       = env != NULL && *env && strcmp(env, "0") != 0;
        /* from a fixed moment, not the start's: the timers kept in whole
           seconds (get_game_seconds: respawn, restock) turned at a tick that
           depended on the start's fraction of a second, and the overwatch's
           sweep (now % an hour) on the hour of the day, so the same seed
           played another match */
        if (fast_mode) {
            time_base = 1700000000ULL * 1000000000ULL;
        } else if (time_base == 0) {
            time_base = get_real_nanos();
        }
    }
    return fast_mode;
}

void fast_tick(void)
{
    fast_ticks++;
}

uint64_t get_nanos(void)
{
    if (get_fast_mode()) {
        return time_base + fast_ticks * (uint64_t) (NANO_60TPS);
    }
    double scale = get_time_scale();
    if (scale == 1.0) {
        return get_real_nanos();
    }
    return time_base + (uint64_t) ((double) (get_real_nanos() - time_base) * scale);
}

uint64_t get_game_seconds(void)
{
    return get_nanos() / 1000000000ULL;
}
