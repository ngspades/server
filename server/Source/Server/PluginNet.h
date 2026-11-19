// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLUGINNET_H
#define PLUGINNET_H

/*
 * The server's HTTP client, for the plugins (Plugins.c) and the master
 * server (Net/Master.c): requests made on threads of their own, never on
 * the tick (an answer takes from tens of milliseconds to seconds, and the
 * server ticks 60 times a second). Nothing here knows Lua or the master.
 *
 * Requests go in lanes, one thread each, and each lane gets its own
 * finished requests back: the master's 25 s long poll must not hold up a
 * plugin's call or the announce, and Plugins.c must never read the
 * master's answers. Built with libcurl (ENABLE_CURL; the server requires
 * it); without it every request fails at once with an error.
 */

#include <stddef.h>
#include <stdint.h>

#define PLUGIN_NET_MAX_BODY (1024 * 1024) /* an answer larger is an error */

typedef enum
{
    NET_LANE_PLUGINS,
    NET_LANE_MASTER,        /* announce and leave */
    NET_LANE_MASTER_POLL,   /* the long poll for offers */
    NET_LANE_MASTER_ANSWER, /* answers to offers: due within 10 s */
    NET_LANES
} net_lane_t;

typedef struct
{
    uint32_t id;
    long     status; /* the HTTP status, 0 when there was no answer */
    char*    body;   /* malloc'd, NUL ended; the caller frees it */
    size_t   body_len;
    char     error[128]; /* empty when status is set */
} plugin_net_result_t;

typedef struct
{
    net_lane_t         lane;
    const char*        method;
    const char*        url;
    const char* const* headers; /* "Name: value" lines */
    int                header_count;
    const char*        body;
    size_t             body_len;
    int                timeout_s;
    int                ip_family; /* 4 or 6 to force one (CURLOPT_IPRESOLVE), 0 either */
} net_request_t;

/* queues a request; its id, 0 when too many are waiting in its lane */
uint32_t net_http(const net_request_t* req);
/* a finished request of the lane into out: 1, or 0 when none is ready */
int net_done(net_lane_t lane, plugin_net_result_t* out);

/* the plugins' lane: headers are "Name: value" lines */
uint32_t plugin_net_http(const char*        method,
                         const char*        url,
                         const char* const* headers,
                         int                header_count,
                         const char*        body,
                         size_t             body_len,
                         int                timeout_s);
int plugin_net_done(plugin_net_result_t* out);
/* the host of a URL, lower case, without the port; 0 when the URL is not
   http:// or https:// */
int plugin_net_host(const char* url, char* host, size_t size);
/* a CURL* set up to verify https: the certificate file of the distribution
   it runs on (Linux), the system's store (Windows), the built-in bundle when
   none is found (SERVER_CA_BUNDLE). Every request of the server goes
   through it */
void net_curl_tls(void* curl);

#endif
