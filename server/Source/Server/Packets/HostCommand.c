// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Bots.h>
#include <Server/Commands/Commands.h>
#include <Server/Commands/CommandManager.h>
#include <Server/HostPackets.h>
#include <Server/Packets/Packets.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Server.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Log.h>
#include <uthash/uthash.h>

/* The lowest player id connected to a listen server is treated as the owner
   (the local player that spawned the dedicated process). Bots have no peer and
   are never the owner. */
static uint8_t is_listen_owner(server_t* server, player_t* player)
{
    uint8_t  lowest_id = 255;
    uint8_t  found     = 0;
    player_t *connected_player, *tmp;
    HASH_ITER(hh, server->players, connected_player, tmp)
    {
        if (connected_player->conn != NULL && connected_player->id < lowest_id) {
            lowest_id = connected_player->id;
            found     = 1;
        }
    }
    return found && player->id == lowest_id;
}

uint8_t player_is_host(server_t* server, player_t* player)
{
    if (player == NULL) {
        return 1; // console
    }
    if ((player->permissions & (1u << 3)) != 0) { // admin access level (raw, avoids recursion with player_has_permission)
        return 1;
    }
    if (server->listen_mode && is_listen_owner(server, player)) {
        return 1;
    }
    return 0;
}

void receive_host_command(server_t* server, player_t* player, stream_t* data)
{
    uint8_t received_id = stream_read_u8(data);
    uint8_t command     = stream_read_u8(data);
    uint8_t arg1        = stream_read_u8(data);
    uint8_t arg2        = stream_read_u8(data);
    if (player->id != received_id) {
        LOG_WARNING("Assigned ID: %d doesnt match sent ID: %d in host command packet", player->id, received_id);
    }
    hostcmd_apply(server, player, command, arg1, arg2);
}
