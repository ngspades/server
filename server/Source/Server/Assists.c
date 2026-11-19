// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Assists.h>
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Util/Nanos.h>

#include <stdio.h>
#include <string.h>

#define ASSIST_WINDOW_NS (10ULL * 1000000000ULL)

void assists_on_damage(player_t* attacker, player_t* victim, long amount)
{
    /* only an enemy's damage counts: a fall or the water passes the victim
       itself, and a teammate (team killing on) assists nobody */
    if (attacker == NULL || attacker == victim || amount <= 0 || attacker->team == victim->team) {
        return;
    }
    damage_log_t* d = &victim->damage_log[victim->damage_next];
    d->attacker     = attacker->id;
    d->amount       = (uint16_t) (amount > 65535 ? 65535 : amount);
    d->at           = get_nanos();
    victim->damage_next = (uint8_t) ((victim->damage_next + 1) % DAMAGE_LOG_SIZE);
}

void assists_on_kill(server_t* server, player_t* killer, player_t* victim)
{
    /* the 0.75 profile has no kill assists (a 1.0 announcement, and a 0.75
       scoreboard has no column for them): the damage log is kept, nobody is
       credited (GM_FEAT_ASSISTS, which the caller asks, is off there too) */
    if (profile_is_075()) {
        memset(victim->damage_log, 0, sizeof victim->damage_log);
        victim->damage_next = 0;
        return;
    }
    uint64_t now = get_nanos();
    /* one assist per attacker, its damage summed over the window */
    uint8_t  ids[DAMAGE_LOG_SIZE];
    long     sums[DAMAGE_LOG_SIZE];
    int      n = 0;
    for (int k = 0; k < DAMAGE_LOG_SIZE; k++) {
        damage_log_t* d = &victim->damage_log[k];
        if (d->amount == 0 || now - d->at > ASSIST_WINDOW_NS) {
            continue;
        }
        if (killer != NULL && d->attacker == killer->id) {
            continue;
        }
        int j = 0;
        while (j < n && ids[j] != d->attacker) {
            j++;
        }
        if (j == n) {
            ids[n]  = d->attacker;
            sums[n] = 0;
            n++;
        }
        sums[j] += d->amount;
    }
    memset(victim->damage_log, 0, sizeof victim->damage_log);
    victim->damage_next = 0;

    for (int j = 0; j < n; j++) {
        player_t* a = NULL;
        HASH_FIND(hh, server->players, &ids[j], sizeof(ids[j]), a);
        /* gone since, or moved to the victim's team: no credit */
        if (a == NULL || a->team == victim->team) {
            continue;
        }
        a->assists++;
        evt_log(a, "assist", "victim=%u killer=%d damage=%ld assists=%u", (unsigned) victim->id,
                killer != NULL ? (int) killer->id : -1, sums[j], a->assists);
        char k[8], s[8];
        snprintf(k, sizeof k, "%d", killer != NULL ? (int) killer->id : (int) victim->id);
        snprintf(s, sizeof s, "%u", (unsigned) a->id);
        const char* parts[] = {"kill-assist", k, s};
        proto10_bridge_generic(server, NULL, 3, parts);
    }
}
