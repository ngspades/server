// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_ANTICHEAT_H
#define SERVER_ANTICHEAT_H

/*
 * Cheap anti-cheat accounting. Validation code reports a refused or merely
 * observed check; the module keeps a per-player counter, logs at most one
 * line per player/kind every 10 s, and can kick a hard protocol violation
 * (non-finite or impossible data) when configured. It never bans and never
 * treats a normal miss as cheating; the statistical report is a separate,
 * later layer.
 */

#include <Server/Structs/ServerStruct.h>

typedef enum
{
    ANTICHEAT_HIT = 0,
    ANTICHEAT_POSITION,
    ANTICHEAT_ORIENTATION,
    ANTICHEAT_GRENADE,
    ANTICHEAT_BLOCK,
    ANTICHEAT_PROTOCOL,
    ANTICHEAT_PLUGIN,
    ANTICHEAT_OTHER,
} anticheat_kind_t;

/* [anticheat] enabled, kick_on_protocol_violation, log_interval_seconds */
void anticheat_init(server_t* server);

/* enforced: 1 when the action was rejected, 0 when only observed */
void anticheat_report(server_t* server, player_t* player, anticheat_kind_t kind, int enforced, const char* why);

#endif
