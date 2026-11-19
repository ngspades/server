// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Outfit.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <uthash/uthash.h>
#include <stdio.h>
#include <string.h>

static uint8_t outfit_own[256], outfit_colour[256];
static uint8_t outfit_teams[2];
static int     outfit_teams_set, outfit_ready;

static void _outfit_init(void)
{
    if (!outfit_ready) {
        memset(outfit_own, OUTFIT_TEAM, sizeof outfit_own);
        memset(outfit_colour, 255, sizeof outfit_colour);
        outfit_ready = 1;
    }
}

static void _outfit_send_player(server_t* server, player_t* to, uint8_t id)
{
    char        pid[8], o[8], c[8];
    snprintf(pid, sizeof pid, "%u", (unsigned) id);
    snprintf(o, sizeof o, "%u", (unsigned) outfit_own[id]);
    snprintf(c, sizeof c, "%u", (unsigned) outfit_colour[id]);
    const char* parts[4] = {"outfit", pid, o, c};
    proto10_bridge_generic(server, to, 4, parts);
}

static void _outfit_send_teams(server_t* server, player_t* to)
{
    char        o1[8], o2[8];
    snprintf(o1, sizeof o1, "%u", (unsigned) outfit_teams[0]);
    snprintf(o2, sizeof o2, "%u", (unsigned) outfit_teams[1]);
    const char* parts[5] = {"team-outfits", "0", o1, "1", o2};
    proto10_bridge_generic(server, to, 5, parts);
}

int outfit_set(server_t* server, player_t* player, int outfit, int c, const char* why)
{
    _outfit_init();
    if (player == NULL || outfit < 0 || outfit > 255 || profile_is_075()) {
        return 0;
    }
    c = c == 0 || c == 1 ? c : 255;
    outfit_own[player->id]    = (uint8_t) outfit;
    outfit_colour[player->id] = (uint8_t) c;
    evt_log(player, "outfit", "action=set outfit=%d colour=%d why=%s", outfit, c, why);
    _outfit_send_player(server, NULL, player->id);
    return 1;
}

int outfit_set_team(server_t* server, int team, int outfit, const char* why)
{
    _outfit_init();
    /* a team outfit is never "the team's" */
    if (team < 0 || team > 1 || outfit < 0 || outfit >= OUTFIT_TEAM || profile_is_075()) {
        return 0;
    }
    outfit_teams[team] = (uint8_t) outfit;
    outfit_teams_set   = 1;
    evt_log(NULL, "outfit", "action=team team=%d outfit=%d why=%s", team + 1, outfit, why);
    _outfit_send_teams(server, NULL);
    return 1;
}

int outfit_of(const player_t* player)
{
    _outfit_init();
    return player != NULL ? outfit_own[player->id] : OUTFIT_TEAM;
}

int outfit_of_team(int team)
{
    return team == 0 || team == 1 ? outfit_teams[team] : 0;
}

void outfit_send_to(server_t* server, player_t* player)
{
    _outfit_init();
    if (profile_is_075()) {
        return;
    }
    if (outfit_teams_set) {
        _outfit_send_teams(server, player);
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (outfit_own[p->id] != OUTFIT_TEAM || outfit_colour[p->id] != 255) {
            _outfit_send_player(server, player, p->id);
        }
    }
}

void outfit_player_left(player_t* player)
{
    _outfit_init();
    if (player != NULL) {
        outfit_own[player->id]    = OUTFIT_TEAM;
        outfit_colour[player->id] = 255;
    }
}

void outfit_map_start(void)
{
    _outfit_init();
    memset(outfit_own, OUTFIT_TEAM, sizeof outfit_own);
    memset(outfit_colour, 255, sizeof outfit_colour);
}
