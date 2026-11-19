// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SERVER_BOTPROJECTS_H
#define SERVER_BOTPROJECTS_H

/*
 * Shared bot building projects, the simple half of BattleSpades'
 * cooperative layer: a bridge is a short line of cells reserved by one
 * builder and one partner. The owner builds the near cells, the partner the
 * far ones; the cells are reserved so two bots never spend blocks on the
 * same hole. When the line is solid the partner crosses the landing, and the
 * ordinary planner takes over again.
 *
 * This is deliberately bounded: at most BOT_PROJECTS live projects, at most
 * six cells each, 30 seconds, and every placement still goes through the
 * real block action (the server validates rate, reach and support).
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

#define BOT_PROJECTS      8
#define BOT_PROJECT_CELLS 6

/* [bots] cooperative_bridges (default on); SP_BOT_PROJECTS=0 for tests */
void    bot_projects_init(server_t* server);
void    bot_projects_reset(void);
void    bot_projects_tick(server_t* server);

/* create a short bridge from (x,y,z) in direction (dx,dy); 0 when there is
   already a project for this bot/team nearby or no empty cell to build */
uint8_t bot_project_create_bridge(server_t* server, player_t* bot, int x, int y, int z, int dx, int dy);

/* the bot's assigned cell, when it is within reach: place one block */
uint8_t bot_project_step(server_t* server, player_t* bot, int goal_floor);

/* where the bot should walk for its project: 1 and a cell/floor. A finished
   project sends the partner to the landing and lets the owner resume. */
uint8_t bot_project_goal(server_t* server, player_t* bot, float* x, float* y, int* floor);

#endif
