// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Gamemodes/LuaMode.h>

#ifndef ENABLE_LUA

const gamemode_ops_t* lua_mode_find(const char* name)
{
    (void) name;
    return NULL;
}

const gamemode_ops_t* lua_mode_ops(void)
{
    return NULL;
}

int lua_mode_set_time_limit(int seconds)
{
    (void) seconds;
    return 0;
}

int lua_mode_time_left(void)
{
    return -1;
}

#else

#include <Server/EventLog.h>
#include <Server/Gamemodes/Ctf.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/Plugins.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* the plugin played as the mode; its events (on_kill, on_spawn, on_tick...)
   reach it with every other plugin's, through Plugins.c */
static struct
{
    plugin_t* plugin;
    char      name[32];
    char      wire[16];
    /* rules = "ctf": the script plays over capture the flag's rules (the
       intels, tents, pickup and capture, the bots' CTF goals) and moves
       the intels and tents (push.lua) */
    uint8_t rules_ctf;
    /* the time limit: the match's end by the clock (0 none) */
    uint64_t deadline;
    uint8_t  time_over;
} lm;

/* its squads are read from the script at each init (below) */
static gamemode_ops_t lua_ops;

static void _lua_init(server_t* server)
{
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    /* a fresh state each match: the script starts over like a mode does */
    lm.plugin = plugins_load_mode(server, lm.name);
    if (lm.plugin == NULL) {
        LOG_ERROR("Lua mode %s: not loaded, the server plays CTF", lm.name);
        server->protocol.current_gamemode = GAME_MODE_CTF;
        gamemode_ops_of(GAME_MODE_CTF)->init(server);
        return;
    }
    char rules[16];
    lm.rules_ctf = plugins_mode_field(lm.plugin, "rules", rules, sizeof rules) != NULL && !strcmp(rules, "ctf");
    if (plugins_mode_field(lm.plugin, "wire", lm.wire, sizeof lm.wire) == NULL) {
        snprintf(lm.wire, sizeof(lm.wire), "ctf");
    }
    lm.deadline  = 0;
    lm.time_over = 0;
    double limit = 0;
    if (plugins_mode_number(lm.plugin, "time_limit", &limit) && limit >= 1 && limit <= 86400) {
        lm.deadline = get_nanos() + (uint64_t) limit * NANO_IN_SECOND;
    }
    /* squads = true: the generic squads (a mode on the CTF's rules has the
       CTF's own); squad_spawn = true adds the respawn by a mate and the rally */
    lua_ops.squad_teams = !lm.rules_ctf && plugins_mode_flag(lm.plugin, "squads") ? SQUAD_TEAMS_BOTH : 0;
    if (lua_ops.squad_teams != 0 && plugins_mode_flag(lm.plugin, "squad_spawn")) {
        lua_ops.features |= GM_FEAT_SQUAD_SPAWN;
    } else {
        lua_ops.features &= (uint8_t) ~GM_FEAT_SQUAD_SPAWN;
    }
    evt_log(NULL, "lua", "action=init mode=%s rules=%s time_limit=%d", lm.name, lm.rules_ctf ? "ctf" : "own",
            (int) limit);
    if (lm.rules_ctf) {
        ctf_ops.init(server); /* the intels and tents, then the script moves them */
    }
    plugins_mode_init(lm.plugin);
}

static void _lua_tick(server_t* server)
{
    if (lm.rules_ctf && ctf_ops.tick != NULL) {
        ctf_ops.tick(server);
    }
    if (lm.deadline != 0 && !lm.time_over && get_nanos() >= lm.deadline) {
        lm.time_over = 1;
        evt_log(NULL, "lua", "action=time_over mode=%s score=%u-%u", lm.name, server->protocol.gamemode.score[0],
                server->protocol.gamemode.score[1]);
        gamemode_match_won(server);
    }
}

/* /extend: more time for a match that has a limit */
static int _lua_extend(server_t* server, int seconds)
{
    if (lm.deadline == 0 || lm.time_over) {
        return 0;
    }
    lm.deadline += (uint64_t) seconds * NANO_IN_SECOND;
    server->match_extra_seconds += (uint32_t) seconds;
    evt_log(NULL, "lua", "action=extend mode=%s seconds=%d", lm.name, seconds);
    return 1;
}

