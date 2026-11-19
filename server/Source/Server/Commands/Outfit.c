// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Commands/Commands.h>
#include <Server/Outfit.h>
#include <Server/ParseConvert.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* /outfit <player> <outfit> [colour] or /outfit team <1|2> <outfit>: how a
   player or a team looks (Outfit.c, the draft's numbers: 0 soldier, 7
   miner, 64 zombie, 255 the team's), for events and tests */
void cmd_outfit(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    char      line[96];
    if (profile_is_075()) {
        send_server_notice(arguments.player, arguments.console, "Outfits are protocol 1.0's");
        return;
    }
    if (arguments.argc == 4 && !strcmp(arguments.argv[1], "team")) {
        int team = atoi(arguments.argv[2]) - 1, outfit = atoi(arguments.argv[3]);
        if (!outfit_set_team(server, team, outfit, "command")) {
            send_server_notice(arguments.player, arguments.console, "Usage: /outfit team <1|2> <0..254>");
            return;
        }
        snprintf(line, sizeof line, "Team %d wears outfit %d", team + 1, outfit);
        send_server_notice(arguments.player, arguments.console, line);
        return;
    }
    player_t* target = NULL;
    uint8_t   id     = 0;
    if ((arguments.argc == 3 || arguments.argc == 4) && parse_player(arguments.argv[1], &id, NULL)) {
        HASH_FIND(hh, server->players, &id, sizeof(id), target);
    }
    int outfit = arguments.argc >= 3 ? atoi(arguments.argv[2]) : -1;
    int colour = arguments.argc == 4 ? atoi(arguments.argv[3]) : 255;
    if (target == NULL || !outfit_set(server, target, outfit, colour, "command")) {
        send_server_notice(arguments.player,
                           arguments.console,
                           "Usage: /outfit <player> <0..255> [colour 0|1|255] or /outfit team <1|2> <0..254>");
        return;
    }
    snprintf(line, sizeof line, "%s wears outfit %d", target->name, outfit);
    send_server_notice(arguments.player, arguments.console, line);
}
