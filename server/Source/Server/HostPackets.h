// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HOSTPACKETS_H
#define HOSTPACKETS_H

#include <Server/Server.h>

/* Host/bot management protocol shared with the client menu.
   See docs/host-commands.md for the wire contract. */
#define PACKET_TYPE_HOST_CMD    63

#define HOSTCMD_ADD_BOTS     0
#define HOSTCMD_KICK_BOTS    1
#define HOSTCMD_SET_AUTOFILL 2
#define HOSTCMD_KICK_BOT     3
#define HOSTCMD_FILL         4
#define HOSTCMD_DIFFICULTY   5 /* arg1 = BOT_DIFFICULTY_* */
#define HOSTCMD_TUTORIAL_BOT 6 /* arg1 = team: one weak bot on the other team's spawn */
#define HOSTCMD_TEAM_BOTS    7 /* arg1 = count, arg2 = team: that team gets exactly that many bots */

/* "host-status" flags, sent by Proto10/Bridge.c */
#define HOSTSTATUS_FLAG_CAN_MANAGE   1
#define HOSTSTATUS_FLAG_BOTS_ENABLED 2
#define HOSTSTATUS_FLAG_AUTOFILL     4

uint8_t player_is_host(server_t* server, player_t* player);

/* Implemented in Commands/Bots.c: single entry point for the console commands
   and the PACKET_HOSTCMD messages. requester == NULL means the console. */
void    hostcmd_apply(server_t* server, player_t* requester, uint8_t command, uint8_t arg1, uint8_t arg2);
void    hostcmd_process_queue(server_t* server);

#endif /* HOSTPACKETS_H */
