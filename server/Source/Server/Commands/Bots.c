// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Server/Audit.h>

void hostcmd_execute(server_t* server, player_t* requester, uint8_t command, uint8_t arg1, uint8_t arg2);

/* Requests come from other threads (the console) or from the network thread
   (PACKET_HOSTCMD). ENet must only be driven from the network thread, so this
   only records the intent; the tick drains the queue and runs the command. */
void hostcmd_apply(server_t* server, player_t* requester, uint8_t command, uint8_t arg1, uint8_t arg2)
{
    for (size_t i = 0; i < sizeof(server->host_queue) / sizeof(server->host_queue[0]); i++) {
        if (server->host_queue[i].used) {
            continue;
        }
        server->host_queue[i].command      = command;
        server->host_queue[i].arg1         = arg1;
        server->host_queue[i].arg2         = arg2;
        server->host_queue[i].requester_id = (requester == NULL) ? 255 : requester->id;
        server->host_queue[i].used         = 1;
        return;
    }
    LOG_WARNING("Host command queue is full, dropping command %u", command);
}

/* runs every queued request; call from the network thread */
void hostcmd_process_queue(server_t* server)
{
    for (size_t i = 0; i < sizeof(server->host_queue) / sizeof(server->host_queue[0]); i++) {
        struct host_request* request = &server->host_queue[i];
        if (!request->used) {
            continue;
        }
        request->used = 0;

        player_t* requester = NULL;
        if (request->requester_id != 255) {
            HASH_FIND(hh, server->players, &request->requester_id, sizeof(request->requester_id), requester);
        }
        hostcmd_execute(server, requester, request->command, request->arg1, request->arg2);
    }
}

#include <Server/Bots.h>
#include <Server/HostPackets.h>
#include <Server/Commands/CommandManager.h>
#include <Server/Packets/Packets.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Log.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <string.h>

/* Console commands and the shared host action handler for bots.
   The menu (PACKET_HOSTCMD) and the server console both funnel into
   hostcmd_apply() so the permission check and the status broadcast live in
   one place. */

static uint8_t hostcmd_team_from_string(char* string)
{
    if (strcmp(string, "blue") == 0) {
        return 0; // TEAM_A
    }
    if (strcmp(string, "green") == 0 || strcmp(string, "red") == 0) { /* red: the old name of team 2 */
        return 1; // TEAM_B
    }
    return 2; // balance
}

/* picks the team with fewer players (bots included), used when the caller asks
   for a balanced add instead of naming a team */
static uint8_t hostcmd_balanced_team(server_t* server)
{
    return (server->protocol.num_team_users[TEAM_A] <= server->protocol.num_team_users[TEAM_B]) ? TEAM_A : TEAM_B;
}

void hostcmd_execute(server_t* server, player_t* requester, uint8_t command, uint8_t arg1, uint8_t arg2)
{
    uint8_t console = (requester == NULL);
    if (!console && !player_is_host(server, requester)) {
        send_server_notice(requester, console, "You are not allowed to manage bots");
        return;
    }
    audit_event(requester, console, "host", "cmd=%u arg1=%u arg2=%u", (unsigned) command, (unsigned) arg1,
                (unsigned) arg2);

    switch (command) {
        case HOSTCMD_ADD_BOTS: {
            /* a whole team at once: 64 a side (SERVER_MAX_PLAYERS) */
            uint8_t  count = arg1 == 0 ? 1 : (arg1 > SERVER_MAX_PLAYERS / 2 ? SERVER_MAX_PLAYERS / 2 : arg1);
            uint8_t  team  = (arg2 > 2) ? 2 : arg2; /* 2 == balance */
            uint32_t added = 0;
            for (uint8_t i = 0; i < count; i++) {
                /* resolve the team per bot so "balance" keeps the teams even */
                uint8_t spawn_team = (team == 2) ? hostcmd_balanced_team(server) : team;
                if (bot_create(server, spawn_team) != NULL) {
                    added++;
                }
            }
            LOG_INFO("Added %u bot(s)", added);
            send_server_notice(requester, console, "Added %u bots", added);
            break;
        }
        case HOSTCMD_KICK_BOTS: {
            uint32_t removed = bot_remove_all(server);
            LOG_INFO("Removed %u bot(s)", removed);
            send_server_notice(requester, console, "Removed %u bots", removed);
            break;
        }
        case HOSTCMD_SET_AUTOFILL: {
            bot_set_autofill(server, arg1);
            if (arg1 == 0) {
                LOG_INFO("Autofill disabled");
                send_server_notice(requester, console, "Autofill disabled");
            } else {
                LOG_INFO("Autofill target set to %u", arg1);
                send_server_notice(requester, console, "Autofill target set to %u", arg1);
            }
            break;
        }
        case HOSTCMD_KICK_BOT: {
            if (bot_remove(server, arg1)) {
                LOG_INFO("Removed bot #%u", arg1);
                send_server_notice(requester, console, "Removed bot #%u", arg1);
            } else {
                send_server_notice(requester, console, "Bot #%u does not exist", arg1);
            }
            break;
        }
        case HOSTCMD_FILL: {
            uint32_t added = 0;
            for (uint8_t team = 0; team < 2; team++) {
                while (server->protocol.num_team_users[team] < arg1) {
                    if (bot_create(server, team) == NULL) {
                        break;
                    }
                    added++;
                }
            }
            LOG_INFO("Filled teams with %u bot(s)", added);
            send_server_notice(requester, console, "Added %u bots", added);
            break;
        }
        case HOSTCMD_TEAM_BOTS: {
            /* the Bots menu's "- n +" of one team: exactly arg1 bots there,
               added or the newest removed */
            if (arg2 > 1) {
                send_server_notice(requester, console, "Unknown team %u", arg2);
                break;
            }
            int       have = 0;
            player_t *p, *tmp;
            HASH_ITER(hh, server->players, p, tmp)
            {
                have += p->is_bot && p->team == arg2;
            }
            int added = 0, removed = 0;
            while (have < arg1 && bot_create(server, arg2) != NULL) {
                have++;
                added++;
            }
            while (have > arg1) {
                player_t* last = NULL;
                HASH_ITER(hh, server->players, p, tmp)
                {
                    if (p->is_bot && p->team == arg2 && (last == NULL || p->id > last->id)) {
                        last = p;
                    }
                }
                if (last == NULL || !bot_remove(server, last->id)) {
                    break;
                }
                have--;
                removed++;
            }
            LOG_INFO("Team %u: %d bot(s), %d added, %d removed", arg2 + 1, have, added, removed);
            send_server_notice(requester, console, "Team %u: %d bots", arg2 + 1, have);
            break;
        }
        case HOSTCMD_DIFFICULTY: {
            if (arg1 >= BOT_DIFFICULTY_COUNT) {
                send_server_notice(requester, console, "Unknown difficulty %u", arg1);
                break;
            }
            bot_set_difficulty(server, arg1);
            send_server_notice(requester, console, "Bot difficulty: %s", bot_difficulty_name(arg1));
            break;
        }
        case HOSTCMD_TUTORIAL_BOT: {
            /* the tutorial's target: a dummy beside the player (even the
               weakest bot killed a beginner within 2 s) */
            uint8_t   team    = arg1 > 1 ? TEAM_B : arg1;
            player_t* created = bot_create_beside(server, team, 0.05f, 1);
            if (created != NULL && requester != NULL) {
                /* right by the player: a spawn area of a large map is
                   ~190 blocks wide (Busan_Port), and the target stood behind
                   a hill */
                created->spawn_beside = (uint8_t) (requester->id + 1);
            }
            if (created != NULL) {
                LOG_INFO("Tutorial bot %s (#%hhu)", created->name, created->id);
            }
            break;
        }
        default:
            send_server_notice(requester, console, "Unknown host command");
            return;
    }
}

