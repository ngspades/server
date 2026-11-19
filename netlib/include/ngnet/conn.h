#ifndef NGNET_CONN_H
#define NGNET_CONN_H

/*
 * ngnet: a server's view of one client connection, whatever carries it
 * (ENet, WebRTC DataChannels, a Web Worker). The program only ever sends
 * through conn_send and hangs up through conn_disconnect, so a carrier is
 * one more set of these functions. No game types in here: the owner of a
 * connection is an opaque small id (player_id) the program assigns.
 */

#include <stddef.h>
#include <stdint.h>

/* no player yet: a refused or not yet accepted connection */
#define CONN_NO_PLAYER 0xFF

typedef enum
{
    CONN_CLOSE_NOW,   /* at once, no goodbye event (enet_peer_disconnect_now) */
    CONN_CLOSE_LATER, /* once the queued packets went out (a Kick's text first) */
    CONN_CLOSE,       /* a polite goodbye; the close event comes back later */
} conn_close_t;

typedef struct conn conn_t;

typedef struct conn_transport
{
    const char* name; /* "enet", "webrtc": logs and the network counters */
    /* 0 when the packet went into the transport's queue */
    int (*send)(conn_t* conn, uint8_t channel, const uint8_t* data, size_t len, int reliable);
    void (*disconnect)(conn_t* conn, uint32_t reason, conn_close_t how);
    uint32_t (*rtt)(const conn_t* conn); /* round trip in ms, 0 unknown */
    /* bytes queued or in flight, not yet acknowledged: a sender that must not
       run ahead of a slow link paces itself by it (the map transfer, 7.2) */
    size_t (*pending)(const conn_t* conn);
} conn_transport_t;

struct conn
{
    const conn_transport_t* transport;
    void*                   handle;    /* the transport's own: ENetPeer*, a WebRTC peer */
    uint8_t                 player_id; /* CONN_NO_PLAYER until a player owns it */
    uint8_t                 family;    /* 4 or 6 */
    uint8_t                 ip4[4];    /* IPv4 (the bans, p.ip); 0.0.0.0 for IPv6 */
    char                    address[64]; /* for logs: "1.2.3.4" or "2001:db8::1" */
    /* the address as one number for the connection guard (NetGuard.c):
       IPv4 as it is, an IPv6 /64 hashed (one home has a whole /64) */
    uint32_t                key;
};

static inline int conn_send(conn_t* conn, uint8_t channel, const uint8_t* data, size_t len, int reliable)
{
    return conn->transport->send(conn, channel, data, len, reliable);
}

static inline void conn_disconnect(conn_t* conn, uint32_t reason, conn_close_t how)
{
    conn->transport->disconnect(conn, reason, how);
}

static inline uint32_t conn_rtt(const conn_t* conn)
{
    return conn != NULL ? conn->transport->rtt(conn) : 0;
}

/* what the connection still holds: 0 for a bot and for a transport with no
   queue of its own */
static inline size_t conn_pending(const conn_t* conn)
{
    return conn != NULL ? conn->transport->pending(conn) : 0;
}

#endif
