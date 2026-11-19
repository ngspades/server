// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/NetGuard.h>
#include <Server/Profile.h>
#include <Server/EventLog.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

#define NETGUARD_HISTORY 256

static struct
{
    int max_per_ip;
    int window_seconds;
    struct
    {
        uint32_t ip;
        uint64_t at;
    } history[NETGUARD_HISTORY];
    unsigned next;
} guard = {4, 10, {{0, 0}}, 0};

static int _ng_cfg_int(toml_table_t* t, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(t, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(t, key);
    return d.ok ? (int) d.u.d : fallback;
}

void netguard_init(server_t* server)
{
    (void) server;
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "net") : NULL;
        if (t != NULL) {
            guard.max_per_ip     = _ng_cfg_int(t, "max_connections_per_ip", guard.max_per_ip);
            guard.window_seconds = _ng_cfg_int(t, "connection_window_seconds", guard.window_seconds);
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_MAX_PER_IP");
    if (env != NULL && *env != '\0') {
        guard.max_per_ip = atoi(env);
    }
    if (guard.max_per_ip < 0) {
        guard.max_per_ip = 0;
    }
    if (guard.window_seconds < 1) {
        guard.window_seconds = 1;
    }
    memset(guard.history, 0, sizeof guard.history);
    guard.next = 0;
    LOG_INFO("Connection guard: %d per IP in %d s", guard.max_per_ip, guard.window_seconds);
}

uint8_t netguard_allow(server_t* server, const conn_t* conn)
{
    (void) server;
    if (conn == NULL || guard.max_per_ip == 0) {
        return 1;
    }
    uint32_t ip  = conn->key;
    uint64_t now = get_real_nanos();
    uint64_t cut = now > (uint64_t) guard.window_seconds * NANO_IN_SECOND
                   ? now - (uint64_t) guard.window_seconds * NANO_IN_SECOND
                   : 0;

    int recent = 0;
    for (int k = 0; k < NETGUARD_HISTORY; k++) {
        if (guard.history[k].at >= cut && guard.history[k].ip == ip) {
            recent++;
        }
    }
    int connected = conn_count_from(ip, conn);
    /* the new peer counts too: max_per_ip - 1 already connected is full */
    int over = connected + 1 > guard.max_per_ip || recent >= guard.max_per_ip;
    guard.history[guard.next].ip = ip;
    guard.history[guard.next].at = now;
    guard.next                   = (guard.next + 1) % NETGUARD_HISTORY;
    if (over) {
        evt_log(NULL, "net", "action=refused why=per_ip ip=%s connected=%d recent=%d", conn->address, connected,
                recent);
        return 0;
    }
    return 1;
}
