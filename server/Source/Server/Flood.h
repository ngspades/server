// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_FLOOD_H
#define SERVER_FLOOD_H

/*
 * Flood limits: a token bucket per player for each kind of input (a burst,
 * then a steady rate). What is over its budget is dropped before anything
 * parses it; the first drops warn the player, a flood that goes on kicks it
 * (EVT abuse action=warn|kick why=packet|chat|command). Blocks, digs and
 * pings keep their own rates; this is the floor under everything else.
 *
 * Bots and the console never pass here. Staff are exempt from the chat and
 * command limits (a login, an admin's commands), not from the packet one.
 * Normal play never reaches them: a 1.0 client sends ~60 inputs a second,
 * the quick chat a line every second or two.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

typedef enum
{
    FLOOD_PACKET,  /* every packet of a connection, before it is parsed */
    FLOOD_CHAT,    /* a chat line */
    FLOOD_COMMAND, /* a protocol 1.0 GenericCommand */
    FLOOD_KINDS
} flood_kind_t;

/* 1: let it through; 0: drop it (counted, the player warned or kicked) */
int flood_take(server_t* server, player_t* player, flood_kind_t kind);

/* packets dropped since the last call, for the Tick: line */
unsigned flood_dropped_packets(void);

#endif
