// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The browser's server's transport (Net/Worker.h): a client is a pair of
 * MessagePorts the page handed over, and the wire on them is the WebRTC
 * one (docs/protocol-1.0.md 3.4), so a client's code for it is the one it
 * already has for a server's DataChannels (the client's transport_worker.c).
 *
 * The client sends "connect <n>" as its first message on "reliable", before
 * its first 1.0 packet (InitiateConnection): the ports keep their order, so
 * the server always meets the text first. The desktop's join by code once
 * sent the game's handshake first and the server dropped it as
 * not_initiated (docs/protocol-1.0.md 3.4); a packet before the
 * "connect" is dropped here too, and logged.
 *
 * JavaScript only queues (server/web/pre.js, worker_lib.js): every event is
 * taken here, on the tick, so no game code runs from a message callback.
 */

#ifdef SP_WEB_HOST

#include <Server/EventLog.h>
#include <Server/Net/Worker.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* worker_lib.js: info[0] the kind, [1] the peer, [2] the channel, [3] the
   message's length */
extern int  sd_srv_peek(int* info);
extern void sd_srv_pop(void* into);
extern void sd_srv_send(int peer, int channel, const void* data, int len, int text);
extern void sd_srv_close(int peer);

enum
{
    WORKER_EV_OPEN   = 1, /* the page handed a client's ports over */
    WORKER_EV_TEXT   = 2,
    WORKER_EV_BINARY = 3,
    WORKER_EV_CLOSED = 4, /* the page dropped the client (its ports closed) */
};

/* from the ports to "connect": the page sends it with the ports, so only
   a broken page is this slow */
#define WORKER_CONNECT_NS (10ULL * NANO_IN_SECOND)

typedef struct worker_peer
{
    conn_t              conn; /* conn.handle points back here */
    int                 id;   /* the page's */
    uint64_t            created;
    uint8_t             connected; /* "connect" came */
    uint8_t             closing;
    uint8_t             close_event; /* closing: tell the game when closed */
    uint8_t             dead;        /* closed, freed once no player points at it */
    uint8_t             warned_early;
    struct worker_peer* next;
} worker_peer_t;

static worker_peer_t* worker_peers;
static uint64_t       worker_sent;

static int _worker_send(conn_t* conn, uint8_t channel, const uint8_t* data, size_t len, int reliable)
{
    (void) reliable; /* the channel says it, as on WebRTC: 0 reliable, 1 not */
    worker_peer_t* p = (worker_peer_t*) conn->handle;
    if (p->closing || p->dead) {
        return -1;
    }
    sd_srv_send(p->id, channel > 1 ? 0 : channel, data, (int) len, 0);
    worker_sent += len;
    return 0;
}

static void _worker_disconnect(conn_t* conn, uint32_t reason, conn_close_t how)
{
    worker_peer_t* p = (worker_peer_t*) conn->handle;
    if (p->closing || p->dead) {
        return;
    }
    /* the reason ENet would carry in its disconnect data, as text: a binary
       message is always a game packet */
    char text[32];
    int  n = snprintf(text, sizeof text, "disconnect %u", (unsigned) reason);
    sd_srv_send(p->id, 0, text, n, 1);
    p->closing = 1;
    /* ENet's disconnect_now brings no event back: neither does this */
    p->close_event = how != CONN_CLOSE_NOW;
    evt_log(NULL, "worker", "action=closing peer=%d reason=%u how=%s", p->id, (unsigned) reason,
            how == CONN_CLOSE_NOW ? "now" : (how == CONN_CLOSE_LATER ? "later" : "close"));
}

/* the page is the same machine and a message takes no network: a round
   trip of 1 ms ("0" is "unknown" to the lag compensation and the board) */
static uint32_t _worker_rtt(const conn_t* conn)
{
    (void) conn;
    return 1;
}

/* a MessagePort queues without limit and delivers at once: nothing to wait
   for, the map transfer goes at its own pace (64 fragments a tick) */
static size_t _worker_pending(const conn_t* conn)
{
    (void) conn;
    return 0;
}

static const conn_transport_t worker_transport = {"worker", _worker_send, _worker_disconnect, _worker_rtt,
                                                  _worker_pending};

size_t worker_peer_count(void)
{
    size_t n = 0;
    for (worker_peer_t* p = worker_peers; p != NULL; p = p->next) {
        n += p->connected && !p->dead && p->conn.player_id != CONN_NO_PLAYER;
    }
    return n;
}

uint64_t worker_sent_bytes(void)
{
    return worker_sent;
}

void worker_sent_reset(void)
{
    worker_sent = 0;
}

static worker_peer_t* _peer(int id)
{
    for (worker_peer_t* p = worker_peers; p != NULL; p = p->next) {
        if (p->id == id && !p->dead) {
            return p;
        }
    }
    return NULL;
}

