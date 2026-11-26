// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SQUADS_H
#define SQUADS_H

/*
 * Squads (CTF): small groups inside a team, like the squad.py script of
 * pyspades servers. A player who respawns appears next to a living squadmate
 * who is not in a fight, so a squad stays together at the front; with no such
 * mate it is the team's spawn as usual.
 *
 * Everyone gets a squad when they spawn (the smallest one of their team with
 * room); the client's menu can ask for another one or none ("squad <n>", n 0
 * = alone). Clients learn every player's squad from GenericCommand
 * "squad <player> <n>", and "squads <size>" says squads are on.
 *
 * The squad's leader (its first human by id) may drop a rally point where it
 * stands ("squad-rally", from the quick chat ring): for 60 s the squad
 * respawns there first; the squad hears "squad-rally <leader> x y z", a
 * refusal "squad-refused <no_squad|dead|not_leader|too_close|no_ground>".
 * EVT rally action=dropped|refused, EVT squad_spawn rally=1.
 *
 * config.toml [squads]: enabled (default true), size (default 4), spawn
 * (default true: the respawn by a mate and the rally point where the mode
 * offers them, GM_FEAT_SQUAD_SPAWN). SP_SQUADS=0 turns them off for a test,
 * SP_SQUAD_SPAWN=0 the respawn alone. Logged as EVT squad ... and
 * EVT squad_spawn ...
 *
 * The CTFs wire these hooks into their ops themselves (Ctf.c, NgCtf.c); any
 * other mode sets gamemode_ops_t.squad_teams and Gamemode.c's wrappers call
 * them (king of the hill, territory control, team deathmatch, the arena, the
 * zombie mode's survivors, a Lua mode with squads = true).
 */

#include <Server/BotGoal.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

/* enough squads of 4 for a team of 64 (6 left most of a big team alone) */
#define SQUADS_MAX 16

/* the mark of a bot kept out of squads by its role (squad_team): not its
   team, so it gets a squad again when its role goes */
#define SQUAD_TEAM_ROLE 0xFE

/* gamemode_ops_t.squad_teams: the teams that have squads, a bit a team
   (1 << TEAM_A, 1 << TEAM_B) */
#define SQUAD_TEAMS_BOTH 0x03

/* a bot with a role of its own (guard, infiltrator, cover, camper) is in no
   squad: it does not lead or follow one (Ctf.c decides, ctf_bot_has_post) */
void squads_leave_for_role(server_t* server, player_t* player);

/* the mode hooks (Ctf.c wires them into ctf_ops) */
int  squads_spawn_point(server_t* server, player_t* player);
void squads_client_in_game(server_t* server, player_t* player);
int  squads_command(server_t* server, player_t* player, const char* command, const char* arg);

/* every 30 s of the game: how the players of each team sit in squads
   (EVT squad_census), called from the mode's tick */
void squads_census(server_t* server);

/* 1 while the squads and their respawn are on ([squads] enabled and spawn):
   gamemode_has(GM_FEAT_SQUAD_SPAWN) asks it */
int squads_spawn_on(void);

/* the player's squad's leader (its first human by id, else its first bot),
   NULL when it is in no squad */
player_t* squads_leader(server_t* server, player_t* player);

/* a bot of a squad keeps up with its leader on the way to an objective at
   ox, oy: 1 and a "squad-follow" goal while both are farther than split
   from it; 0 (out untouched) when the bot goes about its own job (it leads,
   it or its leader is there, the leader is dead, no squad). EVT bot_squad
   action=follow|split on each change */
int squads_follow(server_t* server, player_t* bot, float ox, float oy, float split, gm_bot_goal_t* out);

#endif
