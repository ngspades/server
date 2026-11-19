// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Commands/Commands.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Proto10/Crates.h>
#include <Server/Server.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>

/* /crate <ammo|health> [player]: a supply crate falls on the player (on
   oneself without one), as a plugin's reward does; for tests and events */
void cmd_crate(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint8_t   type   = arguments.argc >= 2 ? crates_type(arguments.argv[1]) : 0;
    if (type == 0 || arguments.argc > 3) {
        send_server_notice(arguments.player, arguments.console, "Usage: /crate <ammo|health> [player]");
        return;
    }
    player_t* target = arguments.player;
    if (arguments.argc == 3) {
        uint8_t id = 0;
        target     = NULL;
        if (parse_player(arguments.argv[2], &id, NULL)) {
            HASH_FIND(hh, server->players, &id, sizeof(id), target);
        }
    }
    if (target == NULL || (arguments.console && arguments.argc < 3)) {
        send_server_notice(arguments.player, arguments.console, "No such player");
        return;
    }
    if (!crates_drop(server, target->movement.position.x, target->movement.position.y, 0.0f, type, "command")) {
        send_server_notice(arguments.player, arguments.console, "No crate: too many out, or no ground there");
    }
}
