// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Stats.h>
#include <Server/MapVote.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    player_t* p;
    uint32_t  n;
} stats_row_t;

static void _top(stats_row_t* rows, int* count, player_t* p, uint32_t n)
{
    if (p == NULL || p->state == STATE_DISCONNECTED || n == 0) {
        return;
    }
    rows[*count].p = p;
    rows[*count].n = n;
    (*count)++;
}

static void _sort(stats_row_t* rows, int count)
{
    for (int i = 1; i < count; i++) {
        stats_row_t key = rows[i];
        int         j   = i - 1;
        while (j >= 0 && rows[j].n < key.n) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = key;
    }
}

static void _append(char* out, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

static void _append(char* out, size_t size, const char* fmt, ...)
{
    size_t used = strlen(out);
    if (used >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + used, size - used, fmt, args);
    va_end(args);
}

void stats_match_start(server_t* server)
{
    server->match_started_at  = get_nanos();
    server->match_extra_seconds = 0;
}

void stats_match_summary(server_t* server)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    stats_row_t      kills[128];
    stats_row_t      caps[128];
    int              nk = 0, nc = 0;
    player_t *       p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        _top(kills, &nk, p, p->kills);
        _top(caps, &nc, p, p->captures);
    }
    _sort(kills, nk);
    _sort(caps, nc);
    const char* winner = gm->score[0] == gm->score[1]
                         ? "Draw"
                         : (gm->score[0] > gm->score[1] ? server->protocol.name_team[0] : server->protocol.name_team[1]);
    unsigned seconds = (unsigned) ((get_nanos() - server->match_started_at) / NANO_IN_SECOND);
    broadcast_server_notice_kind(server, 0, NOTICE_BIG, "Match over: %s %u-%u (%u:%02u)", winner, gm->score[0], gm->score[1],
                            seconds / 60, seconds % 60);
    if (nk > 0) {
        char line[512];
        line[0] = '\0';
        _append(line, sizeof line, "Top kills:");
        for (int k = 0; k < nk && k < 3; k++) {
            _append(line, sizeof line, "%s %s (%u)", k ? "," : "", kills[k].p->name, kills[k].n);
        }
        broadcast_server_notice(server, 0, "%s", line);
    }
    if (nc > 0) {
        char line[512];
        line[0] = '\0';
        _append(line, sizeof line, "Top captures:");
        for (int k = 0; k < nc && k < 3; k++) {
            _append(line, sizeof line, "%s %s (%u)", k ? "," : "", caps[k].p->name, caps[k].n);
        }
        broadcast_server_notice(server, 0, "%s", line);
    }
    /* a vote picks the next map: the notice says it when the vote closes */
    const char* next = mapvote_possible(server) ? NULL : stats_next_map(server);
    if (next != NULL && *next != '\0') {
        broadcast_server_notice(server, 0, "Next map: %s", next);
    }
}

void stats_player_line(server_t* server, player_t* player, char* out, size_t size)
{
    (void) server;
    if (out == NULL || size == 0) {
        return;
    }
    if (player == NULL || player->state == STATE_DISCONNECTED) {
        snprintf(out, size, "No such player.");
        return;
    }
    unsigned accuracy = player->shots > 0 ? (unsigned) ((uint64_t) player->hits * 100 / player->shots) : 0;
    snprintf(out, size, "%s: K %u D %u A %u CAP %u BLOCKS %u ACC %u%% SCORE %u", player->name, player->kills,
             player->deaths, player->assists, player->captures, player->blocks_placed, accuracy,
             player->kills + player->score);
}

const char* stats_next_map(server_t* server)
{
    if (server->s_map.current_map == NULL) {
        return NULL;
    }
    const string_node_t* next = server->s_map.current_map->next != NULL ? server->s_map.current_map->next
                                                                        : server->s_map.map_list;
    return next != NULL ? next->string : NULL;
}
