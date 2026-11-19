#include <Server/Commands/Commands.h>
#include <Server/Plugins.h>
#include <Server/Server.h>
#include <Util/Notice.h>
#include <uthash/utlist.h>

void cmd_help(void* p_server, command_args_t arguments)
{
    server_t*  server = (server_t*) p_server;
    command_t* cmd    = NULL;
    if (arguments.console)
        send_server_notice(arguments.player, arguments.console, "Commands available to you:");

    LL_FOREACH(server->cmds_list, cmd)
    {
        if (cmd == NULL) {
            return; // Just for safety
        }
        if (player_has_permission(arguments.server, arguments.player, arguments.console, cmd->permissions) || cmd->permissions == 0) {
            send_server_notice(
            arguments.player, arguments.console, "[Command: %s, Description: %s]", cmd->id, cmd->description);
        }
    }
    plugins_help(server, arguments.player, arguments.console);
    if (!arguments.console)
        send_server_notice(arguments.player, arguments.console, "Commands available to you:");
}
