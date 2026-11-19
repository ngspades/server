#include <Server/Player.h>
#include <Server/Commands/CommandManager.h>
#include <Server/Commands/Commands.h>
#include <Server/HostPackets.h>
#include <Server/Audit.h>
#include <Server/Plugins.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/DataDir.h>
#include <Util/Enums.h>
#include <Util/JSONHelpers.h>
#include <Util/Log.h>
#include <Util/Notice.h>
#include <Util/Types.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>
#include <ctype.h>
#include <enet/enet.h>
#include <json-c/json.h>
#include <json-c/json_object.h>
#include <json-c/json_util.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The longest "/" line a command is read from. A longer one is refused
   before anything parses or copies it. */
enum
{
    COMMAND_LINE_MAX = 1000,
};

static uint32_t _cmds_compare(command_t* first, command_t* second)
{
    /* strcmp truncated to unsigned, which is how the list has always been
       sorted: that order is what /help lists and must not move */
    return strcmp(first->id, second->id);
}

uint8_t player_has_permission(server_t* server, player_t* player, uint8_t console, uint32_t permission)
{
    if (console) {
        return permission;
    }
    if (player == NULL) {
        return 0;
    }
    /* The owner of a listen server is the host of the match: grant it every
       staff permission without a /login, so /map, /nextmap, /kick, /ban,
       /shutdown and friends work out of the box. Dedicated servers still
       require an explicit login. */
    if (server != NULL && server->listen_mode && player_is_host(server, player)) {
        return 1;
    }
    return (permission & player->permissions);
}

/* The address a ban is filed under: an IPv6 player's /64, else the IPv4. */
static void _ban_address(char* out, size_t size, ip_t ip, const char* ipv6)
{
    if (ipv6 != NULL) {
        snprintf(out, size, "%s", ipv6);
    } else {
        format_ip_to_str(out, ip);
    }
}

/* Whether a player sits at the address a ban names: an IPv6 ban carries its
   /64 (theirs, NULL for an IPv4 player), an IPv4 one the address alone. */
static uint8_t _at_ban_address(const player_t* player, const char* theirs, ip_t ip, const char* ipv6)
{
    if (ipv6 != NULL) {
        return theirs != NULL && strcmp(theirs, ipv6) == 0;
    }
    return theirs == NULL && player->ip.ip32 == ip.ip32;
}

/* Kicks everyone at that address and answers the name to file the ban under:
   the first match's, the default name when nobody at it is connected. One
   ban covers the address, so the rest of its players go with it. */
static const char* _kick_banned_address(server_t* server, ip_t ip, const char* ipv6)
{
    const char* name  = "Deuce";
    uint8_t     named = 0;
    player_t*   player;
    player_t*   tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        char        prefix[64];
        const char* theirs = conn_player_prefix(player, prefix, sizeof prefix);
        if (player->state == STATE_DISCONNECTED || player->is_bot ||
            !_at_ban_address(player, theirs, ip, ipv6))
        {
            continue;
        }
        if (!named) {
            name  = player->name;
            named = 1; // one name for the address, whatever it kicks
        }
        notice_kick(player, REASON_BANNED, CONN_CLOSE, "You were banned from this server");
    }
    return name;
}

/* The reason as typed after the address; the space that separates it from
   the address is not part of it. NULL when none was given. */
static const char* _ban_reason(char* reason)
{
    return (*reason == ' ' && reason[1] != '\0') ? reason + 1 : NULL;
}

void cmd_generate_ban(server_t* server,
                      command_args_t arguments,
                      float          time,
                      ip_t           ip,
                      const char*    ipv6,
                      char*          reason)
{
    char address[64];
    _ban_address(address, sizeof address, ip, ipv6);

    struct json_object* ban   = json_object_new_object();
    struct json_object* array = NULL;
    struct json_object* root  = bans_open(&array);

    const char* name = _kick_banned_address(server, ip, ipv6);

    json_object_object_add(ban, "Name", json_object_new_string(name));
    json_object_object_add(ban, "IP", json_object_new_string(address));
    /* the minute of the game's clock the ban ends at, 0 for good: what the
       ban list's Time row reads */
    json_object_object_add(ban, "Time", json_object_new_double(time));
    const char* text = _ban_reason(reason);
    if (text != NULL) {
        json_object_object_add(ban, "Reason", json_object_new_string(text));
    }
    json_object_array_add(array, ban);
    json_object_to_file(data_bans_file(), root);
    json_object_put(root);

    if (time == 0) {
        send_server_notice(arguments.player, arguments.console, "IP %s has been permanently banned", address);
    } else {
        send_server_notice(arguments.player, arguments.console, "IP %s has been for %f minutes", address, time);
    }
}

