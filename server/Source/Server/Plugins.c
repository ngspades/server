// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Plugins.h>
#include <Server/Proto10/Crates.h>
#include <Server/Profile.h>

#ifndef ENABLE_LUA

void plugins_map_start(server_t* server)
{
    (void) server;
}
void plugins_map_ready(server_t* server)
{
    (void) server;
}
void plugins_tick(server_t* server)
{
    (void) server;
}
int plugins_on_join(server_t* s, player_t* p, uint8_t* team)
{
    (void) s, (void) p, (void) team;
    return 1;
}
int plugins_on_team(server_t* s, player_t* p, uint8_t team)
{
    (void) s, (void) p, (void) team;
    return 1;
}
void plugins_on_leave(server_t* s, player_t* p)
{
    (void) s, (void) p;
}
void plugins_on_spawn(server_t* s, player_t* p)
{
    (void) s, (void) p;
}
int plugins_on_spawn_point(server_t* s, player_t* p)
{
    (void) s, (void) p;
    return 0;
}
int plugins_allows_respawn(server_t* s, player_t* p)
{
    (void) s, (void) p;
    return 1;
}
int plugins_on_damage(server_t* s, player_t* a, player_t* v, long* amount, uint8_t reason)
{
    (void) s, (void) a, (void) v, (void) amount, (void) reason;
    return 1;
}
int plugins_on_hit(server_t* s, player_t* a, player_t* v, uint8_t kind)
{
    (void) s, (void) a, (void) v, (void) kind;
    return 1;
}
void plugins_on_kill(server_t* s, player_t* k, player_t* v, uint8_t reason)
{
    (void) s, (void) k, (void) v, (void) reason;
}
int plugins_on_build(server_t* s, player_t* p, int x, int y, int z)
{
    (void) s, (void) p, (void) x, (void) y, (void) z;
    return 1;
}
int plugins_on_destroy(server_t* s, player_t* p, int x, int y, int z, const char* kind)
{
    (void) s, (void) p, (void) x, (void) y, (void) z, (void) kind;
    return 1;
}
int plugins_on_chat(server_t* s, player_t* p, char* text, size_t size, int team)
{
    (void) s, (void) p, (void) text, (void) size, (void) team;
    return 1;
}
int plugins_on_tool(server_t* s, player_t* p, uint8_t tool)
{
    (void) s, (void) p, (void) tool;
    return 1;
}
int plugins_on_weapon(server_t* s, player_t* p, uint8_t weapon)
{
    (void) s, (void) p, (void) weapon;
    return 1;
}
int plugins_on_grenade(server_t* s, player_t* p)
{
    (void) s, (void) p;
    return 1;
}
int plugins_on_flashlight(server_t* s, player_t* p, int on)
{
    (void) s, (void) p, (void) on;
    return 1;
}
int plugins_on_class(server_t* s, player_t* p, int klass, const char* by)
{
    (void) s, (void) p, (void) klass, (void) by;
    return 1;
}
void plugins_on_explode(server_t* s, player_t* p, float x, float y, float z)
{
    (void) s, (void) p, (void) x, (void) y, (void) z;
}
void plugins_on_reload(server_t* s, player_t* p)
{
    (void) s, (void) p;
}
void plugins_on_intel(server_t* s, player_t* p, const char* action)
{
    (void) s, (void) p, (void) action;
}
void plugins_on_player_tick(server_t* s, player_t* p)
{
    (void) s, (void) p;
}
int plugins_bot_goal(server_t* s, player_t* p, gm_bot_goal_t* out)
{
    (void) s, (void) p, (void) out;
    return 0;
}
void plugins_on_bot_stuck(server_t* s, player_t* p, const char* why)
{
    (void) s, (void) p, (void) why;
}
int plugins_on_login(server_t* s, player_t* p, const char* role)
{
    (void) s, (void) p, (void) role;
    return 1;
}
int plugins_exists(const char* name)
{
    (void) name;
    return 0;
}
void plugins_names(char* out, size_t size)
{
    if (size > 0) {
        *out = 0;
    }
}
plugin_t* plugins_load_mode(server_t* server, const char* name)
{
    (void) server, (void) name;
    return NULL;
}
const char* plugins_mode_field(plugin_t* plugin, const char* field, char* buf, size_t size)
{
    (void) plugin, (void) field, (void) buf, (void) size;
    return NULL;
}
void plugins_mode_init(plugin_t* plugin)
{
    (void) plugin;
}
int plugins_mode_objective(float* x, float* y, float* r)
{
    (void) x, (void) y, (void) r;
    return 0;
}
int plugins_on_command(server_t* s, player_t* p, uint8_t console, const char* message)
{
    (void) s, (void) p, (void) console, (void) message;
    return 0;
}
void plugins_help(server_t* s, player_t* p, uint8_t console)
{
    (void) s, (void) p, (void) console;
}
void plugins_shutdown(server_t* s)
{
    (void) s;
}

#else

#include <Server/Bots.h>
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Ctf.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Gamemodes/LuaMode.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/Nodes.h>
#include <Server/Proto10/Helicopter.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/World.h>
#include <Util/Weapon.h>
#include <Server/Packets/Packets.h>
#include <Server/Player.h>
#include <Server/PluginNet.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Commands/CommandManager.h>
#include <Server/Commands/Commands.h>
#include <Server/ParseConvert.h>
#include <Server/Server.h>
#include <Server/Sky.h>
#include <Server/Outfit.h>
#include <Util/DataDir.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <lauxlib.h>
#include <math.h>
#include <lua.h>
#include <lualib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <json-c/json.h>
#include <tomlc99/toml.h>

#define PLUGIN_MAX        32
#define PLUGIN_TICK_EVERY 6   /* on_tick and the timers: 10 times a second */
#define PLUGIN_MAX_STEPS  200 /* a call stopped past 200 x 100000 instructions */
#define PLUGIN_TIMERS     64
#define PLUGIN_DIR        "Resources/plugins"
#define PLUGIN_HOSTS      16  /* allow_hosts entries a plugin may list */
#define PLUGIN_HTTP_EACH  16  /* requests a plugin may have waiting */
#define PLUGIN_HTTP       64  /* waiting, all plugins */
#define PLUGIN_COMMANDS   16  /* game.command names a plugin may register */
#define PLUGIN_DATA_DIR   PLUGIN_DIR "/data"
#define PLUGIN_STORE_MAX  (1024 * 1024) /* bytes of game.store on disk */
#define PLUGIN_CELLS_EACH 4096 /* game.set_block/clear_block a plugin tick: big edits go over several */
#define PLUGIN_CLEAR_COST 4    /* a cleared cell counts 4: its check for what then floats (check_node) is
                                  the cost, 4096 of them took 26 ms of one tick */

struct plugin
{
    lua_State* L;
    char       name[32];
    int        ref;       /* the table the script returned */
    uint8_t    broken;    /* an error turned it off */
    uint8_t    is_mode;   /* played as the game mode: asked last */
    uint8_t    rules_ctf; /* the mode plays over capture the flag's rules */
    int        steps;
    struct
    {
        uint64_t due;
        int      fn; /* registry ref, LUA_NOREF free */
    } timers[PLUGIN_TIMERS];
    /* the network: the hosts the server's owner lets it reach
       ([plugins.<name>] allow_hosts), and its requests waiting */
    char     hosts[PLUGIN_HOSTS][96];
    int      host_count;
    int      http_waiting;
    unsigned gen; /* which load: an answer for an older one is dropped */
    /* game.command: chat commands typed with "/" that the server lacks */
    struct
    {
        char     name[32]; /* with its "/", as the server's are kept */
        char     help[128];
        uint32_t permissions; /* the server's bits; 0 = anyone */
        int      fn;
    } commands[PLUGIN_COMMANDS];
    int     command_count;
    uint64_t bot_team_at; /* the last on_bot_team_tick */
    int     cells; /* map cells changed since the last plugin tick */
    uint8_t store_dirty; /* game.store_save() asked: written on the next plugin tick */
    /* the profiles (docs/plan-075-server.md section 5) */
    uint8_t  loading;          /* its chunk is running: game.supported_profiles is only taken then */
    uint8_t  profile_refused;  /* it declared profiles and the running one is not among them */
    unsigned refused_logged;   /* the 1.0-only calls already logged as refused (one line each a map) */
};

static plugin_t  plugins[PLUGIN_MAX];
static unsigned  pl_gen;
static int       plugin_count;
static unsigned  plugin_ticks;
static server_t* pl_server;

/* game.objective(): where the bots go and hold (a hill); the mode's */
static struct
{
    uint8_t set;
    float   x, y, r;
} pl_obj;

/* queued by game.set_map/set_mode/win/next_map/run_command: applying any of
   them from inside a Lua call would reload the map and close the very Lua
   state that is running (server_reset -> plugins_map_start -> lua_close).
   They are applied at the end of plugins_tick, after every plugin returned. */
static struct
{
    char     map[MAP_NAME_MAX + 1];
    int      mode;       /* gamemode id, -1 none */
    uint8_t  restart;    /* next map, no forced map */
    uint8_t  win;
    char     command[1001];
    uint8_t  allow_map;  /* [plugins] allow_map_change */
    uint8_t  allow_commands_count;
    char     allow_commands[16][32];
} pl_pending = {.mode = -1};

static uint64_t pl_reset_at; /* last plugin map/win reset, at most every 5 s */

static plugin_t* _me(lua_State* L)
{
    return *(plugin_t**) lua_getextraspace(L);
}

static void _hook(lua_State* L, lua_Debug* ar)
{
    (void) ar;
    if (++_me(L)->steps > PLUGIN_MAX_STEPS) {
        luaL_error(L, "the call ran too long (an endless loop?)");
    }
}

static void _fail(plugin_t* P, const char* where)
{
    const char* msg = lua_tostring(P->L, -1);
    LOG_ERROR("Plugin %s: error in %s: %s; the plugin is off", P->name, where, msg ? msg : "?");
    evt_log(NULL, "plugin", "action=error plugin=%s in=%s msg=\"%s\"", P->name, where, msg ? msg : "?");
    lua_pop(P->L, 1);
    P->broken = 1;
}

/* pushes the plugin's table[name] if it is a function; 0 when absent or off */
static int _event(plugin_t* P, const char* name)
{
    if (P->L == NULL || P->broken) {
        return 0;
    }
    lua_rawgeti(P->L, LUA_REGISTRYINDEX, P->ref);
    lua_getfield(P->L, -1, name);
    lua_remove(P->L, -2);
    if (!lua_isfunction(P->L, -1)) {
        lua_pop(P->L, 1);
        return 0;
    }
    return 1;
}

/* calls the function pushed with nargs arguments; 1 and nres results left
   on the stack, 0 on an error */
static int _call(plugin_t* P, const char* where, int nargs, int nres)
{
    P->steps = 0;
    if (lua_pcall(P->L, nargs, nres, 0) != LUA_OK) {
        _fail(P, where);
        return 0;
    }
    return 1;
}

/* --- the profiles: which server (1.0 or 0.75) a plugin is for --- */

/* the profile named as the flag takes it ("10", "1.0", "075", "0.75"):
   the server_profile_t, or -1 for no profile */
static int _profile_of(const char* s)
{
    if (!strcmp(s, "10") || !strcmp(s, "1.0")) {
        return SERVER_PROFILE_10;
    }
    if (!strcmp(s, "075") || !strcmp(s, "0.75")) {
        return SERVER_PROFILE_075;
    }
    return -1;
}

static void _log_profile_refused(const char* name, const char* from)
{
    LOG_WARNING("Plugin %s: not loaded, it is not for the %s profile (%s)", name, server_profile_name(), from);
    evt_log(NULL, "plugin", "action=refused name=%s why=profile profile=%s from=%s", name, server_profile_name(),
            from);
}

/* game.supported_profiles("10", "075"): the profiles the plugin is written
   for, at its top level. How the declaration is caught before the rest of
   the top level acts: when the running profile is not listed, this call
   raises an error, so the chunk stops right here and nothing after it runs
   (no command registered, no block set). The loader sees profile_refused
   and logs a refusal instead of an error. The flag, not the error's text,
   decides: a plugin that wraps the call in pcall goes on running its top
   level, and is still refused when the chunk returns. Code above the
   declaration has run, which is why the documentation puts it first. */
static int api_supported_profiles(lua_State* L)
{
    plugin_t* P = _me(L);
    if (!P->loading) {
        return luaL_error(L, "game.supported_profiles is only taken at the plugin's top level");
    }
    int n = lua_gettop(L);
    if (n == 0) {
        return luaL_error(L, "game.supported_profiles needs at least one profile (\"10\", \"075\")");
    }
    int listed = 0;
    for (int i = 1; i <= n; i++) {
        int k = _profile_of(luaL_checkstring(L, i));
        if (k < 0) {
            return luaL_error(L, "unknown profile '%s' (the profiles are \"10\" and \"075\")", lua_tostring(L, i));
        }
        listed |= k == (int) server_profile();
    }
    if (!listed) {
        P->profile_refused = 1;
        return luaL_error(L, "not for the %s profile", server_profile_name());
    }
    return 0;
}

/* [plugins.<name>] profiles = ["10"]: the operator's own restriction; 1
   when the running profile passes it (no key: every profile) */
static int _config_allows_profile(const char* name, const toml_table_t* config)
{
    toml_array_t* list = config != NULL ? toml_array_in(config, "profiles") : NULL;
    if (list == NULL) {
        return 1;
    }
    int listed = 0;
    for (int k = 0; k < toml_array_nelem(list); k++) {
        toml_datum_t d = toml_string_at(list, k);
        if (!d.ok) {
            continue;
        }
        int p = _profile_of(d.u.s);
        if (p < 0) {
            LOG_WARNING("Plugin %s: [plugins.%s] profiles lists '%s', which is no profile (\"10\", \"075\")", name,
                        name, d.u.s);
        }
        listed |= p == (int) server_profile();
        free(d.u.s);
    }
    return listed;
}

/* the 1.0-only API in the 0.75 profile (plan section 6.3): the classes,
   the perks, the squads. The caller returns what this pushed (false or nil,
   and the reason) when it returns nonzero; one EVT line per call and map */
enum
{
    REFUSED_CLASSES       = 1u << 0,
    REFUSED_CLASS         = 1u << 1,
    REFUSED_SET_CLASS     = 1u << 2,
    REFUSED_P_SET_CLASS   = 1u << 3,
    REFUSED_P_SET_JETPACK = 1u << 4,
    REFUSED_P_SET_SQUAD   = 1u << 5,
    REFUSED_SET_MODE      = 1u << 6,
    REFUSED_NEXT_MODE     = 1u << 7,
    REFUSED_ON_CLASS      = 1u << 8,
    REFUSED_DROP_CRATE    = 1u << 9,
    REFUSED_SKY           = 1u << 10,
    REFUSED_OUTFIT        = 1u << 11,
};

static void _log_api_refused(plugin_t* P, unsigned bit, const char* what)
{
    if (P->refused_logged & bit) {
        return;
    }
    P->refused_logged |= bit;
    LOG_WARNING("Plugin %s: %s is protocol 1.0 only, refused in the %s profile", P->name, what,
                server_profile_name());
    evt_log(NULL, "plugin", "action=refused name=%s why=profile what=%s", P->name, what);
}

static const char* const PROFILE_REFUSAL = "profile: protocol 1.0 only, the server runs the 0.75 profile";

/* 1 in the 0.75 profile, with nil (a query) or false (an action) and the
   reason pushed: the caller returns 2 */
static int _refuse_10_only(lua_State* L, unsigned bit, const char* what, int nil)
{
    if (!profile_is_075()) {
        return 0;
    }
    _log_api_refused(_me(L), bit, what);
    if (nil) {
        lua_pushnil(L);
    } else {
        lua_pushboolean(L, 0);
    }
    lua_pushstring(L, PROFILE_REFUSAL);
    return 1;
}

