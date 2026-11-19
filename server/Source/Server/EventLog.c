// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See EventLog.h for the format contract. */

static int8_t evt_state = -1; /* -1 unknown, 0 off, 1 on, 2 verbose */

static void evt_init(void)
{
    /* off unless asked for (SP_EVENTLOG=1, or all for the per-frame lines;
       the test harness asks): with 128 players it is a lot of writing, and
       a public server does not need it */
    const char* env = getenv("SP_EVENTLOG");
    if (env == NULL || env[0] == 0) {
        evt_state = 0;
        return;
    }
    if (strcmp(env, "0") == 0 || strcmp(env, "off") == 0) {
        evt_state = 0;
    } else if (strcmp(env, "all") == 0 || strcmp(env, "verbose") == 0) {
        evt_state = 2;
    } else {
        evt_state = 1;
    }
}

uint8_t evt_enabled(void)
{
    if (evt_state < 0) {
        evt_init();
    }
    return evt_state > 0;
}

uint8_t evt_verbose(void)
{
    if (evt_state < 0) {
        evt_init();
    }
    return evt_state > 1;
}

void evt_actor(char* out, size_t out_size, const player_t* player)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if (player == NULL) {
        snprintf(out, out_size, "console");
        return;
    }
    snprintf(out, out_size, "%s%u:%s", player->is_bot ? "bot" : "p", player->id, player->name);
}

/* EVT <ms since start> <actor> <event> <details> */
static void evt_emit(const player_t* player, const char* event, const char* fmt, va_list args)
{
    if (!evt_enabled()) {
        return;
    }

    char actor[EVT_ACTOR_LEN];
    evt_actor(actor, sizeof(actor), player);

    char details[512];
    details[0] = 0;
    if (fmt != NULL) {
        vsnprintf(details, sizeof(details), fmt, args);
    }

    /* the game log already prefixes a wall clock; a monotonic stamp makes the
       ordering of events inside one tick unambiguous */
    LOG_INFO("EVT %llu %s %s %s", (unsigned long long) (get_nanos() / NANO_IN_MILLI), actor, event, details);
}

void evt_log(const player_t* player, const char* event, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    evt_emit(player, event, fmt, args);
    va_end(args);
}

void evt_log_xy(const player_t* player, const char* event, float x, float y)
{
    evt_log(player, event, "x=%.2f y=%.2f", x, y);
}

void evt_log_cell(const player_t* player, const char* event, const char* key, int x, int y, int z)
{
    evt_log(player, event, "%s=%d,%d,%d", key, x, y, z);
}

void evt_spawn(const player_t* player)
{
    evt_log(player, "spawn", "team=%u hp=%d pos=%.1f,%.1f,%.1f cell=%d,%d,%d", player->team, player->hp,
            player->movement.position.x, player->movement.position.y, player->movement.position.z,
            (int) player->movement.position.x, (int) player->movement.position.y, (int) player->movement.position.z);
}

void evt_move(const player_t* player, const char* source)
{
    if (!evt_verbose()) {
        return; /* per-frame lines: only with SP_EVENTLOG=all */
    }
    evt_log(player, "move", "src=%s pos=%.1f,%.1f,%.1f vel=%.2f,%.2f,%.2f onground=%u water=%u", source,
            player->movement.position.x, player->movement.position.y, player->movement.position.z,
            player->movement.velocity.x, player->movement.velocity.y, player->movement.velocity.z,
            !player->airborne, player->wade);
}

void evt_damage(const player_t* attacker, const player_t* victim, long amount, uint8_t hit_type)
{
    char a[EVT_ACTOR_LEN];
    evt_actor(a, sizeof(a), attacker);
    evt_log(victim, "damage", "from=%s amount=%ld hit_type=%u hp_after=%d pos=%.1f,%.1f,%.1f", a, amount, hit_type,
            victim->hp, victim->movement.position.x, victim->movement.position.y, victim->movement.position.z);
}

void evt_death(const player_t* victim, const player_t* killer, uint8_t reason)
{
    char k[EVT_ACTOR_LEN];
    evt_actor(k, sizeof(k), killer);
    evt_log(victim, "death", "killer=%s reason=%u pos=%.1f,%.1f,%.1f", k, reason, victim->movement.position.x,
            victim->movement.position.y, victim->movement.position.z);
}

void evt_block(const player_t* player, const char* action, int x, int y, int z, uint8_t ok, uint32_t blocks)
{
    evt_log(player, "block", "action=%s cell=%d,%d,%d accepted=%u blocks_left=%u bot=%u", action, x, y, z, ok,
            blocks, player->is_bot);
}

void evt_weapon(const player_t* player, const char* what, uint8_t weapon)
{
    evt_log(player, "weapon", "what=%s weapon=%u clip=%u reserve=%u", what, weapon, player->weapon_clip,
            player->weapon_reserve);
}

void evt_bot_phase(const player_t* bot, const char* phase, float skill)
{
    evt_log(bot, "bot_phase", "phase=%s skill=%.2f pos=%.1f,%.1f,%.1f", phase, skill, bot->movement.position.x,
            bot->movement.position.y, bot->movement.position.z);
}

void evt_bot_target(const player_t* bot, const player_t* target, float distance, uint8_t visible)
{
    char t[EVT_ACTOR_LEN];
    evt_actor(t, sizeof(t), target);
    evt_log(bot, "bot_target", "target=%s dist=%.1f visible=%u", t, distance, visible);
}

void evt_bot_water(const player_t* bot, int x, int y, uint8_t is_water)
{
    evt_log(bot, "bot_water", "column=%d,%d water=%u", x, y, is_water);
}
