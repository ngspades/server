// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/GameplayPhysics.h>
#include <Server/Profile.h>
#include <Util/Log.h>
#include <Util/Physics.h>
#include <stdlib.h>
#include <tomlc99/toml.h>

static float _gp_cfg_float(toml_table_t* t, const char* key, float fallback)
{
    toml_datum_t d = toml_double_in(t, key);
    if (d.ok) {
        return (float) d.u.d;
    }
    d = toml_int_in(t, key);
    return d.ok ? (float) d.u.i : fallback;
}

static float _gp_clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void gameplay_physics_init(server_t* server)
{
    (void) server;
    physics_jump_velocity        = PHYSICS_JUMP_VELOCITY;
    physics_fall_damage_velocity = PHYSICS_FALL_DAMAGE_VELOCITY;
    if (profile_is_075()) {
        return;
    }
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "gameplay") : NULL;
        if (t != NULL) {
            physics_jump_velocity        = _gp_cfg_float(t, "jump_velocity", physics_jump_velocity);
            physics_fall_damage_velocity = _gp_cfg_float(t, "fall_damage_velocity", physics_fall_damage_velocity);
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_JUMP_VELOCITY");
    if (env != NULL && *env != '\0') {
        physics_jump_velocity = (float) atof(env);
    }
    env = getenv("SP_FALL_DAMAGE_VELOCITY");
    if (env != NULL && *env != '\0') {
        physics_fall_damage_velocity = (float) atof(env);
    }
    physics_jump_velocity        = _gp_clamp(physics_jump_velocity, 0.1f, 1.5f);
    physics_fall_damage_velocity = _gp_clamp(physics_fall_damage_velocity, 0.3f, 3.0f);
    LOG_INFO("Physics: jump %.2f, falls hurt from %.2f", physics_jump_velocity, physics_fall_damage_velocity);
}
