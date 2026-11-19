// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Conduct.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/HostPackets.h>
#include <Server/Staff.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

static struct
{
    int   afk_kick_seconds;
    int   afk_warn_seconds;
    int   afk_spectator_seconds;
    float grief_warn_points;
    float grief_kick_points;
    int   grief_decay_seconds;
    float grief_team_damage_points;
    float grief_team_kill_points;
} conduct = {
    600, 540, 1800, 5.0f, 10.0f, 60, 1.0f, 3.0f,
};

static int _conduct_cfg_int(toml_table_t* t, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(t, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(t, key);
    return d.ok ? (int) d.u.d : fallback;
}

static float _conduct_cfg_float(toml_table_t* t, const char* key, float fallback)
{
    toml_datum_t d = toml_double_in(t, key);
    if (d.ok) {
        return (float) d.u.d;
    }
    d = toml_int_in(t, key);
    return d.ok ? (float) d.u.i : fallback;
}

static int _conduct_env_int(const char* name, int fallback)
{
    const char* value = getenv(name);
    return value != NULL && *value != '\0' ? atoi(value) : fallback;
}

/* the 0.75 profile's tolerant preset (docs/plan-075-server.md 10.1): a
   vanilla 0.75 client is used to pyspades' patience, so a few team hits and
   a short idle kick nobody there. On that profile these keys are the
   preset's, over [conduct] (written for the 1.0 server, config.toml's is
   five times stricter), and config-075.toml's [profile.075] over the preset;
   the 1.0 profile never reads that table */
#define CONDUCT_075_GRIEF_WARN 20.0f
#define CONDUCT_075_GRIEF_KICK 40.0f
#define CONDUCT_075_AFK_KICK   300

static void _conduct_profile(toml_table_t* conf)
{
    if (!profile_is_075()) {
        return;
    }
    conduct.grief_warn_points = CONDUCT_075_GRIEF_WARN;
    conduct.grief_kick_points = CONDUCT_075_GRIEF_KICK;
    conduct.afk_kick_seconds  = CONDUCT_075_AFK_KICK;
    toml_table_t* profile     = conf != NULL ? toml_table_in(conf, "profile") : NULL;
    toml_table_t* pt          = profile != NULL ? toml_table_in(profile, "075") : NULL;
    if (pt != NULL) {
        conduct.afk_kick_seconds  = _conduct_cfg_int(pt, "afk_kick_seconds", conduct.afk_kick_seconds);
        conduct.afk_warn_seconds  = _conduct_cfg_int(pt, "afk_warn_seconds", conduct.afk_warn_seconds);
        conduct.grief_warn_points = _conduct_cfg_float(pt, "grief_warn_points", conduct.grief_warn_points);
        conduct.grief_kick_points = _conduct_cfg_float(pt, "grief_kick_points", conduct.grief_kick_points);
    }
}

void conduct_init(server_t* server)
{
    (void) server;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp == NULL) {
        _conduct_profile(NULL);
    } else {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = conf != NULL ? toml_table_in(conf, "conduct") : NULL;
        if (t != NULL) {
            conduct.afk_kick_seconds       = _conduct_cfg_int(t, "afk_kick_seconds", conduct.afk_kick_seconds);
            conduct.afk_warn_seconds       = _conduct_cfg_int(t, "afk_warn_seconds", conduct.afk_warn_seconds);
            conduct.afk_spectator_seconds  = _conduct_cfg_int(t, "afk_spectator_kick_seconds", conduct.afk_spectator_seconds);
            conduct.grief_warn_points      = _conduct_cfg_float(t, "grief_warn_points", conduct.grief_warn_points);
            conduct.grief_kick_points      = _conduct_cfg_float(t, "grief_kick_points", conduct.grief_kick_points);
            conduct.grief_decay_seconds    = _conduct_cfg_int(t, "grief_decay_seconds", conduct.grief_decay_seconds);
            conduct.grief_team_damage_points =
                _conduct_cfg_float(t, "grief_team_damage_points", conduct.grief_team_damage_points);
            conduct.grief_team_kill_points =
                _conduct_cfg_float(t, "grief_team_kill_points", conduct.grief_team_kill_points);
        }
        _conduct_profile(conf);
        if (conf != NULL) {
            toml_free(conf);
        }
    }
    conduct.afk_kick_seconds = _conduct_env_int("SP_AFK_KICK", conduct.afk_kick_seconds);
    conduct.afk_warn_seconds = _conduct_env_int("SP_AFK_WARN", conduct.afk_warn_seconds);
    conduct.grief_kick_points = (float) _conduct_env_int("SP_GRIEF_KICK", (int) conduct.grief_kick_points);
    if (conduct.afk_kick_seconds < 0) {
        conduct.afk_kick_seconds = 0;
    }
    if (conduct.afk_warn_seconds < 0) {
        conduct.afk_warn_seconds = 0;
    }
    /* the preset's shorter AFK kick under [conduct]'s 1.0 warning (540 s)
       would kick without a warning: warn a minute before */
    if (profile_is_075() && conduct.afk_kick_seconds > 0 && conduct.afk_warn_seconds >= conduct.afk_kick_seconds) {
        conduct.afk_warn_seconds = conduct.afk_kick_seconds > 120 ? conduct.afk_kick_seconds - 60
                                                                  : conduct.afk_kick_seconds / 2;
    }
    if (conduct.grief_warn_points < 0.0f) {
        conduct.grief_warn_points = 0.0f;
    }
    if (conduct.grief_kick_points <= conduct.grief_warn_points) {
        conduct.grief_kick_points = conduct.grief_warn_points + 1.0f;
    }
    if (conduct.grief_decay_seconds < 1) {
        conduct.grief_decay_seconds = 1;
    }
    LOG_INFO("Conduct: AFK %d s (warn %d), grief %.0f/%.0f%s", conduct.afk_kick_seconds, conduct.afk_warn_seconds,
             conduct.grief_warn_points, conduct.grief_kick_points, profile_is_075() ? " (0.75 preset)" : "");
}

