// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NET_WORKER_H
#define SERVER_NET_WORKER_H

/*
 * The browser's server's one transport (docs/web.md phase 6):
 * the server runs in a Web Worker of the page (SP_WEB_HOST) and each client
 * reaches it through two MessagePorts the page hands over, "reliable" and
 * "unreliable", carrying exactly the WebRTC wire of docs/protocol-1.0.md
 * 3.4: text is control ("connect <n>" first, "disconnect <reason>"), a
 * binary message is one 1.0 packet. The worker's JavaScript
 * (server/web/pre.js) only queues what arrives; worker_poll hands it to the
 * game on the tick through server_conn_*, as Net/WebRtc.c does with
 * libdatachannel's events. Nothing else in the server knows the difference.
 */

#include <Server/Net/Conn.h>
#include <stddef.h>
#include <stdint.h>

struct server;

/* on the tick: the connections opened, their messages, their closes */
void worker_poll(struct server* server);

/* the connections a player owns, and what the server sent them since the
   last reset: the Tick: line's figures (ENet's host counts them natively) */
size_t   worker_peer_count(void);
uint64_t worker_sent_bytes(void);
void     worker_sent_reset(void);

#endif
