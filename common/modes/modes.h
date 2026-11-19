// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The game modes a player can choose and their built-in maps, shared by the
	client (the mode and map picker, the command line it starts the server
	with) and the server (--gamemode / --map). The maps live in
	server/Resources/maps/<Name>/ and ship with every release.

	Header only, like common/flight: functions instead of a global table, so a
	file that includes it and uses nothing gets no unused-variable warning
	(the server builds with -Werror).
*/

#ifndef SHARED_MODES_H
#define SHARED_MODES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* marks a text the client translates (tools/lang_update.py finds it; the
   client's lang.h defines it too) */
#ifndef N_
#define N_(s) s
#endif

#define GAME_MODES_COUNT 9
#define GAME_MODE_MAPS 24 /* at most; a mode with fewer ends its list with NULL */

/* the bots a team gets by default on a map: 10 where the bases are close,
   20 where they are far apart (Raindance ~320 blocks, Busan_Port ~390) */
#define GAME_MAP_BOTS_SMALL 10
#define GAME_MAP_BOTS_LARGE 20
/* the next gen CTF plays only maps whose two spawn areas are at least this
   far apart (the server's map_fits): its classes need the room */
#define GAME_NGCTF_MIN_BASES 200.0f

struct game_mode_info {
	const char* name;  /* command line and protocol 1.0 name: "ctf" */
	const char* title; /* shown in menus: "Capture the flag" */
	int id;            /* the server's gamemode_t (0 ctf, 2 babel, 3 arena, 6 ngctf) */
	const char* maps[GAME_MODE_MAPS];
	int bots[GAME_MODE_MAPS]; /* each map's default bots per team */
	uint8_t classes;   /* the mode has the classes option (the zombie mode's and
	                     king of the hill's: the survivors pick a class, the
	                     host's choice) */
};

/* the zombies' leap (mode "zombie") at its full charge: the jump's upward
   speed, blocks per physics unit like the usual -0.36 (z grows downwards).
   The height goes with its square: ~5 blocks, over a four block wall
   (-0.62, ~3.5 blocks, kept zombies under most roofs). Both programs use
   it, the client to predict its own jump */
#define GAME_ZOMBIE_JUMP_VELOCITY (-0.74f)
/* the leap is charged: a zombie holds the jump button and lets go. A tap
   (held under GAME_ZOMBIE_LEAP_MIN_HOLD) is a normal jump; held longer,
   the leap grows with the time held up to GAME_ZOMBIE_LEAP_CHARGE_SECONDS:
   from GAME_ZOMBIE_LEAP_UP_MIN up and nothing ahead to the full
   GAME_ZOMBIE_JUMP_VELOCITY up and GAME_ZOMBIE_LEAP_FORWARD ahead (~25
   blocks, running). The charge reaches the server in the jump's
   PlayerAction param (game_zombie_leap_param: 1..255, 0 a normal jump) */
#define GAME_ZOMBIE_LEAP_MIN_HOLD 0.15f
/* a zombie's tap, the normal jump: rises ~2.45 blocks (the height goes with the
   speed's square; a human's -0.36 ~1.5, -0.50 2.9: over a fort's 3), so a two block step or wall is
   one jump and only higher ones wait for the charged leap. Zombies stopped
   at every waist high wall a survivor put up */
#define GAME_ZOMBIE_TAP_JUMP_VELOCITY (-0.46f)
#define GAME_ZOMBIE_TAP_JUMP_UP 2
#define GAME_ZOMBIE_LEAP_CHARGE_SECONDS 1.0f
#define GAME_ZOMBIE_LEAP_UP_MIN (-0.45f)
#define GAME_ZOMBIE_LEAP_FORWARD 0.8f
/* a zombie runs this much faster on the ground (Infected's fast infected);
   the server and the client move it the same */
#define GAME_ZOMBIE_SPEED 1.25f
/* a zombie's health: the server takes 100 / this of every hit on one (the
   hp on the wire and in every check stays 0..100), and the zombie's HUD
   shows its hp times this / 100 */
