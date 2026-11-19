// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NET_CONN_H
#define SERVER_NET_CONN_H

/*
 * A client's connection, whatever carries it (docs/web.md phase 1): ENet for
 * the native clients (Net/EnetConn.c), WebRTC DataChannels for the browser
 * (Net/WebRtc.cpp). The game only ever sends through conn_send and hangs up
 * through conn_disconnect, so a transport is one more set of these functions.
 * A player's conn is NULL for a bot, as its ENet peer was.
 *
 * Every transport runs its events on the tick (conn_*_poll): a connect, a
 * packet and a close reach the game only through server_conn_* below.
 */

#include <stddef.h>
#include <stdint.h>

#include <ngnet/conn.h>

/* conn_send for the 1.0 senders: reliable on channel 0, counted by the
   network statistics. 0 when it was queued */
int conn_send_reliable(conn_t* conn, const uint8_t* data, size_t len);

struct server;
/* the transport's events, on the tick. connect_data is ENet's (or the
   WebRTC "connect <n>"): the protocol a client asks for */
void server_conn_connect(struct server* server, conn_t* conn, uint32_t connect_data);
void server_conn_receive(struct server* server, conn_t* conn, const uint8_t* data, size_t len);
void server_conn_closed(struct server* server, conn_t* conn);
/* a player (one kept for its grenades too) still points at conn */
int server_conn_in_use(struct server* server, const conn_t* conn);

/* the open connections of every transport whose key is key, except one
   (the connection guard, Server/NetGuard.c) */
int conn_count_from(uint32_t key, const conn_t* except);
/* key of an address (conn_t.key): IPv4's 32 bits, a hash of an IPv6 /64 */
uint32_t conn_key_of(const char* address);
/* an IPv6 address (no brackets, no port) to the /64 it is in,
   "2001:db8:1:2::/64": what a ban of an IPv6 player holds, since one home
   has a whole /64. 0 for anything else (IPv4, a mapped IPv4, not an
   address) */
int conn_ipv6_prefix(const char* address, char* out, size_t size);
/* the player's /64 into out when it plays over IPv6, else NULL */
struct player;
const char* conn_player_prefix(const struct player* player, char* out, size_t size);

#endif