/* the listen server's own player (or an admin): kicking it from the game it
   hosts only throws the host out of its own match */
static const char* _conduct_exempt(server_t* server, player_t* player)
{
    return is_staff(server, player) ? "staff" : (server->listen_mode && player_is_host(server, player) ? "host" : NULL);
}

static void _conduct_kick(server_t* server, player_t* player, const char* why)
{
    if (player == NULL || player->is_bot || player->conn == NULL) {
        return;
    }
    player->afk_warned   = 2;
    player->grief_warned = 2;
    evt_log(player, "conduct", "action=kick why=%s", why);
    broadcast_server_notice(server, 0, "%s was kicked (%s)", player->name, why);
    notice_kick(player, 0, CONN_CLOSE_LATER, "You were kicked: %s", why);
}

static uint8_t _input_flags(player_t* player)
{
    return (uint8_t) ((player->move_forward ? 1u : 0u) | (player->move_backwards ? 2u : 0u) |
                      (player->move_left ? 4u : 0u) | (player->move_right ? 8u : 0u) |
                      (player->jumping ? 16u : 0u) | (player->crouching ? 32u : 0u) |
                      (player->sneaking ? 64u : 0u) | (player->sprinting ? 128u : 0u));
}

void conduct_player_tick(server_t* server, player_t* player)
{
    if (player == NULL) {
        return;
    }
    if (player->afk_warned == 2) {
        return; /* already kicked, its disconnect is on the way */
    }
    if (player->is_bot || player->conn == NULL || player->state != STATE_READY || !player->alive) {
        player->afk_since  = get_nanos();
        player->afk_warned = 0;
        return;
    }

    uint64_t now   = get_nanos();
    uint8_t  flags = _input_flags(player);
    vector3f_t o   = player->movement.forward_orientation;
    int active     = flags != player->afk_last_flags;
    active         = active || fabsf(o.x - player->afk_last_orientation.x) > 0.02f ||
             fabsf(o.y - player->afk_last_orientation.y) > 0.02f ||
             fabsf(o.z - player->afk_last_orientation.z) > 0.02f;
    if (active || player->afk_since == 0) {
        player->afk_since            = now;
        player->afk_last_flags       = flags;
        player->afk_last_orientation = o;
        player->afk_warned           = 0;
        return;
    }

    if (conduct.afk_kick_seconds <= 0 || _conduct_exempt(server, player)) {
        return;
    }
    uint64_t idle = (now - player->afk_since) / NANO_IN_SECOND;
    if (idle >= (uint64_t) conduct.afk_kick_seconds) {
        _conduct_kick(server, player, "afk");
        return;
    }
    if (!player->afk_warned && idle >= (uint64_t) conduct.afk_warn_seconds) {
        player->afk_warned = 1;
        evt_log(player, "conduct", "action=afk_warn idle=%llu", (unsigned long long) idle);
        send_server_notice_kind(player, NOTICE_WARNING, "You are AFK; move or fire within %d s or you will be kicked",
                           conduct.afk_kick_seconds - (int) idle);
    }
}

void conduct_on_damage(server_t* server, player_t* attacker, player_t* victim, long damage)
{
    if (attacker == NULL || victim == NULL || attacker == victim || attacker->is_bot || damage <= 0 ||
        attacker->team != victim->team || attacker->grief_warned == 2) {
        return;
    }
    const char* exempt = _conduct_exempt(server, attacker);
    if (exempt != NULL) {
        evt_log(attacker, "conduct", "action=grief_exempt why=%s victim=%u", exempt, victim->id);
        return;
    }
    uint64_t now = get_nanos();
    if (attacker->grief_decay_at != 0) {
        float lost = (float) ((now - attacker->grief_decay_at) / (double) NANO_IN_SECOND) /
                     (float) conduct.grief_decay_seconds;
        attacker->grief_points -= lost;
        if (attacker->grief_points < 0.0f) {
            attacker->grief_points = 0.0f;
        }
    }
    attacker->grief_decay_at = now;
    attacker->grief_points += conduct.grief_team_damage_points;
    if (attacker->grief_warned == 0 && attacker->grief_points >= conduct.grief_warn_points) {
        attacker->grief_warned = 1;
        evt_log(attacker, "conduct", "action=grief_warn points=%.1f victim=%u", attacker->grief_points, victim->id);
        send_server_notice_kind(attacker, NOTICE_WARNING, "Warning: damaging teammates (%0.0f/%.0f)", attacker->grief_points,
                           conduct.grief_kick_points);
    }
    if (attacker->grief_points >= conduct.grief_kick_points) {
        _conduct_kick(server, attacker, "grief");
    }
}

void conduct_on_kill(server_t* server, player_t* killer, player_t* victim)
{
    if (killer == NULL || victim == NULL || killer == victim || killer->is_bot || killer->team != victim->team ||
        killer->grief_warned == 2) {
        return;
    }
    const char* exempt = _conduct_exempt(server, killer);
    if (exempt != NULL) {
        evt_log(killer, "conduct", "action=grief_exempt why=%s victim=%u kill=1", exempt, victim->id);
        return;
    }
    killer->grief_points += conduct.grief_team_kill_points;
    evt_log(killer, "conduct", "action=grief_kill points=%.1f victim=%u", killer->grief_points, victim->id);
    if (killer->grief_points >= conduct.grief_kick_points) {
        _conduct_kick(server, killer, "grief");
    }
}
