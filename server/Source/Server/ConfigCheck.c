// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/ConfigCheck.h>
#include <Server/Map.h>
#include <Server/MapDownload.h>
#include <Server/ConfigKeys.h>
#include <Server/Profile.h>
#include <tomlc99/toml.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _errors;
static int _warnings;

static void _add(char* out, size_t size, const char* fmt, ...)
{
    if (out == NULL || size == 0) {
        return;
    }
    size_t used = strlen(out);
    if (used >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + used, size - used, fmt, args);
    va_end(args);
    if (strlen(out) + 1 < size) {
        strncat(out, "\n", size - strlen(out) - 1);
    }
}

static void _error(char* out, size_t size, const char* fmt, ...)
{
    _errors++;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    _add(out, size, "error: %s", line);
}

static void _warn(char* out, size_t size, const char* fmt, ...)
{
    _warnings++;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    _add(out, size, "warning: %s", line);
}

/* the table's own name is not stored by tomlc99; callers pass it for messages */
static int _int_in(char* out, size_t size, const char* table, toml_table_t* t, const char* key, long lo, long hi,
                   long fallback)
{
    toml_datum_t i = toml_int_in(t, key);
    if (!i.ok) {
        toml_datum_t d = toml_double_in(t, key);
        if (!d.ok) {
            if (toml_raw_in(t, key) != NULL) {
                _error(out, size, "[%s] %s must be a number", table, key);
            }
            return (int) fallback;
        }
        if ((long) d.u.d < lo || (long) d.u.d > hi) {
            _error(out, size, "[%s] %s = %.0f is outside %ld..%ld", table, key, d.u.d, lo, hi);
        }
        return (int) d.u.d;
    }
    if (i.u.i < lo || i.u.i > hi) {
        _error(out, size, "[%s] %s = %lld is outside %ld..%ld", table, key, (long long) i.u.i, lo, hi);
    }
    return (int) i.u.i;
}

static int _bool_in(char* out, size_t size, const char* table, toml_table_t* t, const char* key)
{
    toml_datum_t d = toml_bool_in(t, key);
    if (!d.ok) {
        _error(out, size, "[%s] %s must be true or false", table, key);
        return 0;
    }
    return d.u.b ? 1 : 0;
}

static void _string_in(char* out, size_t size, const char* table, toml_table_t* t, const char* key, size_t max,
                       int required)
{
    toml_datum_t d = toml_string_in(t, key);
    if (!d.ok) {
        if (required || toml_raw_in(t, key) != NULL) {
            _error(out, size, "[%s] %s must be a string", table, key);
        }
        return;
    }
    if (strlen(d.u.s) > max) {
        _error(out, size, "[%s] %s is longer than %zu bytes", table, key, max);
    }
    free(d.u.s);
}

static void _color(char* out, size_t size, const char* table, toml_table_t* t, const char* key)
{
    toml_array_t* a = toml_array_in(t, key);
    if (a == NULL || toml_array_nelem(a) != 3) {
        _error(out, size, "[%s] %s must be three numbers 0..255", table, key);
        return;
    }
    for (int k = 0; k < 3; k++) {
        toml_datum_t d = toml_int_at(a, k);
        if (!d.ok || d.u.i < 0 || d.u.i > 255) {
            _error(out, size, "[%s] %s[%d] is not 0..255", table, key, k);
        }
    }
}

