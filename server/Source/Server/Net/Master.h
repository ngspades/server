// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NET_MASTER_H
#define SERVER_NET_MASTER_H

/*
 * Our own server list (docs/web.md phase 3; the API is the master
 * repository's README): signed HTTPS calls through libcurl on threads of
 * their own (PluginNet.c), never on the tick.
 *
 * - announce: at start, every 30 s and when the name, map, mode or player
 *   count changes, once over IPv4 and once over IPv6 (the master lists the
 *   address each call came from);
 * - poll: one long poll waiting for browsers' offers, handed to WebRTC
 *   (Net/WebRtc.cpp), whose answer or refusal goes back within 10 s;
 * - leave: on shutdown.
 *
 * Every body is signed with the server's Ed25519 key, made once and kept in
 * the data folder (master.key): it is the server's identity on the list.
 * [master] enabled, url, country in config.toml; SP_MASTER_URL (turns it
 * on) and SP_MASTER=0 win. EVT master action=listed|error|left family=4|6.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <Server/Net/Conn.h>
#include <stddef.h>
#include <stdint.h>

struct server;

void master_start(struct server* server);
/* on the tick: finished calls, the announce period, new polls */
void master_tick(struct server* server);
/* sends leave (waits up to 3 s for it) */
void master_stop(void);

/* WebRTC's answer to an offer, or its refusal (the reason is shown to the
   player) */
void master_answer(const char* offer_id, const char* sdp);
void master_reject(const char* offer_id, const char* reason);

/* connect tokens: the reason a ClientCertificate must carry one from this
   connection ("internet"), NULL when it need not (tokens off, no key from
   the master lately, a local address, WebRTC) */
const char* master_token_needed(const conn_t* conn);
/* 1 when the certificate is the master's token for this server and this
   connection's Greeting nonce; else 0 and why (format, server, expired,
   nonce, signature) */
int master_token_check(const uint8_t* nonce,
                       size_t         nonce_len,
                       const uint8_t* cert,
                       size_t         cert_len,
                       const uint8_t* sig,
                       size_t         sig_len,
                       const char**   why);

#ifdef __cplusplus
}
#endif

#endif
