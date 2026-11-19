// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Flood.h>
#include <Server/Staff.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>

/* rate (a second) and burst of each kind, and how many drops within
   FLOOD_WINDOW make a kick */
static const struct
{
    const char* name;
    float       rate, burst;
    unsigned    kick_after;
} limits[FLOOD_KINDS] = {
    [FLOOD_PACKET]  = {"packet", 400.0f, 800.0f, 2000},
    [FLOOD_CHAT]    = {"chat", 1.0f, 5.0f, 15},
    [FLOOD_COMMAND] = {"command", 20.0f, 40.0f, 200},
};

#define FLOOD_WINDOW (10ULL * NANO_IN_SECOND)

static unsigned dropped_packets;

unsigned flood_dropped_packets(void)
{
    unsigned n      = dropped_packets;
    dropped_packets = 0;
    return n;
}

int flood_take(server_t* server, player_t* player, flood_kind_t kind)
{
    if (player == NULL || player->is_bot) {
        return 1;
    }
    if (kind != FLOOD_PACKET && is_staff(server, player)) {
        return 1;
    }
    flood_bucket_t* b   = &player->flood[kind];
    uint64_t        now = get_real_nanos();
    if (b->at == 0) {
        b->tokens = limits[kind].burst;
    } else {
        b->tokens += (float) ((double) (now - b->at) / (double) NANO_IN_SECOND) * limits[kind].rate;
        if (b->tokens > limits[kind].burst) {
            b->tokens = limits[kind].burst;
        }
    }
    b->at = now;
    if (b->tokens >= 1.0f) {
        b->tokens -= 1.0f;
        return 1;
    }

    if (kind == FLOOD_PACKET) {
        dropped_packets++;
    }
    if (b->window_at == 0 || now - b->window_at > FLOOD_WINDOW) {
        b->window_at = now;
        b->dropped   = 0;
    }
    b->dropped++;
    if (b->dropped == 1) {
        evt_log(player, "abuse", "action=warn why=%s", limits[kind].name);
        if (kind != FLOOD_PACKET) {
            send_server_notice_kind(player, NOTICE_WARNING, "You are sending too fast: slow down or be kicked");
        }
    } else if (b->dropped == limits[kind].kick_after) {
        evt_log(player, "abuse", "action=kick why=%s dropped=%u", limits[kind].name, b->dropped);
        LOG_WARNING("Player %s (#%hhu) kicked for flooding (%s)", player->name, player->id, limits[kind].name);
        notice_kick(player, REASON_KICKED, CONN_CLOSE_LATER, "Kicked for flooding the server");
    }
    return 0;
}
