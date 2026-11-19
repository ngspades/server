// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Net/EnetConn.h>
#include <Server/EventLog.h>
#include <Server/Ping.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Log.h>
#include <netaddr/netaddr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ENetHost* enet_conn_host;
static conn_t*   enet_conns; /* one per peer slot of the host */

/* ENet resets a peer before it hands out its DISCONNECT event, so the line's
   quality at the moment it dropped is gone by then: it is sampled while the
   peer is connected and read back at the event */
typedef struct {
    uint8_t  valid;
    uint32_t rtt;
    uint32_t loss_percent;
    size_t   pending;
} link_sample_t;
static link_sample_t* enet_samples; /* parallel to enet_conns */
static uint32_t       enet_sampled_at;

conn_t* conn_enet_of(ENetPeer* peer)
{
    return &enet_conns[peer - enet_conn_host->peers];
}

static int _enet_send(conn_t* conn, uint8_t channel, const uint8_t* data, size_t len, int reliable)
{
    ENetPacket* packet = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send((ENetPeer*) conn->handle, channel, packet) != 0) {
        enet_packet_destroy(packet);
        return -1;
    }
    return 0;
}

static void _enet_disconnect(conn_t* conn, uint32_t reason, conn_close_t how)
{
    ENetPeer* peer = (ENetPeer*) conn->handle;
    switch (how) {
        case CONN_CLOSE_NOW:
            enet_peer_disconnect_now(peer, reason);
            break;
        /* a goodbye waits for the peer's acknowledgement, up to ENet's 30 s
           for one that never answers (a forged connect, a dead client),
           holding its player slot all along: 1 to 3 s is ample for a
           real client to answer */
        case CONN_CLOSE_LATER:
            enet_peer_timeout(peer, 0, 1000, 3000);
            enet_peer_disconnect_later(peer, reason);
            break;
        default:
            enet_peer_timeout(peer, 0, 1000, 3000);
            enet_peer_disconnect(peer, reason);
            break;
    }
}

static uint32_t _enet_rtt(const conn_t* conn)
{
    return ((const ENetPeer*) conn->handle)->roundTripTime;
}

/* what the peer still holds: the reliable bytes in flight (sent, waiting for
   an acknowledgement) plus every command queued to go out. ENet only sends
   its queue when the host is serviced, and never more than its window at
   once, so a sender that does not read this writes megabytes into the queue
   of a slow client and it takes minutes to drain them */
static size_t _pending_list(const ENetList* list)
{
    /* a packet larger than the MTU travels as several commands sharing one
       packet, so the packet's own length would count once per fragment: the
       fragment's length is the bytes that really go out */
    size_t           pending = 0;
    ENetListIterator it;
    for (it = enet_list_begin(list); it != enet_list_end(list); it = enet_list_next(it)) {
        pending += ((const ENetOutgoingCommand*) it)->fragmentLength;
    }
    return pending;
}

static size_t _enet_pending(const conn_t* conn)
{
    const ENetPeer* peer = (const ENetPeer*) conn->handle;
    /* an acknowledged command waits in outgoingSendReliableCommands until the
       window lets it out (the map's fragments, reliable, land there), the
       rest in outgoingCommands; sentReliableCommands is counted by
       reliableDataInTransit */
    return peer->reliableDataInTransit + _pending_list(&peer->outgoingSendReliableCommands) +
           _pending_list(&peer->outgoingCommands);
}

static const conn_transport_t enet_transport = {"enet", _enet_send, _enet_disconnect, _enet_rtt, _enet_pending};

ENetHost* conn_enet_start(struct server* server, uint16_t port, size_t peers, size_t channels, uint32_t in_bw,
                          uint32_t out_bw)
{
    (void) server;
    /* both families on one socket, so a host with IPv6 alone can be joined
       (IPv4 players arrive as ::ffff:a.b.c.d, unmapped on connect) */
    ENetAddressType type;
    enet_conn_host = netaddr_host_create(port, peers, channels, in_bw, out_bw, &type);
    if (enet_conn_host == NULL) {
        return NULL;
    }
    LOG_STATUS("Listening on UDP port %u over %s", (unsigned) port,
               type == ENET_ADDRESS_TYPE_ANY ? "IPv4 and IPv6" : "IPv4 only (no IPv6 on this machine)");
    enet_conns   = calloc(enet_conn_host->peerCount, sizeof *enet_conns);
    enet_samples = calloc(enet_conn_host->peerCount, sizeof *enet_samples);
    for (size_t i = 0; i < enet_conn_host->peerCount; i++) {
        enet_conns[i].transport = &enet_transport;
        enet_conns[i].handle    = &enet_conn_host->peers[i];
        enet_conns[i].player_id = CONN_NO_PLAYER;
        enet_conns[i].family    = 4;
    }
    if (enet_host_compress_with_range_coder(enet_conn_host) != 0) {
        LOG_WARNING("Compress with range coder failed");
    }
    enet_conn_host->intercept = &raw_udp_intercept_callback;
    return enet_conn_host;
}