static void _maps(char* out, size_t size, const char* table, toml_table_t* t, const char* key, int required)
{
    toml_array_t* a = toml_array_in(t, key);
    if (a == NULL) {
        if (required) {
            _error(out, size, "[%s] %s is missing", table, key);
        }
        return;
    }
    int n = toml_array_nelem(a);
    if (n == 0 && required) {
        _error(out, size, "[%s] %s is empty", table, key);
    }
    for (int k = 0; k < n; k++) {
        toml_datum_t d = toml_string_at(a, k);
        if (!d.ok || d.u.s[0] == '\0') {
            _error(out, size, "[%s] %s[%d] is not a map name", table, key, k);
            if (d.ok) {
                free(d.u.s);
            }
            continue;
        }
        if (strchr(d.u.s, '/') != NULL || strchr(d.u.s, '\\') != NULL) {
            _error(out, size, "[%s] %s[%d] = '%s' is a path, not a map name", table, key, k, d.u.s);
        } else {
            char path[512];
            /* the folder the name means, when one is here (an alias): a
               config written the way a 0.75 server or aos.party has the map
               is not a fault either. Off the network: a check asks nothing
               from the repository */
            char        folder[MAP_NAME_MAX + 1];
            const char* name = map_resolve_alias(d.u.s, folder, sizeof folder, 0) ? folder : d.u.s;
            map_file_path(path, sizeof path, name, "vxl");
            FILE* fp = fopen(path, "rb");
            if (fp == NULL && map_download_enabled() && map_name_ok(name)) {
                /* not a fault: the start downloads it (MapDownload.c) */
                if (name != d.u.s) {
                    _add(out, size, "note: [%s] %s[%d] = '%s' is %s (alias): downloaded at start-up", table, key, k,
                         d.u.s, name);
                } else {
                    _add(out, size, "note: [%s] %s[%d] = '%s' is not installed yet: it is downloaded at start-up",
                         table, key, k, d.u.s);
                }
                free(d.u.s);
                continue;
            }
            if (fp == NULL) {
                _error(out, size, "[%s] %s[%d] = '%s': %s is missing", table, key, k, name, path);
            } else {
                fclose(fp);
            }
            map_file_path(path, sizeof path, name, "toml");
            fp = fopen(path, "r");
            if (fp == NULL) {
                _warn(out, size, "[%s] %s[%d] = '%s': %s is missing", table, key, k, name, path);
            } else {
                fclose(fp);
            }
        }
        free(d.u.s);
    }
}

/* the keys the running profile does not read (ConfigKeys.c): the lines are
   word for word the ones the start-up logs, so the two agree */
typedef struct config_out {
    char*  out;
    size_t size;
} config_out_t;

static void _key_line(void* ctx, const char* line)
{
    config_out_t* o = ctx;
    _warnings++;
    _add(o->out, o->size, "%s", line);
}

static void _known_keys(char* out, size_t size, toml_table_t* root)
{
    config_out_t o = {out, size};
    config_keys_walk(root, server_profile(), _key_line, &o);
}

static void _server(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "server");
    if (t == NULL) {
        _error(out, size, "[server] is missing");
        return;
    }
    _string_in(out, size, "server", t, "name", 31, 0);
    _int_in(out, size, "server", t, "port", 1, 65535, 32887);
    _int_in(out, size, "server", t, "capture_limit", 1, 255, 10);
    _int_in(out, size, "server", t, "gamemode", 0, 9, 0);
    _maps(out, size, "server", t, "maps", 1);
    toml_datum_t rot = toml_string_in(t, "map_rotation_mode");
    if (rot.ok) {
        if (strcmp(rot.u.s, "toml") && strcmp(rot.u.s, "alphabetic") && strcmp(rot.u.s, "random")) {
            _error(out, size, "[server] map_rotation_mode = '%s' is not toml, alphabetic or random", rot.u.s);
        }
        free(rot.u.s);
    }
    if (toml_raw_in(t, "aim_assist") != NULL) {
        _bool_in(out, size, "server", t, "aim_assist");
    }
    _int_in(out, size, "server", t, "fog_distance", 32, 256, 128);
}

static void _teams(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "teams");
    if (t == NULL) {
        _error(out, size, "[teams] is missing");
        return;
    }
    static const char* names[] = {"team1", "team2"};
    for (int k = 0; k < 2; k++) {
        toml_table_t* team = toml_table_in(t, names[k]);
        if (team == NULL) {
            _error(out, size, "[teams.%s] is missing", names[k]);
            continue;
        }
        _string_in(out, size, names[k], team, "name", 31, 1);
        _color(out, size, names[k], team, "color");
    }
}

static void _net(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "net");
    if (t == NULL) {
        return;
    }
    if (toml_raw_in(t, "lag_compensation") != NULL) {
        _bool_in(out, size, "net", t, "lag_compensation");
    }
    _int_in(out, size, "net", t, "lag_compensation_max_ms", 0, 2000, 250);
    _int_in(out, size, "net", t, "lag_compensation_extra_ms", 0, 1000, 50);
    _int_in(out, size, "net", t, "max_connections_per_ip", 0, 1024, 4);
    _int_in(out, size, "net", t, "connection_window_seconds", 1, 3600, 10);
}

