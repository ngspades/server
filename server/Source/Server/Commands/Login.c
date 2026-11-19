#include <Server/Plugins.h>
#include <Server/Server.h>
#include <Util/Log.h>
#include <Util/Notice.h>
#include <ctype.h>

/* the role the player named, from the list init_player gave it; NULL when
   the list has no role of that name */
static const permissions_t* _role_named(player_t* player, const char* name)
{
    for (size_t i = 0; i < sizeof player->role_list / sizeof(permissions_t); ++i) {
        if (strcmp(name, player->role_list[i].access_level) == 0) {
            return &player->role_list[i];
        }
    }
    return NULL;
}

/* a role is on only when the server set it a password and the player's
   matches: an empty password disables the role, as it always has */
static uint8_t _password_matches(const permissions_t* role, const char* password)
{
    const char* expected = *role->access_password;
    return expected[0] != '\0' && strcmp(password, expected) == 0;
}

void cmd_login(void* p_server, command_args_t arguments)
{
    (void) p_server;
    if (arguments.console) {
        LOG_INFO("You cannot use this command from console");
        return;
    }
    if (arguments.player->permissions != 0) {
        send_server_notice(arguments.player, arguments.console, "You are already logged in");
        return;
    }
    if (arguments.argc != 3) {
        send_server_notice(arguments.player, arguments.console, "Incorrect number of arguments to login");
        return;
    }

    /* the roles are written in lower case, so "Admin" is "admin" */
    char* level = arguments.argv[1];
    for (size_t i = 0; level[i] != '\0'; ++i) {
        level[i] = (char) tolower((unsigned char) level[i]);
    }

    const permissions_t* role = _role_named(arguments.player, level);
    if (role == NULL) {
        send_server_notice(arguments.player, arguments.console, "Invalid role");
        return;
    }
    if (!_password_matches(role, arguments.argv[2])) {
        send_server_notice(arguments.player, arguments.console, "Wrong password");
        return;
    }

    uint32_t bit = 1u << role->perm_level_offset;
    arguments.player->permissions |= bit;
    /* a plugin may ask for more than the password (totp): the role is held
       while it is asked, so it sees the player in it; the plugin that
       refuses tells the player why */
    if (!plugins_on_login(arguments.server, arguments.player, role->access_level)) {
        arguments.player->permissions &= ~bit;
        return;
    }
    send_server_notice(arguments.player, arguments.console, "You logged in as %s", role->access_level);
}

void cmd_logout(void* p_server, command_args_t arguments)
{
    (void) p_server;
    if (arguments.console) {
        LOG_INFO("You cannot use this command from console");
        return;
    }
    arguments.player->permissions = 0;
    send_server_notice(arguments.player, arguments.console, "You logged out");
}