/* the modes the 0.75 profile registers (plan section 6.4): the classic
   ones, whose state a 0.75 client is told */
static int _mode_in_profile(const gamemode_ops_t* ops)
{
    static const char* const classic[] = {"ctf", "babel"};
    if (!profile_is_075()) {
        return 1;
    }
    for (size_t k = 0; k < sizeof classic / sizeof classic[0]; k++) {
        if (ops != NULL && ops->name != NULL && !strcmp(ops->name, classic[k]) && gamemode_find(classic[k]) == ops) {
            return 1;
        }
    }
    return 0;
}

static player_t* _player_by_id(lua_Integer id)
{
    if (id < 0 || id > 255) {
        return NULL;
    }
    uint8_t   key = (uint8_t) id;
    player_t* p;
    HASH_FIND(hh, pl_server->players, &key, sizeof(key), p);
    return p != NULL && p->state != STATE_DISCONNECTED ? p : NULL;
}

/* a player as a table (a snapshot: game.player(id) reads it again) whose
   methods act on the player through its id */
static void _push_player(lua_State* L, player_t* p)
{
    if (p == NULL) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 20);
    lua_pushinteger(L, p->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, p->name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, p->team == TEAM_A ? 1 : p->team == TEAM_B ? 2 : 0);
    lua_setfield(L, -2, "team");
    lua_pushboolean(L, p->alive);
    lua_setfield(L, -2, "alive");
    lua_pushinteger(L, p->hp);
    lua_setfield(L, -2, "hp");
    lua_pushinteger(L, p->kills);
    lua_setfield(L, -2, "kills");
    lua_pushinteger(L, p->deaths);
    lua_setfield(L, -2, "deaths");
    lua_pushinteger(L, p->assists);
    lua_setfield(L, -2, "assists");
    lua_pushinteger(L, p->score);
    lua_setfield(L, -2, "score");
    lua_pushinteger(L, p->captures);
    lua_setfield(L, -2, "captures");
    lua_pushinteger(L, p->shots);
    lua_setfield(L, -2, "shots");
    lua_pushinteger(L, p->hits);
    lua_setfield(L, -2, "hits");
    lua_pushinteger(L, p->blocks_placed);
    lua_setfield(L, -2, "blocks_placed");
    lua_pushinteger(L, p->squad);
    lua_setfield(L, -2, "squad");
    lua_pushboolean(L, p->reloading);
    lua_setfield(L, -2, "reloading");
    lua_pushboolean(L, p->crouching);
    lua_setfield(L, -2, "crouching");
    lua_pushboolean(L, p->sprinting);
    lua_setfield(L, -2, "sprinting");
    lua_pushboolean(L, p->airborne);
    lua_setfield(L, -2, "airborne");
    lua_pushboolean(L, p->wade);
    lua_setfield(L, -2, "wade");
    lua_pushboolean(L, p->jetpack_equipped);
    lua_setfield(L, -2, "jetpack");
    lua_pushboolean(L, p->is_bot);
    lua_setfield(L, -2, "bot");
    if (p->is_bot) {
        /* its goal's name and the enemy it fights (an id, nil none) */
        lua_pushstring(L, bot_goal_name(p));
        lua_setfield(L, -2, "goal");
        if (bot_target_id(p) >= 0) {
            lua_pushinteger(L, bot_target_id(p));
            lua_setfield(L, -2, "target");
        }
    }
    lua_pushinteger(L, p->item);
    lua_setfield(L, -2, "tool");
    lua_pushinteger(L, p->weapon);
    lua_setfield(L, -2, "weapon");
    lua_pushinteger(L, p->blocks);
    lua_setfield(L, -2, "blocks");
    lua_pushinteger(L, p->grenades);
    lua_setfield(L, -2, "grenades");
    lua_pushboolean(L, p->has_intel);
    lua_setfield(L, -2, "intel");
    /* its class's name where the mode has classes (ngCTF), else nil */
    if (!profile_is_075() && classes_on(pl_server)) {
        lua_pushstring(L, game_class_get(p->klass) ? game_class_get(p->klass)->name : "commando");
        lua_setfield(L, -2, "class");
    }
    if (!p->is_bot) {
        char ip[64];
        player_address(p, ip, sizeof ip);
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "ip");
    }
    /* map coordinates: z grows downwards, the hip's height */
    lua_pushnumber(L, p->movement.position.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, p->movement.position.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, p->movement.position.z);
    lua_setfield(L, -2, "z");
    /* where it looks, a unit vector */
    lua_pushnumber(L, p->movement.forward_orientation.x);
    lua_setfield(L, -2, "ox");
    lua_pushnumber(L, p->movement.forward_orientation.y);
    lua_setfield(L, -2, "oy");
    lua_pushnumber(L, p->movement.forward_orientation.z);
    lua_setfield(L, -2, "oz");
    luaL_setmetatable(L, "player");
}

/* the player a method was called on; an error when it left */
static player_t* _self(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    player_t* p = _player_by_id(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    if (p == NULL) {
        luaL_error(L, "that player has left");
    }
    return p;
}

static int _team_index(lua_State* L, int arg)
{
    lua_Integer t = luaL_checkinteger(L, arg);
    luaL_argcheck(L, t == 1 || t == 2, arg, "a team is 1 or 2");
    return (int) t - 1;
}

static void _send_fog(player_t* to, int r, int g, int b)
{
    char        v[3][8];
    const char* parts[4] = {"fog", v[0], v[1], v[2]};
    snprintf(v[0], sizeof v[0], "%d", r & 255);
    snprintf(v[1], sizeof v[1], "%d", g & 255);
    snprintf(v[2], sizeof v[2], "%d", b & 255);
    proto10_bridge_generic(pl_server, to, 4, parts);
}

/* --- player methods --- */

static int m_message(lua_State* L)
{
    player_t*   p    = _self(L);
    const char* text = luaL_checkstring(L, 2);
    if (p->conn != NULL) {
        send_server_notice(p, 0, "%s", text);
    }
    return 0;
}

static int m_set_hp(lua_State* L)
{
    player_t*   p  = _self(L);
    lua_Integer hp = luaL_checkinteger(L, 2);
    if (!p->alive) {
        return 0;
    }
    if (hp <= 0) {
        send_kill_action_packet(pl_server, p, p, 0, 5, 0);
        return 0;
    }
    p->hp = hp > 100 ? 100 : (int) hp;
    return 0;
}

/* p:damage(amount, by): hurt through the normal path (marks, kills) */
static int m_damage(lua_State* L)
{
    player_t*   p      = _self(L);
    lua_Integer amount = luaL_checkinteger(L, 2);
    player_t*   by     = p;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "id");
        player_t* a = _player_by_id(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        if (a != NULL) {
            by = a;
        }
    }
    if (p->alive && amount > 0) {
        send_set_hp(pl_server, by, p, (long) amount, 1, 0, 5, 0, p->movement.position);
    }
    return 0;
}

static int m_kill(lua_State* L)
{
    player_t* p = _self(L);
    if (p->alive) {
        send_kill_action_packet(pl_server, p, p, 0, 5, 0);
    }
    return 0;
}

/* p:teleport(x, y, z): map coordinates of the hip, as p.x/p.y/p.z read */
static int m_teleport(lua_State* L)
{
    player_t*  p   = _self(L);
    vector3f_t pos = {(float) luaL_checknumber(L, 2), (float) luaL_checknumber(L, 3), (float) luaL_checknumber(L, 4)};
    map_store_t*  map = &pl_server->s_map.map;
    luaL_argcheck(L, pos.x >= 0 && pos.x < map->size_x && pos.y >= 0 && pos.y < map->size_y, 2, "off the map");
    vector3f_t still              = {0, 0, 0};
    p->movement.position          = pos;
    p->movement.prev_position     = pos;
    p->movement.prev_legit_pos    = pos;
    p->movement.velocity          = still;
    evt_log(p, "plugin", "action=teleport plugin=%s to=%.1f,%.1f,%.1f", _me(L)->name, pos.x, pos.y, pos.z);
    proto10_bridge_teleport(p);
    return 0;
}

/* p:spawn_heli(): a helicopter on dry ground in front of the player, of the
   player's team (team 1 for a spectator); its entity id, or nil and why
   (profile, no_ground, too_many_objects). Not a base slot: it does not come
   back after a wreck, the script asks again */
static int m_spawn_heli(lua_State* L)
{
    player_t*     p   = _self(L);
    const char*   why = NULL;
    p10_object_t* o   = heli_spawn_in_front(pl_server, p, &why);
    if (o == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why != NULL ? why : "no_ground");
        return 2;
    }
    evt_log(p, "plugin", "action=spawn_heli plugin=%s entity=%u", _me(L)->name, o->id);
    lua_pushinteger(L, (lua_Integer) o->id);
    return 1;
}

static int m_restock(lua_State* L)
{
    player_t* p = _self(L);
    if (p->alive) {
        player_restock(pl_server, p);
    }
    return 0;
}

/* p:drop_crate("ammo"|"health"): a supply crate falls on the player, a
   reward (Proto10/Crates.c); true when it falls, false and why when not */
static int m_drop_crate(lua_State* L)
{
    player_t* p = _self(L);
    if (_refuse_10_only(L, REFUSED_DROP_CRATE, "p:drop_crate", 0)) {
        return 2;
    }
    uint8_t type = crates_type(luaL_checkstring(L, 2));
    if (type == 0) {
        return luaL_argerror(L, 2, "\"ammo\" or \"health\"");
    }
    int ok = crates_drop(pl_server, p->movement.position.x, p->movement.position.y, 0.0f, type, "plugin");
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, "too many crates out, or no ground there");
        return 2;
    }
    return 1;
}

/* p:fog(r, g, b): this player's fog only (game.fog sets everyone's) */
static int m_fog(lua_State* L)
{
    player_t* p = _self(L);
    _send_fog(p, (int) luaL_checkinteger(L, 2), (int) luaL_checkinteger(L, 3), (int) luaL_checkinteger(L, 4));
    return 0;
}

/* p:set_outfit(n[, colour]): how p looks (Outfit.c, the aosprotocol
   draft's numbers: 0 soldier, 7 miner, 64 zombie, 255 its team's), wearing
   team colour 0 or 1, or its own (255, the default); true or false and why */
static int m_set_outfit(lua_State* L)
{
    player_t* p = _self(L);
    if (_refuse_10_only(L, REFUSED_OUTFIT, "p:set_outfit", 0)) {
        return 2;
    }
    int outfit = (int) luaL_checkinteger(L, 2);
    int colour = (int) luaL_optinteger(L, 3, 255);
    if (!outfit_set(pl_server, p, outfit, colour, "plugin")) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "outfit: 0..255");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* p:outfit(): what p was dressed in (255: its team's) */
static int m_outfit(lua_State* L)
{
    player_t* p = _self(L);
    if (_refuse_10_only(L, REFUSED_OUTFIT, "p:outfit", 1)) {
        return 2;
    }
    lua_pushinteger(L, outfit_of(p));
    return 1;
}

/* p:has_role("admin"): logged in as that role or a higher one (trusted,
   guard, mod, admin, manager) */
/* p:sees(q): a clear line from p's eye to q's chest (the bots' sight test,
   for any player) */
static int m_sees(lua_State* L)
{
    player_t* p = _self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "id");
    player_t* q = _player_by_id(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, q != NULL && bot_line_of_sight(pl_server, p, q));
    return 1;
}

static int m_has_role(lua_State* L)
{
    player_t*   p    = _self(L);
    const char* role = luaL_checkstring(L, 2);
    int         yes  = 0;
    for (int k = 0; k < 5; k++) {
        if (!strcmp(p->role_list[k].access_level, role)) {
            yes = (p->permissions >> p->role_list[k].perm_level_offset) != 0;
        }
    }
    lua_pushboolean(L, yes);
    return 1;
}

/* p:kick(reason): disconnected, everyone told (bots cannot be kicked:
   they have no connection; game.players and the host's menu remove them) */
static int m_kick(lua_State* L)
{
    player_t*   p      = _self(L);
    const char* reason = luaL_optstring(L, 2, "");
    if (p->conn == NULL) {
        return luaL_error(L, "a bot cannot be kicked");
    }
    evt_log(p, "plugin", "action=kick plugin=%s reason=\"%s\"", _me(L)->name, reason);
    broadcast_server_notice(pl_server, 0, "Player %s (#%hhu) has been kicked%s%s", p->name, p->id, *reason ? ": " : "",
                            reason);
    notice_kick(p, REASON_KICKED, CONN_CLOSE, "You were kicked%s%s", *reason ? ": " : "", reason);
    return 0;
}

/* a class by name ("miner") or number (0..3), -1 when there is none */
static int _class_arg(lua_State* L, int i)
{
    if (lua_type(L, i) == LUA_TNUMBER) {
        int k = (int) lua_tointeger(L, i);
        return game_class_get(k) ? k : -1;
    }
    return game_class_find(luaL_checkstring(L, i));
}

/* p:set_class(name): picks it for the player, as its class screen does (a
   living one respawns with it); false when refused (a class turned off,
   or the mode has none) */
static int m_set_class(lua_State* L)
{
    player_t* p = _self(L);
    if (_refuse_10_only(L, REFUSED_P_SET_CLASS, "p:set_class", 0)) {
        return 2;
    }
    int k = _class_arg(L, 2);
    lua_pushboolean(L, k >= 0 && classes_pick(pl_server, p, k, "plugin"));
    return 1;
}


/* p:set_team(0|1|2): spectators, blue, red; the mode's team rules still
   apply (a zombie does not cross), and a team change goes through the same
   out-of-play/spawn path as the client packet */
