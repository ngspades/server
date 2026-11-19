// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Anticheat.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

static const char* kind_names[ANTICHEAT_KINDS] = {
    "hit", "position", "orientation", "grenade", "block", "protocol", "plugin", "other",
};

static struct
{
    uint8_t enabled;
    uint8_t kick_protocol;
    int     log_interval;
} anticheat = {1, 0, 10};

static int _ac_cfg_int(toml_table_t* t, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(t, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(t, key);
    return d.ok ? (int) d.u.d : fallback;
}

void anticheat_init(server_t* server)
{
    (void) server;
    anticheat.enabled       = 1;
    anticheat.kick_protocol = 0;
    anticheat.log_interval  = 10;

    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = conf != NULL ? toml_table_in(conf, "anticheat") : NULL;
        if (t != NULL) {
            toml_datum_t enabled = toml_bool_in(t, "enabled");
            if (enabled.ok) {
                anticheat.enabled = enabled.u.b ? 1 : 0;
            }
            toml_datum_t kick = toml_bool_in(t, "kick_on_protocol_violation");
            if (kick.ok) {
                anticheat.kick_protocol = kick.u.b ? 1 : 0;
            }
            anticheat.log_interval = _ac_cfg_int(t, "log_interval_seconds", anticheat.log_interval);
        }
        /* the 0.75 profile's tolerant preset (docs/plan-075-server.md
           10.1): a vanilla client's packet outside the standard is logged,
           never a kick, unless [profile.075] anticheat_protocol_violation
           says so. Over [anticheat], written for the 1.0 server */
        if (profile_is_075()) {
            anticheat.kick_protocol = 0;
            toml_table_t* profile   = conf != NULL ? toml_table_in(conf, "profile") : NULL;
            toml_table_t* pt        = profile != NULL ? toml_table_in(profile, "075") : NULL;
            toml_datum_t  kick      = pt != NULL ? toml_bool_in(pt, "anticheat_protocol_violation") : (toml_datum_t) {0};
            if (kick.ok) {
                anticheat.kick_protocol = kick.u.b ? 1 : 0;
            }
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }
    const char* env = getenv("SP_ANTICHEAT");
    if (env != NULL && *env != '\0') {
        anticheat.enabled = atoi(env) != 0;
    }
    if (anticheat.log_interval < 1) {
        anticheat.log_interval = 1;
    }
    LOG_INFO("Anti-cheat: %s (protocol kick %s, log every %d s)%s", anticheat.enabled ? "on" : "off",
             anticheat.kick_protocol ? "on" : "off", anticheat.log_interval, profile_is_075() ? " (0.75 preset)" : "");
}

static void _ac_kick(server_t* server, player_t* player, const char* why)
{
    if (player == NULL || player->is_bot || player->conn == NULL) {
        return;
    }
    evt_log(player, "anticheat", "action=kick why=%s", why);
    broadcast_server_notice(server, 0, "%s was kicked (protocol violation)", player->name);
    notice_kick(player, 0, CONN_CLOSE_LATER, "You were kicked: protocol violation (%s)", why);
}

void anticheat_report(server_t* server, player_t* player, anticheat_kind_t kind, int enforced, const char* why)
{
    if (!anticheat.enabled || player == NULL || player->is_bot || kind < 0 || kind >= ANTICHEAT_KINDS) {
        return;
    }
    uint16_t* count = &player->anticheat_count[kind];
    if (*count < UINT16_MAX) {
        (*count)++;
    }
    uint64_t now = get_nanos();
    uint64_t at  = player->anticheat_log_at[kind];
    if (at == 0 || now - at >= (uint64_t) anticheat.log_interval * NANO_IN_SECOND) {
        player->anticheat_log_at[kind] = now;
        evt_log(player, "anticheat", "kind=%s count=%u enforced=%d why=%s", kind_names[kind], *count, enforced,
                why != NULL ? why : "");
    }
    if (kind == ANTICHEAT_PROTOCOL && anticheat.kick_protocol) {
        _ac_kick(server, player, why != NULL ? why : "protocol");
    }
}