static int _lua_spawn_point(server_t* server, player_t* player)
{
    if (lm.rules_ctf && ctf_ops.spawn_point != NULL) {
        return ctf_ops.spawn_point(server, player); /* squads */
    }
    return 0; /* the team's spawn, as usual */
}

static void _lua_player_out(server_t* server, player_t* player)
{
    if (lm.rules_ctf && ctf_ops.player_out != NULL) {
        ctf_ops.player_out(server, player); /* the intel drops */
    }
}

/* the intel's carrier, where the mode plays capture the flag */
static int _lua_player_carries(server_t* server, player_t* player)
{
    return lm.rules_ctf && ctf_ops.player_carries != NULL && ctf_ops.player_carries(server, player);
}

/* the script's objective (game.objective), else the nearest enemy */
static int _lua_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    float ox, oy, or;
    if (!plugins_mode_objective(&ox, &oy, &or)) {
        return lm.rules_ctf ? ctf_ops.bot_goal(server, bot, out) : gamemode_hunt_nearest(server, bot, out);
    }
    memset(out, 0, sizeof(*out));
    gm_goal_name(out, "objective", 0);
    /* each bot its own spot inside, or they all queue on one cell */
    float a      = (float) bot->id * 2.399963f; /* the golden angle */
    float r      = or * 0.6f * (float) ((bot->id % 4) + 1) / 4.0f;
    out->x       = ox + cosf(a) * r;
    out->y       = oy + sinf(a) * r;
    out->hold    = 1.5f;
    out->watch_x = out->x + cosf(a) * 10.0f;
    out->watch_y = out->y + sinf(a) * 10.0f;
    return 1;
}

/* rules = "ctf": capture the flag's own hooks under the script */
static void _lua_player_step(server_t* server, player_t* player)
{
    if (lm.rules_ctf && ctf_ops.player_step != NULL) {
        ctf_ops.player_step(server, player);
    }
}

static void _lua_terrain_changed(server_t* server, int built)
{
    if (lm.rules_ctf && ctf_ops.terrain_changed != NULL) {
        ctf_ops.terrain_changed(server, built);
    }
}

static void _lua_state_keys(server_t* server, gm_keys_t* out)
{
    if (lm.rules_ctf && ctf_ops.state_keys != NULL) {
        ctf_ops.state_keys(server, out);
    }
}

static void _lua_client_in_game(server_t* server, player_t* player)
{
    if (lm.rules_ctf && ctf_ops.client_in_game != NULL) {
        ctf_ops.client_in_game(server, player);
    }
}

static int _lua_player_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    if (lm.rules_ctf && ctf_ops.player_command != NULL) {
        return ctf_ops.player_command(server, player, command, arg);
    }
    return 0;
}

static gamemode_ops_t lua_ops = {
.id              = GAME_MODE_LUA,
.name            = lm.name,
.wire_name       = lm.wire,
.init            = _lua_init,
.tick            = _lua_tick,
.extend          = _lua_extend,
.spawn_point     = _lua_spawn_point,
.player_out      = _lua_player_out,
.bot_goal        = _lua_bot_goal,
.player_carries  = _lua_player_carries,
.player_step     = _lua_player_step,
.terrain_changed = _lua_terrain_changed,
.state_keys      = _lua_state_keys,
.client_in_game  = _lua_client_in_game,
.player_command  = _lua_player_command,
};

const gamemode_ops_t* lua_mode_find(const char* name)
{
    if (!plugins_exists(name)) {
        return NULL;
    }
    snprintf(lm.name, sizeof(lm.name), "%s", name);
    snprintf(lm.wire, sizeof(lm.wire), "ctf");
    return &lua_ops;
}

const gamemode_ops_t* lua_mode_ops(void)
{
    return lm.name[0] ? &lua_ops : NULL;
}

int lua_mode_set_time_limit(int seconds)
{
    if (!lm.name[0] || lm.plugin == NULL) {
        return 0;
    }
    lm.time_over = 0;
    lm.deadline  = seconds > 0 ? get_nanos() + (uint64_t) seconds * NANO_IN_SECOND : 0;
    return 1;
}

int lua_mode_time_left(void)
{
    if (lm.deadline == 0) {
        return -1;
    }
    uint64_t now = get_nanos();
    return now >= lm.deadline ? 0 : (int) ((lm.deadline - now) / NANO_IN_SECOND);
}

#endif
