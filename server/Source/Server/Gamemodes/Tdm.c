// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/Gamemodes/Tdm.h>
#include <Server/Server.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>

#define TDM_SCORE_LIMIT 100
#define TDM_SECONDS     900

static struct
{
    uint64_t start;
    uint64_t deadline;
    uint8_t  ended;
} tdm;

static void _tdm_init(server_t* server)
{
    server->protocol.gamemode.intel_flags   = 0;
    server->protocol.gamemode.intel_held[0] = 0;
    server->protocol.gamemode.intel_held[1] = 0;
    server->protocol.gamemode.score_limit   = TDM_SCORE_LIMIT;
    server->protocol.gamemode.score[0]      = 0;
    server->protocol.gamemode.score[1]      = 0;
    tdm.start    = get_nanos();
    tdm.deadline = tdm.start + (uint64_t) gamemode_match_seconds("tdm", TDM_SECONDS) * NANO_IN_SECOND;
    tdm.ended    = 0;
    int seconds = (int) ((tdm.deadline - tdm.start) / NANO_IN_SECOND);
    LOG_INFO("Team deathmatch: %d kills, %d s", TDM_SCORE_LIMIT, seconds);
    evt_log(NULL, "tdm", "action=start limit=%d seconds=%d", TDM_SCORE_LIMIT, seconds);
}

static void _tdm_tick(server_t* server)
{
    if (tdm.ended) {
        return;
    }
    if (server->protocol.gamemode.score[0] >= TDM_SCORE_LIMIT ||
        server->protocol.gamemode.score[1] >= TDM_SCORE_LIMIT || get_nanos() >= tdm.deadline)
    {
        int winner = server->protocol.gamemode.score[0] == server->protocol.gamemode.score[1]
                         ? -1
                         : (server->protocol.gamemode.score[0] > server->protocol.gamemode.score[1] ? TEAM_A : TEAM_B);
        tdm.ended = 1;
        evt_log(NULL, "tdm", "action=end winner=%d score=%u-%u", winner, server->protocol.gamemode.score[0],
                server->protocol.gamemode.score[1]);
        gamemode_match_won(server);
    }
}

static void _tdm_killed(server_t* server, player_t* killer, player_t* victim)
{
    if (killer == NULL || killer == victim || killer->team == TEAM_SPECTATOR || killer->team == victim->team ||
        tdm.ended) {
        return;
    }
    if (server->protocol.gamemode.score[killer->team] < 255) {
        server->protocol.gamemode.score[killer->team]++;
    }
}

static int _tdm_time_left(server_t* server)
{
    (void) server;
    return tdm.ended ? 0 : gamemode_seconds_until(tdm.deadline);
}

static int _tdm_extend(server_t* server, int seconds)
{
    tdm.deadline += (uint64_t) seconds * NANO_IN_SECOND;
    server->match_extra_seconds += (uint32_t) seconds;
    evt_log(NULL, "tdm", "action=extend seconds=%d", seconds);
    return 1;
}

static int _tdm_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    return gamemode_hunt_nearest(server, bot, out);
}

const gamemode_ops_t tdm_ops = {
    .id            = GAME_MODE_TDM,
    .name          = "tdm",
    .wire_name     = "ctf", /* the client already draws two team scores */
    .squad_teams   = SQUAD_TEAMS_BOTH, /* squads, without the respawn by a mate */
    .init          = _tdm_init,
    .tick          = _tdm_tick,
    .extend        = _tdm_extend,
    .time_left     = _tdm_time_left,
    .bot_goal      = _tdm_bot_goal,
    .player_killed = _tdm_killed,
};
