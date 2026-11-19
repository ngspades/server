// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Player.h>
#include <Server/Profile.h>
#include <Server/Audit.h>
#include <Server/EventLog.h>
#include <Server/ParseConvert.h>
#include <Server/Structs/PlayerStruct.h>
#include <Util/DataDir.h>
#include <Util/Log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tomlc99/toml.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static struct
{
    int             on;
    char            path[1100];
    pthread_mutex_t lock;
} audit = {.on = 1, .path = "logs/admin.log", .lock = PTHREAD_MUTEX_INITIALIZER};

static void _mkdir_logs(void)
{
    char logs[1100];
    data_path(logs, sizeof logs, "logs");
#ifdef _WIN32
    _mkdir(logs);
#else
    mkdir(logs, 0755);
#endif
}

void audit_init(server_t* server)
{
    (void) server;
    audit.on = 1;
    data_path(audit.path, sizeof audit.path, "logs/admin.log");
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "audit") : NULL;
        if (t != NULL) {
            toml_datum_t on = toml_bool_in(t, "enabled");
            if (on.ok) {
                audit.on = on.u.b ? 1 : 0;
            }
            toml_datum_t d = toml_string_in(t, "file");
            if (d.ok) {
                /* relative to the data folder (--data), as the default is */
                if (d.u.s[0] == '/') {
                    snprintf(audit.path, sizeof audit.path, "%s", d.u.s);
                } else {
                    data_path(audit.path, sizeof audit.path, d.u.s);
                }
                free(d.u.s);
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_AUDIT");
    if (env != NULL && *env != '\0') {
        audit.on = atoi(env) != 0;
    }
    if (audit.on) {
        _mkdir_logs();
        LOG_INFO("Admin audit: %s", audit.path);
    }
}

static void _stamp(char* out, size_t size)
{
    time_t    now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(out, size, "%Y-%m-%d %H:%M:%S", &tm);
}

void audit_event(player_t* actor, uint8_t console, const char* action, const char* fmt, ...)
{
    char detail[768];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof detail, fmt, args);
    va_end(args);
    char who[128];
    if (actor != NULL) {
        char ip[64] = "";
        if (actor->conn != NULL) {
            player_address(actor, ip, sizeof ip);
        }
        snprintf(who, sizeof who, "%s#%u%s%s", actor->name, (unsigned) actor->id, *ip ? " " : "", ip);
    } else {
        snprintf(who, sizeof who, "%s", console ? "console" : "rcon");
    }
    evt_log(actor, "admin", "action=%s %s", action, detail);
    if (!audit.on) {
        return;
    }
    char stamp[32];
    _stamp(stamp, sizeof stamp);
    pthread_mutex_lock(&audit.lock);
    FILE* fp = fopen(audit.path, "a");
    if (fp != NULL) {
        fprintf(fp, "%s actor=%s action=%s %s\n", stamp, who, action, detail);
        fclose(fp);
    }
    pthread_mutex_unlock(&audit.lock);
}

void audit_command(player_t* actor, uint8_t console, const char* command, int argc)
{
    audit_event(actor, console, "command", "cmd=%s argc=%d", command != NULL ? command : "?", argc);
}
