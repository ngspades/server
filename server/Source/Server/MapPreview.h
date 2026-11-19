// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAPPREVIEW_H
#define MAPPREVIEW_H

#include <Server/MapStore.h>
#include <Server/Structs/ServerStruct.h>
#include <stddef.h>
#include <stdint.h>

/* The map's top view: a low quality picture the server makes once from the
   map as loaded, so the server list's details page can show a server's map
   before joining it. It is made at the load, before any match changes the
   map, so the digs and craters of a match never show up in it. Kept on the
   server as the encoded blob (SDP1, its size, zlib RGB) and served in
   chunks by Ping.c's HELLOPREVIEW. */
void map_preview_make(server_t* server, map_store_t* map);
/* the previous map's picture, freed at every load (and when one fails) */
void map_preview_free(server_t* server);

#endif
