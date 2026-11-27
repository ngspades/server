// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Classes.h>
#include <Server/Bots.h>
#include <Server/EventLog.h>
#include <Server/Plugins.h>
#include <Server/Gamemodes/Arena.h>
#include <Server/Gamemodes/Zombie.h>
#include <Server/Gamemodes/Creative.h>
#include <Server/Gamemodes/Tdm.h>
#include <Server/Gamemodes/Koth.h>
#include <Server/Gamemodes/Ptb.h>
#include <Server/Gamemodes/Tc.h>
#include <Server/Proto10/Crates.h>
#include <Server/Gamemodes/LuaMode.h>
#include <Server/Gamemodes/Ctf.h>
#include <Server/Gamemodes/NgCtf.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/Sky.h>
#include <Server/Map.h>
#include <Server/Proto10/Bridge.h>
#include <Util/Nanos.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Profile.h>
#include <Server/Stats.h>
#include <Server/MapVote.h>
#include <Server/BotProjects.h>
#include <Server/BotForts.h>
#include <Server/Proto10/GameMode.h>
#include <Server/Server.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <uthash/uthash.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the match summary stays on the clients' screens this long before the
   next map loads (gamemode_match_won) */
#define MATCH_SUMMARY_SECONDS 16
/* when the next map loads, 0 while a match is on */
static uint64_t match_reset_at;
static void     _match_reset(server_t* server);

/* indexed by gamemode_t */
static const gamemode_ops_t* const registry[] = {
[GAME_MODE_CTF]   = &ctf_ops,
[GAME_MODE_TC]    = &tc_ops,
[GAME_MODE_BABEL] = &babel_ops,
[GAME_MODE_ARENA] = &arena_ops,
[GAME_MODE_ZOMBIE] = &zombie_ops,
[GAME_MODE_NGCTF] = &ngctf_ops,
[GAME_MODE_CREATIVE] = &creative_ops,
[GAME_MODE_TDM] = &tdm_ops,
[GAME_MODE_KOTH] = &koth_ops,
[GAME_MODE_PTB] = &ptb_ops,
};

#define REGISTRY_SIZE (sizeof(registry) / sizeof(registry[0]))

const gamemode_ops_t* gamemode_ops_of(uint8_t id)
{
    if (id == GAME_MODE_LUA) {
        return lua_mode_ops();
    }
    return id < REGISTRY_SIZE ? registry[id] : NULL;
}

/* the 0.75 profile registers the classic CTF and Babel only
   (docs/plan-075-server.md 6.4): the other modes are 1.0-shaped (their
   state is a GameModeState key a 0.75 client has no packet for: the arena's
   round clock, the zombie's rounds, ngCTF's perks and classes, team
   deathmatch's and king of the hill's scores, a Lua mode's keys) */
static int _in_profile(const gamemode_ops_t* ops)
{
    /* Lua modes (plugins with mode = true) are allowed in the 0.75 profile
       when the plugin declares it through game.supported_profiles("075"):
       the real check happens in plugins_load_mode (profile_refused), which
       makes the mode fall back to ctf when it is not supported there. */
    if (ops != NULL && ops->id == GAME_MODE_LUA) {
        return 1;
    }
    return !profile_is_075() || ops->id == GAME_MODE_CTF || ops->id == GAME_MODE_BABEL ||
           ops->id == GAME_MODE_TC;
}

const gamemode_ops_t* gamemode_find(const char* name)
{
    const gamemode_ops_t* found = NULL;
    /* In the 0.75 profile a plugin mode (Resources/plugins/<name>.lua) that
       declares itself for 0.75 takes precedence over a built-in 1.0-only
       mode of the same name (arena, zombie, ...), which cannot run there. */
    if (profile_is_075()) {
        found = lua_mode_find(name);
    }
    if (found == NULL) {
        for (size_t k = 0; name != NULL && k < REGISTRY_SIZE && found == NULL; k++) {
            if (registry[k] != NULL && !strcmp(registry[k]->name, name)) {
                found = registry[k];
            }
        }
    }
    /* not built in: a plugin of that name that is a mode (Resources/plugins/<name>.lua) */
    if (found == NULL) {
        found = lua_mode_find(name);
    }
    if (found != NULL && !_in_profile(found)) {
        LOG_WARNING("Mode '%s' is not in the 0.75 profile (it has ctf, tc and babel)", name);
        evt_log(NULL, "gamemode", "action=refused mode=%s why=profile", name);
        return NULL;
    }
    return found;
}

