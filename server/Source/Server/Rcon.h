// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RCON_H
#define RCON_H

/*
 * A remote console (docs/rcon.md): the console's commands over a TCP
 * connection, for a tool that manages the server (sadctl, a monitor). Off
 * unless config.toml's [rcon] enables it with a password; it listens on
 * 127.0.0.1 by default and the password is the only authentication, so the
 * port is never opened to the internet.
 *
 * The wire is text. The client sends "auth <password>", then one console
 * command a line; the server answers each line with its output followed by
 * an EOT byte (0x04) and a newline. The line "status" is answered with the
 * server's state as one JSON object. The socket thread never touches the
 * game: a line is one job, run by rcon_tick on the server's tick.
 */

#include <Server/Structs/ServerStruct.h>

/* reads [rcon] (SP_RCON_PASSWORD, SP_RCON_PORT and SP_RCON_BIND win) and
   starts the thread when it is on */
void rcon_start(server_t* server);
/* every tick, under server_lock: the waiting line, if any */
void rcon_tick(server_t* server);
/* the thread ends with server->running */
void rcon_stop(void);

#endif
