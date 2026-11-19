// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Classes.h>
#include <Util/Log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

/* TEST ONLY: every spawn gets the jetpack, so it can be exercised without the
   perk system. [jetpack] test_grant_on_spawn = true or SP_JETPACK_TEST=1. */
static int grant_on_spawn;

static void _equip(server_t* server, player_t* player);

/* a number in the [jetpack] table; integers are accepted too ("fuel = 3") */
static float _number(toml_table_t* table, const char* key, float fallback)
{
    toml_datum_t d = toml_double_in(table, key);
    if (d.ok) {
        return (float) d.u.d;
    }
    d = toml_int_in(table, key);
    if (d.ok) {
        return (float) d.u.i;
    }
    return fallback;
}

void jetpack_init(server_t* server)
{
    flight_rules rules   = FLIGHT_JETPACK_DEFAULTS;
    int          enabled = 1;
    grant_on_spawn       = 0;

    /* Main.c already parsed this file for its own tables; reading it again
       here keeps the jetpack out of the start arguments. The table is
       optional: without it the defaults apply. */
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* conf = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = conf != NULL ? toml_table_in(conf, "jetpack") : NULL;
        if (t != NULL) {
            toml_datum_t b = toml_bool_in(t, "enabled");
            if (b.ok) {
                enabled = b.u.b;
            }
            b = toml_bool_in(t, "test_grant_on_spawn");
            if (b.ok) {
                grant_on_spawn = b.u.b;
            }
            /* human units in the file: seconds, blocks/s, blocks/s^2 */
            rules.fuel   = _number(t, "fuel_seconds", rules.fuel);
            rules.regen  = _number(t, "regen_seconds", rules.regen);
            rules.thrust = _number(t, "thrust", rules.thrust / FLIGHT_UNITS_PER_BLOCK_S) * FLIGHT_UNITS_PER_BLOCK_S;
            rules.max_rise =
            _number(t, "max_rise_speed", rules.max_rise / FLIGHT_UNITS_PER_BLOCK_S) * FLIGHT_UNITS_PER_BLOCK_S;
            rules.air_control   = _number(t, "air_control", rules.air_control);
            rules.glide_control = _number(t, "glide_control", rules.glide_control);
            rules.ignition      = _number(t, "ignition_cost_seconds", rules.ignition);
            rules.refill_delay  = _number(t, "refill_delay_seconds", rules.refill_delay);
        }
        if (conf != NULL) {
            toml_free(conf);
        }
    }
    const char* env = getenv("SP_JETPACK_TEST");
    if (env != NULL && strcmp(env, "1") == 0) {
        grant_on_spawn = 1;
    }
    if (!enabled) {
        rules.fuel = 0;
    }
    server->jetpack = rules;
    if (flight_enabled(&rules)) {
        LOG_INFO("Jetpack perk: %.2f s of fuel, refilled in %.1f s (%.1f s blocked after a hit), thrust %.0f blocks/s^2, climb %.1f blocks/s%s",
                 rules.fuel,
                 rules.regen,
                 rules.refill_delay,
                 rules.thrust / FLIGHT_UNITS_PER_BLOCK_S,
                 rules.max_rise / FLIGHT_UNITS_PER_BLOCK_S,
                 grant_on_spawn ? " (TEST: granted on every spawn)" : "");
    } else {
        LOG_INFO("Jetpack perk: disabled");
    }
}

void jetpack_grant(server_t* server, player_t* player)
{
    /* the jetpack is a CTF perk: every other mode refuses it here too, not
       only where perks are earned */
    if (!gamemode_has_perks(server)) {
        evt_log(player, "jetpack", "grant_refused why=mode");
        return;
    }
    if (!flight_enabled(&server->jetpack)) {
        evt_log(player, "jetpack", "grant_refused why=disabled");
        return;
    }
    if (player->state != STATE_READY || !player->alive) {
        evt_log(player, "jetpack", "grant_refused why=not_alive");
        return;
    }
    _equip(server, player);
}

static void _equip(server_t* server, player_t* player)
{
    player->jetpack_equipped = 1;
    player->jetpack_fuel     = server->jetpack.fuel;
    evt_log(player, "jetpack", "action=grant fuel=%.2f", player->jetpack_fuel);
}

void jetpack_remove(server_t* server, player_t* player, const char* why)
{
    (void) server;
    if (!player->jetpack_equipped) {
        return;
    }
    player->jetpack_equipped  = 0;
    player->jetpack_input     = 0;
    player->jetpack_thrusting = 0;
    player->jetpack_fuel      = 0;
    evt_log(player, "jetpack", "action=remove why=%s", why);
}

int jetpack_equipped(const player_t* player)
{
    return player->jetpack_equipped;
}