const gamemode_ops_t* gamemode_current(server_t* server)
{
    const gamemode_ops_t* ops = gamemode_ops_of(server->protocol.current_gamemode);
    return ops != NULL ? ops : &ctf_ops;
}

void gm_keys_put(gm_keys_t* out, const char* key, const char* fmt, ...)
{
    if (out->count >= GM_KEYS_MAX) {
        LOG_ERROR("GameModeState: more than %d mode keys, %s dropped", GM_KEYS_MAX, key);
        return;
    }
    va_list args;
    va_start(args, fmt);
    snprintf(out->key[out->count], GM_KEY_LEN, "%s", key);
    vsnprintf(out->value[out->count], GM_KEY_VALUE_LEN, fmt, args);
    va_end(args);
    out->count++;
}

void gamemode_read_map(server_t* server, uint8_t id, toml_table_t* map_toml)
{
    const gamemode_ops_t* ops = gamemode_ops_of(id);
    /* a mode the 0.75 profile leaves out reads nothing: gamemode_init
       plays ctf instead */
    if (ops != NULL && ops->read_map != NULL && _in_profile(ops)) {
        ops->read_map(server, map_toml);
    }
}

void gamemode_init(server_t* server, uint8_t id)
{
    const gamemode_ops_t* ops = gamemode_ops_of(id);
    if (ops == NULL) {
        LOG_ERROR("Unknown GameMode %u", id);
        server->running = 0;
        return;
    }
    /* a config or a preset written for the 1.0 server (config.toml names
       ngctf) plays the classic CTF on the 0.75 profile, on the same maps:
       the CTF takes any map */
    if (!_in_profile(ops)) {
        LOG_WARNING("Mode '%s' is not in the 0.75 profile, playing ctf", ops->name);
        evt_log(NULL, "gamemode", "action=fallback mode=%s to=ctf why=profile", ops->name);
        id  = GAME_MODE_CTF;
        ops = gamemode_ops_of(id);
    }
    server->protocol.current_gamemode = id;
    snprintf(server->gamemode_name, sizeof(server->gamemode_name), "%s", ops->name);
    /* the features the mode turns on for this map (the zombie mode's
       classes): the last map's are gone */
    server->features_on = 0;
    /* the last map's supply crates are gone with it */
    crates_reset();
    /* the scores of the previous map never carry over */
    server->protocol.gamemode.score[0] = 0;
    server->protocol.gamemode.score[1] = 0;
    stats_match_start(server);
    /* a map loaded any other way (/map) during a summary: no reset left */
    match_reset_at = 0;
    bot_projects_reset();
    bot_forts_reset();
    /* the classes' figures from config.toml again (a plugin's last a map),
       then the plugins: a Lua mode is loaded after them, in its init */
    classes_map_start(server);
    plugins_map_start(server);
    if (ops->init != NULL) {
        ops->init(server);
    }
    /* the mode's night (the zombies') after its rules are read; the plugins
       may change it again in on_map_ready */
    /* config.toml's [sky] again after a mode's own (the zombies' night),
       then this mode's: a CTF after a zombie map played at frozen midnight */
    sky_map_start(server, ops->sky_start != NULL && ops->sky_start(server));
    plugins_map_ready(server);
    proto10_gamemode_round_start(server);
    /* what was on, so a run's log says it (classic CTF: none) */
    evt_log(NULL, "gamemode", "mode=%s perks=%u assists=%d spotting=%d squad_spawn=%d classes=%d class_perks=%d",
            ops->name, ops->perks, (ops->features & GM_FEAT_ASSISTS) != 0, (ops->features & GM_FEAT_SPOTTING) != 0,
            gamemode_has(server, GM_FEAT_SQUAD_SPAWN), gamemode_has(server, GM_FEAT_CLASSES),
            gamemode_has(server, GM_FEAT_CLASS_PERKS));
    /* a line of its own (the CTFs' log stays as it was: bench.py check) */
    if (ops->squad_teams != 0) {
        evt_log(NULL, "squads", "mode=%s teams=%u spawn=%d", ops->name, ops->squad_teams,
                gamemode_has(server, GM_FEAT_SQUAD_SPAWN));
    }
    /* the 1.0 inventions this profile leaves out, said once a map (the
       line above prints the mode's own flags, not what the profile keeps) */
    if (profile_is_075()) {
        evt_log(NULL, "perk", "action=off why=profile");
        evt_log(NULL, "profile", "name=075 modes=ctf,tc,babel off=perks,classes,squad_wire,squad_spawn,rally,assists,"
                                 "spotting");
    }
}

