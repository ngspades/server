// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOT_HELI_H
#define BOT_HELI_H

/*
 * Bots and the helicopter (protocol 1.0 only; Proto10/Helicopter.c,
 * Proto10/Mount.c). A bot is a player: it boards with the very `use` a
 * client sends (mount_use), flies with the pilot's ordinary keys, look and
 * held jump / crouch (what Helicopter.c reads of any pilot), and leaves with
 * `use` again. Nothing in the seat or the flight knows it is a bot.
 *
 * - Boarding: a bot of an attacking role, with nothing to fight and its goal
 *   far away, that sees its team's helicopter standing free within
 *   BOT_HELI_BOARD_R takes it on a [bots] heli_chance roll (once a
 *   helicopter a minute), says so in the team chat and to its team's 1.0
 *   clients (`bot-voice <player> take_heli`), and walks to the pilot's door
 *   (goal `board-heli`). A human waiting in a passenger seat of a pilotless
 *   one is taken every time.
 * - Flying: toward the mode's goal at a height over the highest ground ahead,
 *   lands on flat dry ground near it and gets out; also after
 *   BOT_HELI_MAX_FLIGHT, below BOT_HELI_LOW_HEALTH, or when an enemy
 *   helicopter or machine gunner threatens it. A wreck is `bot-voice
 *   <player> heli_down`.
 * - Shooting: with no player to shoot at, a bot shoots the enemy's
 *   helicopters (occupied ones), sentries and machine guns it sees, through
 *   the hit path a client's HitEntity takes (proto10_objects_on_hit), the
 *   rocket launcher through rockets_fire, leading a moving target.
 *
 * EVT bot_heli action=roll|decide|cancel|board|takeoff|land|dismount|down
 * EVT bot_heli_shot target= type= result=hit|miss|rocket dist=
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Server/BotGoal.h>
#include <stdint.h>

struct bot_ai;

/* [bots] heli_enabled and heli_chance (SP_BOT_HELI, SP_BOT_HELI_CHANCE) */
void bot_heli_init(server_t* server);
/* once a tick, before the bots: which helicopters stand on the map */
void bot_heli_tick_begin(server_t* server);
/* every tick for every bot, before anything else of it: a lost helicopter
   (heli_down), a boarding that ended */
void bot_heli_watch(server_t* server, player_t* bot);
/* every tick for a living bot: 1 when it sits in a helicopter (it flew, or
   left it): the rest of its tick (think, turn) is skipped */
int bot_heli_pilot(server_t* server, player_t* bot, struct bot_ai* ai);
/* every tick for a living bot on foot: deciding to board, and boarding */
void bot_heli_consider(server_t* server, player_t* bot, struct bot_ai* ai);
/* the goal while it walks to its helicopter: 1 and the goal filled */
int bot_heli_goal(server_t* server, player_t* bot, gm_bot_goal_t* out);
/* after a think that aimed at nobody: an enemy helicopter, sentry or MG */
void bot_heli_shoot(server_t* server, player_t* bot, struct bot_ai* ai);

#endif