static int m_set_team(lua_State* L)
{
    player_t* p = _self(L);
    lua_Integer t = luaL_checkinteger(L, 2);
    if (t < 0 || t > 2) {
        lua_pushboolean(L, 0);
        return 1;
    }
    uint8_t requested = t == 0 ? TEAM_SPECTATOR : (uint8_t) (t - 1);
    uint8_t old       = p->team;
    if (old == requested) {
        lua_pushboolean(L, 1);
        return 1;
    }
    uint8_t chosen = requested;
    if (gamemode_team_change(pl_server, p, requested, &chosen)) {
        if (chosen != TEAM_A && chosen != TEAM_B && chosen != TEAM_SPECTATOR) {
            chosen = old;
        }
        if (chosen == old) {
            lua_pushboolean(L, 0);
            return 1;
        }
        requested = chosen;
    }
    if (old != TEAM_SPECTATOR) {
        gamemode_player_out(pl_server, p);
    }
    if (pl_server->protocol.num_team_users[old] > 0) {
        pl_server->protocol.num_team_users[old]--;
    }
    p->team = requested;
    pl_server->protocol.num_team_users[requested]++;
    if (old == TEAM_SPECTATOR) {
        send_respawn(pl_server, p);
    } else {
        send_kill_action_packet(pl_server, p, p, 5, 5, 0);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* p:set_tool(tool): through the mode's rules, but not through the plugin
   event again (a plugin calling it would otherwise re-enter itself) */
static int m_set_tool(lua_State* L)
{
    player_t* p    = _self(L);
    lua_Integer t  = luaL_checkinteger(L, 2);
    if (t < 0 || t > 3) {
        lua_pushboolean(L, 0);
        return 1;
    }
    unsigned rules = gamemode_player_rules(pl_server, p);
    if ((t == TOOL_GUN && (rules & GM_RULE_NO_GUN)) || (t == TOOL_GRENADE && (rules & GM_RULE_NO_GRENADE)) ||
        (t == TOOL_BLOCK && (rules & GM_RULE_NO_BLOCK)))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    p->item = (tool_t) t;
    if (p->reloading && p->item != TOOL_GUN) {
        p->reloading = 0;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* p:set_weapon(0|1|2): rifle, SMG, shotgun; ammo is refilled to its usual
   clip, the client sees the new gun at its next update */
static int m_set_weapon(lua_State* L)
{
    player_t* p   = _self(L);
    lua_Integer w = luaL_checkinteger(L, 2);
    if (w < 0 || w > 2) {
        lua_pushboolean(L, 0);
        return 1;
    }
    p->weapon = (weapon_t) w;
    set_default_player_ammo(p);
    lua_pushboolean(L, 1);
    return 1;
}

/* p:set_ammo(clip, reserve) */
static int m_set_ammo(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer c = luaL_checkinteger(L, 2);
    lua_Integer r = luaL_checkinteger(L, 3);
    p->weapon_clip    = (uint8_t) (c < 0 ? 0 : c > 255 ? 255 : c);
    p->weapon_reserve = (uint8_t) (r < 0 ? 0 : r > 255 ? 255 : r);
    return 0;
}

static int m_set_blocks(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    p->blocks     = (uint16_t) (n < 0 ? 0 : n > 65535 ? 65535 : n);
    proto10_bridge_wallet(pl_server, p);
    return 0;
}

static int m_set_grenades(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    p->grenades   = (uint8_t) (n < 0 ? 0 : n > 255 ? 255 : n);
    return 0;
}

/* personal score (the extra column PlayerUpdate.score carries), not the team
   score; kills/deaths/assists are the board's columns */
static int m_set_score(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    p->score      = (uint32_t) (n < 0 ? 0 : n);
    return 0;
}

static int m_set_kills(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    p->kills      = (uint32_t) (n < 0 ? 0 : n);
    return 0;
}

static int m_set_deaths(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    p->deaths     = (uint32_t) (n < 0 ? 0 : n);
    return 0;
}

static int m_set_assists(lua_State* L)
{
    player_t*   p = _self(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    p->assists    = (uint32_t) (n < 0 ? 0 : n);
    return 0;
}

/* p:set_squad(n): 0 alone, 1..16; the same rules as the client's command */
static int m_set_squad(lua_State* L)
{
    player_t* p = _self(L);
    if (_refuse_10_only(L, REFUSED_P_SET_SQUAD, "p:set_squad", 0)) {
        return 2;
    }
    lua_Integer n = luaL_checkinteger(L, 2);
    char        arg[16];
    snprintf(arg, sizeof arg, "%d", (int) n);
    lua_pushboolean(L, squads_command(pl_server, p, "squad", arg) != 0);
    return 1;
}

/* p:set_jetpack(true|false): grants/removes the jetpack for this life */
static int m_set_jetpack(lua_State* L)
{
    player_t* p = _self(L);
    if (_refuse_10_only(L, REFUSED_P_SET_JETPACK, "p:set_jetpack", 0)) {
        return 2;
    }
    int on = lua_toboolean(L, 2);
    if (on && !p->jetpack_equipped) {
        jetpack_grant(pl_server, p);
    } else if (!on && p->jetpack_equipped) {
        jetpack_remove(pl_server, p, "plugin");
    }
    lua_pushboolean(L, p->jetpack_equipped != 0);
    return 1;
}

static const luaL_Reg player_methods[] = {
    {"message", m_message}, {"set_hp", m_set_hp},   {"damage", m_damage}, {"kill", m_kill},
    {"teleport", m_teleport}, {"spawn_heli", m_spawn_heli}, {"restock", m_restock}, {"fog", m_fog}, {"drop_crate", m_drop_crate}, {"set_outfit", m_set_outfit}, {"outfit", m_outfit},       {"has_role", m_has_role}, {"sees", m_sees}, {"kick", m_kick},
    {"set_class", m_set_class},
    {"set_team", m_set_team},       {"set_tool", m_set_tool},     {"set_weapon", m_set_weapon},
    {"set_ammo", m_set_ammo},       {"set_blocks", m_set_blocks}, {"set_grenades", m_set_grenades},
    {"set_score", m_set_score},     {"set_kills", m_set_kills},   {"set_deaths", m_set_deaths},
    {"set_assists", m_set_assists}, {"set_squad", m_set_squad},   {"set_jetpack", m_set_jetpack},
    {NULL, NULL},
};

/* --- game.* --- */

/* game.players(): every player in a team or spectating */
static int api_players(lua_State* L)
{
    player_t *p, *tmp;
    int       k = 1;
    lua_newtable(L);
    HASH_ITER(hh, pl_server->players, p, tmp)
    {
        if (p->state == STATE_DISCONNECTED) {
            continue;
        }
        _push_player(L, p);
        lua_rawseti(L, -2, k++);
    }
    return 1;
}

/* game.player(id): the player, nil when there is none */
static int api_player(lua_State* L)
{
    _push_player(L, _player_by_id(luaL_checkinteger(L, 1)));
    return 1;
}

/* game.find(name): the player with that name, or the only one whose name
   starts with it */
static int api_find(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    size_t      n    = strlen(name);
    player_t *  p, *tmp, *found = NULL;
    int         count = 0;
    HASH_ITER(hh, pl_server->players, p, tmp)
    {
        if (p->state == STATE_DISCONNECTED) {
            continue;
        }
        if (!strcasecmp(p->name, name)) {
            found = p;
            count = 1;
            break;
        }
        if (n > 0 && !strncasecmp(p->name, name, n)) {
            found = p;
            count++;
        }
    }
    _push_player(L, count == 1 ? found : NULL);
    return 1;
}

static int api_score(lua_State* L)
{
    lua_pushinteger(L, pl_server->protocol.gamemode.score[_team_index(L, 1)]);
    return 1;
}

static int api_set_score(lua_State* L)
{
    int t = _team_index(L, 1);
    pl_server->protocol.gamemode.score[t] = (uint8_t) luaL_checkinteger(L, 2);
    return 0;
}

static int api_score_limit(lua_State* L)
{
    lua_pushinteger(L, pl_server->protocol.gamemode.score_limit);
    return 1;
}

/* game.set_time_limit(seconds): the match ends by the score that many seconds
   from now (0 takes the limit off); a mode's `time_limit` field does it at the
   start. false when the plugin is not the mode being played. */
static int api_set_time_limit(lua_State* L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n < 0 || n > 86400) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the limit is 0..86400 seconds");
        return 2;
    }
    if (!lua_mode_set_time_limit((int) n)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no script mode is being played");
        return 2;
    }
    evt_log(NULL, "plugin", "action=time_limit plugin=%s seconds=%d", _me(L)->name, (int) n);
    lua_pushboolean(L, 1);
    return 1;
}

/* game.time_left(): seconds left of the time limit, nil when there is none */
static int api_time_left(lua_State* L)
{
    int left = lua_mode_time_left();
    if (left < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, left);
    }
    return 1;
}

/* game.win(team): the match is over, the map reloads. It is queued: calling
   server_reset here would close this Lua state while it is running. */
static int api_win(lua_State* L)
{
    int t = _team_index(L, 1);
    evt_log(NULL, "plugin", "action=win plugin=%s team=%d score=%u-%u", _me(L)->name, t + 1,
            pl_server->protocol.gamemode.score[0], pl_server->protocol.gamemode.score[1]);
    pl_pending.win = 1;
    return 0;
}

/* game.end_match(team): the same, named for what it does */
static int api_end_match(lua_State* L)
{
    return api_win(L);
}

/* game.set_score_limit(n) / game.set_capture_limit(n): the score the match
   ends at (the map toml or config.toml still decides the next map) */
static int api_set_score_limit(lua_State* L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n < 1 || n > 255) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the limit is 1..255");
        return 2;
    }
    pl_server->protocol.gamemode.score_limit = (uint8_t) n;
    pl_server->capture_limit                 = (uint8_t) n;
    evt_log(NULL, "plugin", "action=score_limit plugin=%s limit=%d", _me(L)->name, (int) n);
    lua_pushboolean(L, 1);
    return 1;
}

static int api_set_capture_limit(lua_State* L)
{
    return api_set_score_limit(L);
}

/* a team's name/colour; GameModeState's per-tick delta tells the clients */
static int api_team_name(lua_State* L)
{
    int         t = _team_index(L, 1);
    const char* n = luaL_checkstring(L, 2);
    if (strlen(n) >= sizeof(pl_server->protocol.name_team[0])) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the name is too long");
        return 2;
    }
    snprintf(pl_server->protocol.name_team[t], sizeof(pl_server->protocol.name_team[t]), "%s", n);
    return 0;
}

static int api_team_color(lua_State* L)
{
    int t = _team_index(L, 1);
    int r = (int) luaL_checkinteger(L, 2);
    int g = (int) luaL_checkinteger(L, 3);
    int b = (int) luaL_checkinteger(L, 4);
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "a colour is 0..255");
        return 2;
    }
    pl_server->protocol.color_team[t].r = (uint8_t) r;
    pl_server->protocol.color_team[t].g = (uint8_t) g;
    pl_server->protocol.color_team[t].b = (uint8_t) b;
    return 0;
}

/* game.next_map(): load the next map of the rotation at the end of this
   plugin tick. Map changes are opt-in ([plugins] allow_map_change). */
static int api_next_map(lua_State* L)
{
    if (!pl_pending.allow_map) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "[plugins] allow_map_change is false");
        return 2;
    }
    /* the next map keeps the running mode: one the 0.75 profile does not
       register (it should never run there) is not carried on */
    if (!_mode_in_profile(gamemode_current(pl_server)) &&
        _refuse_10_only(L, REFUSED_NEXT_MODE, "game.next_map", 0))
    {
        return 2;
    }
    pl_server->forced_map[0] = '\0';
    pl_pending.map[0]        = '\0';
    pl_pending.mode          = -1;
    pl_pending.restart       = 1;
    evt_log(NULL, "plugin", "action=next_map plugin=%s", _me(L)->name);
    lua_pushboolean(L, 1);
    return 1;
}

/* game.set_map(name): the next load uses this map. The request is queued,
   so the map reset cannot close this Lua state under its own call. */
static int api_set_map(lua_State* L)
{
    if (!pl_pending.allow_map) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "[plugins] allow_map_change is false");
        return 2;
    }
    const char* name = luaL_checkstring(L, 1);
    if (!server_force_map(pl_server, name)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "map not found (or not a safe map name)");
        return 2;
    }
    if (pl_pending.mode >= 0) {
        const gamemode_ops_t* mode = gamemode_ops_of((uint8_t) pl_pending.mode);
        if (mode != NULL && mode->map_fits != NULL && !mode->map_fits(name)) {
            pl_server->forced_map[0] = '\0';
            lua_pushboolean(L, 0);
            lua_pushstring(L, "the pending game mode does not suit that map");
            return 2;
        }
    }
    /* server_force_map filled the server's field; keep it in the pending
       slot instead, because the map currently loading clears it when its
       own init finishes (this call may be in on_init) */
    snprintf(pl_pending.map, sizeof pl_pending.map, "%s", name);
    pl_server->forced_map[0] = '\0';
    pl_pending.restart       = 1;
    evt_log(NULL, "plugin", "action=set_map plugin=%s map=%s", _me(L)->name, name);
    lua_pushboolean(L, 1);
    return 1;
}

/* game.set_mode(name): the next load starts this mode, on the current map
   unless set_map follows. Queued for the same reason as set_map. */
static int api_set_mode(lua_State* L)
{
    if (!pl_pending.allow_map) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "[plugins] allow_map_change is false");
        return 2;
    }
    const char* name = luaL_checkstring(L, 1);
    const gamemode_ops_t* ops = gamemode_find(name);
    if (ops == NULL) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "unknown game mode");
        return 2;
    }
    if (!_mode_in_profile(ops) && _refuse_10_only(L, REFUSED_SET_MODE, "game.set_mode", 0)) {
        return 2;
    }
    const char* map = pl_pending.map[0] != '\0' ? pl_pending.map : pl_server->map_name;
    if (ops->map_fits != NULL && !ops->map_fits(map)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "that mode does not suit the map");
        return 2;
    }
    pl_pending.mode    = ops->id;
    if (pl_pending.map[0] == '\0') {
        snprintf(pl_pending.map, sizeof pl_pending.map, "%s", pl_server->map_name);
    }
    pl_pending.restart = 1;
    evt_log(NULL, "plugin", "action=set_mode plugin=%s mode=%s", _me(L)->name, ops->name);
    lua_pushboolean(L, 1);
    return 1;
}

/* game.run_command(text): one console command, only from
   [plugins] allow_commands and never /shutdown, /login or /logout. It runs
   after every plugin returned, so a /reset cannot close a live Lua state. */
static int api_run_command(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    if (strlen(text) > 1000 || strpbrk(text, "\n\r;|&`") != NULL || strstr(text, "$(") != NULL) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "one plain command line, no chaining");
        return 2;
    }
    char first[32] = {0};
    sscanf(text[0] == '/' ? text + 1 : text, "%31s", first);
    for (char* c = first; *c; c++) {
        *c = (char) tolower((unsigned char) *c);
    }
    if (first[0] == '\0' || !strcmp(first, "shutdown") || !strcmp(first, "login") || !strcmp(first, "logout")) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "that command is not available to plugins");
        return 2;
    }
    int allowed = 0;
    for (int k = 0; k < pl_pending.allow_commands_count; k++) {
        if (!strcmp(first, pl_pending.allow_commands[k])) {
            allowed = 1;
            break;
        }
    }
    if (!allowed) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "not in [plugins] allow_commands");
        return 2;
    }
    if (pl_pending.command[0] != '\0') {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "another plugin command is already waiting");
        return 2;
    }
    snprintf(pl_pending.command, sizeof pl_pending.command, "%s%s", text[0] == '/' ? "" : "/", text);
    evt_log(NULL, "plugin", "action=command plugin=%s command=%s", _me(L)->name, first);
    lua_pushboolean(L, 1);
    return 1;
}

/* game.notice(text[, team]): a line every player (of that team) reads */
static int api_notice(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    int         team = lua_isnoneornil(L, 2) ? -1 : _team_index(L, 2);
    player_t *  p, *tmp;
    HASH_ITER(hh, pl_server->players, p, tmp)
    {
        if (p->conn != NULL && p->state != STATE_DISCONNECTED && (team < 0 || (int) p->team == team)) {
            send_server_notice(p, 0, "%s", text);
        }
    }
    evt_log(NULL, "plugin", "action=notice plugin=%s team=%d text=\"%s\"", _me(L)->name, team + 1, text);
    return 0;
}

/* game.drop_crate(x, y, "ammo"|"health"): a supply crate falls there (the
   nearest dry column), true or false and why */
static int api_drop_crate(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_DROP_CRATE, "game.drop_crate", 0)) {
        return 2;
    }
    float   x    = (float) luaL_checknumber(L, 1), y = (float) luaL_checknumber(L, 2);
    uint8_t type = crates_type(luaL_checkstring(L, 3));
    if (type == 0) {
        return luaL_argerror(L, 3, "\"ammo\" or \"health\"");
    }
    int ok = crates_drop(pl_server, x, y, 0.0f, type, "plugin");
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, "too many crates out, or no ground there");
        return 2;
    }
    return 1;
}

/* game.fog(r, g, b): everyone's fog, also for those who join later */
static int api_fog(lua_State* L)
{
    int r = (int) luaL_checkinteger(L, 1) & 255, g = (int) luaL_checkinteger(L, 2) & 255,
        b = (int) luaL_checkinteger(L, 3) & 255;
    pl_server->protocol.color_fog.r = (uint8_t) r;
    pl_server->protocol.color_fog.g = (uint8_t) g;
    pl_server->protocol.color_fog.b = (uint8_t) b;
    _send_fog(NULL, r, g, b);
    return 0;
}

/* game.get_fog(): r, g, b */
static int api_get_fog(lua_State* L)
{
    lua_pushinteger(L, pl_server->protocol.color_fog.r);
    lua_pushinteger(L, pl_server->protocol.color_fog.g);
    lua_pushinteger(L, pl_server->protocol.color_fog.b);
    return 3;
}

/* game.set_team_outfit(team, n): every player of team 1 or 2 whose outfit
   is its team's wears n (0..254); true or false and why */