void gamemode_tick(server_t* server)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (match_reset_at != 0) {
        /* the match is over and its summary on every screen: the mode
           counts nothing more until the next map */
        mapvote_tick(server);
        if (get_nanos() >= match_reset_at) {
            match_reset_at = 0;
            evt_log(NULL, "match", "action=reset");
            mapvote_reset();
            _match_reset(server);
        }
        plugins_tick(server);
        return;
    }
    if (ops->squad_teams != 0) {
        squads_census(server); /* the CTFs call it from their own tick */
    }
    if (ops->tick != NULL) {
        ops->tick(server);
    }
    plugins_tick(server);
}

int gamemode_extend(server_t* server, int seconds)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->extend == NULL || seconds <= 0) {
        return 0;
    }
    return ops->extend(server, seconds);
}

static void _match_reset(server_t* server)
{
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state != STATE_DISCONNECTED) {
            p->state = STATE_STARTING_MAP;
        }
    }
    server_reset(server);
}

/* every 1.0 client is told the match's summary before the map changes: a
   line per player in a team (match-player), then the result (match-end,
   which shows it). docs/protocol-1.0.md, GenericCommand. */
static void _match_summary_send(server_t* server, int wait_seconds, int voting)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    gamemode_vars_t*      gm  = &server->protocol.gamemode;
    player_t *            p, *tmp;
    int                   players = 0;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state == STATE_DISCONNECTED || (p->team != TEAM_A && p->team != TEAM_B)) {
            continue;
        }
        char     id[8], k[12], d[12], a[12], s[12];
        uint32_t stat = ops->player_stat != NULL ? ops->player_stat(server, p) : 0;
        snprintf(id, sizeof id, "%u", (unsigned) p->id);
        snprintf(k, sizeof k, "%u", (unsigned) p->kills);
        snprintf(d, sizeof d, "%u", (unsigned) p->deaths);
        snprintf(a, sizeof a, "%u", (unsigned) p->assists);
        snprintf(s, sizeof s, "%u", (unsigned) stat);
        const char* parts[] = {"match-player", id, k, d, a, s};
        proto10_bridge_generic(server, NULL, 6, parts);
        players++;
    }
    const char* winner = gm->score[0] == gm->score[1] ? "draw" : gm->score[0] > gm->score[1] ? "team1" : "team2";
    unsigned    length = (unsigned) ((get_nanos() - server->match_started_at) / NANO_IN_SECOND);
    /* a vote decides the next map (vote-result tells it) */
    const char* next   = voting ? NULL : stats_next_map(server);
    char        s1[8], s2[8], secs[12], wait[8];
    snprintf(s1, sizeof s1, "%u", (unsigned) gm->score[0]);
    snprintf(s2, sizeof s2, "%u", (unsigned) gm->score[1]);
    snprintf(secs, sizeof secs, "%u", length);
    snprintf(wait, sizeof wait, "%d", wait_seconds);
    const char* parts[] = {"match-end", winner, s1, s2, secs, wait,
                           ops->summary_stat != NULL ? ops->summary_stat : "none",
                           next != NULL && *next != '\0' ? next : "-"};
    proto10_bridge_generic(server, NULL, 8, parts);
    evt_log(NULL, "match", "action=summary winner=%s score=%u-%u seconds=%u players=%d stat=%s next=%s wait=%d",
            winner, gm->score[0], gm->score[1], length, players, parts[6], parts[7], wait_seconds);
}

void gamemode_match_won(server_t* server)
{
    if (match_reset_at != 0) {
        return; /* already over: a second win in the summary's time */
    }
    mapvote_record_match(server);
    stats_match_summary(server);
    /* SP_EXIT_AT_MATCH_END (tests): a run that plays one match from its start
       to its end stops here, with the summary in the log */
    if (getenv("SP_EXIT_AT_MATCH_END") != NULL) {
        _match_summary_send(server, MATCH_SUMMARY_SECONDS, 0);
        LOG_INFO("Match over: exiting (SP_EXIT_AT_MATCH_END)");
        server->running = 0;
        return;
    }
    /* the 0.75 profile's clients have no summary screen: the next map at
       once, as it always was */
    if (profile_is_075()) {
        _match_reset(server);
        return;
    }
    /* the summary for everyone, then the next map: the match used to end
       at once, the map reloaded and everyone back at the team screen with
       no word of who won but a chat line */
    int voting  = mapvote_possible(server);
    int seconds = MATCH_SUMMARY_SECONDS + (voting ? MAP_VOTE_EXTRA_SECONDS : 0);
    _match_summary_send(server, seconds, voting);
    if (voting) {
        mapvote_start(server, seconds); /* after match-end: the client shows it over the table */
    }
    match_reset_at = get_nanos() + (uint64_t) seconds * NANO_IN_SECOND;
}

