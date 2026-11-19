// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NET_WEBRTC_H
#define SERVER_NET_WEBRTC_H

/*
 * The browsers' transport (docs/web.md phase 2, docs/protocol-1.0.md
 * "WebRTC transport"): libdatachannel, every peer on one UDP port (ICE UDP
 * mux, [webrtc] port, default the game port + 1). A browser's offer comes
 * through the master (Net/Master.c), the answer goes back through it. Two
 * DataChannels the browser opens, "reliable" (ENet channel 0) and
 * "unreliable" (channel 1); text messages are control ("connect <n>",
 * "disconnect <reason>"), binary ones are game packets, one a message, of
 * the protocol "connect <n>" chose: n is the ENet connect data (3 = 0.75,
 * 4 = 0.76, 0x08101919 = 1.0), handed to server_conn_connect as ENet's,
 * so the profile accepts or refuses a browser as it does a UDP client.
 *
 * libdatachannel calls back on threads of its own: every callback only
 * queues an event, and webrtc_poll hands them to the game on the tick.
 */

#include <Server/Net/Conn.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct server;

/* reads [webrtc] (port; stun, turn and turns, each a comma separated list
   or an array of URLs; turn_username and turn_credential for every TURN
   entry; SP_WEBRTC_PORT, SP_WEBRTC_STUN, SP_WEBRTC_TURN, SP_WEBRTC_TURNS,
   SP_WEBRTC_TURN_USERNAME and SP_WEBRTC_TURN_CREDENTIAL win) and starts
   libdatachannel; the server still runs (no browsers) when that fails */
void webrtc_start(struct server* server);
/* on the tick: the waiting events, the answers due, the closes */
void webrtc_poll(struct server* server);
void webrtc_stop(void);

/* the ICE UDP port, 0 when WebRTC did not start */
uint16_t webrtc_port(void);

/* the STUN and TURN servers WebRTC started with, in the order given (STUN
   first), for the master's row (`ice_servers`): the browser uses the same,
   a TURN server only helping when both sides relay through it. *n is 0
   when WebRTC did not start. At most the master's 8. */
#define WEBRTC_MAX_ICE 8
typedef struct webrtc_ice
{
    char url[256];
    char username[128];   /* TURN only */
    char credential[128]; /* TURN only */
} webrtc_ice_t;
const webrtc_ice_t* webrtc_ice_servers(size_t* n);
/* the DTLS certificate's fingerprint as SDP writes it ("sha-256 AB:..."),
   empty until known */
const char* webrtc_fingerprint(void);

/* a browser's offer from the master: answered (or rejected) through
   master_answer / master_reject from webrtc_poll, within the master's 10 s */
void webrtc_offer(struct server* server, const char* offer_id, const char* sdp, const char* ip);

int webrtc_count_from(uint32_t key, const conn_t* except);

#ifdef __cplusplus
}
#endif

#endif