static void _match(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "match");
    if (t == NULL) {
        return;
    }
    _int_in(out, size, "match", t, "ctf_seconds", 30, 86400, 1200);
    _int_in(out, size, "match", t, "ngctf_seconds", 30, 86400, 1200);
    _int_in(out, size, "match", t, "babel_seconds", 30, 86400, 1200);
    _int_in(out, size, "match", t, "tc_seconds", 30, 86400, 1200);
    _int_in(out, size, "match", t, "tdm_seconds", 30, 86400, 900);
    _int_in(out, size, "match", t, "koth_seconds", 30, 86400, 900);
}

static void _zombie(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "zombie");
    if (t == NULL) {
        return;
    }
    _int_in(out, size, "zombie", t, "rounds", 1, 255, 3);
    _int_in(out, size, "zombie", t, "countdown", 0, 600, 10);
    _int_in(out, size, "zombie", t, "round_seconds", 1, 3600, 120);
    _int_in(out, size, "zombie", t, "infection_bonus", 0, 600, 30);
    _int_in(out, size, "zombie", t, "end_seconds", 0, 600, 5);
    _int_in(out, size, "zombie", t, "first_infected", 0, 64, 0); /* 0: one per 6 players */
    _int_in(out, size, "zombie", t, "minimum_players", 1, 128, 2);
    _int_in(out, size, "zombie", t, "radar_seconds", 1, 3600, 5);
    _int_in(out, size, "zombie", t, "claw_damage", 1, 100, 50);
    if (toml_raw_in(t, "classes") != NULL) {
        _bool_in(out, size, "zombie", t, "classes");
    }
}

static void _conduct(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "conduct");
    if (t == NULL) {
        return;
    }
    _int_in(out, size, "conduct", t, "afk_kick_seconds", 0, 86400, 600);
    _int_in(out, size, "conduct", t, "afk_warn_seconds", 0, 86400, 540);
    _int_in(out, size, "conduct", t, "grief_warn_points", 0, 1000, 5);
    _int_in(out, size, "conduct", t, "grief_kick_points", 1, 1000, 10);
    _int_in(out, size, "conduct", t, "grief_decay_seconds", 1, 86400, 60);
}

static void _anticheat(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "anticheat");
    if (t == NULL) {
        return;
    }
    if (toml_raw_in(t, "enabled") != NULL) {
        _bool_in(out, size, "anticheat", t, "enabled");
    }
    if (toml_raw_in(t, "kick_on_protocol_violation") != NULL) {
        _bool_in(out, size, "anticheat", t, "kick_on_protocol_violation");
    }
    _int_in(out, size, "anticheat", t, "log_interval_seconds", 1, 3600, 10);
}

static void _gameplay(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "gameplay");
    if (t != NULL) {
        _int_in(out, size, "gameplay", t, "spawn_protection_seconds", 0, 10, 2);
    }
    toml_table_t* a = toml_table_in(root, "audit");
    if (a != NULL) {
        if (toml_raw_in(a, "enabled") != NULL) {
            _bool_in(out, size, "audit", a, "enabled");
        }
        _string_in(out, size, "audit", a, "file", 255, 0);
    }
}

static void _plugins(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "plugins");
    if (t == NULL) {
        return;
    }
    if (toml_raw_in(t, "allow_map_change") != NULL) {
        _bool_in(out, size, "plugins", t, "allow_map_change");
    }
    static const char* arrays[] = {"load", "allow_commands", NULL};
    for (int a = 0; arrays[a] != NULL; a++) {
        toml_array_t* list = toml_array_in(t, arrays[a]);
        for (int k = 0; list != NULL && k < toml_array_nelem(list); k++) {
            toml_datum_t d = toml_string_at(list, k);
            if (!d.ok) {
                _error(out, size, "[plugins] %s[%d] is not a name", arrays[a], k);
            } else {
                free(d.u.s);
            }
        }
    }
}

static void _bots(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "bots");
    if (t == NULL) {
        return;
    }
    toml_datum_t d = toml_string_in(t, "difficulty");
    if (d.ok) {
        if (strcmp(d.u.s, "easy") && strcmp(d.u.s, "normal") && strcmp(d.u.s, "hard")) {
            _error(out, size, "[bots] difficulty = '%s' is not easy, normal or hard", d.u.s);
        }
        free(d.u.s);
    }
}

