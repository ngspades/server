// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Player.h>
#include <Server/Commands/CommandManager.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/DataDir.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <json-c/json_object.h>
#include <json-c/json_util.h>

/* A ban's "Time" is the minute of the game clock it ends in, 0 for a
   permanent one: a ban of n minutes ends n minutes from now. */
static float _ban_ends_at(float minutes)
{
    if (minutes > 0) {
        return ((long double) get_nanos() / (uint64_t) NANO_IN_MINUTE) + minutes;
    }
    return minutes;
}

/* the lengths the fixed ban commands ask for, in minutes; /ban takes its own
   from its arguments and is not one of them */
static const struct
{
    const char* command;
    float       minutes;
} FIXED_BAN_LENGTHS[] = {
    {"/pban", 0.0f},
    {"/hban", 360.0f},
    {"/dban", 1440.0f},
    {"/wban", 10080.0f},
    {"/mban", 44640.0f},
};

static uint8_t _fixed_ban_minutes(const char* command, float* minutes)
{
    for (size_t i = 0; i < sizeof FIXED_BAN_LENGTHS / sizeof FIXED_BAN_LENGTHS[0]; ++i) {
        if (strcmp(command, FIXED_BAN_LENGTHS[i].command) == 0) {
            *minutes = FIXED_BAN_LENGTHS[i].minutes;
            return 1;
        }
    }
    return 0;
}

/* the rest of an argument after the character it points at, never past the
   end: "/ban #3" ends right after the id, and ++ there read past the
   argument's buffer */
static char* _rest_after(char* p)
{
    return *p ? p + 1 : p;
}

/* a custom ban's length and the reason after it: "/ban #3 60 spawn killing"
   bans for 60 minutes with "spawn killing" as the reason. A reason that
   starts with no number leaves the length as it came (0: permanent). */
static char* _custom_ban_reason(char* rest, float* minutes)
{
    char* reason = rest;
    if (*rest != '\0') {
        parse_float(rest + 1, minutes, &reason);
    }
    return reason;
}

/* the /64 of an IPv6 address at the start of an argument (out), and where
   the arguments after the address begin (rest): one home has a whole /64,
   so that is what a ban of an IPv6 player holds. NULL for anything else:
   an IPv4 address, a player id. */
static const char* _ipv6_target(char* text, char* out, size_t size, char** rest)
{
    char   word[64];
    size_t head = strcspn(text, " ");
    size_t kept = head < sizeof word ? head : sizeof word - 1;
    snprintf(word, sizeof word, "%.*s", (int) kept, text);
    if (!conn_ipv6_prefix(word, out, size)) {
        return NULL;
    }
    *rest = text + head;
    return out;
}

static player_t* _player_by_id(server_t* server, uint8_t id)
{
    player_t* player;
    HASH_FIND(hh, server->players, &id, sizeof id, player);
    return player;
}

void cmd_ban_custom(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.argc != 2) {
        send_server_notice(arguments.player, arguments.console, "You did not enter IP or entered incorrect argument");
        return;
    }

    float   minutes = 0.0f;
    uint8_t custom  = 0;
    if (strcmp(arguments.argv[0], "/ban") == 0) {
        custom = 1;
    } else if (!_fixed_ban_minutes(arguments.argv[0], &minutes)) {
        LOG_WARNING("Cant recognize ban command"); // Should never happen
    }

    ip_t        ip;
    char        prefix[64];
    char*       rest;
    const char* ipv6 = _ipv6_target(arguments.argv[1], prefix, sizeof prefix, &rest);
    memset(&ip, 0, sizeof ip);
    if (ipv6 == NULL) {
        if (!parse_ip(arguments.argv[1], &ip, &rest)) {
            uint8_t player_id = 33;
            if (!parse_player(arguments.argv[1], &player_id, &rest)) {
                send_server_notice(arguments.player, arguments.console, "Invalid IP format");
                return;
            }
            player_t* player = _player_by_id(server, player_id);
            if (player == NULL || player->state == STATE_DISCONNECTED) {
                send_server_notice(arguments.player, arguments.console, "Player with ID %hhu does not exist", player_id);
                return;
            }
            if (custom) {
                rest = _custom_ban_reason(rest, &minutes);
            }
            ip.ip32 = player->ip.ip32;
            ip.cidr = player->ip.cidr;
            cmd_generate_ban(server,
                             arguments,
                             _ban_ends_at(minutes),
                             ip,
                             conn_player_prefix(player, prefix, sizeof prefix),
                             rest);
            return;
        }
    }
    if (custom) {
        rest = _custom_ban_reason(rest, &minutes);
    }
    cmd_generate_ban(server, arguments, _ban_ends_at(minutes), ip, ipv6, rest);
}

/* the players a range ban covers, kicked with it; the first one met names
   the ban, since one entry covers them all ("Deuce" when nobody is here) */
static const char* _ban_range_players(server_t* server, ip_t start, ip_t end)
{
    const char* name  = "Deuce";
    uint8_t     named = 0;
    player_t *  player, *tmp;
    HASH_ITER(hh, server->players, player, tmp)
    {
        if (player->state == STATE_DISCONNECTED || !octets_in_range(start, end, player->ip)) {
            continue;
        }
        if (!named) {
            name  = player->name;
            named = 1;
        }
        notice_kick(player, REASON_BANNED, CONN_CLOSE, "You were banned from this server");
    }
    return name;
}

void cmd_ban_range(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    ip_t      start;
    ip_t      end;
    char*     rest;
    if (arguments.argc != 2) {
        send_server_notice(arguments.player, arguments.console, "You did not enter IP or entered incorrect argument");
        return;
    }
    /* the two addresses, then the length and the reason of the ban */
    if (!parse_ip(arguments.argv[1], &start, &rest) || !parse_ip(_rest_after(rest), &end, &rest)) {
        send_server_notice(arguments.player, arguments.console, "Invalid IP format");
        return;
    }
    float minutes = 0.0f;
    parse_float(_rest_after(rest), &minutes, &rest);

    char start_text[64];
    char end_text[64];
    /* written out again, so the ban does not keep "001.02.3.4" as typed */
    format_ip_to_str(start_text, start);
    format_ip_to_str(end_text, end);

    struct json_object* bans;
    struct json_object* ban  = json_object_new_object();
    struct json_object* root = bans_open(&bans);
    const char*         name = _ban_range_players(server, start, end);

    json_object_object_add(ban, "Name", json_object_new_string(name));
    json_object_object_add(ban, "start_of_range", json_object_new_string(start_text));
    json_object_object_add(ban, "end_of_range", json_object_new_string(end_text));
    /* the list's "Time" is the game clock's minute a ban ends in, and 0
       never ends: a range ban writes the clock now, as it always has, so a
       length typed for it is an epoch minute and ends at once */
    if (minutes == 0) {
        json_object_object_add(ban,
                               "Time",
                               json_object_new_double(((long double) get_nanos() / (uint64_t) NANO_IN_MINUTE) + minutes));
    } else {
        json_object_object_add(ban, "Time", json_object_new_double(minutes));
    }
    if (*rest == 32 && strlen(++rest) > 0) {
        json_object_object_add(ban, "Reason", json_object_new_string(rest));
    }
    json_object_array_add(bans, ban);
    json_object_to_file(data_bans_file(), root);
    send_server_notice(arguments.player,
                       arguments.console,
                       "IP range %s-%s has been permanently banned",
                       start_text,
                       end_text);
    json_object_put(root);
}
