// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Legacy/Wire075.h>
#include <Server/Profile.h>
#include <Server/Proto10/NetStats.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Perks.h>
#include <Server/Proto10/GameMode.h>
#include <Server/Proto10/Flags.h>
#include <Server/Proto10/Session.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <enet/enet.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <string.h>

/* The state is a list of key/value strings: the keys every mode has (mode
   first: a packet that starts with it is a snapshot), then the mode's own
   (gamemode_state_keys: intel and bases, the arena's round). A key with an
   empty value is not sent. */

/* what 1.0 clients were last told; deltas are computed against it */
static gm_keys_t gm_last;
static uint8_t   gm_last_valid;

static int _gm_peer_in_game(player_t* p)
{
    return p->protocol == PROTOCOL_10 && p->conn != NULL && p->p10_state == P10_STATE_GAME;
}

static void _gm_send(player_t* p, const p10_writer* w)
{
    conn_send_reliable(p->conn, w->data, w->size);
}

static void _gm_broadcast(server_t* server, const p10_writer* w)
{
    if (w->error) {
        LOG_ERROR("proto10: GameModeState did not fit its buffer");
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_gm_peer_in_game(p)) {
            _gm_send(p, w);
        }
    }
}

const char* proto10_gamemode_name(server_t* server)
{
    return gamemode_current(server)->wire_name;
}

static void _gm_compute(server_t* server, gm_keys_t* s)
{
    gamemode_vars_t* gm = &server->protocol.gamemode;
    memset(s, 0, sizeof(*s));
    gm_keys_put(s, P10_GM_MODE, "%s", proto10_gamemode_name(server));
    gm_keys_put(s, P10_GM_TEAM1_NAME, "%s", server->protocol.name_team[0]);
    gm_keys_put(s, P10_GM_TEAM2_NAME, "%s", server->protocol.name_team[1]);
    for (int t = 0; t < 2; t++) {
        color_t c = server->protocol.color_team[t];
        gm_keys_put(s, t == 0 ? P10_GM_TEAM1_COLOR : P10_GM_TEAM2_COLOR, "%d,%d,%d", c.r, c.g, c.b);
        gm_keys_put(s, t == 0 ? P10_GM_TEAM1_SCORE : P10_GM_TEAM2_SCORE, "%u", (unsigned) gm->score[t]);
    }
    gm_keys_put(s, P10_GM_CAPTURE_LIMIT, "%u", (unsigned) gm->score_limit);
    /* the mode marks the objectives it has (Flags.c) while it writes its keys */
    proto10_flags_begin();
    gamemode_state_keys(server, s);
}

/* the value of key in s, "" when absent */
static const char* _gm_value(const gm_keys_t* s, const char* key)
{
    for (int k = 0; k < s->count; k++) {
        if (!strcmp(s->key[k], key)) {
            return s->value[k];
        }
    }
    return "";
}

static void _gm_put_snapshot(p10_writer* w, const gm_keys_t* s)
{
    p10_start_game_mode_state(w);
    for (int k = 0; k < s->count; k++) {
        if (s->value[k][0]) {
            p10_put_map_entry(w, s->key[k], s->value[k]);
        }
    }
    p10_put_map_end(w);
}

void proto10_gamemode_send_snapshot(server_t* server, player_t* player)
{
    gm_keys_t  cur;
    uint8_t    buf[1024];
    p10_writer w;
    _gm_compute(server, &cur);
    p10_writer_init(&w, buf, sizeof buf);
    _gm_put_snapshot(&w, &cur);
    if (w.error || player->conn == NULL) {
        return;
    }
    _gm_send(player, &w);
    evt_log(player, "proto10", "gamemode_snapshot %s=%s %s=%s %s=%s %s=%s flags=%u", P10_GM_TEAM1_SCORE,
            _gm_value(&cur, P10_GM_TEAM1_SCORE), P10_GM_TEAM2_SCORE, _gm_value(&cur, P10_GM_TEAM2_SCORE),
            P10_GM_TEAM1_INTEL, _gm_value(&cur, P10_GM_TEAM1_INTEL), P10_GM_TEAM2_INTEL,
            _gm_value(&cur, P10_GM_TEAM2_INTEL), (unsigned) proto10_flags_count());
}

void proto10_gamemode_round_start(server_t* server)
{
    _gm_compute(server, &gm_last);
    gm_last_valid = 1;
    proto10_flags_send(server); /* the objectives before the state that names them */
    uint8_t    buf[1024];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    _gm_put_snapshot(&w, &gm_last);
    _gm_broadcast(server, &w);
    LOG_INFO("proto10: round start, game mode %s", _gm_value(&gm_last, P10_GM_MODE));
}

/* broadcasts the keys that changed since the last broadcast */
static void _gm_delta(server_t* server)
{
    gm_keys_t cur;
    _gm_compute(server, &cur);
    /* the objectives first: a client reading "carried" or a capture's
       score must already have the Flag's carrier and place */
    proto10_flags_send(server);
    if (!gm_last_valid) {
        gm_last       = cur;
        gm_last_valid = 1;
        return;
    }
    uint8_t    buf[1024];
    p10_writer w;
    int        changed = 0;
    char       log[256];
    size_t     used = 0;
    log[0]          = 0;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_game_mode_state(&w);
    /* the mode never changes inside a round (that is a new round and a
       snapshot), so a delta never starts with it */
    for (int k = 0; k < cur.count; k++) {
        if (!strcmp(cur.key[k], P10_GM_MODE) || !cur.value[k][0] ||
            !strcmp(cur.value[k], _gm_value(&gm_last, cur.key[k])))
        {
            continue;
        }
        p10_put_map_entry(&w, cur.key[k], cur.value[k]);
        if (used < sizeof log) {
            used += (size_t) snprintf(log + used, sizeof log - used, " %s=%s", cur.key[k], cur.value[k]);
        }
        changed = 1;
    }
    p10_put_map_end(&w);
    if (!changed) {
        return;
    }
    gm_last = cur;
    _gm_broadcast(server, &w);
    LOG_INFO("proto10: game mode delta%s", log);
}

void proto10_gamemode_intel_event(server_t* server, uint8_t team, p10_gm_event_t event)
{
    /* the 0.75 profile's IntelPickup/Capture/Drop (Legacy/Wire075.c); the
       rest runs too: the perks' bookkeeping is the gameplay's */
    if (profile_is_075()) {
        legacy_intel_event(server, team, (int) event);
    }
    if (team > 1) {
        return;
    }
    _gm_delta(server);
    if (event != P10_GM_EVENT_DROP) {
        /* team is the intel's team; its carrier is on the other one */
        uint8_t   carrier_team = team == TEAM_A ? TEAM_B : TEAM_A;
        uint8_t   id           = server->protocol.gamemode.player_intel_team[carrier_team];
        player_t* carrier;
        HASH_FIND(hh, server->players, &id, sizeof(id), carrier);
        perks_on_intel(server, carrier, event == P10_GM_EVENT_CAPTURE);
    }
}

void proto10_gamemode_tick(server_t* server)
{
    _gm_delta(server);
}