static void _open(int id)
{
    worker_peer_t* p = calloc(1, sizeof *p);
    if (p == NULL) {
        sd_srv_close(id);
        return;
    }
    p->id             = id;
    p->created        = get_real_nanos();
    p->conn.transport = &worker_transport;
    p->conn.handle    = p;
    p->conn.player_id = CONN_NO_PLAYER;
    /* the page's own client, on this machine: a local address, as the
       desktop's listen server sees its player on 127.0.0.1 (no connect
       token, the connection guard's local rules) */
    p->conn.family = 4;
    p->conn.ip4[0] = 127;
    p->conn.ip4[3] = 1;
    snprintf(p->conn.address, sizeof p->conn.address, "127.0.0.1");
    p->conn.key  = conn_key_of(p->conn.address);
    p->next      = worker_peers;
    worker_peers = p;
    evt_log(NULL, "worker", "action=open peer=%d", id);
}

static void _gone(server_t* server, worker_peer_t* p, int tell, const char* why)
{
    if (p->dead) {
        return;
    }
    sd_srv_close(p->id);
    p->dead = 1;
    evt_log(NULL, "worker", "action=closed peer=%d player=%d why=%s", p->id,
            p->conn.player_id == CONN_NO_PLAYER ? -1 : (int) p->conn.player_id, why);
    if (tell && p->conn.player_id != CONN_NO_PLAYER) {
        server_conn_closed(server, &p->conn);
    }
    p->conn.player_id = CONN_NO_PLAYER;
}

static void _on_text(server_t* server, worker_peer_t* p, const char* text)
{
    unsigned long data;
    char          tail;
    /* the client's goodbye (transport_worker.c sends it as it leaves): the
       slot is free now */
    if (strncmp(text, "disconnect", 10) == 0 && p->connected) {
        _gone(server, p, 1, "peer_disconnect");
        return;
    }
    if (strncmp(text, "connect ", 8) != 0 || p->connected || sscanf(text + 8, "%lu%c", &data, &tail) != 1) {
        evt_log(NULL, "worker", "action=text_ignored peer=%d connected=%u", p->id, p->connected);
        return;
    }
    p->connected = 1;
    server_conn_connect(server, &p->conn, (uint32_t) data);
    /* after: the player it became, -1 when it was refused */
    evt_log(NULL, "worker", "action=connected peer=%d player=%d data=%lu", p->id,
            p->conn.player_id == CONN_NO_PLAYER ? -1 : (int) p->conn.player_id, data);
}

void worker_poll(server_t* server)
{
    int info[4];
    while (sd_srv_peek(info)) {
        int            kind = info[0], id = info[1], len = info[3];
        worker_peer_t* p    = kind == WORKER_EV_OPEN ? NULL : _peer(id);
        uint8_t*       data = NULL;
        if (kind == WORKER_EV_TEXT || kind == WORKER_EV_BINARY) {
            /* one more byte: a text is read as a C string */
            data = malloc((size_t) len + 1);
            if (data == NULL) {
                sd_srv_pop(NULL);
                continue;
            }
            data[len] = 0;
        }
        sd_srv_pop(data);
        switch (kind) {
            case WORKER_EV_OPEN:
                _open(id);
                break;
            case WORKER_EV_TEXT:
                if (p != NULL && !p->closing) {
                    _on_text(server, p, (const char*) data);
                }
                break;
            case WORKER_EV_BINARY:
                if (p == NULL || p->closing) {
                    break;
                }
                if (p->conn.player_id != CONN_NO_PLAYER) {
                    server_conn_receive(server, &p->conn, data, (size_t) len);
                } else if (!p->connected && !p->warned_early) {
                    /* a game packet before "connect": the client broke the
                       contract above (the not_initiated bug's shape) */
                    p->warned_early = 1;
                    evt_log(NULL, "worker", "action=dropped peer=%d why=before_connect len=%d", p->id, len);
                }
                break;
            case WORKER_EV_CLOSED:
                if (p != NULL) {
                    _gone(server, p, 1, "page_closed");
                }
                break;
            default:
                break;
        }
        free(data);
    }
    uint64_t        now = get_real_nanos();
    worker_peer_t** at  = &worker_peers;
    while (*at != NULL) {
        worker_peer_t* p = *at;
        if (!p->dead && !p->connected && !p->closing && now - p->created > WORKER_CONNECT_NS) {
            _gone(server, p, 0, "no_connect");
        } else if (!p->dead && p->closing) {
            /* the goodbye's text is in the port already, in order behind
               the packets before it: nothing to wait for */
            _gone(server, p, p->close_event, "server");
        }
        /* a player kept for its grenades still points at its conn: free
           only once none does */
        if (p->dead && !server_conn_in_use(server, &p->conn)) {
            *at = p->next;
            free(p);
            continue;
        }
        at = &p->next;
    }
}

#endif
