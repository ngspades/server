// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "NetStats.h"

#include <Util/Log.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <string.h>

static uint64_t net_bytes[256];
static uint64_t net_packets[256];
static uint64_t band_bytes[NETSTATS_BANDS];
static uint64_t band_items[NETSTATS_BANDS];

void proto10_netstats_add(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return;
    }
    net_bytes[data[0]] += size;
    net_packets[data[0]]++;
}

void proto10_netstats_band(int band, size_t bytes)
{
    if (band < 0 || band >= NETSTATS_BANDS) {
        return;
    }
    band_bytes[band] += bytes;
    band_items[band]++;
}

static const char* _netstats_name(int type)
{
    switch (type) {
        case P10_PKT_GREETING:
            return "greeting";
        case P10_PKT_GAME_STATE_HEADER:
            return "state_header";
        case P10_PKT_MAP_DATA:
            return "map_data";
        case P10_PKT_GAME_STATE_FINAL:
            return "state_final";
        case P10_PKT_GENERIC_COMMAND:
            return "generic";
        case P10_PKT_MAP_DATA_FINAL:
            return "map_final";
        case P10_PKT_ENTITY_UPDATE:
            return "entity_update";
        case P10_PKT_ENTITY_EVENT:
            return "entity_event";
        case P10_PKT_ENTITY_DIE:
            return "entity_die";
        case P10_PKT_ENTITY_REMOVE:
            return "entity_remove";
        case P10_PKT_TERRAIN_UPDATE:
            return "terrain";
        case P10_PKT_PLAYER_UPDATE:
            return "player_update";
        case P10_PKT_PLAYER_REMOVE:
            return "player_remove";
        case P10_PKT_DAMAGE:
            return "damage";
        case P10_PKT_CHAT_BROADCAST:
            return "chat";
        case P10_PKT_GAME_MODE_STATE:
            return "mode_state";
        default:
            return NULL;
    }
}

void proto10_netstats_log(double seconds, size_t clients, uint32_t total_sent)
{
    if (seconds <= 0.0 || clients == 0) {
        memset(net_bytes, 0, sizeof net_bytes);
        memset(net_packets, 0, sizeof net_packets);
        memset(band_bytes, 0, sizeof band_bytes);
        memset(band_items, 0, sizeof band_items);
        return;
    }
    double   per = seconds * (double) clients * 1024.0; /* KB/s per client */
    uint64_t sum = 0;
    char     line[1024];
    int      at = 0;
    for (int t = 0; t < 256; t++) {
        sum += net_bytes[t];
    }
    /* largest first, every type with 0.1 KB/s or more a client */
    uint8_t done[256] = {0};
    for (;;) {
        int best = -1;
        for (int t = 0; t < 256; t++) {
            if (!done[t] && net_bytes[t] > 0 && (best < 0 || net_bytes[t] > net_bytes[best])) {
                best = t;
            }
        }
        if (best < 0 || (double) net_bytes[best] / per < 0.1 || at >= (int) sizeof line - 64) {
            break;
        }
        done[best]      = 1;
        const char* nm  = _netstats_name(best);
        char        num[16];
        if (!nm) {
            snprintf(num, sizeof num, "type%d", best);
            nm = num;
        }
        at += snprintf(line + at, sizeof line - (size_t) at, "%s%s %.1f (%.0f/s, %.0f B each)", at ? ", " : "", nm,
                       (double) net_bytes[best] / per, (double) net_packets[best] / seconds / (double) clients,
                       (double) net_bytes[best] / (double) net_packets[best]);
    }
    LOG_INFO("Net out per client (KB/s, before ENet's compression): %.1f in all, ENet sent %.1f; %s",
             (double) sum / per, (double) total_sent / per, at ? line : "nothing");

    static const char* band_names[NETSTATS_BANDS] = {"create", "own", "<32", "32-64", "64-128", ">128"};
    at = 0;
    for (int b = 0; b < NETSTATS_BANDS; b++) {
        at += snprintf(line + at, sizeof line - (size_t) at, "%s%s %.1f (%.0f items/s, %.0f B each)", b ? ", " : "",
                       band_names[b], (double) band_bytes[b] / per, (double) band_items[b] / seconds / (double) clients,
                       band_items[b] ? (double) band_bytes[b] / (double) band_items[b] : 0.0);
    }
    LOG_INFO("Net entity items per client by the body's distance (KB/s): %s", line);

    memset(net_bytes, 0, sizeof net_bytes);
    memset(net_packets, 0, sizeof net_packets);
    memset(band_bytes, 0, sizeof band_bytes);
    memset(band_items, 0, sizeof band_items);
}