void command_create(server_t* server,
                    uint8_t   parse_args,
                    void (*command)(void* p_server, command_args_t arguments),
                    char     id[30],
                    char     description[1024],
                    uint32_t permissions)
{
    command_t* cmd   = spadesx_malloc(sizeof(command_t));
    cmd->execute     = command;
    cmd->parse_args  = parse_args;
    cmd->permissions = permissions;
    snprintf(cmd->description, sizeof cmd->description, "%s", description);
    snprintf(cmd->id, sizeof cmd->id, "%s", id);
    HASH_ADD_STR(server->cmds_map, id, cmd);
    LL_APPEND(server->cmds_list, cmd);
}

void command_populate_all(server_t* server)
{
    server->cmds_list = NULL; // UTlist requires root to be initialized to NULL
    //  Add custom commands into this array definition
    command_manager_t commands[] = {
    {"/addbot", 1, &cmd_addbot, 0, "Adds bots to the server. Usage: addbot [n] [blue|green]"},
    {"/admin", 0, &cmd_admin, 0, "Sends a message to all online admins."},
    {"/adminmute", 1, &cmd_admin_mute, 30, "Mutes or unmutes player from /admin usage"},
    {"/autofill", 1, &cmd_autofill, 0, "Keeps the given player count as bots. Usage: autofill <n>"},
    {"/ban", 0, &cmd_ban_custom, 30, "Puts specified IP into ban list"},
    {"/banrange", 0, &cmd_ban_range, 30, "Bans specified IP range"},
    {"/botfill", 1, &cmd_botfill, 0, "Tops both teams up to n players each. Usage: botfill <n>"},
    {"/bots", 0, &cmd_bots, 0, "Shows bot, human and autofill counts"},
    // We can have 2+ commands for same function even with different permissions and name
    {"/client", 1, &cmd_clin, 0, "Shows players client info"},
    {"/clin", 1, &cmd_clin, 0, "Shows players client info"},
    {"/classes", 1, &cmd_classes, 30, "Turns the classes on or off (the zombie mode's). Usage: classes [on|off]"},
    {"/dban", 0, &cmd_ban_custom, 30, "Bans specified player for a day"},
    {"/hban", 0, &cmd_ban_custom, 30, "Bans specified player for 6 hours"},
    {"/help", 0, &cmd_help, 0, "Shows commands and their description"},
    {"/intel", 0, &cmd_intel, 0, "Shows info about intel"},
    {"/inv", 0, &cmd_inv, 30, "Makes you go invisible"},
    {"/kick", 1, &cmd_kick, 30, "Kicks specified player from the server"},
    {"/kickbot", 1, &cmd_kickbot, 0, "Removes the bot with the given id. Usage: kickbot <id>"},
    {"/kickbots", 0, &cmd_kickbots, 0, "Removes every bot from the server"},
    {"/colinfo", 1, &cmd_colinfo, 0, "debug: dump a map column"},
    {"/kill", 1, &cmd_kill, 0, "Kills player who sent it or player specified in argument"},
    {"/login", 1, &cmd_login, 0, "Login command. First argument is a role. Second password"},
    {"/logout", 0, &cmd_logout, 31, "Logs out logged in player"},
    {"/mban", 0, &cmd_ban_custom, 30, "Bans specified player for a month"},
    {"/mute", 1, &cmd_mute, 30, "Mutes or unmutes specified player"},
    {"/pban", 0, &cmd_ban_custom, 30, "Permanently bans a specified player"},
    {"/pm", 0, &cmd_pm, 0, "Private message to specified player"},
    /* staff only: it names the machine's folders */
    {"/profile", 0, &cmd_profile, 30, "Shows the server's profile (1.0 or 0.75), config, data folder, ports and masters"},
    {"/ratio", 1, &cmd_ratio, 0, "Shows yours or requested player ratio"},
    {"/reset", 0, &cmd_reset, 24, "Resets server and loads next map"},
    {"/say", 0, &cmd_say, 30, "Send message to everyone as the server"},
    {"/server", 0, &cmd_server, 0, "Shows info about the server"},
    {"/score", 0, &cmd_score, 0, "Shows the teams' score and the match limit"},
    {"/stats", 0, &cmd_stats, 0, "Shows your match stats, or a player's. Usage: stats [player]"},
    {"/time", 0, &cmd_time, 0, "Shows how long this match has been running"},
    {"/nextmap", 0, &cmd_nextmap, 24, "Loads the next map of the rotation"},
    {"/crate", 1, &cmd_crate, 24, "Drops a supply crate on a player (yourself without one). Usage: crate <ammo|health> [player]"},
    {"/night", 1, &cmd_night, 24, "Night or day for the 0.75 clients (Daytime and Weather). Usage: night [on|off]"},
    {"/sky", 1, &cmd_sky, 24, "The time of day and the weather (1.0). Usage: sky [time <hh:mm|day|night|dawn|dusk> | rate <n> | weather <kind> [0..100] | moon <0..100>]"},
    {"/outfit", 1, &cmd_outfit, 24, "How a player or a team looks (1.0). Usage: outfit <player> <0..255> [colour] | outfit team <1|2> <0..254>"},
    {"/perk", 1, &cmd_perk, 30, "Grants a perk for a test. Usage: perk <mg|sentry|jetpack|landmine|dynamite> [player]"},
    {"/endmatch", 0, &cmd_endmatch, 24, "Ends the match now (the summary, then the next map). Usage: endmatch [1|2]: that team wins"},
    {"/map", 1, &cmd_map, 24, "Loads a map now. Usage: map <Name>"},
    {"/restart", 0, &cmd_restart, 24, "Reloads the current map"},
    {"/extend", 1, &cmd_extend, 24, "Adds minutes to a timed match. Usage: extend <minutes>"},
    {"/tb", 1, &cmd_toggle_build, 30, "Toggles ability to build for everyone or specified player"},
    {"/tk", 1, &cmd_toggle_kill, 30, "Toggles ability to kill for everyone or specified player"},
    {"/tp", 1, &cmd_tp, 24, "Teleports specified player to another specified player"},
    {"/tpc", 1, &cmd_tpc, 24, "Teleports to specified cordinates"},
    {"/ttk", 1, &cmd_toggle_team_kill, 30, "Toggles ability to team kill for everyone or specified player"},
    {"/unban", 0, &cmd_unban, 30, "Unbans specified IP"},
    {"/unbanrange", 0, &cmd_unban_range, 30, "Unbans specified IP range"},
    {"/undoban", 0, &cmd_undo_ban, 30, "Reverts the last ban"},
    {"/ups", 1, &cmd_ups, 0, "Sets UPS of player to requested ammount. Range: 1-300"},
    {"/wban", 0, &cmd_ban_custom, 30, "Bans specified player for a week"},
    {"/shutdown", 0, &cmd_shutdown, 16, "Shutdown the server"}};
    for (unsigned long i = 0; i < sizeof(commands) / sizeof(command_manager_t); i++) {
        command_create(server,
                       commands[i].parse_args,
                       commands[i].command,
                       commands[i].id,
                       commands[i].description,
                       commands[i].permissions);
    }
    LL_SORT(server->cmds_list, _cmds_compare);
}

