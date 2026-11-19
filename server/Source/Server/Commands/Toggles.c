#include <Server/Gamemodes/Gamemode.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Notice.h>
#include <strings.h>

/* the player an argument names: 0 when the argument is no id at all (the
   commands say nothing then), 1 when it parsed, with *player NULL when no
   connection holds the id */
static uint8_t named_player(server_t* server, char* argument, player_t** player)
{
    uint8_t id;
    if (!parse_player(argument, &id, NULL)) {
        return 0;
    }
    player_t* found = NULL;
    HASH_FIND(hh, server->players, &id, sizeof(id), found);
    *player = found;
    return 1;
}

void cmd_toggle_build(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;

    if (arguments.argc == 1) {
        const uint8_t enabled = !server->global_ab;
        server->global_ab     = enabled;
        if (enabled) {
            broadcast_server_notice(server, arguments.console, "Building has been enabled");
        } else {
            broadcast_server_notice(server, arguments.console, "Building has been disabled");
        }
        return;
    }

    player_t* player = NULL;
    if (!named_player(server, arguments.argv[1], &player)) {
        return;
    }
    if (player == NULL) {
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }
    if (!is_past_join_screen(player)) {
        return;
    }

    const uint8_t enabled = !player->can_build;
    player->can_build     = enabled;
    if (enabled) {
        broadcast_server_notice(server, arguments.console, "Building has been enabled for %s", player->name);
    } else {
        broadcast_server_notice(server, arguments.console, "Building has been disabled for %s", player->name);
    }
}

void cmd_toggle_kill(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;

    if (arguments.argc == 1) {
        const uint8_t enabled = !server->global_ak;
        server->global_ak     = enabled;
        if (enabled) {
            broadcast_server_notice(server, arguments.console, "Killing has been enabled");
        } else {
            broadcast_server_notice(server, arguments.console, "Killing has been disabled");
        }
        return;
    }

    player_t* player = NULL;
    if (!named_player(server, arguments.argv[1], &player)) {
        return;
    }
    if (player == NULL) {
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }
    if (!is_past_join_screen(player)) {
        return;
    }

    const uint8_t enabled = !player->allow_killing;
    player->allow_killing = enabled;
    if (enabled) {
        broadcast_server_notice(server, arguments.console, "Killing has been enabled for %s", player->name);
    } else {
        broadcast_server_notice(server, arguments.console, "Killing has been disabled for %s", player->name);
    }
}

/* the on/off a word asks for: 1 on, 0 off, -1 no word of ours */
static int classes_word(const char* word)
{
    if (!strcasecmp(word, "on") || !strcasecmp(word, "1") || !strcasecmp(word, "yes")) {
        return 1;
    }
    if (!strcasecmp(word, "off") || !strcasecmp(word, "0") || !strcasecmp(word, "no")) {
        return 0;
    }
    return -1;
}

/* the host's choice for the classes (the zombie mode's): the survivors
   pick a class at the class screen, the zombies stay zombies */
void cmd_classes(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    int       on;

    if (arguments.argc == 1) {
        /* no word: the other way round */
        on = !gamemode_has(server, GM_FEAT_CLASSES);
    } else {
        on = classes_word(arguments.argv[1]);
        if (on < 0) {
            send_server_notice(arguments.player, arguments.console, "Usage: classes [on|off]");
            return;
        }
    }

    if (!gamemode_set_classes(server, on)) {
        send_server_notice(arguments.player, arguments.console, "This mode has no classes option");
        return;
    }
    if (on) {
        broadcast_server_notice(server, arguments.console, "The classes are on");
    } else {
        broadcast_server_notice(server, arguments.console, "The classes are off");
    }
}

void cmd_toggle_team_kill(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;

    if (arguments.argc != 2) {
        return;
    }

    player_t* player = NULL;
    if (!named_player(server, arguments.argv[1], &player)) {
        return;
    }
    if (player == NULL) {
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }
    if (!is_past_join_screen(player)) {
        /* a body still joining is no more a player here than an unknown id */
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }

    const uint8_t enabled      = !player->allow_team_killing;
    player->allow_team_killing = enabled;
    if (enabled) {
        broadcast_server_notice(server, arguments.console, "Team killing has been enabled for %s", player->name);
    } else {
        broadcast_server_notice(server, arguments.console, "Team killing has been disabled for %s", player->name);
    }
}
