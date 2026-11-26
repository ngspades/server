// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/ConfigKeys.h>
#include <Util/Log.h>
#include <stdarg.h>
#include <classes/classes.h>
#include <stdio.h>
#include <string.h>

/* who reads a section or a key: both profiles, or only one of them */
typedef enum ck_prof {
    CK_ALL = 0,
    CK_10,  /* the 1.0 inventions: perks, classes, squads, the sentry, the jetpack, the zombie mode, aim assist */
    CK_075, /* the 0.75 server's own: ZeroSpades' extensions, the public 0.75 masters, its tolerant rules */
} ck_prof_t;

typedef struct ck_key {
    const char* name;
    ck_prof_t   prof;
} ck_key_t;

typedef struct ck_table ck_table_t;
struct ck_table {
    const char*       name;
    ck_prof_t         prof;
    const ck_key_t*   keys;             /* ends with a NULL name; NULL: none */
    const ck_table_t* subs;             /* the sub-tables known by name, ends with a NULL name */
    const ck_table_t* any;              /* the shape of a sub-table whose name is free (a preset, a plugin) */
    int (*sub_known)(const char* name); /* with `any`: only these names are sub-tables (the classes) */
    int (*key_known)(const char* name); /* keys that are generated, not listed (a class's figures) */
    int opaque;                         /* not ours: never looked into (a plugin's own settings) */
};

#define CK_END {NULL, CK_ALL}

/* Main.c */
static const ck_key_t ck_server_keys[] = {
    {"name", CK_ALL},
    {"port", CK_ALL},
    {"max_players", CK_ALL},
    {"master", CK_ALL},
    {"gamemode", CK_ALL},
    {"capture_limit", CK_ALL},
    {"aim_assist", CK_10}, /* the `aim-assist` world parameter: a 1.0 GenericCommand */
    {"fog_distance", CK_10}, /* the `fog-distance` world parameter: a 0.75 client draws its fog at 128 */
    {"maps", CK_ALL},
    {"map_rotation_mode", CK_ALL},
    {"preset", CK_ALL},
    {"periodic_delays", CK_ALL},
    {"welcome_messages", CK_ALL},
    {"periodic_messages", CK_ALL},
    CK_END,
};

static const ck_key_t ck_team_keys[] = {{"name", CK_ALL}, {"color", CK_ALL}, CK_END};

static const ck_table_t ck_teams_subs[] = {
    {.name = "team1", .keys = ck_team_keys},
    {.name = "team2", .keys = ck_team_keys},
    {.name = NULL},
};

static const ck_key_t ck_passwords_keys[] = {
    {"manager", CK_ALL}, {"admin", CK_ALL}, {"moderator", CK_ALL}, {"guard", CK_ALL}, {"trusted", CK_ALL}, CK_END,
};

/* Proto10/Jetpack.c */
static const ck_key_t ck_jetpack_keys[] = {
    {"enabled", CK_ALL},
    {"fuel_seconds", CK_ALL},
    {"regen_seconds", CK_ALL},
    {"thrust", CK_ALL},
    {"max_rise_speed", CK_ALL},
    {"air_control", CK_ALL},
    {"glide_control", CK_ALL},
    {"ignition_cost_seconds", CK_ALL},
    {"refill_delay_seconds", CK_ALL},
    {"test_grant_on_spawn", CK_ALL},
    CK_END,
};

/* Perks.c: [perks] and one [perks.<name>] per perk (its perk_names) */
static const ck_key_t ck_perks_keys[] = {
    {"kill_points", CK_ALL},
    {"intel_pickup_points", CK_ALL},
    {"intel_capture_points", CK_ALL},
    {"jetpack_points", CK_ALL},
    {"sentry_points", CK_ALL},
    CK_END,
};

static const ck_key_t ck_perk_keys[] = {
    {"points", CK_ALL},
    {"kill_points", CK_ALL},
    {"intel_pickup_points", CK_ALL},
    {"intel_capture_points", CK_ALL},
    {"enabled", CK_ALL},
    {"reset_on_death", CK_ALL},
    {"activation", CK_ALL}, /* no longer used: Perks.c says so itself */
    CK_END,
};

static const ck_table_t ck_perks_subs[] = {
    {.name = "jetpack", .keys = ck_perk_keys},
    {.name = "sentry", .keys = ck_perk_keys},
    {.name = "mg", .keys = ck_perk_keys},
    {.name = "landmine", .keys = ck_perk_keys},
    {.name = "dynamite", .keys = ck_perk_keys},
    {.name = NULL},
};

/* Classes.c: [classes.<name>], the names and the figures of
   common/classes/classes.h (game_class_set takes the same keys) */
static int ck_class_name(const char* name)
{
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        if (!strcmp(name, game_class_default(k)->name)) {
            return 1;
        }
    }
    return 0;
}