static int api_set_team_outfit(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_OUTFIT, "game.set_team_outfit", 0)) {
        return 2;
    }
    int team = (int) luaL_checkinteger(L, 1) - 1, outfit = (int) luaL_checkinteger(L, 2);
    if (!outfit_set_team(pl_server, team, outfit, "plugin")) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "team: 1 or 2, outfit: 0..254");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* game.sky(): {time = minutes since midnight, rate, weather, intensity,
   moonlight} (Sky.c; nil in the 0.75 profile) */
static int api_sky(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_SKY, "game.sky", 1)) {
        return 2;
    }
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, sky_minutes());
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, sky_rate());
    lua_setfield(L, -2, "rate");
    lua_pushstring(L, sky_weather_name(sky_weather()));
    lua_setfield(L, -2, "weather");
    lua_pushinteger(L, sky_intensity());
    lua_setfield(L, -2, "intensity");
    lua_pushinteger(L, sky_moonlight());
    lua_setfield(L, -2, "moonlight");
    return 1;
}

/* game.set_sky{time = minutes or "hh:mm"/"night"..., rate, weather,
   intensity, moonlight}: any of them; true, or false and the reason */
static int api_set_sky(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_SKY, "game.set_sky", 0)) {
        return 2;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    float minutes = -1.0f;
    lua_getfield(L, 1, "time");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        minutes = (float) lua_tonumber(L, -1);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        minutes = (float) sky_parse_time(lua_tostring(L, -1));
        if (minutes < 0.0f) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "time: hh:mm, day, night, dawn or dusk");
            return 2;
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "weather");
    int weather = sky_weather();
    if (!lua_isnil(L, -1)) {
        weather = sky_weather_find(luaL_checkstring(L, -1));
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "intensity");
    int intensity = lua_isnil(L, -1) ? sky_intensity() : (int) luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "moonlight");
    int moonlight = lua_isnil(L, -1) ? sky_moonlight() : (int) luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "rate");
    float rate = lua_isnil(L, -1) ? sky_rate() : (float) luaL_checknumber(L, -1);
    lua_pop(L, 1);
    const char* bad = weather < 0                         ? "weather: clear, rain, snow, fog or storm"
                      : intensity < 0 || intensity > 100  ? "intensity: 0..100"
                      : moonlight < 0 || moonlight > 100  ? "moonlight: 0..100"
                      : rate < 0.0f || rate > 1440.0f     ? "rate: 0..1440 minutes a second"
                                                          : NULL;
    if (bad != NULL) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, bad);
        return 2;
    }
    if (minutes >= 0.0f) {
        sky_set_time(pl_server, minutes, "plugin");
    }
    sky_set_rate(pl_server, rate, "plugin");
    sky_set_weather(pl_server, weather, intensity, "plugin");
    sky_set_moonlight(pl_server, moonlight, "plugin");
    lua_pushboolean(L, 1);
    return 1;
}

static int api_cell(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2), z = luaL_checkinteger(L, 3);
    map_store_t*   map = &pl_server->s_map.map;
    int ok = x >= 0 && y >= 0 && z >= 0 && x < map->size_x && y < map->size_y && z < map->size_z;
    lua_pushboolean(L, ok && map_solid(map, (int) x, (int) y, (int) z));
    return 1;
}

/* game.color(x, y, z): the block's r, g, b, or nil when it is air */
static int api_color(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2), z = luaL_checkinteger(L, 3);
    map_store_t*   map = &pl_server->s_map.map;
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= map->size_z ||
        !map_solid(map, (int) x, (int) y, (int) z))
    {
        return 0;
    }
    color_t c = {.raw = map_color(map, (uint16_t) x, (uint16_t) y, (uint16_t) z)};
    lua_pushinteger(L, c.r);
    lua_pushinteger(L, c.g);
    lua_pushinteger(L, c.b);
    return 3;
}

/* the checks a plugin's edit passes, the same a player's build or dig
   meets: inside the map, above the water layers (62 and 63), a cell the
   mode and the map let change; NULL when it may, else why not */
static const char* _cell_refused(plugin_t* P, lua_Integer x, lua_Integer y, lua_Integer z)
{
    map_store_t* map = &pl_server->s_map.map;
    if (x < 0 || y < 0 || z < 0 || x >= map->size_x || y >= map->size_y || z >= 62) {
        return "outside";
    }
    if (!gamemode_cell_editable(pl_server, (int) x, (int) y, (int) z)) {
        return "protected";
    }
    if (P->cells >= PLUGIN_CELLS_EACH) {
        return "rate";
    }
    return NULL;
}

/* game.set_block(x, y, z, r, g, b): true, or false and why (outside,
   protected, rate: more than PLUGIN_CELLS_EACH this tick, try again with
   game.later) */
static int api_set_block(lua_State* L)
{
    plugin_t*   P = _me(L);
    lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2), z = luaL_checkinteger(L, 3);
    lua_Integer r = luaL_checkinteger(L, 4), g = luaL_checkinteger(L, 5), b = luaL_checkinteger(L, 6);
    const char* why = _cell_refused(P, x, y, z);
    if (why != NULL) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, why);
        return 2;
    }
    color_t c = {.r = (uint8_t) (r & 255), .g = (uint8_t) (g & 255), .b = (uint8_t) (b & 255), .a = 0xFF};
    map_set_color(&pl_server->s_map.map, (int) x, (int) y, (int) z, c.raw);
    proto10_world_cell(pl_server, (int) x, (int) y, (int) z, P10_WORLD_CAUSE_NONE);
    P->cells++;
    lua_pushboolean(L, 1);
    return 1;
}

/* game.clear_block(x, y, z): like a spade, what then hangs on nothing
   falls; true, or false and why */
static int api_clear_block(lua_State* L)
{
    plugin_t*   P = _me(L);
    lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2), z = luaL_checkinteger(L, 3);
    const char* why = _cell_refused(P, x, y, z);
    if (why != NULL) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, why);
        return 2;
    }
    map_store_t* map = &pl_server->s_map.map;
    if (map_solid(map, (int) x, (int) y, (int) z)) {
        map_set_air(map, (int) x, (int) y, (int) z);
        proto10_world_cell(pl_server, (int) x, (int) y, (int) z, P10_WORLD_CAUSE_NONE);
        P->cells += PLUGIN_CLEAR_COST;
        vector3i_t  position = {(int) x, (int) y, (int) z};
        vector3i_t* neigh    = get_neighbours(position);
        for (int i = 0; i < 6; ++i) {
            if (neigh[i].z < 62) {
                check_node(pl_server, neigh[i]);
            }
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* game.unix_time(): seconds since 1970 by the machine's clock (game.time is
   the game's, which a test may speed up) */
static int api_unix_time(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer) time(NULL));
    return 1;
}

/* game.map_size(): x, y, z */
static int api_map_size(lua_State* L)
{
    map_store_t* map = &pl_server->s_map.map;
    lua_pushinteger(L, map->size_x);
    lua_pushinteger(L, map->size_y);
    lua_pushinteger(L, map->size_z);
    return 3;
}

/* game.ground(x, y): the column's top block (z grows downwards: the smallest
   solid z; size_z - 1 is the water) */
static int api_ground(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    map_store_t*   map = &pl_server->s_map.map;
    luaL_argcheck(L, x >= 0 && x < map->size_x, 1, "off the map");
    luaL_argcheck(L, y >= 0 && y < map->size_y, 2, "off the map");
    lua_pushinteger(L, map_top(map, (uint16_t) x, (uint16_t) y));
    return 1;
}

/* game.spawn(team): the middle of the team's spawn area, x, y */
static int api_spawn(lua_State* L)
{
    quad3d_t* spawn = &pl_server->protocol.spawns[_team_index(L, 1)];
    lua_pushnumber(L, (spawn->from.x + spawn->to.x) * 0.5f);
    lua_pushnumber(L, (spawn->from.y + spawn->to.y) * 0.5f);
    return 2;
}

/* game.objective(x, y, radius): every bot goes there and holds within the
   radius; game.objective() and they hunt the nearest enemy again. The
   mode's only */
static int api_objective(lua_State* L)
{
    if (!_me(L)->is_mode) {
        return luaL_error(L, "game.objective is for the game mode");
    }
    if (lua_isnoneornil(L, 1)) {
        pl_obj.set = 0;
        return 0;
    }
    pl_obj.x   = (float) luaL_checknumber(L, 1);
    pl_obj.y   = (float) luaL_checknumber(L, 2);
    pl_obj.r   = (float) luaL_optnumber(L, 3, 4.0);
    pl_obj.set = 1;
    evt_log(NULL, "plugin", "action=objective plugin=%s at=%.0f,%.0f radius=%.0f", _me(L)->name, pl_obj.x, pl_obj.y,
            pl_obj.r);
    return 0;
}

/* game.set_intel_spot(team, x, y) / game.set_tent_spot(team, x, y): a mode
   with rules = "ctf" only; the dry ground nearest x, y, now and after every
   capture */
static int _api_spot(lua_State* L, int kind)
{
    if (!_me(L)->is_mode || !_me(L)->rules_ctf) {
        return luaL_error(L, "intels and tents need a game mode with rules = \"ctf\"");
    }
    int t = _team_index(L, 1);
    ctf_set_spot(pl_server, (uint8_t) t, kind, (float) luaL_checknumber(L, 2), (float) luaL_checknumber(L, 3));
    return 0;
}

static int api_set_intel_spot(lua_State* L)
{
    return _api_spot(L, MAP_META_FLAG);
}

static int api_set_tent_spot(lua_State* L)
{
    return _api_spot(L, MAP_META_BASE);
}

static int api_time(lua_State* L)
{
    lua_pushnumber(L, (lua_Number) get_nanos() / (lua_Number) NANO_IN_SECOND);
    return 1;
}

/* game.later(seconds, fn): fn() that many seconds of the game from now */
static int api_later(lua_State* L)
{
    plugin_t*  P    = _me(L);
    lua_Number secs = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    for (int k = 0; k < PLUGIN_TIMERS; k++) {
        if (P->timers[k].fn == LUA_NOREF) {
            lua_pushvalue(L, 2);
            P->timers[k].fn  = luaL_ref(L, LUA_REGISTRYINDEX);
            P->timers[k].due = get_nanos() + (uint64_t) (secs > 0 ? secs * (lua_Number) NANO_IN_SECOND : 0);
            return 0;
        }
    }
    return luaL_error(L, "too many timers (%d)", PLUGIN_TIMERS);
}

static int api_log(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    evt_log(NULL, "plugin", "action=log plugin=%s text=\"%s\"", _me(L)->name, text);
    LOG_INFO("Plugin %s: %s", _me(L)->name, text);
    return 0;
}

/* --- data: JSON, URLs; what plugins exchange and what web APIs speak --- */

#define PLUGIN_JSON_DEPTH 32

/* a Lua value as JSON: a table with keys 1..n only is an array, any other
   an object (keys as strings); functions and the like are refused */
static json_object* _to_json(lua_State* L, int idx, int depth)
{
    idx = lua_absindex(L, idx);
    if (depth > PLUGIN_JSON_DEPTH) {
        luaL_error(L, "json: nested too deep");
    }
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            return NULL;
        case LUA_TBOOLEAN:
            return json_object_new_boolean(lua_toboolean(L, idx));
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                return json_object_new_int64(lua_tointeger(L, idx));
            }
            return json_object_new_double(lua_tonumber(L, idx));
        case LUA_TSTRING:
            return json_object_new_string(lua_tostring(L, idx));
        case LUA_TTABLE: {
            lua_Integer n     = (lua_Integer) lua_rawlen(L, idx);
            int         array = 1;
            lua_Integer count = 0;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                count++;
                if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n) {
                    array = 0;
                }
                lua_pop(L, 1);
            }
            if (array && count == n && n > 0) {
                json_object* a = json_object_new_array();
                for (lua_Integer k = 1; k <= n; k++) {
                    lua_rawgeti(L, idx, k);
                    json_object_array_add(a, _to_json(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return a;
            }
            json_object* o = json_object_new_object();
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                lua_pushvalue(L, -2); /* lua_tostring would change the key in place */
                const char* key = lua_tostring(L, -1);
                if (key != NULL) {
                    json_object_object_add(o, key, _to_json(L, -2, depth + 1));
                }
                lua_pop(L, 2);
            }
            return o;
        }
        default:
            luaL_error(L, "json: a %s cannot be encoded", luaL_typename(L, idx));
            return NULL;
    }
}

static void _from_json(lua_State* L, json_object* j, int depth)
{
    if (depth > PLUGIN_JSON_DEPTH) {
        luaL_error(L, "json: nested too deep");
    }
    luaL_checkstack(L, 4, "json");
    switch (json_object_get_type(j)) {
        case json_type_null:
            lua_pushnil(L);
            break;
        case json_type_boolean:
            lua_pushboolean(L, json_object_get_boolean(j));
            break;
        case json_type_int:
            lua_pushinteger(L, json_object_get_int64(j));
            break;
        case json_type_double:
            lua_pushnumber(L, json_object_get_double(j));
            break;
        case json_type_string:
            lua_pushlstring(L, json_object_get_string(j), (size_t) json_object_get_string_len(j));
            break;
        case json_type_array: {
            size_t n = json_object_array_length(j);
            lua_createtable(L, (int) n, 0);
            for (size_t k = 0; k < n; k++) {
                _from_json(L, json_object_array_get_idx(j, k), depth + 1);
                lua_rawseti(L, -2, (lua_Integer) k + 1);
            }
            break;
        }
        case json_type_object: {
            lua_newtable(L);
            struct json_object_iterator it = json_object_iter_begin(j), end = json_object_iter_end(j);
            for (; !json_object_iter_equal(&it, &end); json_object_iter_next(&it)) {
                _from_json(L, json_object_iter_peek_value(&it), depth + 1);
                lua_setfield(L, -2, json_object_iter_peek_name(&it));
            }
            break;
        }
    }
}

/* game.json_encode(value) -> string */
static int api_json_encode(lua_State* L)
{
    luaL_checkany(L, 1);
    json_object* j = _to_json(L, 1, 0);
    lua_pushstring(L, j != NULL ? json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN) : "null");
    json_object_put(j);
    return 1;
}

/* game.json_decode(string) -> value, or nil and the error */
static int api_json_decode(lua_State* L)
{
    size_t                  len;
    const char*             text = luaL_checklstring(L, 1, &len);
    struct json_tokener*    tok  = json_tokener_new_ex(PLUGIN_JSON_DEPTH);
    json_object*            j    = json_tokener_parse_ex(tok, text, (int) len);
    enum json_tokener_error err  = json_tokener_get_error(tok);
    json_tokener_free(tok);
    if (err != json_tokener_success) {
        json_object_put(j);
        lua_pushnil(L);
        lua_pushstring(L, json_tokener_error_desc(err));
        return 2;
    }
    _from_json(L, j, 0);
    json_object_put(j);
    return 1;
}

/* game.url_encode(text): for a query string */
static int api_url_encode(lua_State* L)
{
    size_t      len;
    const char* s = luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t k = 0; k < len; k++) {
        unsigned char c = (unsigned char) s[k];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
        {
            luaL_addchar(&b, (char) c);
        } else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            luaL_addstring(&b, hex);
        }
    }
    luaL_pushresult(&b);
    return 1;
}

/* --- plugins talking to each other: game.send(topic, data) reaches every
   other plugin's on_message(from, topic, data) at the next plugin tick.
   Each plugin is a state of its own, so the data crosses as JSON (plain
   data only); queued, not called at once, so a reply cannot recurse --- */

#define PLUGIN_MESSAGES 256

static struct
{
    char  from[32];
    char  topic[64];
    char* json;
} pl_messages[PLUGIN_MESSAGES];
static int pl_message_count;

