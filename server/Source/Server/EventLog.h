// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EVENTLOG_H
#define EVENTLOG_H

#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

/* Structured gameplay logging.
 *
 * Goal: every meaningful action of every actor (human or bot) shows up in one
 * line with the same shape, so a match can be replayed from the log alone:
 *
 *   EVT <tick> <actor> <event> key=value key=value ...
 *
 * <actor> is "p<id>:<name>" for players and "bot<id>:<name>" for bots, always
 * carrying the id, the name and the team so lines can be correlated even when
 * names repeat. Positions are printed as floating point x,y,z in server space
 * plus the rounded block cell, which is what walls and raycasts operate on.
 *
 * Set the environment variable SP_EVENTLOG=0 to silence it (the normal log
 * stays untouched either way). SP_EVENTLOG=all additionally emits the noisy
 * per-frame movement lines. */

#define EVT_ACTOR_LEN 48

/* builds "bot3:BotCharlie" / "p4:Deuce"; never fails, always NUL terminated */
void      evt_actor(char* out, size_t out_size, const player_t* player);
uint8_t   evt_enabled(void);
uint8_t   evt_verbose(void);

void      evt_log(const player_t* player, const char* event, const char* fmt, ...);
void      evt_log_xy(const player_t* player, const char* event, float x, float y); /* only x/y, for column probes */
void      evt_log_cell(const player_t* player, const char* event, const char* key, int x, int y, int z);

/* domain helpers, so the call sites stay one liners */
void      evt_spawn(const player_t* player);
void      evt_move(const player_t* player, const char* source);
void      evt_damage(const player_t* attacker, const player_t* victim, long amount, uint8_t hit_type);
void      evt_death(const player_t* victim, const player_t* killer, uint8_t reason);
void      evt_block(const player_t* player, const char* action, int x, int y, int z, uint8_t ok, uint32_t blocks);
void      evt_weapon(const player_t* player, const char* what, uint8_t weapon);
void      evt_bot_phase(const player_t* bot, const char* phase, float skill);
void      evt_bot_target(const player_t* bot, const player_t* target, float distance, uint8_t visible);
void      evt_bot_water(const player_t* bot, int x, int y, uint8_t is_water);

#endif /* EVENTLOG_H */
