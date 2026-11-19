#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>

/* the player the command names; invalid says the argument was no player id
   at all, which the callers word apart from an id nobody holds */
static player_t* _mute_target(server_t* server, command_args_t arguments, uint8_t* invalid)
{
    uint8_t id = 33;
    *invalid   = 0;
    if (arguments.argc != 2 || !parse_player(arguments.argv[1], &id, NULL)) {
        *invalid = 1;
        return NULL;
    }
    player_t* player;
    HASH_FIND(hh, server->players, &id, sizeof id, player);
    return player;
}

void cmd_admin_mute(void* p_server, command_args_t arguments)
{
    server_t* server  = (server_t*) p_server;
    uint8_t   invalid = 0;
    player_t* player  = _mute_target(server, arguments, &invalid);
    if (invalid) {
        send_server_notice(arguments.player, arguments.console, "You did not enter ID or entered incorrect argument");
        return;
    }
    if (player == NULL) {
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }
    if (!is_past_join_screen(player)) {
        return;
    }
    player->admin_muted = player->admin_muted ? 0 : 1;
    if (player->admin_muted) {
        send_server_notice(arguments.player, arguments.console, "%s has been admin muted", player->name);
    } else {
        send_server_notice(arguments.player, arguments.console, "%s has been admin unmuted", player->name);
    }
}

void cmd_mute(void* p_server, command_args_t arguments)
{
    server_t* server  = (server_t*) p_server;
    uint8_t   invalid = 0;
    player_t* player  = _mute_target(server, arguments, &invalid);
    if (invalid) {
        send_server_notice(arguments.player, arguments.console, "You did not enter ID or entered incorrect argument");
        return;
    }
    if (player == NULL) {
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }
    if (!is_past_join_screen(player)) {
        return;
    }
    player->muted = player->muted ? 0 : 1;
    if (player->muted) {
        broadcast_server_notice(server, arguments.console, "%s has been muted", player->name);
    } else {
        broadcast_server_notice(server, arguments.console, "%s has been unmuted", player->name);
    }
}