static int api_send(lua_State* L)
{
    const char* topic = luaL_checkstring(L, 1);
    if (pl_message_count >= PLUGIN_MESSAGES) {
        return luaL_error(L, "too many messages waiting (%d)", PLUGIN_MESSAGES);
    }
    json_object* j    = lua_isnoneornil(L, 2) ? NULL : _to_json(L, 2, 0);
    const char*  text = j != NULL ? json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN) : "null";
    int          k    = pl_message_count++;
    snprintf(pl_messages[k].from, sizeof pl_messages[k].from, "%s", _me(L)->name);
    snprintf(pl_messages[k].topic, sizeof pl_messages[k].topic, "%s", topic);
    pl_messages[k].json = strdup(text);
    json_object_put(j);
    return 0;
}

static void _deliver_messages(void)
{
    /* a copy: what is sent while these are delivered fills the queue again,
       for the next tick */
    static __typeof__(pl_messages) batch;
    int count = pl_message_count;
    memcpy(batch, pl_messages, sizeof(batch[0]) * (size_t) count);
    pl_message_count = 0;
    for (int m = 0; m < count; m++) {
        json_object* j = json_tokener_parse(batch[m].json);
        for (int k = 0; k < plugin_count; k++) {
            plugin_t* P = &plugins[k];
            if (!strcmp(P->name, batch[m].from) || !_event(P, "on_message")) {
                continue;
            }
            lua_pushstring(P->L, batch[m].from);
            lua_pushstring(P->L, batch[m].topic);
            if (j != NULL) {
                _from_json(P->L, j, 0);
            } else {
                lua_pushnil(P->L);
            }
            _call(P, "on_message", 3, 0);
        }
        json_object_put(j);
        free(batch[m].json);
    }
}

/* game.ban(player, minutes, reason): banned by its address (0 minutes:
   for good), through the ban list the /ban command writes */
static int api_ban(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    player_t* p = _player_by_id(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_Number  minutes = luaL_optnumber(L, 2, 0);
    const char* reason  = luaL_optstring(L, 3, "");
    if (p == NULL) {
        return luaL_error(L, "that player has left");
    }
    if (p->conn == NULL) {
        return luaL_error(L, "a bot cannot be banned"); /* bots share no address: every bot would go */
    }
    char text[256];
    snprintf(text, sizeof text, " %s", *reason ? reason : "banned by a plugin");
    command_args_t args = {0};
    args.server         = pl_server;
    args.console        = 1;
    float until = minutes > 0 ? (float) ((long double) get_nanos() / (uint64_t) NANO_IN_MINUTE + minutes) : 0.0f;
    evt_log(p, "plugin", "action=ban plugin=%s minutes=%.0f reason=\"%s\"", _me(L)->name, (double) minutes, reason);
    char prefix[64];
    cmd_generate_ban(pl_server, args, until, p->ip, conn_player_prefix(p, prefix, sizeof prefix), text);
    return 0;
}

/* game.server_info(): name, map, mode, players, max_players, port */
static int api_server_info(lua_State* L)
{
    lua_createtable(L, 0, 6);
    lua_pushstring(L, pl_server->server_name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, pl_server->map_name);
    lua_setfield(L, -2, "map");
    lua_pushstring(L, gamemode_current(pl_server)->name);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, pl_server->protocol.num_players);
    lua_setfield(L, -2, "players");
    lua_pushinteger(L, pl_server->protocol.max_players);
    lua_setfield(L, -2, "max_players");
    lua_pushinteger(L, pl_server->port);
    lua_setfield(L, -2, "port");
    return 1;
}

/* --- the network: game.http. The server's owner lists the hosts each
   plugin may reach ([plugins.<name>] allow_hosts = ["api.example.com",
   "*.example.org"]; "*" any); none listed, no network. The request runs on
   PluginNet.c's thread and on_done(status, body, err) is called on a plugin
   tick after it ends --- */

static struct
{
    uint32_t id;
    int      slot;
    unsigned gen;
    int      fn; /* LUA_NOREF: nobody waits for it */
    char     host[96];
} pl_http[PLUGIN_HTTP];
static int pl_http_count;

/* host (lower case) against one allow_hosts entry; a port in the entry is
   for game.tcp and ignored here */
static int _host_matches(const char* pattern, const char* host)
{
    char p[96];
    snprintf(p, sizeof p, "%s", pattern);
    for (char* c = p; *c; c++) {
        *c = (char) tolower((unsigned char) *c);
    }
    char* colon = strrchr(p, ':');
    if (colon != NULL) {
        *colon = '\0';
    }
    if (!strcmp(p, "*")) {
        return 1;
    }
    if (p[0] == '*' && p[1] == '.') {
        size_t hl = strlen(host), sl = strlen(p + 1); /* ".example.org" */
        return hl > sl && !strcmp(host + hl - sl, p + 1);
    }
    return !strcmp(p, host);
}

static int _host_allowed(plugin_t* P, const char* host)
{
    for (int k = 0; k < P->host_count; k++) {
        if (_host_matches(P->hosts[k], host)) {
            return 1;
        }
    }
    return 0;
}

/* game.http{url, method, headers, body | json, timeout, on_done}: true, or
   nil and why it was refused */
static int api_http(lua_State* L)
{
    plugin_t* P = _me(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "url");
    const char* url = luaL_checkstring(L, -1);
    char        host[96];
    const char* why = NULL;
    if (!plugin_net_host(url, host, sizeof host)) {
        why = "the url must be http:// or https://";
    } else if (!_host_allowed(P, host)) {
        why = "host not in this plugin's allow_hosts (config.toml)";
    } else if (P->http_waiting >= PLUGIN_HTTP_EACH || pl_http_count >= PLUGIN_HTTP) {
        why = "too many requests waiting";
    }
    if (why != NULL) {
        evt_log(NULL, "plugin", "action=http_refused plugin=%s host=%s why=\"%s\"", P->name,
                plugin_net_host(url, host, sizeof host) ? host : "-", why);
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    lua_getfield(L, 1, "method");
    const char* method = luaL_optstring(L, -1, "GET");
    lua_getfield(L, 1, "timeout");
    int timeout = (int) luaL_optinteger(L, -1, 10);
    timeout     = timeout < 1 ? 1 : timeout > 30 ? 30 : timeout;

    /* the body: a string, or json = value (and its Content-Type) */
    const char*  body     = NULL;
    size_t       body_len = 0;
    json_object* j        = NULL;
    lua_getfield(L, 1, "json");
    if (!lua_isnil(L, -1)) {
        j    = _to_json(L, -1, 0);
        body = json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN);
        body_len = strlen(body);
    } else {
        lua_getfield(L, 1, "body");
        if (!lua_isnil(L, -1)) {
            body = luaL_checklstring(L, -1, &body_len);
        }
    }
    const char* headers[32];
    char        lines[32][256];
    int         n = 0;
    if (j != NULL) {
        snprintf(lines[n], sizeof lines[n], "Content-Type: application/json");
        headers[n] = lines[n];
        n++;
    }
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0 && n < 32) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
                const char* name  = lua_tostring(L, -2);
                const char* value = lua_tostring(L, -1);
                /* a header is one line: a line break would add another */
                if (!strpbrk(name, "\r\n:") && !strpbrk(value, "\r\n")) {
                    snprintf(lines[n], sizeof lines[n], "%s: %s", name, value);
                    headers[n] = lines[n];
                    n++;
                }
            }
            lua_pop(L, 1);
        }
    }
    uint32_t id = plugin_net_http(method, url, headers, n, body, body_len, timeout);
    json_object_put(j);
    if (id == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "too many requests waiting");
        return 2;
    }
    int k             = pl_http_count++;
    pl_http[k].id     = id;
    pl_http[k].slot   = (int) (P - plugins);
    pl_http[k].gen    = P->gen;
    snprintf(pl_http[k].host, sizeof pl_http[k].host, "%s", host);
    lua_getfield(L, 1, "on_done");
    if (lua_isfunction(L, -1)) {
        pl_http[k].fn = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        pl_http[k].fn = LUA_NOREF;
    }
    P->http_waiting++;
    evt_log(NULL, "plugin", "action=http plugin=%s id=%u method=%s host=%s bytes=%zu", P->name, id, method, host,
            body_len);
    lua_pushboolean(L, 1);
    return 1;
}

/* finished requests to their callbacks (a plugin tick) */
static void _deliver_http(void)
{
    plugin_net_result_t r;
    while (plugin_net_done(&r)) {
        int k = 0;
        while (k < pl_http_count && pl_http[k].id != r.id) {
            k++;
        }
        if (k == pl_http_count) {
            free(r.body);
            continue;
        }
        int      slot = pl_http[k].slot, fn = pl_http[k].fn;
        unsigned gen  = pl_http[k].gen;
        evt_log(NULL, "plugin", "action=http_done id=%u host=%s status=%ld bytes=%zu error=\"%s\"", r.id,
                pl_http[k].host, r.status, r.body_len, r.error);
        pl_http[k] = pl_http[--pl_http_count];
        plugin_t* P = &plugins[slot];
        /* the plugin was loaded again since (a new map): its callback is gone */
        if (P->L != NULL && P->gen == gen) {
            P->http_waiting--;
            if (fn != LUA_NOREF && !P->broken) {
                lua_rawgeti(P->L, LUA_REGISTRYINDEX, fn);
                luaL_unref(P->L, LUA_REGISTRYINDEX, fn);
                lua_pushinteger(P->L, r.status);
                if (r.body != NULL) {
                    lua_pushlstring(P->L, r.body, r.body_len);
                } else {
                    lua_pushnil(P->L);
                }
                if (r.error[0]) {
                    lua_pushstring(P->L, r.error);
                } else {
                    lua_pushnil(P->L);
                }
                _call(P, "on_done", 3, 0);
            }
        }
        free(r.body);
    }
}

/* a class's figures as a table: its name, its number n, and every key of
   game_class_set typed (numbers, booleans, words) */
static void _push_class(lua_State* L, int k)
{
    const struct game_class_info* c = game_class_get(k);
    lua_newtable(L);
    lua_pushstring(L, c->name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, k);
    lua_setfield(L, -2, "n");
    for (int i = 0; i < GAME_CLASS_KEYS; i++) {
        const char* key = game_class_key(i);
        char        v[128];
        game_class_value(c, key, v, sizeof v);
        if (!strcmp(key, "jetpack") || !strcmp(key, "enabled")) {
            lua_pushboolean(L, v[0] == '1');
        } else if (!strcmp(key, "weapon") || !strcmp(key, "gear") || !strcmp(key, "perk") || !strcmp(key, "title") ||
                   !strcmp(key, "blurb"))
        {
            lua_pushstring(L, v);
        } else {
            lua_pushnumber(L, strtod(v, NULL));
        }
        lua_setfield(L, -2, key);
    }
}

/* game.classes(): every class's figures, in order (Commando, Rocketeer,
   Marksman, Miner) */
static int api_classes(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_CLASSES, "game.classes", 1)) {
        return 2;
    }
    lua_newtable(L);
    for (int k = 0; k < GAME_CLASSES_COUNT; k++) {
        _push_class(L, k);
        lua_rawseti(L, -2, k + 1);
    }
    return 1;
}

/* game.class(name or n): one class's figures, nil for none */
static int api_class(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_CLASS, "game.class", 1)) {
        return 2;
    }
    int k = _class_arg(L, 1);
    if (k < 0) {
        lua_pushnil(L);
    } else {
        _push_class(L, k);
    }
    return 1;
}

/* game.set_class(name or n, {health = 150, speed = 1.2, ...}): changes
   those figures for this map (every map starts from config.toml's) and
   tells every client. true, or false and the first key refused (unknown,
   out of its range, or the last class enabled turned off); the keys before
   it are applied */
static int api_set_class(lua_State* L)
{
    if (_refuse_10_only(L, REFUSED_SET_CLASS, "game.set_class", 0)) {
        return 2;
    }
    int k = _class_arg(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (k < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "class");
        return 2;
    }
    char by[64];
    snprintf(by, sizeof by, "plugin %s", _me(L)->name);
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        char        v[128];
        if (lua_type(L, -1) == LUA_TBOOLEAN) {
            snprintf(v, sizeof v, "%d", lua_toboolean(L, -1));
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            snprintf(v, sizeof v, "%.6g", lua_tonumber(L, -1));
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            snprintf(v, sizeof v, "%s", lua_tostring(L, -1));
        } else {
            v[0] = '\0';
        }
        if (key == NULL || !classes_set(pl_server, k, key, v, by)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, key ? key : "?");
            return 2;
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* --- game.store: a table of the plugin's own kept across maps and restarts
   as Resources/plugins/data/<name>.json; the plugin itself has no file
   access, the server reads it at load and writes it --- */

/* the stores' folder: under the plugins, unless the server was given a data
   folder of its own (--data: a root inside a read-only .app cannot hold
   them) */
static const char* _store_dir(void)
{
    static char dir[1100];
    if (*data_dir() == '\0') {
        return PLUGIN_DATA_DIR;
    }
    return data_path(dir, sizeof dir, "plugins-data");
}

static void _store_path(const plugin_t* P, char* out, size_t size, const char* suffix)
{
    snprintf(out, size, "%s/%s.json%s", _store_dir(), P->name, suffix);
}

/* game.store = the decoded file (the json_object, argument 1), in a
   protected call: a file nested too deep raises */
static int _store_push(lua_State* L)
{
    json_object* j = (json_object*) lua_touserdata(L, 1);
    lua_getglobal(L, "game");
    if (j != NULL) {
        _from_json(L, j, 0);
    } else {
        lua_newtable(L);
    }
    lua_setfield(L, -2, "store");
    return 0;
}

static void _store_load(plugin_t* P)
{
    char        path[128];
    struct stat st;
    _store_path(P, path, sizeof path, "");
    int          exists = stat(path, &st) == 0;
    json_object* j      = exists ? json_object_from_file(path) : NULL;
    if (exists && (j == NULL || !json_object_is_type(j, json_type_object))) {
        /* kept aside, not overwritten by the next save: the owner may mend it */
        char bad[140];
        _store_path(P, bad, sizeof bad, ".bad");
        remove(bad);
        rename(path, bad);
        LOG_ERROR("Plugin %s: %s is not a JSON object, moved to %s; game.store starts empty", P->name, path, bad);
        evt_log(NULL, "plugin", "action=store_bad plugin=%s", P->name);
        json_object_put(j);
        j = NULL;
    }
    lua_pushcfunction(P->L, _store_push);
    lua_pushlightuserdata(P->L, j);
    P->steps = 0;
    if (lua_pcall(P->L, 1, 0, 0) != LUA_OK) {
        LOG_ERROR("Plugin %s: %s could not be read: %s", P->name, path, lua_tostring(P->L, -1));
        lua_pop(P->L, 1);
        lua_pushcfunction(P->L, _store_push);
        lua_pushlightuserdata(P->L, NULL);
        lua_pcall(P->L, 1, 0, 0);
    }
    json_object_put(j);
}

/* pushes game.store as JSON text; raises on what JSON cannot hold */
static int _store_encode(lua_State* L)
{
    lua_getglobal(L, "game");
    lua_getfield(L, -1, "store");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "game.store is not a table");
    }
    json_object* j = _to_json(L, -1, 0);
    lua_pushstring(L, json_object_to_json_string_ext(j, JSON_C_TO_STRING_PRETTY));
    json_object_put(j);
    return 1;
}

/* written to a temporary file and renamed over the old one, so a crash
   mid-write never leaves half a file in its place */
