#ifndef NANOS_H
#define NANOS_H

#include <Util/Types.h>

/* the game's clock: every timer of the game (physics, bots, perks, modes).
   Runs SP_TIME_SCALE times faster than the real one in tests */
uint64_t get_nanos(void);
/* the same clock in whole seconds (respawn waits, restock timers) */
uint64_t get_game_seconds(void);
/* the real clock: network deadlines (a joining client is in real time) and
   performance measurements (a tick's real cost) */
uint64_t get_real_nanos(void);
/* SP_TIME_SCALE, 1 when unset */
double get_time_scale(void);
/* SP_FAST: the game's clock counts ticks and the loop never sleeps
   (headless benchmarks, deterministic with SP_RAND_SEED) */
int  get_fast_mode(void);
/* one tick of game time, from the main loop in SP_FAST */
void fast_tick(void);

#endif
