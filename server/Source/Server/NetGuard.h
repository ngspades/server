// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NETGUARD_H
#define SERVER_NETGUARD_H

/*
 * A cheap connection guard: at most N new connections from one address in a
 * window, and at most N peers connected from it. A single machine opening
 * hundreds of sockets (a flood, a broken reconnect loop) is refused before it
 * reaches the player table. [net] max_connections_per_ip (default 4, 0 off)
 * and connection_window_seconds (default 10); SP_MAX_PER_IP overrides it.
 *
 * The transport still accepts the socket; the guard drops it at its connect, so
 * no player, ban lookup or map download happens for it.
 */

#include <Server/Structs/ServerStruct.h>
#include <Server/Net/Conn.h>

void    netguard_init(server_t* server);
uint8_t netguard_allow(server_t* server, const conn_t* conn);

#endif