static void _store_save(plugin_t* P, const char* why)
{
    if (P->L == NULL) {
        return;
    }
    P->store_dirty = 0;
    lua_State* L   = P->L;
    P->steps       = 0;
    lua_pushcfunction(L, _store_encode);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        LOG_ERROR("Plugin %s: game.store not saved: %s", P->name, lua_tostring(L, -1));
        evt_log(NULL, "plugin", "action=store_refused plugin=%s why=encode msg=\"%s\"", P->name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    size_t      len;
    const char* text = lua_tolstring(L, -1, &len);
    char        path[128], tmp[140];
    struct stat st;
    _store_path(P, path, sizeof path, "");
    _store_path(P, tmp, sizeof tmp, ".tmp");
    if (len > PLUGIN_STORE_MAX) {
        LOG_ERROR("Plugin %s: game.store not saved: %zu bytes, more than %d", P->name, len, PLUGIN_STORE_MAX);
        evt_log(NULL, "plugin", "action=store_refused plugin=%s why=size bytes=%zu", P->name, len);
        lua_pop(L, 1);
        return;
    }
    /* a plugin that never stored anything leaves no file behind */
    if (!strcmp(text, "{ }") || !strcmp(text, "{}")) {
        if (stat(path, &st) != 0) {
            lua_pop(L, 1);
            return;
        }
    }
#ifdef _WIN32
    _mkdir(_store_dir());
#else
    mkdir(_store_dir(), 0755);
#endif
    FILE* f  = fopen(tmp, "wb");
    int   ok = f != NULL && fwrite(text, 1, len, f) == len;
    if (f != NULL) {
        ok = fclose(f) == 0 && ok;
    }
#ifdef _WIN32
    if (ok) {
        remove(path); /* Windows' rename does not replace a file */
    }
#endif
    ok = ok && rename(tmp, path) == 0;
    if (!ok) {
        remove(tmp);
        LOG_ERROR("Plugin %s: could not write %s", P->name, path);
    }
    evt_log(NULL, "plugin", "action=store_save plugin=%s bytes=%zu why=%s ok=%d", P->name, len, why, ok);
    lua_pop(L, 1);
}

/* game.store_save(): game.store written on the next plugin tick (the map's
   end and the server's shutdown write it anyway) */
static int api_store_save(lua_State* L)
{
    _me(L)->store_dirty = 1;
    return 0;
}

/* --- game.command(name, [opts], fn): a chat command typed with "/" ---
   The server's own commands come first: a name it has is refused, as is
   one another plugin took. opts.role (trusted, guard, mod, admin, manager)
   is checked by the server like its own commands' permissions; opts.help
   is listed by /help. fn(p, args, line): p nil from the console, args the
   words after the name, line the rest as typed; a string returned goes
   back to whoever typed it. */

static const struct
{
    const char* role;
    int         offset; /* its bit, Player.c's role list */
} pl_roles[] = {{"trusted", 0}, {"guard", 1}, {"mod", 2}, {"admin", 3}, {"manager", 4}};

static int _command_taken(const char* name)
{
    command_t* cmd;
    HASH_FIND_STR(pl_server->cmds_map, name, cmd);
    if (cmd != NULL) {
        return 1;
    }
    for (int k = 0; k < PLUGIN_MAX; k++) {
        for (int c = 0; c < plugins[k].command_count; c++) {
            if (!strcmp(plugins[k].commands[c].name, name)) {
                return 1;
            }
        }
    }
    return 0;
}

static int api_command(lua_State* L)
{
    plugin_t*   P     = _me(L);
    const char* given = luaL_checkstring(L, 1);
    int         fn    = lua_isfunction(L, 2) ? 2 : 3;
    luaL_checktype(L, fn, LUA_TFUNCTION);
    if (*given == '/') {
        given++;
    }
    size_t n = strlen(given);
    if (n == 0 || n > 30) {
        return luaL_error(L, "game.command: a name of 1 to 30 characters");
    }
    for (const char* c = given; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) {
            return luaL_error(L, "game.command: '%s' (lowercase letters, digits, _ and -)", given);
        }
    }
    uint32_t    permissions = 0;
    const char* help        = "";
    if (fn == 3 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "role");
        if (!lua_isnil(L, -1)) {
            const char* role  = luaL_checkstring(L, -1);
            int         found = -1;
            for (size_t r = 0; r < sizeof pl_roles / sizeof pl_roles[0]; r++) {
                if (!strcmp(pl_roles[r].role, role)) {
                    found = pl_roles[r].offset;
                }
            }
            if (found < 0) {
                return luaL_error(L, "game.command: no role '%s'", role);
            }
            /* that role or a higher one: the bits from its offset up */
            permissions = 31u & ~((1u << found) - 1u);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "help");
        if (lua_isstring(L, -1)) {
            help = lua_tostring(L, -1);
        }
    }
    char name[32];
    snprintf(name, sizeof name, "/%s", given);
    if (_command_taken(name)) {
        evt_log(NULL, "plugin", "action=command_refused plugin=%s command=%s why=taken", P->name, name);
        LOG_WARNING("Plugin %s: the command %s exists already", P->name, name);
        lua_pushboolean(L, 0);
        return 1;
    }
    if (P->command_count >= PLUGIN_COMMANDS) {
        return luaL_error(L, "game.command: more than %d commands", PLUGIN_COMMANDS);
    }
    int k = P->command_count++;
    snprintf(P->commands[k].name, sizeof P->commands[k].name, "%s", name);
    snprintf(P->commands[k].help, sizeof P->commands[k].help, "%s", help);
    P->commands[k].permissions = permissions;
    lua_pushvalue(L, fn);
    P->commands[k].fn = luaL_ref(L, LUA_REGISTRYINDEX);
    evt_log(NULL, "plugin", "action=command plugin=%s command=%s permissions=%u", P->name, name, permissions);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg game_api[] = {
    {"command", api_command},
    {"color", api_color},
    {"set_block", api_set_block},
    {"clear_block", api_clear_block},
    {"unix_time", api_unix_time},
    {"store_save", api_store_save},
    {"classes", api_classes},
    {"class", api_class},
    {"set_class", api_set_class},
    {"supported_profiles", api_supported_profiles},
    {"players", api_players},
    {"player", api_player},
    {"find", api_find},
    {"score", api_score},
    {"set_score", api_set_score},
    {"score_limit", api_score_limit},
    {"set_score_limit", api_set_score_limit},
    {"set_capture_limit", api_set_capture_limit},
    {"set_time_limit", api_set_time_limit},
    {"time_left", api_time_left},
    {"win", api_win},
    {"end_match", api_end_match},
    {"next_map", api_next_map},
    {"set_map", api_set_map},
    {"set_mode", api_set_mode},
    {"run_command", api_run_command},
    {"team_name", api_team_name},
    {"team_color", api_team_color},
    {"notice", api_notice},
    {"fog", api_fog},
    {"drop_crate", api_drop_crate},
    {"get_fog", api_get_fog},
    {"sky", api_sky},
    {"set_team_outfit", api_set_team_outfit},
    {"set_sky", api_set_sky},
    {"cell", api_cell},
    {"time", api_time},
    {"later", api_later},
    {"log", api_log},
    {"map_size", api_map_size},
    {"ground", api_ground},
    {"spawn", api_spawn},
    {"objective", api_objective},
    {"set_intel_spot", api_set_intel_spot},
    {"set_tent_spot", api_set_tent_spot},
    {"json_encode", api_json_encode},
    {"json_decode", api_json_decode},
    {"url_encode", api_url_encode},
    {"send", api_send},
    {"ban", api_ban},
    {"server_info", api_server_info},
    {"http", api_http},
    {NULL, NULL},
};

/* --- loading --- */

static int _valid_name(const char* name)
{
    if (name == NULL || !*name || strlen(name) >= sizeof(plugins[0].name)) {
        return 0;
    }
    for (const char* c = name; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) {
            return 0; /* a name, never a path */
        }
    }
    return 1;
}

static void _push_toml_table(lua_State* L, const toml_table_t* t);

static void _push_toml_array(lua_State* L, const toml_array_t* a)
{
    int n = toml_array_nelem(a);
    lua_createtable(L, n, 0);
    for (int k = 0; k < n; k++) {
        toml_datum_t d;
        if ((d = toml_string_at(a, k)).ok) {
            lua_pushstring(L, d.u.s);
            free(d.u.s);
        } else if ((d = toml_int_at(a, k)).ok) {
            lua_pushinteger(L, d.u.i);
        } else if ((d = toml_double_at(a, k)).ok) {
            lua_pushnumber(L, d.u.d);
        } else if ((d = toml_bool_at(a, k)).ok) {
            lua_pushboolean(L, d.u.b);
        } else if (toml_table_at(a, k) != NULL) {
            _push_toml_table(L, toml_table_at(a, k));
        } else if (toml_array_at(a, k) != NULL) {
            _push_toml_array(L, toml_array_at(a, k));
        } else {
            lua_pushnil(L);
        }
        lua_rawseti(L, -2, k + 1);
    }
}

/* a [plugins.<name>] table as the plugin's `config` */
static void _push_toml_table(lua_State* L, const toml_table_t* t)
{
    lua_newtable(L);
    if (t == NULL) {
        return;
    }
    const char* key;
    for (int k = 0; (key = toml_key_in(t, k)) != NULL; k++) {
        toml_datum_t d;
        if ((d = toml_string_in(t, key)).ok) {
            lua_pushstring(L, d.u.s);
            free(d.u.s);
        } else if ((d = toml_int_in(t, key)).ok) {
            lua_pushinteger(L, d.u.i);
        } else if ((d = toml_double_in(t, key)).ok) {
            lua_pushnumber(L, d.u.d);
        } else if ((d = toml_bool_in(t, key)).ok) {
            lua_pushboolean(L, d.u.b);
        } else if (toml_table_in(t, key) != NULL) {
            _push_toml_table(L, toml_table_in(t, key));
        } else if (toml_array_in(t, key) != NULL) {
            _push_toml_array(L, toml_array_in(t, key));
        } else {
            continue;
        }
        lua_setfield(L, -2, key);
    }
}

static void _plugin_close(plugin_t* P)
{
    if (P->L != NULL) {
        lua_close(P->L);
    }
    memset(P, 0, sizeof(*P));
    P->ref = LUA_NOREF;
}

/* a fresh state with the plugin loaded into the next slot; NULL when it
   does not load */
/* a plugin's script: Resources/plugins/<name>.lua, or SP_PLUGINS_DIR's
   (tests load their own: server/tools/bottest) */
static void _script_path(const char* name, char* out, size_t size)
{
    const char* dir = getenv("SP_PLUGINS_DIR");
    snprintf(out, size, "%s/%s.lua", dir != NULL && *dir ? dir : PLUGIN_DIR, name);
}

static plugin_t* _load(const char* name, const toml_table_t* config, uint8_t is_mode)
{
    if (plugin_count >= PLUGIN_MAX) {
        LOG_ERROR("Plugin %s: more than %d plugins, not loaded", name, PLUGIN_MAX);
        return NULL;
    }
    plugin_t* P = &plugins[plugin_count];
    _plugin_close(P);
    snprintf(P->name, sizeof(P->name), "%s", name);
    P->is_mode = is_mode;
    P->gen     = ++pl_gen;
    for (int k = 0; k < PLUGIN_TIMERS; k++) {
        P->timers[k].fn = LUA_NOREF;
    }
    /* the operator's restriction is read before the chunk runs: a plugin
       turned off for this profile never runs at all */
    if (!_config_allows_profile(name, config)) {
        _log_profile_refused(name, "config");
        _plugin_close(P);
        return NULL;
    }
    toml_array_t* hosts = config != NULL ? toml_array_in(config, "allow_hosts") : NULL;
    for (int k = 0; hosts != NULL && k < toml_array_nelem(hosts) && P->host_count < PLUGIN_HOSTS; k++) {
        toml_datum_t d = toml_string_at(hosts, k);
        if (d.ok) {
            snprintf(P->hosts[P->host_count++], sizeof(P->hosts[0]), "%s", d.u.s);
            free(d.u.s);
        }
    }
    char path[1100];
    _script_path(name, path, sizeof(path));
    lua_State* L                       = luaL_newstate();
    *(plugin_t**) lua_getextraspace(L) = P;
    P->L                               = L;
    /* the sandbox: no io, os, package, debug; no loading other code */
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 4);
    static const char* removed[] = {"dofile", "loadfile", "load", "require", "collectgarbage"};
    for (size_t k = 0; k < sizeof(removed) / sizeof(removed[0]); k++) {
        lua_pushnil(L);
        lua_setglobal(L, removed[k]);
    }
    luaL_newmetatable(L, "player");
    luaL_newlib(L, player_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newlib(L, game_api);
    lua_setglobal(L, "game");
    _store_load(P);
    _push_toml_table(L, config);
    lua_setglobal(L, "config");
    lua_sethook(L, _hook, LUA_MASKCOUNT, 100000);
    P->loading = 1;
    int loaded = luaL_loadfile(L, path) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK;
    P->loading = 0;
    if (P->profile_refused) {
        /* game.supported_profiles stopped the chunk (or was caught by the
           plugin's own pcall): a refusal, not an error */
        _log_profile_refused(name, "script");
        _plugin_close(P);
        return NULL;
    }
    if (!loaded) {
        _fail(P, "load");
        _plugin_close(P);
        return NULL;
    }
    if (!lua_istable(L, -1)) {
        LOG_ERROR("Plugin %s: the script must return a table", name);
        _plugin_close(P);
        return NULL;
    }
    lua_getfield(L, -1, "mode");
    int declares_mode = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (is_mode && !declares_mode) {
        LOG_ERROR("Plugin %s is not a game mode (no mode = true)", name);
        _plugin_close(P);
        return NULL;
    }
    /* the class event is never asked in the 0.75 profile (no class is
       picked there): a plugin that listens for it is told once */
    if (profile_is_075()) {
        lua_getfield(L, -1, "on_class");
        if (lua_isfunction(L, -1)) {
            _log_api_refused(P, REFUSED_ON_CLASS, "on_class");
        }
        lua_pop(L, 1);
    }
    lua_getfield(L, -1, "rules");
    P->rules_ctf = is_mode && lua_isstring(L, -1) && !strcmp(lua_tostring(L, -1), "ctf");
    lua_pop(L, 1);
    P->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_count++;
    LOG_INFO("Plugin %s loaded from %s%s", name, path, is_mode ? " as the game mode" : "");
    evt_log(NULL, "plugin", "action=load plugin=%s mode=%u", name, is_mode);
    return P;
}

int plugins_exists(const char* name)
{
    char        path[1100];
    struct stat st;
    if (!_valid_name(name)) {
        return 0;
    }
    _script_path(name, path, sizeof(path));
    return stat(path, &st) == 0;
}

void plugins_names(char* out, size_t size)
{
    size_t at = 0;
    if (size == 0) {
        return;
    }
    *out = 0;
    for (int k = 0; k < plugin_count; k++) {
        if (plugins[k].broken) {
            continue;
        }
        int n = snprintf(out + at, size - at, "%s%s", at ? "," : "", plugins[k].name);
        if (n < 0 || (size_t) n >= size - at) {
            break;
        }
        at += (size_t) n;
    }
}