static int ck_class_key(const char* name)
{
    for (int k = 0; k < GAME_CLASS_KEYS; k++) {
        if (!strcmp(name, game_class_key(k))) {
            return 1;
        }
    }
    return 0;
}

static const ck_table_t ck_class_shape = {.name = "class", .key_known = ck_class_key};

/* LagComp.c, NetGuard.c */
static const ck_key_t ck_net_keys[] = {
    {"lag_compensation", CK_ALL},       {"lag_compensation_max_ms", CK_ALL}, {"lag_compensation_extra_ms", CK_ALL},
    {"max_connections_per_ip", CK_ALL}, {"connection_window_seconds", CK_ALL}, CK_END,
};

/* Gamemodes/Zombie.c */
static const ck_key_t ck_zombie_keys[] = {
    {"rounds", CK_ALL},
    {"countdown", CK_ALL},
    {"round_seconds", CK_ALL},
    {"infection_bonus", CK_ALL},
    {"end_seconds", CK_ALL},
    {"first_infected", CK_ALL},
    {"minimum_players", CK_ALL},
    {"zombie_respawn_seconds", CK_ALL},
    {"radar_seconds", CK_ALL},
    {"claw_damage", CK_ALL},
    {"classes", CK_ALL},
    {"night", CK_10},
    {"spawn_near_min", CK_ALL},
    {"spawn_near_max", CK_ALL},
    CK_END,
};

/* Conduct.c, EscapeWatch.c */
static const ck_key_t ck_conduct_keys[] = {
    {"afk_kick_seconds", CK_ALL},
    {"afk_warn_seconds", CK_ALL},
    {"afk_spectator_kick_seconds", CK_ALL},
    {"grief_warn_points", CK_ALL},
    {"grief_kick_points", CK_ALL},
    {"grief_decay_seconds", CK_ALL},
    {"grief_team_damage_points", CK_ALL},
    {"grief_team_kill_points", CK_ALL},
    {"escape_watch", CK_ALL},
    {"escape_watch_sky_seconds", CK_ALL},
    {"escape_watch_embedded_seconds", CK_ALL},
    {"escape_watch_entomb_seconds", CK_ALL},
    CK_END,
};

/* Anticheat.c */
static const ck_key_t ck_anticheat_keys[] = {
    {"enabled", CK_ALL}, {"kick_on_protocol_violation", CK_ALL}, {"log_interval_seconds", CK_ALL}, CK_END,
};

/* Gamemodes/Arena.c: rounds, whose timing the map's [arena] and its .txt
   narrow down (the arena is a 1.0 mode: the 0.75 profile has ctf and babel) */
static const ck_key_t ck_arena_keys[] = {
    {"countdown_seconds", CK_10},
    {"round_seconds", CK_10},
    {"spawn_change", CK_10},
    {"max_spawn_distance", CK_10},
    {"team_color_seconds", CK_10},
    CK_END,
};

/* Gamemodes/Squads.c */
static const ck_key_t ck_squads_keys[] = {{"enabled", CK_ALL}, {"size", CK_ALL}, CK_END};

/* Proto10/Sentry.c */
static const ck_key_t ck_sentry_keys[] = {{"lifetime_seconds", CK_ALL}, CK_END};

/* Sky.c: the time of day and the weather (1.0; the 0.75 profile has [ext075] daytime) */
static const ck_key_t ck_sky_keys[] = {
    {"time", CK_10}, {"rate", CK_10}, {"weather", CK_10}, {"intensity", CK_10}, {"moonlight", CK_10}, CK_END,
};

/* Bots.c, BotProjects.c, BotForts.c */
static const ck_key_t ck_bots_keys[] = {
    {"difficulty", CK_ALL}, {"cooperative_bridges", CK_ALL}, {"cooperative_forts", CK_ALL},
    {"build_chance", CK_10}, {"help_chance", CK_10},       {"build_cooldown", CK_10},
    CK_END,
};

/* Net/WebRtc.c (turn and turns: the TURN servers, plan section 10.3) */
static const ck_key_t ck_webrtc_keys[] = {
    {"enabled", CK_ALL}, {"port", CK_ALL}, {"stun", CK_ALL}, {"turn", CK_ALL}, {"turns", CK_ALL},
    {"turn_username", CK_ALL}, {"turn_credential", CK_ALL}, CK_END,
};

/* Net/Master.c */
static const ck_key_t ck_master_keys[] = {
    {"enabled", CK_ALL}, {"url", CK_ALL}, {"country", CK_ALL}, {"key_file", CK_ALL}, {"connect_tokens", CK_10}, CK_END,
};

/* Rcon.c */
static const ck_key_t ck_rcon_keys[] = {
    {"enabled", CK_ALL}, {"port", CK_ALL}, {"bind", CK_ALL}, {"password", CK_ALL}, CK_END,
};

