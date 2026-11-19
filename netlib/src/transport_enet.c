/*
	Copyright (c) 2026 ngspades contributors

	This file is part of ngspades.

	ngspades is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	ngspades is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with ngspades.  If not, see <http://www.gnu.org/licenses/>.
*/

/* ENet, the transport of every native build: the calls network.c made
   itself before the transport layer, unchanged */

#ifndef __EMSCRIPTEN__

#include <enet/enet.h>
#include <netaddr/netaddr.h>
#include <string.h>

#include "ngnet/host.h"
#include "ngnet/transport.h"

static ENetHost* client;
static ENetPeer* peer;
/* ENET_ADDRESS_TYPE_ANY: the socket reaches IPv4 and IPv6 servers; IPV4 on
   a machine without IPv6 */
static ENetAddressType client_type;

static void client_create(void) {
	client = netaddr_host_create(-1, 1, NGNET_CHANNELS, 0, 0, &client_type); // limit bandwidth here if you want to
	if(client)
		enet_host_compress_with_range_coder(client);
}

static void tc_enet_init(void) {
	enet_initialize();
	client_create();
	ngnet_info("Network: %s", client_type == ENET_ADDRESS_TYPE_ANY ? "IPv4 and IPv6" : "IPv4 only");
}

static int tc_enet_connect(const char* host, int port, int channels, uint32_t connect_data) {
	/* the host is kept for the client's life and reused by every connect:
	   destroying it left a dangling pointer behind (the next connect after a
	   Disconnect crashed in enet_peer_reset). Only the peer is reset */
	if(client == NULL)
		client_create();
	if(client == NULL)
		return -1;
	if(peer != NULL) {
		enet_peer_reset(peer);
		peer = NULL;
	}

	/* an IPv4 or an IPv6 address (without its brackets) or a name */
	ENetAddress address;
	if(!netaddr_resolve(host, client_type, &address)) {
		ngnet_warn("Could not resolve %s", host);
		return -1;
	}
	address.port = port;
	ngnet_event("net", "action=resolve host=%s family=%d", host, netaddr_family(&address));
	peer = enet_host_connect(client, &address, channels, connect_data);
	return peer != NULL;
}

static int tc_enet_service(struct transport_event* event, int timeout_ms) {
	ENetEvent e;
	memset(event, 0, sizeof(*event));
	if(client == NULL || enet_host_service(client, &e, timeout_ms) <= 0)
		return 0;
	switch(e.type) {
		case ENET_EVENT_TYPE_CONNECT: event->type = TRANSPORT_EVENT_CONNECT; break;
		case ENET_EVENT_TYPE_RECEIVE:
			event->type = TRANSPORT_EVENT_RECEIVE;
			event->data = e.packet->data;
			event->length = e.packet->dataLength;
			event->owned = e.packet;
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
		case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: /* enet6 reports a timeout apart; the game treats it as a disconnect */
			event->type = TRANSPORT_EVENT_DISCONNECT;
			event->reason = e.data;
			e.peer->data = NULL;
			/* ENet has freed the peer: nothing to reset any more */
			if(e.peer == peer)
				peer = NULL;
			break;
		default: event->type = TRANSPORT_EVENT_NONE; break;
	}
	return 1;
}

static void tc_enet_event_done(struct transport_event* event) {
	if(event->owned)
		enet_packet_destroy(event->owned);
	event->owned = NULL;
}

static void tc_enet_send(int channel, const void* data, size_t length, int reliable) {
	if(peer != NULL)
		enet_peer_send(peer, channel, enet_packet_create(data, length, reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
}

static int tc_enet_channels(void) {
	return peer ? (int)peer->channelCount : 0;
}

static unsigned int tc_enet_rtt(void) {
	return peer ? peer->roundTripTime : 0;
}

static int tc_enet_active(void) {
	return peer != NULL;
}

static void tc_enet_disconnect(uint32_t reason) {
	if(peer != NULL)
		enet_peer_disconnect(peer, reason);
}

static void tc_enet_disconnect_now(uint32_t reason) {
	if(peer != NULL)
		enet_peer_disconnect_now(peer, reason);
}

static void tc_enet_reset(void) {
	if(peer != NULL)
		enet_peer_reset(peer);
	peer = NULL;
}

static const char* tc_enet_error(void) {
	return "";
}

int transport_enet_peer(char* out, size_t size, int* local) {
	if(peer == NULL)
		return 0;
	netaddr_host_port(&peer->address, out, size);
	*local = netaddr_is_local(&peer->address);
	return 1;
}

/* the build's one, for every connect but a join code's */
const struct transport_impl transport_enet_impl = {
	.init = tc_enet_init,
	.connect = tc_enet_connect,
	.service = tc_enet_service,
	.event_done = tc_enet_event_done,
	.send = tc_enet_send,
	.channels = tc_enet_channels,
	.rtt = tc_enet_rtt,
	.active = tc_enet_active,
	.disconnect = tc_enet_disconnect,
	.disconnect_now = tc_enet_disconnect_now,
	.reset = tc_enet_reset,
	.error = tc_enet_error,
};

#endif