void plugins_map_start(server_t* server)
{
    pl_server = server;
    for (int k = 0; k < PLUGIN_MAX; k++) {
        _store_save(&plugins[k], "map");
        _plugin_close(&plugins[k]);
    }
    plugin_count = 0;
    pl_obj.set   = 0; /* a new map: the mode sets it again */
    for (int m = 0; m < pl_message_count; m++) {
        free(pl_messages[m].json);
    }
    pl_message_count = 0;

    toml_table_t* root = NULL;
    FILE*         fp   = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char err[200];
        root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
    }
    toml_table_t* section = root != NULL ? toml_table_in(root, "plugins") : NULL;
    /* a preset may name its own plugin list; its [plugins.<name>] settings
       stay the global ones */
    toml_table_t* presets = root != NULL ? toml_table_in(root, "presets") : NULL;
    toml_table_t* preset  = presets != NULL && server->preset_name[0] != '\0'
                           ? toml_table_in(presets, server->preset_name)
                           : NULL;
    /* the dangerous APIs are opt-in: a plugin loaded from the community
       must not change the map or run console commands by itself */
    pl_pending.allow_map           = 0;
    pl_pending.allow_commands_count = 0;
    if (section != NULL) {
        toml_datum_t allow = toml_bool_in(section, "allow_map_change");
        if (allow.ok) {
            pl_pending.allow_map = allow.u.b ? 1 : 0;
        }
        toml_array_t* commands = toml_array_in(section, "allow_commands");
        for (int k = 0; commands != NULL && k < toml_array_nelem(commands) && pl_pending.allow_commands_count < 16; k++) {
            toml_datum_t d = toml_string_at(commands, k);
            if (d.ok) {
                snprintf(pl_pending.allow_commands[pl_pending.allow_commands_count++], 32, "%s", d.u.s);
                free(d.u.s);
            }
        }
    }
    const char* allow_env = getenv("SP_PLUGIN_ALLOW_MAP");
    if (allow_env != NULL && *allow_env != '\0') {
        pl_pending.allow_map = atoi(allow_env) != 0;
    }
    const char* command_env = getenv("SP_PLUGIN_COMMANDS");
    if (command_env != NULL && *command_env != '\0') {
        char  cmds[256];
        snprintf(cmds, sizeof cmds, "%s", command_env);
        char* csave = NULL;
        for (char* c = strtok_r(cmds, ", ", &csave); c != NULL && pl_pending.allow_commands_count < 16;
             c = strtok_r(NULL, ", ", &csave))
        {
            snprintf(pl_pending.allow_commands[pl_pending.allow_commands_count++], 32, "%s", c);
        }
    }
    LOG_INFO("Plugin options: map change %s, %d allowed command(s)", pl_pending.allow_map ? "on" : "off",
             pl_pending.allow_commands_count);
    /* a map start is not a queued request any more */
    pl_pending.map[0] = '\0';
    pl_pending.mode   = -1;
    pl_pending.restart = 0;
    pl_pending.win     = 0;
    pl_pending.command[0] = '\0';

    toml_array_t* load    = preset != NULL ? toml_array_in(preset, "plugins")
                                           : (section != NULL ? toml_array_in(section, "load") : NULL);
    /* SP_PLUGINS=a,b replaces the list (tests) */
    char        list[512] = "";
    const char* env       = getenv("SP_PLUGINS");
    if (env != NULL) {
        snprintf(list, sizeof list, "%s", env);
    } else if (load != NULL) {
        for (int k = 0; k < toml_array_nelem(load); k++) {
            toml_datum_t d = toml_string_at(load, k);
            if (d.ok) {
                size_t used = strlen(list);
                snprintf(list + used, sizeof list - used, "%s%s", used ? "," : "", d.u.s);
                free(d.u.s);
            }
        }
    }
    char* save = NULL;
    for (char* name = strtok_r(list, ", ", &save); name != NULL; name = strtok_r(NULL, ", ", &save)) {
        if (!plugins_exists(name)) {
            LOG_ERROR("Plugin %s: no " PLUGIN_DIR "/%s.lua (get it from github.com/ngspades/plugins)", name,
                      name);
            evt_log(NULL, "plugin", "action=missing plugin=%s", name);
            continue;
        }
        _load(name, section != NULL ? toml_table_in(section, name) : NULL, 0);
    }
    if (root != NULL) {
        toml_free(root);
    }
}

void plugins_map_ready(server_t* server)
{
    pl_server = server;
    for (int k = 0; k < plugin_count; k++) {
        plugin_t* P = &plugins[k];
        if (!P->is_mode && _event(P, "on_init")) {
            _call(P, "on_init", 0, 0);
        }
    }
}

plugin_t* plugins_load_mode(server_t* server, const char* name)
{
    pl_server              = server;
    toml_table_t* root     = NULL;
    toml_table_t* settings = NULL;
    FILE*         fp       = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char err[200];
        root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* section = root != NULL ? toml_table_in(root, "plugins") : NULL;
        settings              = section != NULL ? toml_table_in(section, name) : NULL;
    }
    plugin_t* P = _valid_name(name) ? _load(name, settings, 1) : NULL;
    if (root != NULL) {
        toml_free(root);
    }
    return P;
}

const char* plugins_mode_field(plugin_t* P, const char* field, char* buf, size_t size)
{
    if (P == NULL || P->L == NULL) {
        return NULL;
    }
    lua_rawgeti(P->L, LUA_REGISTRYINDEX, P->ref);
    lua_getfield(P->L, -1, field);
    const char* out = NULL;
    if (lua_type(P->L, -1) == LUA_TSTRING) {
        snprintf(buf, size, "%s", lua_tostring(P->L, -1));
        out = buf;
    }
    lua_pop(P->L, 2);
    return out;
}

int plugins_mode_number(plugin_t* P, const char* field, double* out)
{
    if (P == NULL || P->L == NULL) {
        return 0;
    }
    lua_rawgeti(P->L, LUA_REGISTRYINDEX, P->ref);
    lua_getfield(P->L, -1, field);
    int ok = lua_type(P->L, -1) == LUA_TNUMBER;
    if (ok) {
        *out = (double) lua_tonumber(P->L, -1);
    }
    lua_pop(P->L, 2);
    return ok;
}

void plugins_mode_init(plugin_t* P)
{
    if (P != NULL && _event(P, "on_init")) {
        _call(P, "on_init", 0, 0);
    }
}

int plugins_mode_objective(float* x, float* y, float* r)
{
    if (!pl_obj.set) {
        return 0;
    }
    for (int k = 0; k < plugin_count; k++) {
        if (plugins[k].is_mode && plugins[k].broken) {
            return 0;
        }
    }
    *x = pl_obj.x;
    *y = pl_obj.y;
    *r = pl_obj.r;
    return 1;
}

/* apply what a plugin queued after every Lua call has returned: a map
   reload closes all plugin states, which must never happen under a running
   Lua function */
static void _apply_pending(server_t* server)
{
    if (pl_pending.command[0] != '\0') {
        char line[sizeof pl_pending.command];
        snprintf(line, sizeof line, "%s", pl_pending.command);
        pl_pending.command[0] = '\0';
        command_handle(server, NULL, line, 1);
    }
    if (pl_pending.win || pl_pending.restart) {
        uint64_t now = get_nanos();
        if (pl_reset_at != 0 && now - pl_reset_at < 5ULL * NANO_IN_SECOND) {
            pl_pending.win = 0;
            pl_pending.restart = 0;
            pl_pending.mode = -1;
            pl_pending.map[0] = '\0';
            evt_log(NULL, "plugin", "action=reset_refused why=rate");
            return;
        }
        pl_reset_at = now;
    }
    if (pl_pending.win) {
        pl_pending.win = 0;
        pl_pending.restart = 0;
        pl_pending.mode = -1;
        gamemode_match_won(server);
        return;
    }
    if (pl_pending.restart) {
        if (pl_pending.map[0] != '\0') {
            snprintf(server->forced_map, sizeof server->forced_map, "%s", pl_pending.map);
            pl_pending.map[0] = '\0';
        }
        if (pl_pending.mode >= 0) {
            server->forced_gamemode = pl_pending.mode;
        }
        pl_pending.restart = 0;
        pl_pending.mode    = -1;
        server_reset(server);
    }
}

static void _bot_team_tick(plugin_t* P, uint64_t now);

void plugins_tick(server_t* server)
{
    pl_server = server;
    if (plugin_count == 0 || ++plugin_ticks % PLUGIN_TICK_EVERY != 0) {
        return;
    }
    _deliver_messages();
    _deliver_http();
    uint64_t now = get_nanos();
    for (int i = 0; i < plugin_count; i++) {
        plugin_t* P = &plugins[i];
        for (int k = 0; k < PLUGIN_TIMERS && P->L != NULL && !P->broken; k++) {
            if (P->timers[k].fn != LUA_NOREF && now >= P->timers[k].due) {
                int fn          = P->timers[k].fn;
                P->timers[k].fn = LUA_NOREF;
                lua_rawgeti(P->L, LUA_REGISTRYINDEX, fn);
                luaL_unref(P->L, LUA_REGISTRYINDEX, fn);
                _call(P, "a timer", 0, 0);
            }
        }
        if (_event(P, "on_tick")) {
            _call(P, "on_tick", 0, 0);
        }
        if (P->L != NULL && !P->broken) {
            _bot_team_tick(P, now);
        }
        if (P->store_dirty) {
            _store_save(P, "asked");
        }
        if (P->cells > 0) {
            evt_log(NULL, "plugin", "action=cells plugin=%s cells=%d", P->name, P->cells);
            P->cells = 0;
        }
    }
    _apply_pending(server);
}

/* --- events --- */

typedef struct
{
    player_t*   a;
    player_t*   b;
    double      n[3];
    const char* s;
    char*       text;
    size_t      size;
} ask_t;

typedef void (*push_fn)(lua_State* L, ask_t* q);

/* asks every live plugin's `event`, in load order (the mode's last): push
   puts nargs arguments; false refuses (0, the rest are not asked), any
   other value goes to take, which folds it into q for the next plugin */
static int _ask(const char* event, push_fn push, int nargs, ask_t* q, push_fn take)
{
    for (int k = 0; k < plugin_count; k++) {
        plugin_t* P = &plugins[k];
        if (!_event(P, event)) {
            continue;
        }
        push(P->L, q);
        if (!_call(P, event, nargs, 1)) {
            continue;
        }
        if (lua_isboolean(P->L, -1) && !lua_toboolean(P->L, -1)) {
            lua_pop(P->L, 1);
            evt_log(q->a, "plugin_refused", "plugin=%s event=%s", P->name, event);
            return 0;
        }
        if (!lua_isnil(P->L, -1) && take != NULL) {
            take(P->L, q);
        }
        lua_pop(P->L, 1);
    }
    return 1;
}

static void _push_a(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
}

static void _push_a_n0(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    lua_pushinteger(L, (lua_Integer) q->n[0]);
}

static void _push_a_b_n0(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    _push_player(L, q->b);
    lua_pushinteger(L, (lua_Integer) q->n[0]);
}

static void _push_a_xyz(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    lua_pushnumber(L, q->n[0]);
    lua_pushnumber(L, q->n[1]);
    lua_pushnumber(L, q->n[2]);
}

static void _push_a_xyz_s(lua_State* L, ask_t* q)
{
    _push_a_xyz(L, q);
    lua_pushstring(L, q->s);
}

static void _push_a_s(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    lua_pushstring(L, q->s);
}

static void _push_chat(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    lua_pushstring(L, q->text);
    lua_pushboolean(L, q->n[0] != 0);
}

static void _take_n0(lua_State* L, ask_t* q)
{
    if (lua_isnumber(L, -1)) {
        q->n[0] = lua_tonumber(L, -1);
    }
}

static void _take_n1(lua_State* L, ask_t* q)
{
    if (lua_isnumber(L, -1)) {
        q->n[1] = lua_tonumber(L, -1);
    }
}

static void _take_text(lua_State* L, ask_t* q)
{
    if (lua_type(L, -1) == LUA_TSTRING) {
        snprintf(q->text, q->size, "%s", lua_tostring(L, -1));
    }
}

static uint8_t _team_of(int t)
{
    return t == 1 ? TEAM_A : t == 2 ? TEAM_B : TEAM_SPECTATOR;
}

static int _team_number(uint8_t team)
{
    return team == TEAM_A ? 1 : team == TEAM_B ? 2 : 0;
}

int plugins_on_join(server_t* server, player_t* player, uint8_t* team)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {_team_number(*team)}};
    if (!_ask("on_join", _push_a_n0, 2, &q, _take_n0)) {
        return 0;
    }
    *team = _team_of((int) q.n[0]);
    return 1;
}

int plugins_on_team(server_t* server, player_t* player, uint8_t team)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {_team_number(team)}};
    return _ask("on_team", _push_a_n0, 2, &q, NULL);
}

void plugins_on_leave(server_t* server, player_t* player)
{
    pl_server = server;
    ask_t q   = {.a = player};
    _ask("on_leave", _push_a, 1, &q, NULL);
}

void plugins_on_spawn(server_t* server, player_t* player)
{
    pl_server = server;
    ask_t q   = {.a = player};
    _ask("on_spawn", _push_a, 1, &q, NULL);
}

int plugins_on_spawn_point(server_t* server, player_t* player)
{
    pl_server = server;
    for (int k = 0; k < plugin_count; k++) {
        plugin_t* P = &plugins[k];
        if (!_event(P, "on_spawn_point")) {
            continue;
        }
        _push_player(P->L, player);
        if (!_call(P, "on_spawn_point", 1, 3)) {
            continue;
        }
        int placed = lua_isnumber(P->L, -3) && lua_isnumber(P->L, -2) && lua_isnumber(P->L, -1);
        if (placed) {
            vector3f_t pos            = {(float) lua_tonumber(P->L, -3), (float) lua_tonumber(P->L, -2),
                                         (float) lua_tonumber(P->L, -1)};
            player->movement.position = pos;
            player->movement.prev_position  = pos;
            player->movement.prev_legit_pos = pos;
        }
        lua_pop(P->L, 3);
        if (placed) {
            evt_log(player, "plugin", "action=spawn_point plugin=%s at=%.1f,%.1f,%.1f", P->name,
                    player->movement.position.x, player->movement.position.y, player->movement.position.z);
            return 1;
        }
    }
    return 0;
}

int plugins_allows_respawn(server_t* server, player_t* player)
{
    pl_server = server;
    ask_t q   = {.a = player};
    return _ask("allows_respawn", _push_a, 1, &q, NULL);
}

static void _push_damage(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    _push_player(L, q->b);
    lua_pushinteger(L, (lua_Integer) q->n[1]);
    lua_pushinteger(L, (lua_Integer) q->n[0]);
}

int plugins_on_damage(server_t* server, player_t* attacker, player_t* victim, long* amount, uint8_t reason)
{
    pl_server = server;
    ask_t q   = {.a = attacker, .b = victim, .n = {reason, (double) *amount}};
    /* on_damage(attacker, victim, amount, reason) */
    if (!_ask("on_damage", _push_damage, 4, &q, _take_n1)) {
        return 0;
    }
    *amount = (long) q.n[1];
    return 1;
}

int plugins_on_hit(server_t* server, player_t* attacker, player_t* victim, uint8_t kind)
{
    pl_server = server;
    ask_t q   = {.a = attacker, .b = victim, .n = {kind}};
    return _ask("on_hit", _push_a_b_n0, 3, &q, NULL);
}

void plugins_on_kill(server_t* server, player_t* killer, player_t* victim, uint8_t reason)
{
    pl_server = server;
    /* on_kill(killer, victim, reason): killer nil for none */
    ask_t q = {.a = killer, .b = victim, .n = {reason}};
    _ask("on_kill", _push_a_b_n0, 3, &q, NULL);
}

int plugins_on_build(server_t* server, player_t* player, int x, int y, int z)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {x, y, z}};
    return _ask("on_build", _push_a_xyz, 4, &q, NULL);
}

int plugins_on_destroy(server_t* server, player_t* player, int x, int y, int z, const char* kind)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {x, y, z}, .s = kind};
    return _ask("on_destroy", _push_a_xyz_s, 5, &q, NULL);
}

int plugins_on_chat(server_t* server, player_t* player, char* text, size_t size, int team)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {team}, .text = text, .size = size};
    return _ask("on_chat", _push_chat, 3, &q, _take_text);
}

int plugins_on_tool(server_t* server, player_t* player, uint8_t tool)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {tool}};
    return _ask("on_tool", _push_a_n0, 2, &q, NULL);
}

int plugins_on_weapon(server_t* server, player_t* player, uint8_t weapon)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {weapon}};
    return _ask("on_weapon", _push_a_n0, 2, &q, NULL);
}

static void _push_a_class(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    lua_pushstring(L, game_class_get((int) q->n[0]) ? game_class_get((int) q->n[0])->name : "");
    lua_pushstring(L, q->s);
}

