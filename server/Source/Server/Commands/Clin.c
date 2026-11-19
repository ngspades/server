#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Notice.h>
#include <stdio.h>

/* the client a connection's letter stands for, in words; a letter we know
   nothing of is the Voxlap of the original game. 'B' is BetterSpades'
   letter, which our client still sends */
static void clin_client_name(char letter, char* out, size_t size)
{
    const char* name = "Voxlap";
    if (letter == 'o') {
        name = "OpenSpades";
    } else if (letter == 'B') {
        name = GAME_NAME;
    }
    snprintf(out, size, "%s", name);
}

/* the player an id names, NULL for an id no connection holds */
static player_t* clin_player_by_id(server_t* server, uint8_t id)
{
    if (id >= server->protocol.max_players) {
        return NULL;
    }
    player_t* player = NULL;
    HASH_FIND(hh, server->players, &id, sizeof(id), player);
    return player;
}

void cmd_clin(void* p_server, command_args_t arguments)
{
    server_t* server    = (server_t*) p_server;
    uint8_t   player_id = arguments.console ? 33 : arguments.player->id;

    if (arguments.argc > 2) {
        send_server_notice(arguments.player, arguments.console, "Too many arguments");
        return;
    }
    if (arguments.console && arguments.argc != 2) {
        /* a console has no body of its own to look up */
        send_server_notice(arguments.player, arguments.console, "No ID given");
        return;
    }
    if (arguments.argc == 2 && !parse_player(arguments.argv[1], &player_id, NULL)) {
        send_server_notice(arguments.player, arguments.console, "Invalid ID. Wrong format");
        return;
    }

    player_t* player = clin_player_by_id(server, player_id);
    if (player == NULL || !is_past_join_screen(player)) {
        send_server_notice(arguments.player, arguments.console, "Invalid ID. Player doesnt exist");
        return;
    }

    char client[32];
    clin_client_name(player->client, client, sizeof client);
    send_server_notice(arguments.player,
                       arguments.console,
                       "Player %s is running %s version %d.%d.%d on %s",
                       player->name,
                       client,
                       player->version_major,
                       player->version_minor,
                       player->version_revision,
                       player->os_info);
}