#define GAME_ZOMBIE_HEALTH 250
/* a zombie that is hit runs at this speed (instead of GAME_ZOMBIE_SPEED)
   for this many seconds, so shooting one buys time; the server starts it on
   the hit and the zombie's client when its Damage arrives */
#define GAME_ZOMBIE_SLOWED 0.45f
#define GAME_ZOMBIE_SLOW_SECONDS 1.0f
/* a zombie whose claw lands is slowed too (GAME_ZOMBIE_SLOWED), this many
   seconds more for each hit in a row, up to the cap from now: a human it
   caught gets away between swings. The server starts it on the hit, the
   zombie's client when the hit's confirmation arrives */
#define GAME_ZOMBIE_CLAW_SLOW_SECONDS 0.6f
#define GAME_ZOMBIE_CLAW_SLOW_MAX 2.0f
/* the high jump comes back this many seconds after one; in between a zombie
   jumps like anyone. The server allows it this much earlier, as the client's
   jump reaches it a ping late */
#define GAME_ZOMBIE_LEAP_COOLDOWN 4.0f
#define GAME_ZOMBIE_LEAP_SLACK 0.25f

/* the leap's speed up (negative) and ahead for a charge of 0..1 */
static inline float game_zombie_leap_up(float charge) {
	return GAME_ZOMBIE_LEAP_UP_MIN + (GAME_ZOMBIE_JUMP_VELOCITY - GAME_ZOMBIE_LEAP_UP_MIN) * charge;
}

static inline float game_zombie_leap_forward(float charge) {
	return GAME_ZOMBIE_LEAP_FORWARD * charge;
}

/* the jump's PlayerAction param for a charge of 0..1: 1..255 */
static inline unsigned game_zombie_leap_param(float charge) {
	if(charge < 0.0f)
		charge = 0.0f;
	if(charge > 1.0f)
		charge = 1.0f;
	return 1u + (unsigned)(charge * 254.0f + 0.5f);
}

static inline float game_zombie_leap_charge(unsigned param) {
	return param <= 1u ? 0.0f : (param >= 255u ? 1.0f : (float)(param - 1u) / 254.0f);
}

/* the k-th mode (0..GAME_MODES_COUNT-1), NULL past the end. The catalogs
   are the maps of the Ace of Spades 0.75 server list of 2012 (classicgen,
   Hallway, Pinpoint, Harbor 2, Bridge Wars, Normandie...: the maps players
   remember, shipped in server/Resources/maps), the zombie maps, the
   Counter-Strike arenas and the two 1024 maps; each map is one its mode's
   bots play (the maps repository's tags, or tools/bench.py with 16 bots,
   300 s, 2 seeds). Two CTFs, like Battle Builders: next gen (perks,
   revive, spotting, assists, squad spawn and rally) and classic (the 0.75
   rules as they were) */