int plugins_on_class(server_t* server, player_t* player, int klass, const char* by)
{
    pl_server = server;
    if (profile_is_075()) {
        return 1; /* no classes in the 0.75 profile: nothing to ask */
    }
    ask_t q = {.a = player, .n = {klass}, .s = by};
    return _ask("on_class", _push_a_class, 3, &q, NULL);
}

int plugins_on_grenade(server_t* server, player_t* player)
{
    pl_server = server;
    ask_t q   = {.a = player};
    return _ask("on_grenade", _push_a, 1, &q, NULL);
}

static void _push_a_on(lua_State* L, ask_t* q)
{
    _push_player(L, q->a);
    lua_pushboolean(L, q->n[0] != 0);
}

int plugins_on_flashlight(server_t* server, player_t* player, int on)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {on}};
    return _ask("on_flashlight", _push_a_on, 2, &q, NULL);
}

void plugins_on_explode(server_t* server, player_t* player, float x, float y, float z)
{
    pl_server = server;
    ask_t q   = {.a = player, .n = {x, y, z}};
    _ask("on_explode", _push_a_xyz, 4, &q, NULL);
}

void plugins_on_reload(server_t* server, player_t* player)
{
    pl_server = server;
    ask_t q   = {.a = player};
    _ask("on_reload", _push_a, 1, &q, NULL);
}

void plugins_on_intel(server_t* server, player_t* player, const char* action)
{
    pl_server = server;
    ask_t q   = {.a = player, .s = action};
    _ask("on_intel", _push_a_s, 2, &q, NULL);
}

void plugins_on_player_tick(server_t* server, player_t* player)
{
    pl_server = server;
    ask_t q   = {.a = player};
    _ask("on_player_tick", _push_a, 1, &q, NULL);
}


/* --- the bots' API: a plugin gives bots their goal ---
 *
 * on_bot_think(bot) at every think of every bot (5 a second), before the
 * mode's own goal: a table is the goal (the fields of gm_bot_goal_t,
 * Server/BotGoal.h, documented in docs/bots.md "Asking bots for
 * something"), nil leaves it to the mode. The heavy parts (the way, sight,
 * aim, digging) stay the core's: a plugin decides, it never walks a path or
 * casts a ray loop itself. on_bot_arrived(bot, name) once a goal is reached,
 * on_bot_stuck(bot, why) from the core, on_bot_team_tick(team, bots) once a
 * second. EVT plugin action=bot_goal|bot_arrived|bot_stuck. */

#define PLUGIN_BOT_NAME 32

/* a goal's name outlives the Lua string it came from (the core keeps the
   pointer to log a change): two buffers a bot, the new name written in the
   one the core is not pointing at */
static char    bot_names[256][2][PLUGIN_BOT_NAME];
static uint8_t bot_name_cur[256];
/* the goal reached: once per name and destination */
static struct
{
    char    name[PLUGIN_BOT_NAME];
    float   x, y;
    uint8_t arrived;
} bot_dest[256];

static const char* _bot_name_keep(uint8_t id, const char* name)
{
    if (strcmp(bot_names[id][bot_name_cur[id]], name) != 0) {
        bot_name_cur[id] ^= 1;
        snprintf(bot_names[id][bot_name_cur[id]], PLUGIN_BOT_NAME, "%s", name);
    }
    return bot_names[id][bot_name_cur[id]];
}

static float _num_at(lua_State* L, int t, const char* key, float def)
{
    lua_getfield(L, t, key);
    float v = lua_isnumber(L, -1) ? (float) lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static int _bool_at(lua_State* L, int t, const char* key)
{
    lua_getfield(L, t, key);
    int v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

/* t[key] as a table (a point, a player): 1 and x, y, z (z 0 when absent) */
static int _point_at(lua_State* L, int t, const char* key, float* x, float* y, float* z)
{
    lua_getfield(L, t, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    int p = lua_gettop(L);
    *x    = _num_at(L, p, "x", 0.0f);
    *y    = _num_at(L, p, "y", 0.0f);
    *z    = _num_at(L, p, "z", 0.0f);
    lua_pop(L, 1);
    return 1;
}

static const struct
{
    const char* name;
    uint16_t    bit;
} intent_names[] = {
    {"take", GM_INTENT_TAKE},   {"push", GM_INTENT_PUSH},   {"squad", GM_INTENT_SQUAD}, {"with", GM_INTENT_WITH},
    {"post", GM_INTENT_POST},   {"home", GM_INTENT_HOME},   {"guard", GM_INTENT_GUARD}, {"chase", GM_INTENT_CHASE},
    {"brief", GM_INTENT_BRIEF}, {"plant", GM_INTENT_PLANT}, {"cover", GM_INTENT_COVER},
};

static uint16_t _intent_of(const char* s)
{
    for (size_t k = 0; k < sizeof(intent_names) / sizeof(intent_names[0]); k++) {
        if (!strcmp(s, intent_names[k].name)) {
            return intent_names[k].bit;
        }
    }
    return 0;
}

/* the goal table at the top of the stack into out; the plugin's mistakes
   (an unknown role or intent) are errors it sees */
static void _goal_from_table(lua_State* L, player_t* bot, gm_bot_goal_t* out)
{
    int t = lua_gettop(L);
    memset(out, 0, sizeof(*out));
    lua_getfield(L, t, "name");
    const char* name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "plugin";
    uint16_t    intent = 0;
    lua_getfield(L, t, "intent");
    if (lua_isstring(L, -1)) {
        intent = _intent_of(lua_tostring(L, -1));
        if (intent == 0) {
            luaL_error(L, "bot goal: unknown intent '%s'", lua_tostring(L, -1));
        }
    } else if (lua_istable(L, -1)) {
        for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TSTRING; i++) {
            uint16_t bit = _intent_of(lua_tostring(L, -1));
            if (bit == 0) {
                luaL_error(L, "bot goal: unknown intent '%s'", lua_tostring(L, -1));
            }
            intent |= bit;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    gm_goal_name(out, _bot_name_keep(bot->id, name), intent);
    lua_pop(L, 1);

    out->x     = _num_at(L, t, "x", bot->movement.position.x);
    out->y     = _num_at(L, t, "y", bot->movement.position.y);
    out->z     = _num_at(L, t, "z", 0.0f);
    out->exact = (uint8_t) _bool_at(L, t, "exact");
    out->hold  = _num_at(L, t, "hold", 0.0f);
    float wz;
    if (!_point_at(L, t, "watch", &out->watch_x, &out->watch_y, &wz)) {
        out->watch_x = out->x;
        out->watch_y = out->y;
    }
    out->rush         = (uint8_t) _bool_at(L, t, "rush");
    out->assault      = (uint8_t) _bool_at(L, t, "assault");
    out->walk_only    = (uint8_t) _bool_at(L, t, "walk_only");
    out->keep_terrain = (uint8_t) _bool_at(L, t, "keep_terrain");
    out->climb        = (uint8_t) _num_at(L, t, "climb", 0.0f);
    out->camp         = (uint8_t) _num_at(L, t, "camp", 0.0f);
    out->seated       = out->camp > 0;

    lua_getfield(L, t, "role");
    const char* role = lua_isstring(L, -1) ? lua_tostring(L, -1) : "attack";
    static const char* roles[] = {"attack", "defend", "infiltrate", "cover", "rush"};
    int                r       = -1;
    for (int k = 0; k < 5; k++) {
        if (!strcmp(role, roles[k])) {
            r = k;
        }
    }
    if (r < 0) {
        luaL_error(L, "bot goal: unknown role '%s'", role);
    }
    out->role = (uint8_t) r; /* GM_BOT_* in that order */
    lua_pop(L, 1);

    float wx, wy, wz2;
    if (_point_at(L, t, "work", &wx, &wy, &wz2)) {
        lua_getfield(L, t, "work");
        out->work       = _bool_at(L, lua_gettop(L), "dig") ? 2 : 1;
        out->work_color = (uint32_t) _num_at(L, lua_gettop(L), "color", 0.0f);
        lua_pop(L, 1);
        out->work_x = (int) wx;
        out->work_y = (int) wy;
        out->work_z = (int) wz2;
    }
    out->has_home = (uint8_t) _point_at(L, t, "home", &out->home_x, &out->home_y, &out->home_z);
    float gz;
    out->has_guard = (uint8_t) _point_at(L, t, "guard", &out->guard_x, &out->guard_y, &gz);
}

/* the stack's error from a goal we could not read: the plugin is off, as
   with any error of its own */
static int _goal_guarded(lua_State* L)
{
    player_t*      bot = lua_touserdata(L, 1);
    gm_bot_goal_t* out = lua_touserdata(L, 2);
    lua_settop(L, 3);
    _goal_from_table(L, bot, out);
    return 0;
}

/* what the bots' API costs, real time (a measurement, never the game's
   clock): logged with the plugins' tick every 30 s */
static uint64_t bot_api_calls, bot_api_ns, bot_api_logged;

static int _bot_goal_ask(server_t* server, player_t* bot, gm_bot_goal_t* out);

int plugins_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    if (plugin_count == 0) {
        return 0;
    }
    uint64_t t0  = get_real_nanos();
    int      got = _bot_goal_ask(server, bot, out);
    bot_api_ns += get_real_nanos() - t0;
    bot_api_calls++;
    return got;
}

static int _bot_goal_ask(server_t* server, player_t* bot, gm_bot_goal_t* out)
{
    pl_server = server;
    for (int k = 0; k < plugin_count; k++) {
        plugin_t* P = &plugins[k];
        if (!_event(P, "on_bot_think")) {
            continue;
        }
        _push_player(P->L, bot);
        if (!_call(P, "on_bot_think", 1, 1)) {
            continue;
        }
        if (!lua_istable(P->L, -1)) {
            lua_pop(P->L, 1);
            continue;
        }
        /* read under a pcall: a bad field is the plugin's error */
        int goal = lua_gettop(P->L);
        lua_pushcfunction(P->L, _goal_guarded);
        lua_pushlightuserdata(P->L, bot);
        lua_pushlightuserdata(P->L, out);
        lua_pushvalue(P->L, goal);
        int ok = _call(P, "on_bot_think's goal", 3, 0);
        lua_pop(P->L, 1);
        if (!ok) {
            continue;
        }
        /* reached: once per name and destination. A destination that
           follows someone (an escort) moves a little every think: only a
           jump past twice the reach (8 at least) is a new one */
        uint8_t id    = bot->id;
        float   dx    = out->x - bot->movement.position.x;
        float   dy    = out->y - bot->movement.position.y;
        float   reach = out->hold > 1.5f ? out->hold : 1.5f;
        float   moved = reach * 2.0f > 8.0f ? reach * 2.0f : 8.0f;
        if (strcmp(bot_dest[id].name, out->name) != 0 || fabsf(bot_dest[id].x - out->x) > moved ||
            fabsf(bot_dest[id].y - out->y) > moved)
        {
            snprintf(bot_dest[id].name, sizeof bot_dest[id].name, "%s", out->name);
            bot_dest[id].x       = out->x;
            bot_dest[id].y       = out->y;
            bot_dest[id].arrived = 0;
        }
        if (!bot_dest[id].arrived && dx * dx + dy * dy <= reach * reach) {
            bot_dest[id].arrived = 1;
            evt_log(bot, "plugin", "action=bot_arrived plugin=%s goal=%s", P->name, out->name);
            if (_event(P, "on_bot_arrived")) {
                _push_player(P->L, bot);
                lua_pushstring(P->L, out->name);
                _call(P, "on_bot_arrived", 2, 0);
            }
        }
        return 1;
    }
    return 0;
}

void plugins_on_bot_stuck(server_t* server, player_t* bot, const char* why)
{
    pl_server = server;
    ask_t q   = {.a = bot, .s = why};
    _ask("on_bot_stuck", _push_a_s, 2, &q, NULL);
}

/* on_bot_team_tick(team, bots): once a second, each team's bots (alive or
   not) as a list, to hand out roles before the thinks ask */
static void _bot_team_tick(plugin_t* P, uint64_t now)
{
    if (bot_api_calls > 0 && now - bot_api_logged >= 30ULL * NANO_IN_SECOND) {
        bot_api_logged = now;
        LOG_INFO("Plugins: on_bot_think %llu calls, %.1f us each, %.1f ms in all", (unsigned long long) bot_api_calls,
                 (double) bot_api_ns / 1000.0 / (double) bot_api_calls, (double) bot_api_ns / 1e6);
    }
    if (now - P->bot_team_at < NANO_IN_SECOND || !_event(P, "on_bot_team_tick")) {
        return;
    }
    P->bot_team_at = now;
    lua_pop(P->L, 1);
    for (int team = 1; team <= 2; team++) {
        if (!_event(P, "on_bot_team_tick")) {
            return;
        }
        lua_pushinteger(P->L, team);
        lua_newtable(P->L);
        int       n = 0;
        player_t *p, *tmp;
        HASH_ITER(hh, pl_server->players, p, tmp)
        {
            if (p->is_bot && p->team == (team == 1 ? TEAM_A : TEAM_B)) {
                _push_player(P->L, p);
                lua_rawseti(P->L, -2, ++n);
            }
        }
        if (!_call(P, "on_bot_team_tick", 2, 0)) {
            return;
        }
    }
}

int plugins_on_login(server_t* server, player_t* player, const char* role)
{
    pl_server = server;
    ask_t q   = {.a = player, .s = role};
    return _ask("on_login", _push_a_s, 2, &q, NULL);
}

int plugins_on_command(server_t* server, player_t* player, uint8_t console, const char* message)
{
    pl_server = server;
    char name[32];
    if (sscanf(message, "%31s", name) != 1) {
        return 0;
    }
    for (int k = 0; k < plugin_count; k++) {
        plugin_t* P = &plugins[k];
        for (int c = 0; c < P->command_count; c++) {
            if (strcmp(P->commands[c].name, name)) {
                continue;
            }
            uint32_t need = P->commands[c].permissions;
            if (need != 0 && !player_has_permission(server, player, console, need)) {
                send_server_notice(player, console, "You do not have permissions to use this command");
                evt_log(player, "plugin", "action=command_denied plugin=%s command=%s", P->name, name);
                return 1;
            }
            if (P->L == NULL || P->broken) {
                send_server_notice(player, console, "The plugin of this command is off");
                return 1;
            }
            const char* line = message + strlen(name);
            while (*line == ' ') {
                line++;
            }
            lua_State* L = P->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, P->commands[c].fn);
            if (console) {
                lua_pushnil(L);
            } else {
                _push_player(L, player);
            }
            lua_newtable(L);
            char  words[1001];
            char* save = NULL;
            int   n    = 0;
            snprintf(words, sizeof words, "%s", line);
            for (char* w = strtok_r(words, " ", &save); w != NULL; w = strtok_r(NULL, " ", &save)) {
                lua_pushstring(L, w);
                lua_rawseti(L, -2, ++n);
            }
            lua_pushstring(L, line);
            evt_log(console ? NULL : player, "plugin", "action=command_run plugin=%s command=%s args=%d", P->name,
                    name, n);
            if (_call(P, name, 3, 1)) {
                if (lua_type(L, -1) == LUA_TSTRING) {
                    send_server_notice(player, console, "%s", lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
            return 1;
        }
    }
    return 0;
}

void plugins_help(server_t* server, player_t* player, uint8_t console)
{
    pl_server = server;
    for (int k = 0; k < plugin_count; k++) {
        plugin_t* P = &plugins[k];
        for (int c = 0; c < P->command_count && !P->broken; c++) {
            uint32_t need = P->commands[c].permissions;
            if (need == 0 || player_has_permission(server, player, console, need)) {
                send_server_notice(player, console, "[Command: %s, Description: %s (plugin %s)]", P->commands[c].name,
                                   P->commands[c].help, P->name);
            }
        }
    }
}

void plugins_shutdown(server_t* server)
{
    pl_server = server;
    for (int k = 0; k < plugin_count; k++) {
        _store_save(&plugins[k], "shutdown");
    }
}

#endif
