// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NET_ENETCONN_H
#define SERVER_NET_ENETCONN_H

/*
 * The native clients' transport: ENet, as the server always spoke it. One
 * conn_t per ENet peer slot, made with the host, so a conn lives exactly as
 * long as the peer it stands for (a player keeps pointing at its slot after
 * a disconnect, as it kept pointing at the peer).
 */

#include <Server/Net/Conn.h>
#include <enet/enet.h>

struct server;

/* the host on args' port; NULL when it could not be made */
ENetHost* conn_enet_start(struct server* server, uint16_t port, size_t peers, size_t channels, uint32_t in_bw,
                          uint32_t out_bw);
/* every waiting ENet event, through server_conn_* */
void conn_enet_poll(struct server* server);
/* the conn of a peer slot of the host */
conn_t* conn_enet_of(ENetPeer* peer);
void    conn_enet_stop(void);

int     conn_enet_count_from(uint32_t key, const conn_t* except);

#endif