static inline const struct game_mode_info* game_mode_get(int k) {
	static const struct game_mode_info modes[GAME_MODES_COUNT] = {
		{"ngctf",
		 N_("Capture the flag (next gen)"),
		 6,
		 /* the same maps as classic CTF (its own first, Normandie the
		    default), every one large (bases 200 blocks apart) */
		 {"Normandie", "Villages_1024", "Mountain_River_1024", "hoth", "marketgarden", "standoff", "industry", "burbs", "killaman", "trench_warfare", "Build2", "Flatland", "classicgen", "Hallway", "Pinpoint", "Pinpoint2", "Harbor_2", "Bridge_Wars", "Anticbridge", "4rivers", "Platform", NULL},
		 {GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, 0},
		 0},
		{"ctf",
		 N_("Capture the flag (classic)"),
		 0,
		 /* the 2012 list's CTF maps and the next gen's, one list for both
		    CTFs; classicgen the default (its bots take and capture the
		    intel); on Hallway, Bridge_Wars and Anticbridge the bots fight
		    (40 to 74 kills in 300 s) but take no intel */
		 {"classicgen", "Hallway", "Pinpoint", "Pinpoint2", "Harbor_2", "Normandie", "Bridge_Wars", "Anticbridge", "4rivers", "burbs", "hoth", "Platform", "Villages_1024", "Mountain_River_1024", "marketgarden", "standoff", "industry", "killaman", "trench_warfare", "Build2", "Flatland", NULL},
		 {GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, 0},
		 0},
		{"babel",
		 N_("Babel"),
		 2,
		 {"Nine-Eye_Babel", "babelstairs", "Island", "newfort", NULL},
		 {GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, 0},
		 0},
		{"arena",
		 N_("Arena"),
		 3,
		 /* csitaly, de_dust and de_cache first, then the 2012 list's arenas
		    (avstorage, Titanic Arena, Abordage...); every one ends rounds
		    with 16 bots */
		 {"csitaly", "de_dust", "de_cache", "avstorage", "indiaarena", "Titanic_Arena", "Abordage", "planeCrash",
		  "facebattle", "Penthouse", "de_dust2", "cs_assault"},
		 {GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL},
		 0},
		{"zombie",
		 N_("Zombies"),
		 4,
		 /* maps made for zombies (a map named so plays the mode whatever
		    its fitness), pegs (aloha.pk's humans vs. zombies in 2012), the
		    2012 list's maps tagged for zombies, then the towns whose
		    houses the survivors hold */
		 {"Beaulieu_Abbey_Zombie", "Zombie_Island", "pegs", "Ghost_Town", "harborPrototype", "Cannibal_Islands", "20thCenturyTown", "Desert_Rock", "OperationMetro_734", "tennis", "enano", "ChessArena"},
		 {GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_SMALL},
		 1},
		{"tdm",
		 N_("Team deathmatch"),
		 8,
		 {"Hallway", "Pinpoint2", "Harbor_2", "Bridge_Wars", "goonassault", "Assault2", "killaman", "Penthouse", NULL},
		 {GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_SMALL, 0},
		 0},
		/* maps with a hill as far from both spawns that both reach (2Fort's
		   is its bridge, six blocks over the banks: no round ended in 900 s
		   with 12 bots): the two 1024 maps (4 rounds in 600 s with 16 bots
		   each) and the 2012 list's maps tagged for it */
		{"koth",
		 N_("King of the Hill"),
		 9,
		 {"Villages_1024", "Mountain_River_1024", "Normandie", "Platform", "Lone_Tower", "hoth", "industry", "burbs", "Hell", "Bridge_Wars", "hiesville", "20thCenturyTown"},
		 {GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_SMALL, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE},
		 1},
		/* territory control, pyspades' rules (Tc.c): maps where 16 bots
		   capture territories (the two 1024 maps: 3 to 26 captures in
		   300 to 600 s, 2 seeds); the classes are the host's option, off by
		   default. Busan_Port and 2Fort_CTF left: their bots spent too long
		   in the water */
		{"tc",
		 N_("Territory control"),
		 1,
		 {"Villages_1024", "Mountain_River_1024", "Anticbridge", "hiesville", "Bridge_Wars", "Platform", "Normandie", "industry", "killaman", "hoth", "burbs", "Hell"},
		 {GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE, GAME_MAP_BOTS_LARGE},
		 1},
		/* building only: no bots by default, nothing to fight */
		{"creative", N_("Creative"), 7, {"Creative_Flat", NULL, NULL}, {0, 0, 0}, 0},
	};
	return k >= 0 && k < GAME_MODES_COUNT ? &modes[k] : NULL;
}

/* how many maps the mode has */
static inline int game_mode_map_count(const struct game_mode_info* mode) {
	int n = 0;
	while(mode && n < GAME_MODE_MAPS && mode->maps[n])
		n++;
	return n;
}

/* the mode called name, NULL when there is none */
static inline const struct game_mode_info* game_mode_find(const char* name) {
	for(int k = 0; name && k < GAME_MODES_COUNT; k++) {
		if(!strcmp(game_mode_get(k)->name, name))
			return game_mode_get(k);
	}
	return NULL;
}

#endif