void conn_enet_stop(void)
{
    enet_host_destroy(enet_conn_host);
    enet_conn_host = NULL;
    free(enet_conns);
    enet_conns = NULL;
}

int conn_enet_count_from(uint32_t key, const conn_t* except)
{
    int n = 0;
    for (size_t k = 0; enet_conn_host != NULL && k < enet_conn_host->peerCount; k++) {
        ENetPeer* p = &enet_conn_host->peers[k];
        if (&enet_conns[k] != except && p->state == ENET_PEER_STATE_CONNECTED && enet_conns[k].key == key) {
            n++;
        }
    }
    return n;
}

/* a client back from a suspend connects again from the same address and
   port while its old peer still waits for ENet's timeout (up to 30 s):
   that player would stay in the game as a ghost beside the new one. The
   same address and port is the same socket, so the old peer goes now */
static void _drop_stale_peer(struct server* server, ENetPeer* fresh)
{
    for (size_t i = 0; i < enet_conn_host->peerCount; i++) {
        ENetPeer* old = &enet_conn_host->peers[i];
        if (old == fresh || old->state != ENET_PEER_STATE_CONNECTED
            || !enet_address_equal(&old->address, &fresh->address)) {
            continue;
        }
        LOG_INFO("Dropped the stale connection of player #%u: its client connected again",
                 (unsigned) enet_conns[i].player_id);
        server_conn_closed(server, &enet_conns[i]);
        enet_conns[i].player_id = CONN_NO_PLAYER;
        enet_peer_reset(old);
    }
}

static void _sample_links(void)
{
    uint32_t now = enet_time_get();
    if ((uint32_t) (now - enet_sampled_at) < 1000) {
        return;
    }
    enet_sampled_at = now;
    for (size_t i = 0; i < enet_conn_host->peerCount; i++) {
        const ENetPeer* peer = &enet_conn_host->peers[i];
        if (peer->state != ENET_PEER_STATE_CONNECTED) {
            continue;
        }
        enet_samples[i].valid        = 1;
        enet_samples[i].rtt          = peer->roundTripTime;
        enet_samples[i].loss_percent = (uint32_t) ((uint64_t) peer->packetLoss * 100 / ENET_PEER_PACKET_LOSS_SCALE);
        enet_samples[i].pending      = _enet_pending(&enet_conns[i]);
    }
}

static void _log_closed(const ENetPeer* peer, int timed_out)
{
    link_sample_t* sample = &enet_samples[peer - enet_conn_host->peers];
    if (!sample->valid) {
        return;
    }
    const char* kind = timed_out ? "timeout" : "quit";
    LOG_STATUS("Peer closed (%s): last rtt %u ms, loss %u%%, %zu bytes unacknowledged", kind, sample->rtt,
               sample->loss_percent, sample->pending);
    evt_log(NULL, "net", "action=closed kind=%s rtt=%u loss=%u pending=%zu", kind, sample->rtt,
            sample->loss_percent, sample->pending);
    sample->valid = 0;
}

void conn_enet_poll(struct server* server)
{
    ENetEvent event;
    _sample_links();
    while (enet_host_service(enet_conn_host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_NONE:
                LOG_STATUS("Event of type none received. Ignoring");
                break;
            case ENET_EVENT_TYPE_CONNECT:
            {
                _drop_stale_peer(server, event.peer);
                enet_samples[event.peer - enet_conn_host->peers].valid = 0;
                /* the reliable window shrinks with the packet throttle, to one MTU per round trip when the
                   round trip varies, and our entity deltas are reliable: with no deceleration a jittery
                   line keeps its window instead of letting the client's view fall seconds behind */
                enet_peer_throttle_configure(event.peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                             ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
                /* ENet's own minimum is 5 s: a phone or a browser tab stalling for a few seconds must not
                   lose its player. A goodbye still shortens it (_enet_disconnect) */
                enet_peer_timeout(event.peer, 0, 15000, 40000);
                conn_t* conn    = conn_enet_of(event.peer);
                conn->player_id = CONN_NO_PLAYER;
                ENetAddress from = event.peer->address;
                netaddr_unmap(&from);
                conn->family = (uint8_t) netaddr_family(&from);
                memset(conn->ip4, 0, 4);
                if (conn->family == 4) {
                    memcpy(conn->ip4, from.host.v4, 4);
                }
                netaddr_string(&from, conn->address, sizeof conn->address);
                conn->key = conn_key_of(conn->address);
                server_conn_connect(server, conn, event.data);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: /* enet6 reports a timeout apart; the game treats it as a disconnect */
                _log_closed(event.peer, event.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT);
                server_conn_closed(server, conn_enet_of(event.peer));
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                server_conn_receive(server, conn_enet_of(event.peer), event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
        }
    }
}
