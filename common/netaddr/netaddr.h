// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	IPv4 and IPv6 over ENet, shared by the client (its connection, the
	server list's pings, the listen server's port probe) and the server (the
	game's socket). One socket takes both families: an IPv6 socket with
	IPV6_V6ONLY off, where an IPv4 peer is the mapped address
	::ffff:a.b.c.d. A machine with no IPv6 at all (the socket cannot be
	made) falls back to an IPv4 socket, as before.

	Header only, like common/modes: functions the file using them compiles,
	no unused-variable warning (the server builds with -Werror).
*/

#ifndef SHARED_NETADDR_H
#define SHARED_NETADDR_H

#include <enet/enet.h>
#include <stdio.h>
#include <string.h>

/* a UDP host for both families, bound to port (any address) or unbound when
   port < 0. *type says which it got: ENET_ADDRESS_TYPE_ANY (both) or
   ENET_ADDRESS_TYPE_IPV4 (no IPv6 on this machine) */
static inline ENetHost* netaddr_host_create(int port, size_t peers, size_t channels, enet_uint32 in_bw,
                                            enet_uint32 out_bw, ENetAddressType* type)
{
    const ENetAddressType tries[2] = {ENET_ADDRESS_TYPE_ANY, ENET_ADDRESS_TYPE_IPV4};
    for (int i = 0; i < 2; i++) {
        ENetAddress any;
        enet_address_build_any(&any, tries[i] == ENET_ADDRESS_TYPE_ANY ? ENET_ADDRESS_TYPE_IPV6 : tries[i]);
        any.port      = port >= 0 ? (enet_uint16) port : 0;
        ENetHost* host = enet_host_create(tries[i], port >= 0 ? &any : NULL, peers, channels, in_bw, out_bw);
        if (host != NULL) {
            if (type != NULL) {
                *type = tries[i];
            }
            return host;
        }
    }
    return NULL;
}

/* a plain UDP socket on both families (IPv4 when the machine has no IPv6) */
static inline ENetSocket netaddr_socket_create(ENetAddressType* type)
{
    ENetSocket s = enet_socket_create(ENET_ADDRESS_TYPE_ANY, ENET_SOCKET_TYPE_DATAGRAM);
    if (s != ENET_SOCKET_NULL) {
        enet_socket_set_option(s, ENET_SOCKOPT_IPV6ONLY, 0);
        *type = ENET_ADDRESS_TYPE_ANY;
        return s;
    }
    *type = ENET_ADDRESS_TYPE_IPV4;
    return enet_socket_create(ENET_ADDRESS_TYPE_IPV4, ENET_SOCKET_TYPE_DATAGRAM);
}

static inline int netaddr_is_mapped(const ENetAddress* a)
{
    return a->type == ENET_ADDRESS_TYPE_IPV6 && a->host.v6[0] == 0 && a->host.v6[1] == 0 && a->host.v6[2] == 0
           && a->host.v6[3] == 0 && a->host.v6[4] == 0 && a->host.v6[5] == 0xFFFF;
}

/* ::ffff:a.b.c.d back to a.b.c.d (what a dual stack socket reports for an
   IPv4 peer); any other address as it is */
static inline void netaddr_unmap(ENetAddress* a)
{
    if (netaddr_is_mapped(a)) {
        enet_uint8 v4[4] = {(enet_uint8) (a->host.v6[6] >> 8), (enet_uint8) a->host.v6[6],
                            (enet_uint8) (a->host.v6[7] >> 8), (enet_uint8) a->host.v6[7]};
        a->type          = ENET_ADDRESS_TYPE_IPV4;
        memcpy(a->host.v4, v4, 4);
    }
}

/* an address a socket of that type can send to: an IPv4 one is mapped for
   a dual stack socket (sendto with an IPv4 sockaddr fails there) */
static inline void netaddr_for(ENetAddress* a, ENetAddressType socket_type)
{
    if (socket_type == ENET_ADDRESS_TYPE_ANY) {
        enet_address_convert_ipv6(a);
    }
}

/* 4 or 6, the mapped IPv4 counted as 4 */
static inline int netaddr_family(const ENetAddress* a)
{
    return a->type == ENET_ADDRESS_TYPE_IPV4 || netaddr_is_mapped(a) ? 4 : 6;
}

/* an address of the local network or this machine: IPv4 10/8, 172.16/12,
   192.168/16, 127/8 and 169.254/16 (link local), IPv6 ::1, fc00::/7 (unique
   local) and fe80::/10 (link local); a mapped IPv4 address by its IPv4
   form. What only a LAN may ask (the server's HELLOLAN answer) */
static inline int netaddr_is_local(const ENetAddress* a)
{
    ENetAddress x = *a;
    netaddr_unmap(&x);
    if (x.type == ENET_ADDRESS_TYPE_IPV4) {
        const enet_uint8* v = x.host.v4;
        return v[0] == 10 || v[0] == 127 || (v[0] == 172 && (v[1] & 0xF0) == 16) || (v[0] == 192 && v[1] == 168)
            || (v[0] == 169 && v[1] == 254);
    }
    /* ENet keeps the IPv6 words in host order (unix.c) */
    const enet_uint16* w = x.host.v6;
    int loopback = w[7] == 1;
    for (int i = 0; i < 7; i++)
        loopback = loopback && w[i] == 0;
    return loopback || (w[0] & 0xFE00) == 0xFC00 || (w[0] & 0xFFC0) == 0xFE80;
}

/* the same host and port, whichever of the two forms each one is in */
static inline int netaddr_equal(const ENetAddress* a, const ENetAddress* b)
{
    ENetAddress x = *a, y = *b;
    netaddr_unmap(&x);
    netaddr_unmap(&y);
    return x.port == y.port && enet_address_equal_host(&x, &y);
}

/* "1.2.3.4" or "2001:db8::1", without the port (an IPv4 peer of a dual
   stack socket written as IPv4). enet_address_get_host_ip copies an IPv6
   address without its terminating zero: the buffer is cleared first */
static inline void netaddr_string(const ENetAddress* a, char* out, size_t size)
{
    ENetAddress x = *a;
    netaddr_unmap(&x);
    x.port = 0;
    memset(out, 0, size);
    if (size < 2 || enet_address_get_host_ip(&x, out, size - 1) != 0) {
        snprintf(out, size, "?");
    }
}

/* "host:port", or "[v6]:port": the form identifiers take */
static inline void netaddr_host_port(const ENetAddress* a, char* out, size_t size)
{
    char host[64];
    netaddr_string(a, host, sizeof host);
    snprintf(out, size, netaddr_family(a) == 6 ? "[%s]:%u" : "%s:%u", host, (unsigned) a->port);
}

/* 1 when this machine has a route to the internet over that family (4 or
   6): connect() on a UDP socket only asks the routing table, nothing is
   sent. The addresses are documentation ones (RFC 5737, RFC 3849): any
   default route covers them */
static inline int netaddr_has_route(int family)
{
    ENetAddressType type = family == 6 ? ENET_ADDRESS_TYPE_IPV6 : ENET_ADDRESS_TYPE_IPV4;
    ENetSocket      s    = enet_socket_create(type, ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) {
        return 0;
    }
    ENetAddress to;
    memset(&to, 0, sizeof to);
    if (enet_address_set_host_ip(&to, family == 6 ? "2001:db8::1" : "192.0.2.1") != 0) {
        enet_socket_destroy(s);
        return 0;
    }
    to.port = 9;
    int ok  = enet_socket_connect(s, &to) == 0;
    enet_socket_destroy(s);
    return ok;
}

/* name (an address or a host name) to an address a socket of that type can
   reach. A name with both kinds of address takes the IPv4 one while this
   machine can reach IPv4 (the servers out there are IPv4), the IPv6 one
   otherwise. 0 when it does not resolve or the socket cannot reach it */
static inline int netaddr_resolve(const char* name, ENetAddressType socket_type, ENetAddress* out)
{
    ENetAddress v4, v6;
    memset(&v4, 0, sizeof v4);
    memset(&v6, 0, sizeof v6);
    /* each picks its own family first and takes the other when it is all
       the name has */
    int has4 = enet_address_set_host(&v4, ENET_ADDRESS_TYPE_IPV4, name) == 0 && netaddr_family(&v4) == 4;
    int has6 = socket_type == ENET_ADDRESS_TYPE_ANY
               && enet_address_set_host(&v6, ENET_ADDRESS_TYPE_IPV6, name) == 0 && netaddr_family(&v6) == 6;
    if (has4 && has6) {
        *out = netaddr_has_route(4) || !netaddr_has_route(6) ? v4 : v6;
    } else if (has4) {
        *out = v4;
    } else if (has6) {
        *out = v6;
    } else {
        return 0;
    }
    netaddr_unmap(out);
    netaddr_for(out, socket_type);
    return 1;
}

#endif
