// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_AUDIT_H
#define SERVER_AUDIT_H

/*
 * Admin audit trail: one line for every command, host command and remote console line, in
 * logs/admin.log and in the event log. The server's normal log may be rotated
 * or turned off (SP_EVENTLOG); this file is always written while [audit] is on,
 * because "who kicked whom" must survive a restart.
 *
 * Passwords are never written: callers log the command name, not its
 * arguments. Remote console lines log the command line, never the password.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

void audit_init(server_t* server);

/* actor NULL and console 1: the server console; actor NULL and console 0: an
   remote console line, which has no player */
void audit_event(player_t* actor, uint8_t console, const char* action, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

void audit_command(player_t* actor, uint8_t console, const char* command, int argc);

#endif