void gamemode_player_step(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->player_step != NULL) {
        ops->player_step(server, player);
    }
}

void gamemode_player_tick(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->player_tick != NULL) {
        ops->player_tick(server, player);
    }
}

void gamemode_player_out(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->player_out != NULL) {
        ops->player_out(server, player);
    }
}

void gamemode_terrain_changed(server_t* server, int built)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->terrain_changed != NULL) {
        ops->terrain_changed(server, built);
    }
}

unsigned gamemode_player_rules(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->player_rules != NULL ? ops->player_rules(server, player) : 0;
}

int gamemode_weapon_change_instant(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->weapon_change_instant != NULL ? ops->weapon_change_instant(server, player) : 0;
}

long gamemode_hit_damage(server_t* server, player_t* attacker, player_t* victim, long damage, uint8_t kill_reason)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->hit_damage != NULL ? ops->hit_damage(server, attacker, victim, damage, kill_reason) : damage;
}

void gamemode_player_killed(server_t* server, player_t* killer, player_t* victim)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->player_killed != NULL) {
        ops->player_killed(server, killer, victim);
    }
}

int gamemode_allows_respawn(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return (ops->allows_respawn == NULL || ops->allows_respawn(server, player)) && plugins_allows_respawn(server, player);
}

uint8_t gamemode_cell_editable(server_t* server, int x, int y, int z)
{
    /* the map's own rules (protected sectors and coordinates of its .txt)
       hold in every mode, and every edit path and the bots ask here */
    if (map_meta_cell_protected(server, x, y, z)) {
        return 0;
    }
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->cell_protected == NULL || !ops->cell_protected(server, x, y, z);
}

const char* gamemode_cell_why(server_t* server, int x, int y, int z)
{
    if (map_meta_cell_protected(server, x, y, z)) {
        return "map-protected";
    }
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->cell_protect_why != NULL) {
        const char* why = ops->cell_protect_why(server, x, y, z);
        if (why != NULL) {
            return why;
        }
    }
    return "gamemode";
}

/* a grenade breaks blocks only when every cell it would break may be edited:
   the 3x3x3 cube handle_grenade clears (checking only the corners missed
   the middle row, a one-cell protected line like Busan_Port's) */
uint8_t gamemode_grenade_can_destroy(server_t* server, vector3f_t pos)
{
    int x0 = (int) floorf(pos.x - 1);
    int y0 = (int) floorf(pos.y - 1);
    for (int x = x0; x < x0 + 3; x++) {
        for (int y = y0; y < y0 + 3; y++) {
            for (int dz = -1; dz <= 1; dz++) {
                if (!gamemode_cell_editable(server, x, y, (int) pos.z + dz)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

int gamemode_has_perks(server_t* server)
{
    /* no perks on the 0.75 profile: the jetpack's own checks (Jetpack.c)
       and the server's info ask here too */
    if (profile_is_075()) {
        return 0;
    }
    /* a mode whose classes the host turned on gets the classes' perks (the
       Rocketeer's jetpack, each class's streak perk) */
    if (server->features_on & GM_FEAT_CLASS_PERKS) {
        return 1;
    }
    return gamemode_current(server)->perks;
}

/* the features that are 1.0 inventions, absent on the 0.75 profile (one
   team and no bots are rules, not wire: they stay) */
#define GM_FEAT_PROTO10 (GM_FEAT_ASSISTS | GM_FEAT_SPOTTING | GM_FEAT_SQUAD_SPAWN | GM_FEAT_CLASSES | GM_FEAT_CLASS_PERKS)

int gamemode_has(server_t* server, unsigned feature)
{
    if (profile_is_075() && (feature & GM_FEAT_PROTO10)) {
        return 0;
    }
    /* the host's [squads] spawn = false (or the squads off) takes the
       respawn by a mate and the rally out of every mode */
    if (feature == GM_FEAT_SQUAD_SPAWN && !squads_spawn_on()) {
        return 0;
    }
    /* the mode may turn a feature on for this map: the zombie mode's classes
       are the host's choice, not the mode's own list */
    if (server->features_on & feature) {
        return 1;
    }
    return (gamemode_current(server)->features & feature) != 0;
}

int gamemode_team_max(server_t* server)
{
    return gamemode_current(server)->team_max;
}

int gamemode_team_full(server_t* server, uint8_t team)
{
    int most = gamemode_team_max(server);
    return most > 0 && (team == TEAM_A || team == TEAM_B) && server->protocol.num_team_users[team] >= most;
}

int gamemode_team_admit(server_t* server, uint8_t team)
{
    if (!gamemode_team_full(server, team)) {
        return 1;
    }
    /* the host's bots fill a team up; a person joining takes a bot's place
       (the dead ones first: nobody sees one vanish mid fight) */
    player_t *p, *tmp, *pick = NULL;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->is_bot && p->team == team && (pick == NULL || (!p->alive && pick->alive))) {
            pick = p;
        }
    }
    if (pick != NULL && bot_remove(server, pick->id)) {
        evt_log(NULL, "team", "bot_made_room team=%u max=%d", (unsigned) team, gamemode_team_max(server));
        return 1;
    }
    return 0;
}

int gamemode_set_classes(server_t* server, int on)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->set_classes == NULL) {
        return 0;
    }
    return ops->set_classes(server, on);
}

void gamemode_state_keys(server_t* server, gm_keys_t* out)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->state_keys != NULL) {
        ops->state_keys(server, out);
    }
}

