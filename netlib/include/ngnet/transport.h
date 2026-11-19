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

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* channels of a connection: 0 reliable and ordered, 1 unreliable */
#define NGNET_CHANNELS 2

/* The one connection to a game server, whatever carries it (docs/web.md,
   phase 4; game/p2p, the join code): a build can carry more than one:
   transport_enet.c for a plain address, the WebRTC ones (the browser by
   the client's net.js, and the desktop's own by transport_rtc.cpp) for a
   join by code. network.c speaks only to these, so a native build does
   what it did when it called ENet itself. Channel 0 is reliable and
   ordered, channel 1 the movement (unreliable); a packet is the same
   bytes on either transport. */

enum {
	TRANSPORT_EVENT_NONE,
	TRANSPORT_EVENT_CONNECT,
	TRANSPORT_EVENT_RECEIVE,
	TRANSPORT_EVENT_DISCONNECT,
};

struct transport_event {
	int type;
	const uint8_t* data; /* RECEIVE: one packet, until transport_event_done */
	size_t length;
	uint32_t reason; /* DISCONNECT: what ENet carries as disconnect data */
	void* owned;
};

void transport_init(void);
/* starts a connection (the previous one is reset first). -1 when the address
   cannot be used (not resolved; on the web, not one of our servers), 0 when
   no connection could be started, 1 when started: its CONNECT or DISCONNECT
   event follows */
int transport_connect(const char* host, int port, int channels, uint32_t connect_data);
/* the next event, waiting up to timeout_ms for one natively (the web never
   waits: the browser runs its callbacks between frames). > 0 when *event
   was filled */
int transport_service(struct transport_event* event, int timeout_ms);
void transport_event_done(struct transport_event* event);
void transport_send(int channel, const void* data, size_t length, int reliable);
/* channels the server agreed to (ENet negotiates down to what it asked) */
int transport_channels(void);
/* round trip time in ms, 0 when unknown */
unsigned int transport_rtt(void);
/* a connection exists (started, open or closing) */
int transport_active(void);
/* asks the server to end it; the DISCONNECT event says when it did */
void transport_disconnect(uint32_t reason);
/* tells the server and forgets the connection at once */
void transport_disconnect_now(uint32_t reason);
/* forgets the connection without telling the server */
void transport_reset(void);
/* why the last connection could not start or open, for the player ("" when
   the transport has no text of its own: the caller says "No response") */
const char* transport_error(void);

/* ---- the implementations (one build may carry more than one) ---- */

/* every implementation holds one connection at a time; the functions
   above drive the selected one. The impl's connect is called with the
   transport's own address form: ENet's host and port, WebRTC's the
   server's row id on the master (web) or its join code (desktop) */
struct transport_impl {
	void (*init)(void); /* process one's own resources at startup; may be NULL */
	int (*connect)(const char* host, int port, int channels, uint32_t connect_data);
	int (*service)(struct transport_event* event, int timeout_ms);
	void (*event_done)(struct transport_event* event);
	void (*send)(int channel, const void* data, size_t length, int reliable);
	int (*channels)(void);
	unsigned (*rtt)(void);
	int (*active)(void);
	void (*disconnect)(uint32_t reason);
	void (*disconnect_now)(uint32_t reason);
	void (*reset)(void);
	const char* (*error)(void);
};

/* every connect picks its implementation first, never inherits the last
   one's: a join by code left the driver on WebRTC, and the next plain
   address (Singleplayer, the list, --host) went to the master as a code
   ("That is not a join code"). A switch resets what the old one still
   holds. transport_use_address: the build's plain one (ENet; the browser's
   WebRTC on the web). transport_use_code: the desktop's WebRTC, 0 when the
   build has none (the driver is left as it was) */
void transport_use_address(void);
/* a demo file (demo.c): the path goes as the connect's host */
void transport_use_demo(void);
int transport_use_code(void);
/* the browser's local server: the page's server worker, over
   a MessageChannel (transport_worker.c). 0 when the build has none (the
   driver is left as it was) */
int transport_use_worker(void);
/* "enet", "webrtc" (desktop's join by code), "web" (the browser's) or
   "worker" (the browser's local server) */
const char* transport_name(void);
/* native builds: the ENet peer as "ip:port" ("[v6]:port"), and whether it
   is a local address (the LAN, this machine); 0 with no peer */
int transport_enet_peer(char* out, size_t size, int* local);

/* driven from the transport: the ENet one carries the web's WebRTC stand
   by the same struct */
extern const struct transport_impl transport_enet_impl;
extern const struct transport_impl transport_web_rtc_impl;
extern const struct transport_impl transport_rtc_native_impl;
extern const struct transport_impl transport_worker_impl;

/* does this build carry the join code at all (a desktop with WebRTC) */
int transport_has_code_join(void);
/* how long a native connect waits for its CONNECT event (the join by
   code's signalling, ICE and DTLS take seconds longer than ENet's look) */
float transport_connect_wait(void);

#endif
