// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUAMODE_H
#define LUAMODE_H

/*
 * Game modes written in Lua: a plugin (Server/Plugins.h) whose table says
 * mode = true is one more mode, picked by its name (--gamemode <name>) when
 * no built-in mode has it. Its events reach it with every other plugin's,
 * asked last; these ops add what only a mode has: how clients draw it
 * ("wire": ctf or arena), capture the flag's rules under it (rules = "ctf"),
 * the bots' objective (game.objective). One that does not load leaves the
 * server in CTF. Built only with Lua (ENABLE_LUA); without it no mode is
 * found.
 */

#include <Server/Gamemodes/Gamemode.h>

/* the mode of plugin Resources/plugins/<name>.lua, NULL when there is none
   (or the server was built without Lua) */
const gamemode_ops_t* lua_mode_find(const char* name);
/* the script's mode once found (GAME_MODE_LUA), NULL before */
const gamemode_ops_t* lua_mode_ops(void);

/* the match's time limit, in seconds from now (0 none): the mode's `time_limit`
   field at its start, or game.set_time_limit(). When it runs out the match
   ends by the score (the higher team wins, equal is a draw), as the built-in
   team deathmatch's did. Returns 1 when a mode of a script is running. */
int lua_mode_set_time_limit(int seconds);
/* seconds left of the limit, -1 when there is none */
int lua_mode_time_left(void);

#endif
