// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_CONFIGKEYS_H
#define SERVER_CONFIGKEYS_H

/*
 * The registry of every config key the server reads (docs/plan-075-server.md
 * section 12). A key the server does not read was ignored in silence: a typo
 * (`prot = 32887`), or a setting of the other profile (a [perks] section left
 * in config-075.toml, a [profile.075] one in config.toml). The walk below
 * reports each one, at every start-up (the log) and in --check-config (the
 * same lines).
 *
 * The rule: a new config key goes into the registry (ConfigKeys.c) in the
 * same commit that reads it. The shipped config.toml must pass with no
 * warning, so a key read but not registered is caught by the CI.
 */

#include <Server/Profile.h>
#include <tomlc99/toml.h>

/* one "Config: ..." line per key the profile does not read; returns how many */
int config_keys_walk(toml_table_t* root, server_profile_t profile, void (*report)(void* ctx, const char* line),
                     void* ctx);

/* the same lines as LOG_WARNING, for the running profile (Main.c, at start-up) */
void config_keys_log(toml_table_t* root);

#endif
