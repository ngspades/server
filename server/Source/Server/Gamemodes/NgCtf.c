// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Next gen capture the flag (ngctf, gamemode 6): classic CTF's rules and
    bots (Ctf.c, through CtfInternal.h) with every GM_FEAT_* on top, the
    perks and the classes. A behaviour only ngCTF has asks a flag of the ops
    (gamemode_has), never the mode, so it lives where the feature lives.
*/

#include <Server/Classes.h>
#include <Server/Gamemodes/CtfInternal.h>
#include <Server/Gamemodes/NgCtf.h>
#include <Server/Map.h>
#include <Server/Gamemodes/Squads.h>
#include <Util/Log.h>
#include <math.h>
#include <modes/modes.h>
#include <stdio.h>
#include <tomlc99/toml.h>

/* ngCTF's squads and classes both talk to a client entering the game and
   take its requests */
static void _ngctf_client_in_game(server_t* server, player_t* player)
{
    squads_client_in_game(server, player);
    classes_client_in_game(server, player);
}

static int _ngctf_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    return classes_command(server, player, command, arg) || squads_command(server, player, command, arg);
}

/* the middle of a team's spawn area in the map's toml ([spawnpoints]
   teamN = {start, end}); 0 when it has none */
static int _ngctf_spawn_middle(toml_table_t* spawns, const char* team, float* x, float* y)
{
    toml_table_t* t     = spawns != NULL ? toml_table_in(spawns, team) : NULL;
    toml_array_t* start = t != NULL ? toml_array_in(t, "start") : NULL;
    toml_array_t* end   = t != NULL ? toml_array_in(t, "end") : NULL;
    int64_t       a[2], b[2];
    for (int i = 0; i < 2; i++) {
        if (start == NULL || end == NULL || toml_rtoi(toml_raw_at(start, i), &a[i]) ||
            toml_rtoi(toml_raw_at(end, i), &b[i]))
        {
            return 0;
        }
    }
    *x = (float) (a[0] + b[0]) / 2.0f;
    *y = (float) (a[1] + b[1]) / 2.0f;
    return 1;
}

/* ngCTF plays only large maps, where the two teams' spawns are at least
   GAME_NGCTF_MIN_BASES apart: the classes need the room (a Marksman's
   lanes, a Miner's tunnels, a Rocketeer's flights); on 2Fort (67 apart)
   the four of them were one brawl. A map whose toml names no spawn areas
   gets the west and east areas, ~350 apart: it suits */
static int _ngctf_map_fits(const char* map_name)
{
    char path[512];
    map_file_path(path, sizeof path, map_name, "toml");
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    char          err[200];
    toml_table_t* root = toml_parse_file(fp, err, sizeof err);
    fclose(fp);
    float x1, y1, x2, y2;
    int   fits = 1;
    if (root != NULL) {
        toml_table_t* spawns = toml_table_in(root, "spawnpoints");
        if (_ngctf_spawn_middle(spawns, "team1", &x1, &y1) && _ngctf_spawn_middle(spawns, "team2", &x2, &y2)) {
            float d = sqrtf((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
            fits    = d >= GAME_NGCTF_MIN_BASES;
            LOG_INFO("Map '%s': spawns %.0f blocks apart, %s the next gen CTF (%.0f at least)", map_name, d,
                     fits ? "suits" : "too small for", GAME_NGCTF_MIN_BASES);
        }
        toml_free(root);
    }
    return fits;
}

/* ngCTF: the same CTF with perks (jetpack, sentry, MG), assists,
   spotting, the squad spawn and the rally point, and the classes */
const gamemode_ops_t ngctf_ops = {
.id              = GAME_MODE_NGCTF,
.name            = "ngctf",
.wire_name       = "ngctf",
.perks           = 1,
.features        = GM_FEAT_ASSISTS | GM_FEAT_SPOTTING | GM_FEAT_SQUAD_SPAWN | GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS,
.hit_damage      = classes_hit_damage,
.init            = _ctf_init,
.tick            = _ctf_tick,
.player_step     = handleTentAndIntel,
.player_out      = _ctf_player_out,
.terrain_changed = _ctf_terrain_changed,
.state_keys      = _ctf_state_keys,
.summary_stat    = "captures",
.player_stat     = _ctf_player_captures,
.bot_goal        = _ctf_bot_goal,
.patrol_anchor   = _ctf_patrol_anchor,
.spawn_facing    = _ctf_spawn_facing,
.player_carries  = _ctf_player_carries,
.spawn_point     = squads_spawn_point,
.client_in_game  = _ngctf_client_in_game,
.player_command  = _ngctf_command,
.map_fits        = _ngctf_map_fits,
.escape_watch_player = _ctf_escape_watch_player,
};

