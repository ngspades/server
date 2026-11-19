#include <Server/Commands/Commands.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Log.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>

/* a body that may be killed: one that finished joining and is in a team */
static uint8_t killable(player_t* player)
{
    return is_past_join_screen(player) && player->team != TEAM_SPECTATOR;
}

/* /kill all: every team player still standing */
static void kill_everyone(server_t* server, command_args_t arguments)
{
    int       count = 0;
    player_t *player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (killable(player) && player->hp > 0) {
            send_kill_action_packet(server, player, player, 0, 5, 0);
            count++;
        }
    }
    send_server_notice(arguments.player, arguments.console, "Killed %i players.", count);
}

void cmd_kill(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;

    if (arguments.argc == 1) {
        /* our own body: a console has none */
        if (arguments.console) {
            LOG_INFO("You cannot use this command without arguments from console");
            return;
        }
        send_kill_action_packet(server, arguments.player, arguments.player, 0, 5, 0);
        return;
    }

    if (!player_has_permission(server, arguments.player, arguments.console, 30)) {
        send_server_notice(arguments.player, arguments.console, "You have no permission to use this command.");
        return;
    }

    if (strcmp(arguments.argv[1], "all") == 0) {
        kill_everyone(server, arguments);
        return;
    }

    for (uint32_t i = 1; i < arguments.argc; i++) {
        uint8_t id = 0;
        if (!parse_player(arguments.argv[i], &id, NULL) || id > server->protocol.max_players) {
            send_server_notice(arguments.player, arguments.console, "Invalid player \"%s\"!", arguments.argv[i]);
            return;
        }

        player_t* player;
        HASH_FIND(hh, server->players, &id, sizeof(id), player);
        if (player == NULL) {
            send_server_notice(
            arguments.player, arguments.console, "Player %hhu does not exist or is already dead", id);
            return;
        }
        /* a spectator or a body still joining is left alone, quietly */
        if (!killable(player)) {
            continue;
        }
        send_kill_action_packet(server, player, player, 0, 5, 0);
        send_server_notice(arguments.player, arguments.console, "Killing player #%i...", id);
    }
}