static void _rcon(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "rcon");
    if (t == NULL) {
        return;
    }
    int on = 0;
    if (toml_raw_in(t, "enabled") != NULL) {
        on = _bool_in(out, size, "rcon", t, "enabled");
    }
    _int_in(out, size, "rcon", t, "port", 1, 65535, 32890);
    toml_datum_t password = toml_string_in(t, "password");
    if (on && (!password.ok || strlen(password.u.s) < 16)) {
        _error(out, size, "[rcon] password must have 16 characters or more when enabled");
    }
    if (password.ok) {
        free(password.u.s);
    }
}

static void _webrtc_master(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "webrtc");
    if (t != NULL && toml_raw_in(t, "port") != NULL) {
        _int_in(out, size, "webrtc", t, "port", 1, 65535, 32888);
    }
    t = toml_table_in(root, "master");
    if (t == NULL) {
        return;
    }
    if (toml_raw_in(t, "enabled") != NULL) {
        _bool_in(out, size, "master", t, "enabled");
    }
    toml_datum_t url = toml_string_in(t, "url");
    if (url.ok) {
        if (url.u.s[0] && strncmp(url.u.s, "https://", 8) && strncmp(url.u.s, "http://", 7)) {
            _error(out, size, "[master] url '%s' is not http(s)://", url.u.s);
        }
        free(url.u.s);
    }
    toml_datum_t country = toml_string_in(t, "country");
    if (country.ok) {
        if (strlen(country.u.s) > 3) {
            _error(out, size, "[master] country '%s' is longer than 3 letters", country.u.s);
        }
        free(country.u.s);
    }
}

static void _presets(char* out, size_t size, toml_table_t* root)
{
    toml_table_t* t = toml_table_in(root, "presets");
    if (t == NULL) {
        return;
    }
    int total = toml_table_nkval(t) + toml_table_narr(t) + toml_table_ntab(t);
    for (int k = 0; k < total; k++) {
        const char* name = toml_key_in(t, k);
        if (name == NULL) {
            break;
        }
        toml_table_t* p = toml_table_in(t, name);
        if (p == NULL) {
            continue;
        }
        if (toml_raw_in(p, "gamemode") != NULL) {
            _int_in(out, size, "presets", p, "gamemode", 0, 9, 0);
        }
        _int_in(out, size, "presets", p, "capture_limit", 1, 255, 10);
        _int_in(out, size, "presets", p, "fog_distance", 32, 256, 128);
        _maps(out, size, "presets", p, "maps", 0);
        toml_datum_t d = toml_string_in(p, "bot_difficulty");
        if (d.ok) {
            if (strcmp(d.u.s, "easy") && strcmp(d.u.s, "normal") && strcmp(d.u.s, "hard")) {
                _error(out, size, "[presets.%s] bot_difficulty = '%s' is not easy, normal or hard", name, d.u.s);
            }
            free(d.u.s);
        }
        toml_array_t* plugins = toml_array_in(p, "plugins");
        for (int j = 0; plugins != NULL && j < toml_array_nelem(plugins); j++) {
            toml_datum_t pd = toml_string_at(plugins, j);
            if (pd.ok) {
                free(pd.u.s);
            } else {
                _error(out, size, "[presets.%s] plugins[%d] is not a name", name, j);
            }
        }
    }
}

int config_check(const char* path, char* out, size_t size)
{
    _errors = _warnings = 0;
    if (out != NULL && size > 0) {
        out[0] = '\0';
    }
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        _error(out, size, "cannot open %s", path);
        return _errors;
    }
    char          err[256];
    toml_table_t* root = toml_parse_file(fp, err, sizeof err);
    fclose(fp);
    if (root == NULL) {
        _error(out, size, "%s: %s", path, err);
        return _errors;
    }
    _known_keys(out, size, root);
    map_download_configure(root);
    _webrtc_master(out, size, root);
    _server(out, size, root);
    _teams(out, size, root);
    _net(out, size, root);
    _match(out, size, root);
    _zombie(out, size, root);
    _conduct(out, size, root);
    _anticheat(out, size, root);
    _gameplay(out, size, root);
    _plugins(out, size, root);
    _bots(out, size, root);
    _rcon(out, size, root);
    _presets(out, size, root);
    toml_free(root);
    if (_warnings > 0) {
        _add(out, size, "%d warning(s), %d error(s)", _warnings, _errors);
    }
    return _errors;
}
