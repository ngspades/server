// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAPVOTE_H
#define MAPVOTE_H

/*
 * The vote for the next map, held while a match's summary is on the clients'
 * screens (protocol 1.0 only: the 0.75 profile reloads at once). Up to
 * MAP_VOTE_OPTIONS maps of the rotation, drawn at random without the one just
 * played, are offered with their size and the share of this server's matches
 * that were played on each (kept in the data folder's map-stats.txt). Every
 * human votes for one (spectators too, bots never); the most voted map loads
 * next, a tie or no vote draws among the tied (or all) at random.
 * docs/protocol-1.0.md, GenericCommand: vote-start, vote-map, vote-tally,
 * vote-result and the client's vote.
 */

#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>

#define MAP_VOTE_OPTIONS 5
/* seconds the summary lasts longer than without a vote, and how long before
   the next map loads the vote closes (the result stays on screen) */
#define MAP_VOTE_EXTRA_SECONDS 14
#define MAP_VOTE_RESULT_SECONDS 3

/* a match was played on this map: counted and saved */
void mapvote_record_match(server_t* server);

/* there is a vote to hold: 1.0 profile and at least two maps to choose from
   besides the one just played */
int mapvote_possible(server_t* server);

/* the match ended: draws the options and tells every 1.0 client. Returns
   1 when there is a vote (the summary then lasts MAP_VOTE_EXTRA_SECONDS
   more), 0 when there are too few maps to choose from. `summary_seconds` is
   the whole summary's length, the vote closes MAP_VOTE_RESULT_SECONDS
   before it ends. */
int mapvote_start(server_t* server, int summary_seconds);

/* "vote <index>" from a client; 0 when there is no vote or it is refused */
int mapvote_cast(server_t* server, player_t* player, int index);

/* every tick while the summary is on: closes the vote when its time is up */
void mapvote_tick(server_t* server);

/* a vote is open (its result not yet sent) */
int mapvote_open(void);

/* the map being left, the vote forgotten */
void mapvote_reset(void);

/* test hook: the option's name and the votes it has, -1 out of range */
int mapvote_votes(server_t* server, int index);

#endif
