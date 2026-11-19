// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLUGINS_H
#define PLUGINS_H

/*
 * Plugins: Lua scripts that change the behaviour of the whole server, the
 * way pyspades' scripts do. They do not ship with the server: whoever runs
 * one downloads them from the public plugin repository
 * (github.com/ngspades/plugins) into Resources/plugins/<name>.lua
 * and lists them in config.toml, `[plugins] load = ["airstrike", ...]`
 * (SP_PLUGINS=a,b for tests); `[plugins.<name>]` is that plugin's `config`
 * table. A plugin played as the game mode (--gamemode <name>, `mode = true`
 * in its table) is one more plugin, asked last (Gamemodes/LuaMode.c).
 *
 * Each plugin has a Lua state of its own, made again for every map, and
 * runs sandboxed (base, table, string, math; no file, no network; a call
 * that runs too long is stopped). An error turns that plugin off alone
 * (EVT plugin action=error). Events are named like pyspades' and are
 * asked of every plugin in load order: returning false refuses the action
 * (the others are not asked; EVT plugin_refused), returning a value
 * replaces it for the next plugin and the server. The API is the global
 * `game` and the methods of the player tables it hands out; the contract
 * is docs/api.md of the plugin repository.
 *
 * Profiles (docs/plan-075-server.md sections 5 and 6.3): a plugin may say
 * which servers it is for, game.supported_profiles("10", "075") at its top
 * level, and the operator may restrict it, [plugins.<name>] profiles =
 * ["10"]; both must allow the running profile or the plugin is not loaded
 * (EVT plugin action=refused why=profile). In the 0.75 profile the 1.0-only
 * API (classes, perks, squads, a mode the profile lacks) answers false or
 * nil and a reason (EVT plugin action=refused why=profile what=..).
 */

#include <Server/BotGoal.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stddef.h>
#include <stdint.h>

/* a map starts: the listed plugins are loaded again (before the mode's
   init), then their on_init runs (after it) */
void plugins_map_start(server_t* server);
void plugins_map_ready(server_t* server);
/* every server tick: timers and on_tick, 10 times a second */
void plugins_tick(server_t* server);

/* events; those returning int return 0 when a plugin refused */
int  plugins_on_join(server_t* server, player_t* player, uint8_t* team);
int  plugins_on_team(server_t* server, player_t* player, uint8_t team);
void plugins_on_leave(server_t* server, player_t* player);
void plugins_on_spawn(server_t* server, player_t* player);
/* 1 when a plugin placed the player (movement.position set) */
int  plugins_on_spawn_point(server_t* server, player_t* player);
int  plugins_allows_respawn(server_t* server, player_t* player);
int  plugins_on_damage(server_t* server, player_t* attacker, player_t* victim, long* amount, uint8_t reason);
int  plugins_on_hit(server_t* server, player_t* attacker, player_t* victim, uint8_t kind);
void plugins_on_kill(server_t* server, player_t* killer, player_t* victim, uint8_t reason);
int  plugins_on_build(server_t* server, player_t* player, int x, int y, int z);
int  plugins_on_destroy(server_t* server, player_t* player, int x, int y, int z, const char* kind);
/* text may be replaced in place (size bytes) */
int  plugins_on_chat(server_t* server, player_t* player, char* text, size_t size, int team);
int  plugins_on_tool(server_t* server, player_t* player, uint8_t tool);
int  plugins_on_weapon(server_t* server, player_t* player, uint8_t weapon);
/* a class picked (by: client, join, bot): false refuses it */
int  plugins_on_class(server_t* server, player_t* player, int klass, const char* by);
int  plugins_on_grenade(server_t* server, player_t* player);
/* a 0.75 client asks to switch its flashlight (Legacy/Ext075.c,
   on_flashlight(p, on)): false refuses it, and nobody is told */
int  plugins_on_flashlight(server_t* server, player_t* player, int on);
void plugins_on_explode(server_t* server, player_t* player, float x, float y, float z);
void plugins_on_reload(server_t* server, player_t* player);
void plugins_on_intel(server_t* server, player_t* player, const char* action);
void plugins_on_player_tick(server_t* server, player_t* player);
/* the bots' API (docs/bots.md "Asking bots for something"):
   1 and the goal when a plugin's on_bot_think(bot) gave one (asked before
   the mode's, gamemode_bot_goal); on_bot_arrived(bot, name) when the bot
   got there. on_bot_stuck(bot, why) from the core */
int  plugins_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out);
void plugins_on_bot_stuck(server_t* server, player_t* bot, const char* why);
/* /login's password was right for the role (trusted, guard, mod, admin,
   manager): 0 when a plugin refused it (a second factor) */
int  plugins_on_login(server_t* server, player_t* player, const char* role);
/* a "/" command the server has none of: 1 when a plugin's game.command
   took it (run, or refused for permissions) */
int  plugins_on_command(server_t* server, player_t* player, uint8_t console, const char* message);
/* /help: the plugins' commands this player may use */
void plugins_help(server_t* server, player_t* player, uint8_t console);
/* the server stops: every game.store written */
void plugins_shutdown(server_t* server);

/* the game mode's plugin (LuaMode.c) */
typedef struct plugin plugin_t;
/* 1 when Resources/plugins/<name>.lua exists (a name, never a path) */
int       plugins_exists(const char* name);
/* the plugins loaded and working, comma separated (the server's details) */
void      plugins_names(char* out, size_t size);
/* loads it as the mode, after the listed plugins; NULL when it fails */
plugin_t* plugins_load_mode(server_t* server, const char* name);
/* mode.<field> as a string, NULL when not one */
const char* plugins_mode_field(plugin_t* plugin, const char* field, char* buf, size_t size);
/* a number field of the mode's table (time_limit): 1 and *out when it is one */
int plugins_mode_number(plugin_t* plugin, const char* field, double* out);
/* the mode's on_init */
void plugins_mode_init(plugin_t* plugin);
/* game.objective(): 1 and the spot when the mode set one */
int plugins_mode_objective(float* x, float* y, float* r);

#endif
