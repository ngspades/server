#include <Server/Player.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/DataDir.h>
#include <Util/JSONHelpers.h>
#include <Util/Notice.h>
#include <json-c/json_object.h>
#include <json-c/json_util.h>

/* the address a ban of this argument holds, into out (at least 19 bytes):
   the /64 of an IPv6 one ("2001:db8:1:2::/64") or the IPv4 one written out
   again. 0 when the argument is no address at all. */
static uint8_t _banned_address(char* text, char* out, size_t size)
{
    char prefix[64];
    ip_t ip;
    if (conn_ipv6_prefix(text, prefix, sizeof prefix)) {
        snprintf(out, size, "%s", prefix);
        return 1;
    }
    if (!parse_ip(text, &ip, NULL)) {
        return 0;
    }
    format_ip_to_str(out, ip);
    return 1;
}

/* one ban entry, removed from the array and written out again; the file is
   written on each one, as it always was */
static void _ban_entry_removed(struct json_object* root, struct json_object* bans, int index)
{
    json_object_array_del_idx(bans, index, 1);
    json_object_to_file(data_bans_file(), root);
}

void cmd_unban(void* p_server, command_args_t arguments)
{
    (void) p_server;
    char address[64];
    if (arguments.argc != 2 || !_banned_address(arguments.argv[1], address, sizeof address)) {
        send_server_notice(arguments.player, arguments.console, "Incorrect amount of arguments or invalid IP");
        return;
    }

    uint8_t             removed = 0;
    struct json_object* bans;
    struct json_object* root  = bans_open(&bans);
    int                 count = json_object_array_length(bans);
    for (int i = 0; i < count; ++i) {
        struct json_object* entry = json_object_array_get_idx(bans, i);
        const char*         banned;
        READ_STR_FROM_JSON(entry, banned, IP, "IP", "0.0.0.0", 1);
        if (strcmp(address, banned) != 0) {
            continue;
        }
        _ban_entry_removed(root, bans, i);
        removed = 1;
    }
    if (removed) {
        send_server_notice(arguments.player, arguments.console, "IP %s unbanned", address);
    } else {
        send_server_notice(arguments.player, arguments.console, "IP %s not found in banned IP list", address);
    }
    json_object_put(root);
}

void cmd_unban_range(void* p_server, command_args_t arguments)
{
    (void) p_server;
    ip_t  start;
    ip_t  end;
    char* after_start;
    if (arguments.argc != 2 || !parse_ip(arguments.argv[1], &start, &after_start) ||
        !parse_ip(after_start, &end, NULL))
    {
        send_server_notice(arguments.player, arguments.console, "Incorrect amount of arguments or invalid IP");
        return;
    }

    char start_text[19];
    char end_text[19];
    format_ip_to_str(start_text, start);
    format_ip_to_str(end_text, end);

    uint8_t             removed = 0;
    struct json_object* bans;
    struct json_object* root  = bans_open(&bans);
    int                 count = json_object_array_length(bans);
    for (int i = 0; i < count; ++i) {
        struct json_object* entry = json_object_array_get_idx(bans, i);
        const char*         banned_start;
        const char*         banned_end;
        READ_STR_FROM_JSON(entry, banned_start, start_of_range, "start of range", "0.0.0.0", 0);
        READ_STR_FROM_JSON(entry, banned_end, end_of_range, "end of range", "0.0.0.0", 0);
        if (strcmp(start_text, banned_start) != 0 || strcmp(end_text, banned_end) != 0) {
            continue;
        }
        _ban_entry_removed(root, bans, i);
        removed = 1;
    }
    if (removed) {
        send_server_notice(arguments.player,
                           arguments.console,
                           "IP range %s-%s unbanned",
                           start_text,
                           end_text);
    } else {
        send_server_notice(arguments.player,
                           arguments.console,
                           "IP range %s-%s not found in banned IP ranges",
                           start_text,
                           end_text);
    }
    json_object_put(root);
}

void cmd_undo_ban(void* p_server, command_args_t arguments)
{
    (void) p_server;
    if (arguments.argc != 1) {
        send_server_notice(arguments.player, arguments.console, "Too many arguments given to command");
        return;
    }

    struct json_object* bans;
    struct json_object* root  = bans_open(&bans);
    int                 count = json_object_array_length(bans);
    if (count == 0) {
        send_server_notice(arguments.player, arguments.console, "There is no ban to undo");
        json_object_put(root);
        return;
    }

    /* the newest ban is the last one added to the list */
    struct json_object* entry = json_object_array_get_idx(bans, count - 1);
    const char*         address;
    READ_STR_FROM_JSON(entry, address, IP, "IP", "0.0.0.0", 1);
    /* a range ban has no "IP", which the fallback's leading 0 marks */
    if (address[0] == '0') {
        const char* start;
        const char* end;
        READ_STR_FROM_JSON(entry, start, start_of_range, "start of range", "0.0.0.0", 0);
        READ_STR_FROM_JSON(entry, end, end_of_range, "end of range", "0.0.0.0", 0);
        send_server_notice(arguments.player, arguments.console, "IP range %s-%s unbanned", start, end);
    } else {
        send_server_notice(arguments.player, arguments.console, "IP %s unbanned", address);
    }
    _ban_entry_removed(root, bans, count - 1);
    json_object_put(root);
}