void free_all_commands(server_t* server)
{
    command_t* current_command;
    command_t* tmp;

    HASH_ITER(hh, server->cmds_map, current_command, tmp)
    {
        command_free(server, current_command);
    }
}

void command_free(server_t* server, command_t* command)
{
    HASH_DEL(server->cmds_map, command);
    LL_DELETE(server->cmds_list, command);
    free(command);
}

/* Splits what follows the command word into argv: spaces and tabs separate
   the arguments, a pair of quotes wraps one that holds spaces, and a
   backslash escapes the next character for the split (it stays in the copy,
   so a command sees the line as typed). Answers 0, having told the player
   what was wrong with the line, when it cannot be read. */
static uint8_t parse_arguments(command_args_t* arguments, char* message, size_t command_length)
{
    char* p = message + command_length;
    while (*p != '\0' && arguments->argc < sizeof arguments->argv / sizeof arguments->argv[0]) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        uint8_t quoted = (*p == '"');
        if (quoted) {
            p++; // the quotes themselves are not part of the argument
        }
        uint8_t escaped = 0;
        uint8_t closed  = 0;
        char*   end     = p;
        for (; *end != '\0'; end++) {
            if (escaped) {
                escaped = 0;
                continue;
            }
            if (*end == '\\') {
                escaped = 1;
                continue;
            }
            if (*end == '"') {
                if (!quoted) {
                    send_server_notice(arguments->player,
                                       arguments->console,
                                       "Failed to parse the command: found a stray \" symbol");
                    return 0;
                }
                char next = end[1];
                if (next != ' ' && next != '\t' && next != '\0') {
                    send_server_notice(arguments->player,
                                       arguments->console,
                                       "Failed to parse the command: found more symbols after the \" symbol");
                    return 0;
                }
                closed = 1;
                end++; // past the closing quote
                break;
            }
            if (!quoted && (*end == ' ' || *end == '\t')) {
                break;
            }
        }
        if (quoted && !closed) {
            send_server_notice(
            arguments->player, arguments->console, "Failed to parse the command: missing a \" symbol");
            return 0;
        }

        size_t length = (size_t) (end - p) - (closed ? 1 : 0);
        if (length > 0) {
            char* argument            = spadesx_malloc(length + 1);
            argument[length]          = '\0';
            memcpy(argument, p, length);
            arguments->argv[arguments->argc++] = argument;
        }
        p = end;
    }
    return 1;
}