/* Plugins.c: [plugins] and one [plugins.<name>] per plugin, which is the
   plugin's own `config` table (allow_hosts and profiles are read by the
   server, the rest is the plugin's business): never looked into */
static const ck_key_t ck_plugins_keys[] = {
    {"load", CK_ALL}, {"allow_map_change", CK_ALL}, {"allow_commands", CK_ALL}, CK_END,
};

static const ck_table_t ck_plugin_shape = {.name = "plugin", .opaque = 1};

/* SpawnProtect.c */
static const ck_key_t ck_gameplay_keys[] = {{"spawn_protection_seconds", CK_ALL},
                                            {"jump_velocity", CK_10},
                                            {"fall_damage_velocity", CK_10},
                                            CK_END};

/* Audit.c */
static const ck_key_t ck_audit_keys[] = {{"enabled", CK_ALL}, {"file", CK_ALL}, CK_END};

/* Main.c, Bots.c, Plugins.c: [presets.<name>] */
static const ck_key_t ck_preset_keys[] = {
    {"gamemode", CK_ALL}, {"capture_limit", CK_ALL}, {"aim_assist", CK_10}, {"fog_distance", CK_10}, {"maps", CK_ALL},
    {"bot_difficulty", CK_ALL}, {"plugins", CK_ALL}, CK_END,
};

static const ck_table_t ck_preset_shape = {.name = "preset", .keys = ck_preset_keys};

/* Legacy/Ext075.c: the 0.75 protocol extensions (plan section 7.4, the
   aosprotocol specification); the whole section is the 0.75 profile's */
static const ck_key_t ck_ext075_keys[] = {
    {"damage_markers", CK_075},
    {"teamplay", CK_075},
    {"player_properties", CK_075},
    {"team_esp", CK_075},
    {"pings", CK_075},
    {"compass", CK_075},
    {"ping_lifetime", CK_075},
    {"flashlight", CK_075},
    {"flashlight_reach", CK_075},
    {"flashlight_cone", CK_075},
    {"flashlight_color", CK_075},
    {"flashlight_flicker", CK_075},
    {"player_limit", CK_075},
    {"message_types", CK_075},
    {"kick_reason", CK_075},
    {"utf8_chat", CK_075},
    {"version_info", CK_075},
    {"daytime", CK_075},
    {"night", CK_075},
    CK_END,
};

/* Legacy/Master075.c: the public 0.75 masters (plan section 8.6) */
static const ck_key_t ck_master075_keys[] = {{"enabled", CK_ALL}, {"address", CK_ALL}, CK_END};

/* MapDownload.c: the rotation's missing maps come from the maps repository */
static const ck_key_t ck_maps_keys[] = {{"download", CK_ALL}, CK_END};

/* [profile.075]: the tolerant rules a 0.75 player expects (plan section 10.1) */
static const ck_key_t ck_profile075_keys[] = {
    {"anticheat_protocol_violation", CK_ALL}, {"grief_warn_points", CK_ALL}, {"grief_kick_points", CK_ALL},
    {"afk_kick_seconds", CK_ALL},             {"spawn_protection_seconds", CK_ALL}, CK_END,
};

static const ck_table_t ck_profile_subs[] = {
    {.name = "075", .prof = CK_075, .keys = ck_profile075_keys},
    {.name = NULL},
};

static const ck_table_t ck_top_subs[] = {
    {.name = "server", .keys = ck_server_keys},
    {.name = "teams", .subs = ck_teams_subs},
    {.name = "passwords", .keys = ck_passwords_keys},
    {.name = "maps", .keys = ck_maps_keys},
    {.name = "jetpack", .prof = CK_10, .keys = ck_jetpack_keys},
    {.name = "perks", .prof = CK_10, .keys = ck_perks_keys, .subs = ck_perks_subs},
    {.name = "classes", .prof = CK_10, .any = &ck_class_shape, .sub_known = ck_class_name},
    {.name = "net", .keys = ck_net_keys},
    {.name = "zombie", .prof = CK_10, .keys = ck_zombie_keys},
    {.name = "arena", .prof = CK_10, .keys = ck_arena_keys},
    {.name = "conduct", .keys = ck_conduct_keys},
    {.name = "anticheat", .keys = ck_anticheat_keys},
    {.name = "squads", .prof = CK_10, .keys = ck_squads_keys},
    {.name = "sentry", .prof = CK_10, .keys = ck_sentry_keys},
    {.name = "sky", .prof = CK_10, .keys = ck_sky_keys},
    {.name = "bots", .keys = ck_bots_keys},
    {.name = "webrtc", .keys = ck_webrtc_keys},
    {.name = "master", .keys = ck_master_keys},
    {.name = "rcon", .keys = ck_rcon_keys},
    {.name = "plugins", .keys = ck_plugins_keys, .any = &ck_plugin_shape},
    {.name = "gameplay", .keys = ck_gameplay_keys},
    {.name = "audit", .keys = ck_audit_keys},
    {.name = "presets", .any = &ck_preset_shape},
    {.name = "ext075", .prof = CK_075, .keys = ck_ext075_keys},
    {.name = "master075", .prof = CK_075, .keys = ck_master075_keys},
    {.name = "profile", .subs = ck_profile_subs},
    {.name = NULL},
};