/* A flank for a bot on its way to a point far off (territory control, the
   hill): instead of walking the straight line with the whole team, each bot
   bows out to a side of its own and comes in on the objective from the side,
   a half moon, so the two teams meet all over the map and not in one lane.
   The waypoint is drawn once per trip (a goal that moves with the bot made the
   planner redo its way every second) and dropped when the bot respawns (it
   jumped), reaches it, or the goal moves */
#define FLANK_NEAR 55.0f

typedef struct
{
    uint8_t    on;
    uint8_t    reached;
    vector3f_t goal, wp, last;
} flank_t;

static flank_t flank[256];

void gamemode_flank(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    map_store_t* map = &server->s_map.map;
    float        bx = bot->movement.position.x, by = bot->movement.position.y;
    float        dx = out->x - bx, dy = out->y - by, d = sqrtf(dx * dx + dy * dy);
    if (out->exact || d <= FLANK_NEAR) {
        return;
    }
    flank_t* f = &flank[bot->id];
    float jump = hypotf(bx - f->last.x, by - f->last.y);
    f->last.x  = bx;
    f->last.y  = by;
    if (!f->on || jump > 25.0f || hypotf(out->x - f->goal.x, out->y - f->goal.y) > 12.0f) {
        static const float sides[5] = {-1.0f, 1.0f, 0.0f, -0.6f, 0.6f};
        float              side     = sides[bot->id % 5];
        float              px = -dy / d, py = dx / d;
        float              wx = bx + dx * 0.5f + px * side * fminf(d * 0.45f, 90.0f);
        float              wy = by + dy * 0.5f + py * side * fminf(d * 0.45f, 90.0f);
        /* on the map, on dry ground: pulled back toward the goal until it is */
        for (int k = 0; k < 6; k++) {
            wx = fminf(fmaxf(wx, 4.0f), (float) map->size_x - 5.0f);
            wy = fminf(fmaxf(wy, 4.0f), (float) map->size_y - 5.0f);
            int top = map_top(map, (uint16_t) wx, (uint16_t) wy);
            if (top > 4 && top < map->size_z - 2) {
                break;
            }
            wx = (wx + out->x) * 0.5f;
            wy = (wy + out->y) * 0.5f;
        }
        f->on      = 1;
        f->reached = side == 0.0f; /* the middle one goes straight */
        f->goal    = (vector3f_t){out->x, out->y, out->z};
        f->wp      = (vector3f_t){wx, wy, (float) map_top(map, (uint16_t) wx, (uint16_t) wy)};
        evt_log(bot, "bot_flank", "action=plan side=%.2f wp=%d,%d goal=%d,%d", side, (int) wx, (int) wy, (int) out->x,
                (int) out->y);
    }
    if (f->reached) {
        return;
    }
    if (hypotf(f->wp.x - bx, f->wp.y - by) < 14.0f) {
        f->reached = 1;
        return;
    }
    out->x = f->wp.x;
    out->y = f->wp.y;
    out->z = f->wp.z;
}

