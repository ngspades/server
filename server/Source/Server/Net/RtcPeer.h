// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_NET_RTCPEER_H
#define SERVER_NET_RTCPEER_H

/*
 * A thin C face of libdatachannel's C++ API for Net/WebRtc.c. The C++ API
 * rather than rtc.h: only it gives a connection's round trip (SCTP's,
 * PeerConnection::rtt), which the scoreboard and the lag compensation need
 * as they need ENet's.
 *
 * Peers are numbers. libdatachannel's threads never touch the game: every
 * callback becomes an event in a queue that rtcw_poll empties on the tick.
 * A peer's events after rtcw_close are dropped.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    RTCW_GATHERED,     /* the answer holds every candidate: rtcw_local_sdp */
    RTCW_CHANNEL_OPEN, /* channel 0 (reliable) or 1 (unreliable) */
    RTCW_MESSAGE,      /* data/len on channel; text when text is 1 */
    RTCW_CLOSED,       /* the connection or a channel closed or failed: why */
};

typedef struct
{
    int      kind;
    uint32_t peer;
    uint8_t  channel;
    uint8_t  text;
    uint8_t* data; /* malloc'd (NUL ended for text); the receiver frees it */
    size_t   len;
    char     why[32];
} rtcw_event_t;

typedef void (*rtcw_log_fn)(int warning, const char* message);

/* one STUN or TURN server: "stun:host:port", "turn:host:port" or
   "turns:host:port" (libdatachannel's URL form, "?transport=tcp" too), the
   credentials empty for STUN */
typedef struct
{
    const char* url;
    const char* username;
    const char* credential;
} rtcw_ice_t;

/* one UDP port for every peer (ICE UDP mux), the ICE servers (none: n 0;
   one libdatachannel refuses is logged and skipped), the DTLS certificate
   and key as PEM (every peer shows the same fingerprint, the one the
   master lists). 0 when it failed (why in err) */
int rtcw_init(uint16_t port, const rtcw_ice_t* ice, size_t n_ice, const char* cert_pem, const char* key_pem,
              rtcw_log_fn log, char* err, size_t err_size);
void rtcw_shutdown(void);

/* a peer answering the browser's offer; 0 when the offer was refused */
uint32_t rtcw_peer_create(const char* offer_sdp, char* err, size_t err_size);
/* the answer, malloc'd; NULL before RTCW_GATHERED */
char* rtcw_local_sdp(uint32_t peer);
/* 0 queued, -1 not open or failed */
int      rtcw_send(uint32_t peer, uint8_t channel, const void* data, size_t len, int text);
size_t   rtcw_buffered(uint32_t peer);
uint32_t rtcw_rtt(uint32_t peer);
/* the browser's address as ICE picked it, "" unknown */
void rtcw_remote_address(uint32_t peer, char* out, size_t size);
/* closes and forgets the peer */
void rtcw_close(uint32_t peer);

int rtcw_poll(rtcw_event_t* ev);

#ifdef __cplusplus
}
#endif

#endif