static const ck_table_t ck_top = {.name = "", .subs = ck_top_subs};

typedef struct ck_walk {
    server_profile_t profile;
    void (*report)(void* ctx, const char* line);
    void* ctx;
    int   count;
} ck_walk_t;

static int ck_reads(const ck_walk_t* w, ck_prof_t prof)
{
    return prof == CK_ALL || (prof == CK_10) == (w->profile == SERVER_PROFILE_10);
}

static const char* ck_prof_name(ck_prof_t prof)
{
    return prof == CK_10 ? "1.0" : "0.75";
}

static const char* ck_running(const ck_walk_t* w)
{
    return w->profile == SERVER_PROFILE_10 ? "1.0" : "0.75";
}

static void ck_report(ck_walk_t* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void ck_report(ck_walk_t* w, const char* fmt, ...)
{
    char    line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    w->count++;
    if (w->report != NULL) {
        w->report(w->ctx, line);
    }
}

static const ck_key_t* ck_find_key(const ck_table_t* shape, const char* name)
{
    for (int k = 0; shape->keys != NULL && shape->keys[k].name != NULL; k++) {
        if (!strcmp(shape->keys[k].name, name)) {
            return &shape->keys[k];
        }
    }
    return NULL;
}

static const ck_table_t* ck_find_sub(const ck_table_t* shape, const char* name)
{
    for (int k = 0; shape->subs != NULL && shape->subs[k].name != NULL; k++) {
        if (!strcmp(shape->subs[k].name, name)) {
            return &shape->subs[k];
        }
    }
    if (shape->any != NULL && (shape->sub_known == NULL || shape->sub_known(name))) {
        return shape->any;
    }
    return NULL;
}

#define CK_UNKNOWN "(a typo, or a setting of another profile?)"

static void ck_walk_table(ck_walk_t* w, toml_table_t* t, const ck_table_t* shape, const char* path)
{
    for (int k = 0;; k++) {
        const char* name = toml_key_in(t, k);
        if (name == NULL) {
            break;
        }
        toml_table_t* sub = toml_table_in(t, name);
        if (sub == NULL) {
            const ck_key_t* key = ck_find_key(shape, name);
            if (key != NULL || (shape->key_known != NULL && shape->key_known(name))) {
                if (key != NULL && !ck_reads(w, key->prof)) {
                    ck_report(w, "Config: [%s] %s is read only by the %s profile, not by this %s one: ignored", path,
                              name, ck_prof_name(key->prof), ck_running(w));
                }
            } else if (path[0] == '\0') {
                ck_report(w, "Config: unknown top-level key '%s' " CK_UNKNOWN, name);
            } else {
                ck_report(w, "Config: [%s] unknown key '%s' " CK_UNKNOWN, path, name);
            }
            continue;
        }
        char child[256];
        snprintf(child, sizeof child, "%s%s%s", path, path[0] ? "." : "", name);
        const ck_table_t* inner = ck_find_sub(shape, name);
        if (inner == NULL) {
            if (path[0] == '\0') {
                ck_report(w, "Config: unknown table [%s] " CK_UNKNOWN, name);
            } else {
                ck_report(w, "Config: [%s] unknown table '%s' " CK_UNKNOWN, path, name);
            }
            continue;
        }
        if (!ck_reads(w, inner->prof)) {
            /* one line for the whole section: what it holds is ignored with it */
            ck_report(w, "Config: [%s] is read only by the %s profile, not by this %s one: ignored", child,
                      ck_prof_name(inner->prof), ck_running(w));
            continue;
        }
        if (!inner->opaque) {
            ck_walk_table(w, sub, inner, child);
        }
    }
}

int config_keys_walk(toml_table_t* root, server_profile_t profile, void (*report)(void* ctx, const char* line),
                     void* ctx)
{
    ck_walk_t w = {.profile = profile, .report = report, .ctx = ctx, .count = 0};
    if (root != NULL) {
        ck_walk_table(&w, root, &ck_top, "");
    }
    return w.count;
}

static void ck_log_line(void* ctx, const char* line)
{
    (void) ctx;
    LOG_WARNING("%s", line);
}

void config_keys_log(toml_table_t* root)
{
    config_keys_walk(root, server_profile(), ck_log_line, NULL);
}
