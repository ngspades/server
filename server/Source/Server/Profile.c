// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Profile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static server_profile_t profile = SERVER_PROFILE_10;
static const char*      config_path; /* --config, else NULL */

server_profile_t server_profile(void)
{
    return profile;
}

const char* server_profile_name(void)
{
    return profile == SERVER_PROFILE_075 ? "075" : "10";
}

const char* server_config_path(void)
{
    if (config_path != NULL) {
        return config_path;
    }
    return profile == SERVER_PROFILE_075 ? "Resources/config-075.toml" : "Resources/config.toml";
}

static int _from_name(const char* s, server_profile_t* out)
{
    if (!strcmp(s, "10") || !strcmp(s, "1.0")) {
        *out = SERVER_PROFILE_10;
        return 0;
    }
    if (!strcmp(s, "075") || !strcmp(s, "0.75")) {
        *out = SERVER_PROFILE_075;
        return 0;
    }
    return -1;
}

int server_profile_parse(int argc, char** argv)
{
    const char* value = getenv("SP_PROFILE");
    const char* from  = "SP_PROFILE";
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--profile")) {
            value = argv[i + 1];
            from  = "--profile";
        } else if (!strcmp(argv[i], "--config")) {
            config_path = argv[i + 1];
        }
    }
    if (value == NULL || *value == '\0') {
        return 0;
    }
    if (_from_name(value, &profile) != 0) {
        fprintf(stderr, "Unknown %s '%s': the profiles are 10 (protocol 1.0, the default) and 075 (protocol 0.75)\n",
                from, value);
        return -1;
    }
    return 0;
}
