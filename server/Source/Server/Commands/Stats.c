// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Server/Stats.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <stdio.h>

void cmd_stats(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    player_t* player = arguments.player;
    if (arguments.argc == 2) {
        uint8_t id = 33;
        if (!parse_player(arguments.argv[1], &id, NULL)) {
            send_server_notice(arguments.player, arguments.console, "Usage: /stats [player]");
            return;
        }
        HASH_FIND(hh, server->players, &id, sizeof(id), player);
    } else if (arguments.console) {
        send_server_notice(arguments.player, arguments.console, "Usage: /stats <player>");
        return;
    }
    char line[256];
    stats_player_line(server, player, line, sizeof line);
    send_server_notice(arguments.player, arguments.console, "%s", line);
}
