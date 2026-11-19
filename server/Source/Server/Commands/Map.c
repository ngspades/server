// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Commands/CommandManager.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/MapDownload.h>
#include <Server/Server.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _reload(server_t* server, const char* why)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_DISCONNECTED) {
            p->state = STATE_STARTING_MAP;
        }
    }
    broadcast_server_notice_kind(server, 0, NOTICE_INFO, "Loading %s", server->forced_map[0] ? server->forced_map : "the next map");
    evt_log(NULL, "map", "action=%s map=%s", why, server->forced_map[0] ? server->forced_map : "next");
    server_reset(server);
}

void cmd_nextmap(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    (void) arguments;
    server->forced_map[0] = '\0';
    _reload(server, "nextmap");
}

/* the match over now: the summary on every 1.0 screen, then the next map
   (gamemode_match_won). With a team, that team wins: its score one over
   the other's if it was not ahead */
void cmd_endmatch(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint8_t*  score  = server->protocol.gamemode.score;
    if (arguments.argc > 2 ||
        (arguments.argc == 2 && strcmp(arguments.argv[1], "1") != 0 && strcmp(arguments.argv[1], "2") != 0))
    {
        send_server_notice(arguments.player, arguments.console, "Usage: /endmatch [1|2]");
        return;
    }
    int team = arguments.argc == 2 ? atoi(arguments.argv[1]) - 1 : -1;
    if (team >= 0 && score[team] <= score[1 - team] && score[1 - team] < 255) {
        score[team] = score[1 - team] + 1;
    }
    evt_log(NULL, "match", "action=end_command team=%d", team);
    gamemode_match_won(server);
}

void cmd_map(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.argc != 2) {
        send_server_notice(arguments.player, arguments.console, "Usage: /map <Name>");
        return;
    }
    /* the folder the name means, so a download of it starts under the
       repository's name and the notice names the map that will load */
    const char* name = arguments.argv[1];
    char        folder[MAP_NAME_MAX + 1];
    if (map_resolve_alias(name, folder, sizeof folder, 0)) {
        name = folder;
    }
    if (!server_force_map(server, name)) {
        /* not installed: fetched on a thread of its own (never on the tick),
           and the admin asks again once it is there */
        const char* pending = map_download_pending();
        if (*pending != '\0') {
            send_server_notice(arguments.player, arguments.console, "Map '%s' not found (map %s is downloading)",
                               name, pending);
        } else if (map_download_background(name)) {
            send_server_notice(arguments.player, arguments.console,
                               "Map '%s' is not installed: downloading it, /map %s again in a few seconds", name,
                               name);
        } else {
            send_server_notice(arguments.player, arguments.console, "Map '%s' not found", name);
        }
        return;
    }
    _reload(server, "map");
}

void cmd_restart(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    (void) arguments;
    if (!server_force_map(server, server->map_name)) {
        send_server_notice(arguments.player, arguments.console, "The current map '%s' is not on disk", server->map_name);
        return;
    }
    _reload(server, "restart");
}

void cmd_extend(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.argc != 2) {
        send_server_notice(arguments.player, arguments.console, "Usage: /extend <minutes>");
        return;
    }
    int minutes = atoi(arguments.argv[1]);
    if (minutes < 1 || minutes > 240) {
        send_server_notice(arguments.player, arguments.console, "Minutes must be between 1 and 240");
        return;
    }
    if (!gamemode_extend(server, minutes * 60)) {
        send_server_notice(arguments.player, arguments.console, "This mode has no timed match to extend");
        return;
    }
    send_server_notice(arguments.player, arguments.console, "Match extended by %d minute(s)", minutes);
}

void cmd_time(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    unsigned  seconds = (unsigned) ((get_nanos() - server->match_started_at) / NANO_IN_SECOND);
    send_server_notice(arguments.player, arguments.console, "Match time: %u:%02u", seconds / 60, seconds % 60);
}

void cmd_score(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    send_server_notice(arguments.player, arguments.console, "%s %u - %u %s (limit %u)", server->protocol.name_team[0],
                       server->protocol.gamemode.score[0], server->protocol.gamemode.score[1],
                       server->protocol.name_team[1], server->protocol.gamemode.score_limit);
}
