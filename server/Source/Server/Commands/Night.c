// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Commands/Commands.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Notice.h>
#include <string.h>

/* /night [on|off]: Daytime and Weather's time of day for every 0.75 client
   that agreed (the 0.75 profile's: [ext075] night). Without an argument it
   says which it is */
void cmd_night(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (!profile_is_075()) {
        send_server_notice(arguments.player, arguments.console, "Night and day are the 0.75 profile's (Daytime and Weather)");
        return;
    }
    if (arguments.argc == 1) {
        send_server_notice(arguments.player, arguments.console, ext075_night() ? "It is night" : "It is day");
        return;
    }
    const char* a = arguments.argv[1];
    int         on = !strcmp(a, "on") || !strcmp(a, "1");
    if (arguments.argc > 2 || (!on && strcmp(a, "off") != 0 && strcmp(a, "0") != 0)) {
        send_server_notice(arguments.player, arguments.console, "Usage: /night [on|off]");
        return;
    }
    if (!ext075_set_night(server, on, "command")) {
        send_server_notice(arguments.player, arguments.console, on ? "It is night already" : "It is day already");
        return;
    }
    send_server_notice(arguments.player, arguments.console, on ? "Night falls" : "Day breaks");
}
