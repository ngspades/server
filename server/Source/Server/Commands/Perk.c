// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/ParseConvert.h>
#include <Server/Perks.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Notice.h>
#include <proto10/codec.h>
#include <strings.h>

/* /perk <mg|sentry|jetpack|landmine|dynamite> [player]: an admin hands a
   perk out now, for a test. It is the same inventory bit the streak sets, so
   the tool bar and the wire see it; a mode without perks refuses it. */
static int perk_from_name(const char* name)
{
    if (strcasecmp(name, "mg") == 0 || strcasecmp(name, "machinegun") == 0 || strcasecmp(name, "machine") == 0) {
        return P10_PERK_MG;
    }
    if (strcasecmp(name, "sentry") == 0 || strcasecmp(name, "sentrygun") == 0) {
        return P10_PERK_SENTRY;
    }
    if (strcasecmp(name, "jetpack") == 0 || strcasecmp(name, "jet") == 0) {
        return P10_PERK_JETPACK;
    }
    if (strcasecmp(name, "landmine") == 0 || strcasecmp(name, "mine") == 0) {
        return P10_PERK_LANDMINE;
    }
    if (strcasecmp(name, "dynamite") == 0 || strcasecmp(name, "satchel") == 0) {
        return P10_PERK_DYNAMITE;
    }
    return -1;
}

void cmd_perk(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.argc < 2) {
        send_server_notice(arguments.player, arguments.console,
                           "Usage: perk <mg|sentry|jetpack|landmine|dynamite> [player]");
        return;
    }
    int perk = perk_from_name(arguments.argv[1]);
    if (perk < 0) {
        send_server_notice(arguments.player, arguments.console, "No such perk");
        return;
    }
    player_t* target = arguments.player;
    if (arguments.argc >= 3) {
        uint8_t ID = 0;
        if (!parse_player(arguments.argv[2], &ID, NULL)) {
            send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
            return;
        }
        HASH_FIND(hh, server->players, &ID, sizeof(ID), target);
        if (target == NULL) {
            send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
            return;
        }
    }
    if (target == NULL) {
        send_server_notice(arguments.player, arguments.console, "Give a player");
        return;
    }
    /* the class owns one perk: say which when it is not the asked one, so
       the inventory never holds a charge its class cannot use */
    static const char* names[] = {"jetpack", "sentry", "mg", "landmine", "dynamite"};
    int class_perk = classes_perk(server, target);
    if (class_perk >= 0 && class_perk < P10_PERK_COUNT && class_perk != perk) {
        send_server_notice(arguments.player, arguments.console, "Your class's perk is %s", names[class_perk]);
        return;
    }
    if (!perks_grant(server, target, perk)) {
        send_server_notice(arguments.player, arguments.console, "That perk is off in this mode");
        return;
    }
    send_server_notice(target, 0, "Perk granted: %s", arguments.argv[1]);
    if (arguments.player != NULL && arguments.player != target) {
        send_server_notice(arguments.player, 0, "Perk %s granted to %s", arguments.argv[1], target->name);
    }
}