void cmd_addbot(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint8_t   count  = 1;
    uint8_t   team   = 2;
    if (arguments.argc > 1) {
        if (!parse_byte(arguments.argv[1], &count, NULL) || count == 0) {
            send_server_notice(arguments.player, arguments.console, "Invalid bot count \"%s\"", arguments.argv[1]);
            return;
        }
    }
    if (arguments.argc > 2) {
        team = hostcmd_team_from_string(arguments.argv[2]);
    }
    if (count > SERVER_MAX_PLAYERS / 2) {
        count = SERVER_MAX_PLAYERS / 2;
    }
    hostcmd_apply(server, arguments.console ? NULL : arguments.player, HOSTCMD_ADD_BOTS, count, team);
}

/* diagnostic: dump what the server sees in one map column, so map geometry
   and spawn positions can be compared without guessing */
void cmd_colinfo(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    if (arguments.argc < 3) {
        LOG_INFO("usage: /colinfo <x> <y>");
        return;
    }
    int x = atoi(arguments.argv[1]);
    int y = atoi(arguments.argv[2]);
    char solid[80] = {0};
    int  n = 0;
    for (int z = 0; z < (int) server->s_map.map.size_z && n < 70; z++) {
        if (map_solid(&server->s_map.map, (uint16_t) x, (uint16_t) y, (uint16_t) z)) {
            n += snprintf(solid + n, sizeof(solid) - n, " %d", z);
        }
    }
    LOG_INFO("COLINFO x=%d y=%d top_block=%d solid_z:%s", x, y, map_top(&server->s_map.map, x, y),
             n ? solid : " (none)");
}

void cmd_kickbots(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    hostcmd_apply(server, arguments.console ? NULL : arguments.player, HOSTCMD_KICK_BOTS, 0, 0);
}

void cmd_kickbot(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint8_t   id     = 0;
    if (arguments.argc < 2 || !parse_player(arguments.argv[1], &id, NULL)) {
        send_server_notice(arguments.player, arguments.console, "You did not enter a valid bot id");
        return;
    }
    hostcmd_apply(server, arguments.console ? NULL : arguments.player, HOSTCMD_KICK_BOT, id, 0);
}

void cmd_botfill(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint8_t   target = 0;
    if (arguments.argc < 2 || !parse_byte(arguments.argv[1], &target, NULL)) {
        send_server_notice(arguments.player, arguments.console, "You did not enter a valid player count");
        return;
    }
    hostcmd_apply(server, arguments.console ? NULL : arguments.player, HOSTCMD_FILL, target, 0);
}

void cmd_autofill(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint8_t   target = 0;
    if (arguments.argc < 2 || !parse_byte(arguments.argv[1], &target, NULL)) {
        send_server_notice(arguments.player, arguments.console, "You did not enter a valid autofill target");
        return;
    }
    hostcmd_apply(server, arguments.console ? NULL : arguments.player, HOSTCMD_SET_AUTOFILL, target, 0);
}

void cmd_bots(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    uint32_t  bots   = bot_count(server);
    uint32_t  humans = 0;
    player_t *connected_player, *tmp;
    HASH_ITER(hh, server->players, connected_player, tmp)
    {
        if (!bot_exists(connected_player) && is_past_join_screen(connected_player)) {
            humans++;
        }
    }
    send_server_notice(arguments.player,
                       arguments.console,
                       "Bots: %u, Humans: %u, Autofill target: %u, Bot support: %s",
                       bots,
                       humans,
                       bot_autofill_target(server),
                       bots_enabled(server) ? "yes" : "no");
    LOG_INFO("Bots: %u, Humans: %u, Autofill target: %u", bots, humans, bot_autofill_target(server));
}
