// Copyright DarkNeutrino 2022
#ifndef PING_H
#define PING_H

#include <enet/enet.h>

int raw_udp_intercept_callback(ENetHost* host, ENetEvent* event);

struct server;
struct json_object;
/* the HELLOINFO answer (the details page of the server list), which the
   master also carries as the announce's info (Net/Master.c); the caller
   puts it */
struct json_object* ping_info(struct server* server);

#endif