/* A mode with no objective of its own (the arena, a Lua mode): the
   nearest enemy alive, wherever it is. Without a goal the bots stood at
   their spawns, out of each other's guns, flipping between cover and patrol
   (0 kills in 600 s of team deathmatch on 2Fort, 577 flips a bot) */
int gamemode_hunt_nearest(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    player_t *p, *tmp, *best = NULL;
    float     best_d = 1e18f;
    if (bot->team != TEAM_A && bot->team != TEAM_B) {
        return 0;
    }
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team == bot->team || (p->team != TEAM_A && p->team != TEAM_B) || p->state != STATE_READY || !p->alive) {
            continue;
        }
        float dx = p->movement.position.x - bot->movement.position.x;
        float dy = p->movement.position.y - bot->movement.position.y;
        if (dx * dx + dy * dy < best_d) {
            best_d = dx * dx + dy * dy;
            best   = p;
        }
    }
    if (best == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    gm_goal_name(out, "hunt-enemy", 0);
    out->x    = best->movement.position.x;
    out->y    = best->movement.position.y;
    out->z    = best->movement.position.z + 2.4f;
    out->role = GM_BOT_ATTACK;
    return 1;
}

int gamemode_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    /* a plugin's on_bot_think decides first; nil leaves it to
       the mode */
    if (plugins_bot_goal(server, bot, out)) {
        return 1;
    }
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->bot_goal == NULL) {
        return gamemode_hunt_nearest(server, bot, out);
    }
    return ops->bot_goal(server, bot, out);
}

int gamemode_player_carries(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->player_carries != NULL && ops->player_carries(server, player);
}

/* by default the middle of the enemy's spawn area: where the fight is */
void gamemode_patrol_anchor(server_t* server, player_t* bot, float* x, float* y)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->patrol_anchor != NULL) {
        ops->patrol_anchor(server, bot, x, y);
        return;
    }
    quad3d_t* spawn = &server->protocol.spawns[bot->team == TEAM_A ? TEAM_B : TEAM_A];
    *x              = (spawn->from.x + spawn->to.x) * 0.5f;
    *y              = (spawn->from.y + spawn->to.y) * 0.5f;
}

/* where a spawning player looks: the mode's objective, else where its bots
   patrol (the enemy base, the enemy spawn) */
void gamemode_spawn_facing(server_t* server, player_t* player, float* x, float* y)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->spawn_facing != NULL && ops->spawn_facing(server, player, x, y)) {
        return;
    }
    gamemode_patrol_anchor(server, player, x, y);
}

int gamemode_spawn_point(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    /* the squad first: it puts the player in one, and where the mode has
       the respawn by a mate (GM_FEAT_SQUAD_SPAWN) places it there; else the
       mode's own spawn (territory control's front, the arena's sides) */
    if (ops->squad_teams != 0 && squads_spawn_point(server, player)) {
        return 1;
    }
    return ops->spawn_point != NULL && ops->spawn_point(server, player);
}

int gamemode_join_team(server_t* server, player_t* player, uint8_t requested, uint8_t* out)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->join_team != NULL && ops->join_team(server, player, requested, out);
}

int gamemode_team_change(server_t* server, player_t* player, uint8_t requested, uint8_t* out)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->team_change != NULL && ops->team_change(server, player, requested, out);
}

int gamemode_escape_watch_player(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    return ops->escape_watch_player != NULL && ops->escape_watch_player(server, player);
}

void gamemode_client_in_game(server_t* server, player_t* player)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->squad_teams != 0) {
        squads_client_in_game(server, player);
    }
    if (ops->client_in_game != NULL) {
        ops->client_in_game(server, player);
    }
}

int gamemode_player_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    const gamemode_ops_t* ops = gamemode_current(server);
    if (ops->squad_teams != 0 && squads_command(server, player, command, arg)) {
        return 1;
    }
    return ops->player_command != NULL && ops->player_command(server, player, command, arg);
}

int gamemode_map_tagged(const char* map_name, const char* mode)
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
    if (root == NULL) {
        return 0;
    }
    int           tagged = 0;
    toml_table_t* map    = toml_table_in(root, "map");
    toml_array_t* modes  = map != NULL ? toml_array_in(map, "modes") : NULL;
    for (int k = 0; modes != NULL && k < toml_array_nelem(modes) && !tagged; k++) {
        toml_datum_t d = toml_string_at(modes, k);
        if (d.ok) {
            tagged = strcmp(d.u.s, mode) == 0;
            free(d.u.s);
        }
    }
    toml_free(root);
    return tagged;
}