float jetpack_fuel(const player_t* player)
{
    return player->jetpack_equipped ? player->jetpack_fuel : 0.0f;
}

void jetpack_on_spawn(server_t* server, player_t* player)
{
    player->jetpack_equipped  = 0;
    player->jetpack_input     = 0;
    player->jetpack_thrusting = 0;
    player->jetpack_fuel      = 0;
    /* bots are players, but they have no pilot for the thrust input: they
       would carry a useless jetpack and show its flag. The spawning player is
       not READY yet, so this skips jetpack_grant's alive check. */
    if (grant_on_spawn && !player->is_bot && flight_enabled(&server->jetpack) && gamemode_has_perks(server)) {
        _equip(server, player);
    }
    /* the Rocketeer wears one from every spawn, bots too (_bot_jetpack
       flies it): its class's, not a perk */
    if (!player->jetpack_equipped && classes_jetpack(server, player) && flight_enabled(&server->jetpack) &&
        gamemode_has_perks(server))
    {
        _equip(server, player);
        evt_log(player, "class", "action=jetpack class=%s", classes_of(server, player)->name);
    }
}

void jetpack_set_input(server_t* server, player_t* player, int want)
{
    uint8_t mode = (uint8_t) gamemode_has_perks(server);
    uint8_t w    = (uint8_t) (want && player->jetpack_equipped && flight_enabled(&server->jetpack) && mode);
    if (want && !w && !player->jetpack_input_refused) {
        /* once per press, not once per update */
        evt_log(player, "jetpack", "input_refused why=%s",
                !player->jetpack_equipped ? "not_equipped" : !mode ? "mode" : "disabled");
    }
    player->jetpack_input_refused = (uint8_t) (want && !w);
    player->jetpack_input         = w;
}

void jetpack_after_move(server_t* server, player_t* player, uint8_t was_thrusting, float fuel_before)
{
    if (!player->jetpack_equipped) {
        return;
    }
    const vector3f_t* p = &player->movement.position;
    if (player->jetpack_thrusting && !was_thrusting) {
        evt_log(player,
                "jetpack",
                "action=start fuel=%.2f pos=%.1f,%.1f,%.1f cell=%d,%d,%d",
                player->jetpack_fuel,
                p->x,
                p->y,
                p->z,
                (int) p->x,
                (int) p->y,
                (int) p->z);
    } else if (!player->jetpack_thrusting && was_thrusting) {
        const char* why = !player->jetpack_input         ? "released"
                          : player->jetpack_fuel <= 0.0f ? "empty"
                          : !player->airborne            ? "landed"
                                                         : "ceiling";
        evt_log(player,
                "jetpack",
                "action=stop why=%s fuel=%.2f pos=%.1f,%.1f,%.1f cell=%d,%d,%d",
                why,
                player->jetpack_fuel,
                p->x,
                p->y,
                p->z,
                (int) p->x,
                (int) p->y,
                (int) p->z);
    }
    if (fuel_before < server->jetpack.fuel && player->jetpack_fuel >= server->jetpack.fuel) {
        evt_log(player, "jetpack", "action=full fuel=%.2f", player->jetpack_fuel);
    }
}

void jetpack_put_world_params(server_t* server, p10_writer* w)
{
    const flight_rules* r = &server->jetpack;
    if (!flight_enabled(r)) {
        return;
    }
    char v[32];
    snprintf(v, sizeof v, "%g", r->fuel);
    p10_put_map_entry(w, "jetpack-fuel", v);
    snprintf(v, sizeof v, "%g", r->regen);
    p10_put_map_entry(w, "jetpack-regen", v);
    snprintf(v, sizeof v, "%g", r->thrust / FLIGHT_UNITS_PER_BLOCK_S);
    p10_put_map_entry(w, "jetpack-thrust", v);
    snprintf(v, sizeof v, "%g", r->max_rise / FLIGHT_UNITS_PER_BLOCK_S);
    p10_put_map_entry(w, "jetpack-max-rise", v);
    snprintf(v, sizeof v, "%g", r->air_control);
    p10_put_map_entry(w, "jetpack-air-control", v);
    snprintf(v, sizeof v, "%g", r->glide_control);
    p10_put_map_entry(w, "jetpack-glide-control", v);
    snprintf(v, sizeof v, "%g", r->ignition);
    p10_put_map_entry(w, "jetpack-ignition", v);
    snprintf(v, sizeof v, "%g", r->refill_delay);
    p10_put_map_entry(w, "jetpack-refill-delay", v);
    snprintf(v, sizeof v, "%g", r->ceiling);
    p10_put_map_entry(w, "jetpack-ceiling", v);
}
