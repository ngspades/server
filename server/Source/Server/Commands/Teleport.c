#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Checks/PositionChecks.h>
#include <Util/Log.h>
#include <Util/Notice.h>

/* a teleport's coordinates name a cell: half a block off on x and y puts a
   body on the column's middle and 2.36 above the cell's block stands it on
   it (map z grows downwards, as in the packets a player sends) */
static void teleport_body_to(player_t* player, const vector3f_t* target)
{
    player->movement.position.x = target->x - 0.5f;
    player->movement.position.y = target->y - 0.5f;
    player->movement.position.z = target->z - 2.36f;
}

void cmd_tp(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.console) {
        LOG_INFO("You cannot use this command from console");
        return;
    }

    uint8_t from_id = 33;
    uint8_t to_id   = 33;
    if (arguments.argc != 3 || !parse_player(arguments.argv[1], &from_id, NULL) ||
        !parse_player(arguments.argv[2], &to_id, NULL))
    {
        send_server_notice(arguments.player, arguments.console, "Incorrect amount of arguments or wrong argument type");
        return;
    }

    player_t* from = NULL;
    player_t* to   = NULL;
    HASH_FIND(hh, server->players, &to_id, sizeof(to_id), to);
    HASH_FIND(hh, server->players, &from_id, sizeof(from_id), from);
    if (to == NULL || from == NULL) {
        send_server_notice(arguments.player, arguments.console, "ID not in range or player doesnt exist");
        return;
    }

    if (!valid_pos_v3f(server, to->movement.position)) {
        /* upstream's format asks for an id and is handed the player: kept as
           it is so the text and what it prints do not change here */
        send_server_notice(arguments.player, arguments.console, "Player %hhu is at invalid position", to);
        return;
    }

    /* the target's position is a body's already and takes the offset once
       more: upstream's maths, kept so /tp lands where it always did */
    teleport_body_to(from, &to->movement.position);
}

void cmd_tpc(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.console) {
        LOG_INFO("You cannot use this command from console");
        return;
    }

    vector3f_t pos = {0, 0, 0};
    if (arguments.argc != 4 || !PARSE_VECTOR3F(arguments, 1, &pos)) {
        send_server_notice(arguments.player, arguments.console, "Incorrect amount of arguments or wrong argument type");
        return;
    }
    if (!valid_pos_v3f(server, pos)) {
        send_server_notice(arguments.player, arguments.console, "Invalid position");
        return;
    }

    teleport_body_to(arguments.player, &pos);
}
