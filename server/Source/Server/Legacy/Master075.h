// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LEGACY_MASTER075_H
#define LEGACY_MASTER075_H

/*
 * The announce to the public 0.75 masters (docs/plan-075-server.md 8.6),
 * the classic ENet master (`[master075] address`, enabled), only in the
 * 0.75 profile. Our own master (Net/Master.c) is announced to as well, by
 * its own code, with the profile's protocol.
 */

#include <Server/Structs/ServerStruct.h>
#include <tomlc99/toml.h>

/* [master075] of the profile's config; called once from main after the read */
void master075_configure(toml_table_t* root);
/* the server is listening: connect and announce */
void master075_start(server_t* server);
/* once a tick: the connection, the player count, the map changes */
void master075_tick(server_t* server);
/* the server is stopping */
void master075_stop(server_t* server);

#endif
