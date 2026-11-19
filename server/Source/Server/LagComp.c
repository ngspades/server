// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/LagComp.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

static struct
{
    uint8_t on;
    int     max_ms;
    int     extra_ms;
} lagcomp = {1, 250, 50};

static int _lagcomp_config_int(toml_table_t* table, const char* key, int fallback)
{
    toml_datum_t d = toml_int_in(table, key);
    if (d.ok) {
        return (int) d.u.i;
    }
    d = toml_double_in(table, key);
    return d.ok ? (int) d.u.d : fallback;
}

static int _lagcomp_env_int(const char* name, int fallback)
{
    const char* value = getenv(name);
    return value != NULL && *value != '\0' ? atoi(value) : fallback;
}

void lagcomp_init(server_t* server)
{
    (void) server;
    lagcomp.on       = 1;
    lagcomp.max_ms   = 250;
    lagcomp.extra_ms = 50;

    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* table = conf != NULL ? toml_table_in(conf, "net") : NULL;
        if (table != NULL) {
            toml_datum_t enabled = toml_bool_in(table, "lag_compensation");
            if (enabled.ok) {
                lagcomp.on = enabled.u.b ? 1 : 0;
            }
            lagcomp.max_ms   = _lagcomp_config_int(table, "lag_compensation_max_ms", lagcomp.max_ms);
            lagcomp.extra_ms = _lagcomp_config_int(table, "lag_compensation_extra_ms", lagcomp.extra_ms);
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }

    const char* env = getenv("SP_LAGCOMP");
    if (env != NULL && *env != '\0') {
        lagcomp.on = atoi(env) != 0;
    }
    lagcomp.max_ms   = _lagcomp_env_int("SP_LAGCOMP_MAX_MS", lagcomp.max_ms);
    lagcomp.extra_ms = _lagcomp_env_int("SP_LAGCOMP_EXTRA_MS", lagcomp.extra_ms);
    if (lagcomp.max_ms < 0) {
        lagcomp.max_ms = 0;
    }
    if (lagcomp.extra_ms < 0) {
        lagcomp.extra_ms = 0;
    }
    LOG_INFO("Lag compensation: %s (max %d ms, extra %d ms)", lagcomp.on ? "on" : "off", lagcomp.max_ms,
             lagcomp.extra_ms);
}

void lagcomp_record(server_t* server, player_t* player)
{
    (void) server;
    if (player == NULL || player->state == STATE_DISCONNECTED) {
        return;
    }
    uint64_t  now = get_nanos();
    uint8_t   last = (uint8_t) ((player->lag_next + LAG_HISTORY_SIZE - 1) % LAG_HISTORY_SIZE);
    if (player->lag_count > 0) {
        float dx = player->lag_history[last].pos.x - player->movement.position.x;
        float dy = player->lag_history[last].pos.y - player->movement.position.y;
        float dz = player->lag_history[last].pos.z - player->movement.position.z;
        /* a respawn or a teleport: the old life is not a body to shoot at */
        if (dx * dx + dy * dy + dz * dz > 25.0f) {
            player->lag_count = 0;
        }
    }
    lag_sample_t* s = &player->lag_history[player->lag_next];
    s->at           = now;
    s->pos          = player->movement.position;
    s->alive        = player->alive && player->state == STATE_READY;
    player->lag_next = (uint8_t) ((player->lag_next + 1) % LAG_HISTORY_SIZE);
    if (player->lag_count < LAG_HISTORY_SIZE) {
        player->lag_count++;
    }
}

int lagcomp_rewind(server_t* server, player_t* shooter, player_t* target, vector3f_t* out)
{
    (void) server;
    if (out == NULL || !lagcomp.on || lagcomp.max_ms == 0 || shooter == NULL || target == NULL) {
        return 0;
    }
    if (shooter->is_bot || shooter->conn == NULL || target->lag_count < 2 || !target->alive) {
        return 0;
    }
    int rtt = (int) conn_rtt(shooter->conn);
    if (rtt <= 0) {
        return 0;
    }
    int rewind_ms = rtt + lagcomp.extra_ms;
    if (rewind_ms > lagcomp.max_ms) {
        rewind_ms = lagcomp.max_ms;
    }
    /* less than half a tick: no meaningful rewind */
    if (rewind_ms < 8) {
        return 0;
    }

    uint64_t now  = get_nanos();
    uint64_t want = now > (uint64_t) rewind_ms * NANO_IN_MILLI ? now - (uint64_t) rewind_ms * NANO_IN_MILLI : 0;

    uint8_t first = (uint8_t) ((target->lag_next + LAG_HISTORY_SIZE - target->lag_count) % LAG_HISTORY_SIZE);
    lag_sample_t* a = NULL;
    lag_sample_t* b = NULL;
    for (int k = 0; k < target->lag_count; k++) {
        lag_sample_t* s = &target->lag_history[(first + k) % LAG_HISTORY_SIZE];
        if (s->at <= want) {
            a = s;
        } else if (b == NULL) {
            b = s;
            break;
        }
    }
    if (a == NULL && b == NULL) {
        return 0;
    }
    if (a == NULL) {
        a = b; /* older than the ring: the oldest sample is the floor */
    }
    if (!a->alive || (b != NULL && !b->alive)) {
        return 0;
    }
    /* a sample from another life (respawn at the same spot) is not a body */
    float dx = a->pos.x - target->movement.position.x;
    float dy = a->pos.y - target->movement.position.y;
    float dz = a->pos.z - target->movement.position.z;
    if (dx * dx + dy * dy + dz * dz > 64.0f) {
        return 0;
    }

    if (b != NULL && b->at > a->at) {
        float t = (float) (want - a->at) / (float) (b->at - a->at);
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
        out->x = a->pos.x + (b->pos.x - a->pos.x) * t;
        out->y = a->pos.y + (b->pos.y - a->pos.y) * t;
        out->z = a->pos.z + (b->pos.z - a->pos.z) * t;
    } else {
        *out = a->pos;
    }
    return 1;
}

float lagcomp_hit_tolerance(void)
{
    return lagcomp.on ? 1.5f : 5.0f;
}
