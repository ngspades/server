// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTS_H
#define BOTS_H

#include <Server/Structs/ServerStruct.h>
#include <stdint.h>

/* Server side AI bots. A bot is a normal player_t flagged with is_bot and a
   NULL conn, so it shows up on clients like a human but never owns a
   network connection. Movement/fire is driven through the regular player
   input fields and the shared physics, never by teleporting. */

void      server_add_startup_bots(int count);
void      bots_set_startup_teams(int team1, int team2);
uint8_t   bots_enabled(server_t* server);
player_t* bot_create(server_t* server, uint8_t team);
player_t* bot_create_beside(server_t* server, uint8_t team, float skill, uint8_t dummy);
uint8_t   bot_remove(server_t* server, uint8_t player_id);
uint32_t  bot_remove_all(server_t* server);
uint32_t  bot_count(server_t* server);
uint8_t   bot_exists(player_t* player);
/* the bot keeps a post of its own (a camper): it leads or follows no squad */
uint8_t   bot_holds_post(player_t* player);
/* bots are playing: a player is in the match (they stand still otherwise) */
uint8_t   bots_playing(void);
/* SP_BOTS_PLAY (tests, the headless benchmark): bots play with nobody in */
uint8_t   bots_play_alone(void);
/* the bot types text in the chat (its team's with team_only), as a player */
void      bot_say(server_t* server, player_t* bot, uint8_t team_only, const char* text);
/* the team's overwatch: the camper watching our intel (it chases its thief) */
/* the overwatch: the camper the mode seated to watch home (goal camp 2) */
uint8_t   bot_watches_home(player_t* player);
/* a sneaky attacker (the hidden way near the goal): it leads no squad
   while another member can (the squad crawled behind it) */
uint8_t   bot_is_sneaky(player_t* player);
/* a Miner bot on a tunnel to its goal: its way there is dug, not walked */
uint8_t   bot_is_tunnelling(player_t* player);
/* for the plugins (p.goal, p.target): the bot's goal name ("none") and the
   id of the enemy it fights, -1 none */
const char* bot_goal_name(player_t* player);
int         bot_target_id(player_t* player);
/* a clear line from a's eye to b's chest (the bots' own sight test, without
   their cache): the plugins' p:sees(q) */
uint8_t     bot_line_of_sight(server_t* server, player_t* a, player_t* b);

/* Autofill keeps a target number of players on the server: extra bots are
   dropped when a human joins and added back when a human leaves. 0 disables. */
uint8_t   bot_autofill_target(server_t* server);
void      bot_set_autofill(server_t* server, uint8_t players_per_server);

void bots_update(server_t* server);          // call from the server tick
/* someone was hurt by attacker: the victim, if a bot, and the bots of its
   team close to it react (look where the shot came from, take cover) */
void bots_on_damage(server_t* server, player_t* attacker, player_t* victim);
/* digger opened cell (x, y, z) with the spade: enemy bots in earshot hear it */
void bots_on_dig(server_t* server, player_t* digger, int x, int y, int z);
/* a block a player of a team put: that team's bots never dig it while it
   stands (bots_team_built), a mate's stair or pillar included */
void    bots_on_build(server_t* server, player_t* builder, int x, int y, int z);
uint8_t bots_team_built(server_t* server, uint8_t team, int x, int y, int z);
void bots_on_player_join(server_t* server);  // autofill: drop a bot if over target
void bots_on_player_leave(server_t* server); // autofill: add a bot back

/* Difficulty: [bots] difficulty in config.toml, SP_BOT_DIFFICULTY, or the
   host's Bots menu during the match. It picks the range each bot's skill is
   rolled in and scales the aim error. */
#define BOT_DIFFICULTY_EASY   0
#define BOT_DIFFICULTY_NORMAL 1
#define BOT_DIFFICULTY_HARD   2
#define BOT_DIFFICULTY_COUNT  3
void        bots_init(server_t* server); // reads [bots]; call once at startup
uint8_t     bot_difficulty(void);
const char* bot_difficulty_name(uint8_t level);
void        bot_set_difficulty(server_t* server, uint8_t level); // re-rolls every bot's skill

#endif /* BOTS_H */