/* A command that does not parse its arguments takes the rest of the line as
   one, as typed: "/say  hi there" keeps its leading space. */
static void parse_rest_of_line(command_args_t* arguments, char* message, size_t command_length)
{
    size_t message_length = strlen(message);
    if (message_length <= command_length + 1) {
        return; // the command alone, or a separator with nothing after it
    }
    /* the lengths are size_t, never a byte: a line may hold 1000 bytes, and a
       length that wrapped made this buffer 39 bytes for a 300 byte "/pm" */
    char* argument = spadesx_calloc(message_length - command_length, sizeof(message[0]));
    snprintf(argument, message_length - command_length, "%s", message + command_length + 1);
    arguments->argv[arguments->argc++] = argument;
}

void command_handle(server_t* server, player_t* player, char* message, uint8_t console)
{
    if (strlen(message) > COMMAND_LINE_MAX) {
        send_server_notice(player, console, "This command is longer then the 1000 character limit. Thus it has been ignored");
        return;
    }

    /* the command word, lower cased in the line as well (the arguments keep
       their case); the line's own copy is what the command sees */
    char command[COMMAND_LINE_MAX + 1];
    command[0] = '\0'; // an empty line reads no command, and sscanf writes nothing then
    sscanf(message, "%s", command);
    size_t command_length = strlen(command);
    for (size_t i = 1; i < command_length; ++i) {
        message[i] = command[i] = (char) tolower((unsigned char) command[i]);
    }

    command_t* cmd;
    HASH_FIND_STR(server->cmds_map, command, cmd);
    if (cmd == NULL) {
        /* not the server's: maybe a plugin's (game.command) */
        plugins_on_command(server, player, console, message);
        return;
    }

    command_args_t arguments;
    arguments.player      = player;
    arguments.server      = server;
    arguments.console     = console;
    arguments.permissions = cmd->permissions;
    arguments.argc        = 1;
    arguments.argv[0]     = command;

    uint8_t readable = 1;
    if (cmd->parse_args) {
        readable = parse_arguments(&arguments, message, command_length);
    } else {
        parse_rest_of_line(&arguments, message, command_length);
    }

    if (readable) {
        if (player_has_permission(server, player, console, cmd->permissions) > 0 || cmd->permissions == 0) {
            audit_command(player, console, command, arguments.argc);
            cmd->execute((void*) server, arguments);
        } else {
            send_server_notice(player, console, "You do not have permissions to use this command");
        }
    }

    /* from 1: argv[0] is the line's own copy, which was never the heap's */
    for (uint32_t i = 1; i < arguments.argc; i++) {
        free(arguments.argv[i]);
    }
}
