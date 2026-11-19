// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The browser's server (SP_WEB_HOST, cmake/web.cmake) leaves out what opens
 * a socket or calls out: ENet's host, libdatachannel and the master. These
 * answer the calls the rest of the server makes to them the way a native
 * server with that part turned off does ([master] enabled = false, WebRTC
 * that failed to start): nothing listed, nothing counted, no token asked.
 * The worker's own connections are Net/Worker.c's.
 */

#ifdef SP_WEB_HOST

#include <Server/EventLog.h>
#include <Server/Net/EnetConn.h>
#include <Server/Net/Master.h>
#include <Server/Net/WebRtc.h>
#include <Util/Log.h>

/* ---- ENet (Net/EnetConn.c): no UDP host in a worker ---- */

int conn_enet_count_from(uint32_t key, const conn_t* except)
{
    (void) key;
    (void) except;
    return 0;
}

/* ---- WebRTC (Net/WebRtc.c): the page itself carries the connections ---- */

void webrtc_start(struct server* server)
{
    (void) server;
}

void webrtc_stop(void) {}

int webrtc_count_from(uint32_t key, const conn_t* except)
{
    (void) key;
    (void) except;
    return 0;
}

/* ---- the master (Net/Master.c): an offline match is listed nowhere ---- */

void master_start(struct server* server)
{
    (void) server;
    LOG_STATUS("Master server: off (the browser's server is not listed)");
    evt_log(NULL, "master", "action=off why=web_host");
}

void master_tick(struct server* server)
{
    (void) server;
}

void master_stop(void) {}

/* a token proves a connection came through our master: the page's own
   client never does, and needs none (a local address on a native server) */
const char* master_token_needed(const conn_t* conn)
{
    (void) conn;
    return NULL;
}

int master_token_check(const uint8_t* nonce,
                       size_t         nonce_len,
                       const uint8_t* cert,
                       size_t         cert_len,
                       const uint8_t* sig,
                       size_t         sig_len,
                       const char**   why)
{
    (void) nonce;
    (void) nonce_len;
    (void) cert;
    (void) cert_len;
    (void) sig;
    (void) sig_len;
    *why = "web_host";
    return 0;
}

#endif
