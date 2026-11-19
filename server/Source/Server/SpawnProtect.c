// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/SpawnProtect.h>
#include <Server/Profile.h>
#include <Server/Structs/PlayerStruct.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <stdlib.h>
#include <tomlc99/toml.h>

static int seconds = 2;

#define SPAWNPROTECT_075_SECONDS 0

static int _sp_cfg_int(toml_table_t* t, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(t, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(t, key);
    return d.ok ? (int) d.u.d : fallback;
}

void spawnprotect_init(server_t* server)
{
    (void) server;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp == NULL && profile_is_075()) {
        seconds = SPAWNPROTECT_075_SECONDS;
    }
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "gameplay") : NULL;
        if (t != NULL) {
            seconds = _sp_cfg_int(t, "spawn_protection_seconds", seconds);
        }
        /* the 0.75 profile's tolerant preset (docs/plan-075-server.md
           10.1): no spawn protection, as on the pyspades servers a 0.75
           player knows, unless [profile.075] says otherwise; over
           [gameplay], written for the 1.0 server */
        if (profile_is_075()) {
            seconds                = SPAWNPROTECT_075_SECONDS;
            toml_table_t* profile  = root != NULL ? toml_table_in(root, "profile") : NULL;
            toml_table_t* pt       = profile != NULL ? toml_table_in(profile, "075") : NULL;
            if (pt != NULL) {
                seconds = _sp_cfg_int(pt, "spawn_protection_seconds", seconds);
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_SPAWN_PROTECT");
    if (env != NULL && *env != '\0') {
        seconds = atoi(env);
    }
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds > 10) {
        seconds = 10;
    }
    LOG_INFO("Spawn protection: %d s%s", seconds, profile_is_075() ? " (0.75 preset)" : "");
}

void spawnprotect_on_spawn(player_t* player)
{
    if (player == NULL || seconds <= 0) {
        return;
    }
    player->spawn_protected_until = get_nanos() + (uint64_t) seconds * NANO_IN_SECOND;
}

uint8_t spawnprotect_active(player_t* player)
{
    return player != NULL && player->spawn_protected_until != 0 && player->alive &&
           get_nanos() < player->spawn_protected_until;
}

void spawnprotect_clear(player_t* player)
{
    if (player != NULL) {
        player->spawn_protected_until = 0;
    }
}
