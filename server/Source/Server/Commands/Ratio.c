#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Notice.h>
#include <math.h>

/* the ratio of a player who never died is its kills: fmaxf keeps the
   divisor off zero */
static float kill_ratio(player_t* player)
{
    return (float) player->kills / fmaxf(1, (float) player->deaths);
}

void cmd_ratio(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;

    uint8_t id = 33;
    if (arguments.argc == 2 && parse_player(arguments.argv[1], &id, NULL)) {
        player_t* player;
        HASH_FIND(hh, server->players, &id, sizeof(id), player);
        if (player == NULL) {
            send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
            return;
        }
        if (!is_past_join_screen(player)) {
            return;
        }
        send_server_notice(arguments.player,
                           arguments.console,
                           "%s has kill to death ratio of: %f (Kills: %d, Deaths: %d)",
                           player->name,
                           kill_ratio(player),
                           player->kills,
                           player->deaths);
        return;
    }

    /* an id that is missing or unreadable: our own figures, and a console
       holds no player to ask */
    if (arguments.console) {
        send_server_notice(
        arguments.player, arguments.console, "You cannot use this command from console without argument");
        return;
    }
    send_server_notice(arguments.player,
                       arguments.console,
                       "%s has kill to death ratio of: %f (Kills: %d, Deaths: %d)",
                       arguments.player->name,
                       kill_ratio(arguments.player),
                       arguments.player->kills,
                       arguments.player->deaths);
}
