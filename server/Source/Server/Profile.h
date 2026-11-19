// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROFILE_H
#define PROFILE_H

/*
 * The server's profile (docs/plan-075-server.md): one binary, two servers.
 * SERVER_PROFILE_10 is today's server (protocol 1.0 only, the default);
 * SERVER_PROFILE_075 speaks only 0.75/0.76, its wire in Server/Legacy/,
 * the classic game with every server-side part we wrote and none of the
 * 1.0 inventions (perks, classes, squads, assists, spotting).
 *
 * Chosen once at start-up, before the config is read (the profile picks the
 * config file and the data folder): --profile 10|075, else SP_PROFILE.
 * Never changes while the server runs.
 */

typedef enum server_profile {
    SERVER_PROFILE_10  = 0,
    SERVER_PROFILE_075 = 1,
} server_profile_t;

server_profile_t server_profile(void);
/* the shorthand every gate uses */
static inline int profile_is_075(void)
{
    return server_profile() == SERVER_PROFILE_075;
}
/* "10" or "075", as the flag and the plugins write it */
const char* server_profile_name(void);

/* the config file every part of the server reads: --config <path>, else
   the profile's own (Resources/config.toml, Resources/config-075.toml),
   relative to the data root main enters */
const char* server_config_path(void);

/* reads --profile <10|075>, else SP_PROFILE, and --config <path>; an unknown value prints the two
   it takes and returns -1 (main exits: never a silent fall back to 1.0) */
int server_profile_parse(int argc, char** argv);

#endif
