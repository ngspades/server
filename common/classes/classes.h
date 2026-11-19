// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The classes of the next gen CTF (ngctf), after Jagex's Ace of Spades
	(2012): Commando, Rocketeer, Marksman, Miner. Shared by the server (what
	a player of a class is: health, speed, jump, gun, blocks, gear) and the
	client (the class screen, the body each class wears, and the prediction
	of its own speed and jump, which must match the server's physics).

	Header only like common/modes: functions, so a file that includes it
	and uses nothing gets no unused-variable warning (the server builds with
	-Werror). The table a program plays with is one of its own,
	game_class_table, made from the defaults (game_class_default) in one .c
	file of each (server Classes.c, client classes.c; not here behind a
	macro: the server's unity build had included this already). The server owns the figures: config.toml's [classes.<name>]
	and the plugins change them (game_class_set, by key), and every client
	is told with "class-info" (below); a client's table only ever follows.

	Wire (GenericCommands, docs/protocol-1.0.md):
	  server -> client  "classes 1"            the mode has classes (sent when
	                                           a 1.0 client enters the game)
	  server -> client  "class <player> <n>"   a player's class, to everyone,
	                                           on every change and to a client
	                                           entering the game for each player
	  client -> server  "class <n>"            pick a class: applied at the
	                                           next spawn (a living player
	                                           respawns, like a weapon change)
	  client -> server  "join <team> <weapon> <class>"  the class rides the
	                                           join as a fourth part (old
	                                           clients send three: Commando)
	  server -> client  "class-info <n> <key> <value>"  one of a class's
	                                           figures (game_class_set's keys):
	                                           all of them when a client enters
	                                           the game (after "classes 1"), a
	                                           changed one on a change (a client
	                                           keeps six parts of a command)
	The class decides the gun: a client infers a player's weapon from its
	class in a classes mode (game_class_get(n)->weapon), so the rocket
	launcher needs no new weapon parameters on the wire.
*/

#ifndef SHARED_CLASSES_H
#define SHARED_CLASSES_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N_
#define N_(s) s
#endif

#define GAME_CLASSES_COUNT 4

enum game_class_id {
	GAME_CLASS_COMMANDO = 0,
	GAME_CLASS_ROCKETEER = 1,
	GAME_CLASS_MARKSMAN = 2,
	GAME_CLASS_MINER = 3,
};

/* the guns: 0..2 are the 0.75 ones (WEAPON_RIFLE, WEAPON_SMG,
   WEAPON_SHOTGUN on both sides), 3 is the Commando's rocket launcher, 4
   the revolver, the second gun classes share (a hitscan like the rifle) */
#define GAME_WEAPON_ROCKET   3
#define GAME_WEAPON_REVOLVER 4
#define GAME_WEAPONS         5
/* the revolver's figures (both sides): six in the cylinder, a shot every
   0.4 s, a 2 s reload; a head shot 80, the body 45, arms and legs 30 */
/* the Marksman's sniper (a classed player's rifle) hits for this on the body
   and limbs, BattleSpades' stock sniper (torso 50, head 175 = a kill): two
   hits kill a 100 hp body, where the classic rifle's 49 left it at 2 */
#define GAME_SNIPER_BODY 50
/* and plays by BattleSpades' sniper table (docs/WEAPONS_RETAIL.md): one round
   in the chamber, 7 in reserve, a shot a second, a 2 s reload. Only where
   classes play (ngCTF, the zombie mode with classes): the classic rifle of
   classic CTF and the 0.75 profile is 10 + 50, 0.5 s, 2.5 s as ever. */
#define GAME_SNIPER_CLIP       1
#define GAME_SNIPER_RESERVE    7
#define GAME_SNIPER_DELAY_MS   1000
#define GAME_SNIPER_RELOAD_MS  2000

#define GAME_REVOLVER_CLIP      6
#define GAME_REVOLVER_RESERVE   36
#define GAME_REVOLVER_DELAY_MS  400
/* a gun taken out fires after this (WeaponParameters.raiseTime, every gun):
   the client waits it, the 1.0 server refuses a shot sooner */
#define GAME_WEAPON_RAISE_MS 500
#define GAME_REVOLVER_RELOAD_MS 2000
#define GAME_REVOLVER_HEAD      80
#define GAME_REVOLVER_TORSO     45
#define GAME_REVOLVER_LIMB      30

/* the gear in the grenade slot (tool 3) */
enum game_class_gear {
	GAME_GEAR_GRENADES = 0, /* the usual grenades */
	GAME_GEAR_DRILL = 1,    /* the Miner's drill gun: fire bores a tunnel */
};

/* the streak perk of each class (earned like ngCTF's perks, kill points);
   the Rocketeer's jetpack is not a perk: it wears one always */
enum game_class_perk {
	GAME_PERK_NONE = 0,
	GAME_PERK_SENTRY = 1,   /* the sentry gun (Rocketeer) */
	GAME_PERK_LANDMINE = 2, /* a mine that blows up under an enemy (Marksman) */
	GAME_PERK_DYNAMITE = 3, /* a charge that blows a crater after a fuse (Miner) */
	GAME_PERK_MG = 4,       /* a machine gun emplacement, manned (Commando) */
	/* the revive is gone (the MG took its place): kept only until the
	   client's class screen names GAME_PERK_MG */
	GAME_PERK_REVIVE = GAME_PERK_MG,
};

struct game_class_info {
	const char* name;  /* wire and config name: "commando" */
	const char* title; /* shown in menus */
	const char* blurb; /* one line on the class screen */
	int health;        /* the most it has; the wire keeps 0..100 and the
	                      server scales every hit by 100 / health (like the
	                      zombie's GAME_ZOMBIE_HEALTH), the HUD shows
	                      hp * health / 100 */
	float speed;       /* ground speed, times the usual (sprint etc. on top) */
	float jump;        /* the jump's upward speed (the usual is -0.36; z grows
	                      downwards; the height goes with its square) */
	int weapon;        /* 0 rifle, 1 SMG, 2 shotgun, GAME_WEAPON_ROCKET */
	int blocks;        /* blocks a life starts with (retail start; the
	                      usual 50; the wallet's size is blocks_max) */
	int gear;          /* game_class_gear */
	int gear_count;    /* grenades, or drill charges */
	int jetpack;       /* wears the jetpack from every spawn */
	int dig;           /* the spade's dig: 1 the usual three cells, 3 a 3 x 3 x 3 */
	int perk;          /* game_class_perk */
	float swing;       /* seconds between two spade swings (the left button;
	                      the right one's dig is dig_delay) */
	int secondary;     /* a second gun swapped with the first ("gun-swap"), -1 none */
	int enabled;       /* 0: nobody may pick it (the server's choice) */
	int block_damage;  /* what a pellet of its guns does to a block, of the
	                      100 that break it; 0 the gun's own (rifle 50, SMG
	                      34, shotgun 20 a pellet). The Miner's 100: every
	                      pellet breaks the block it hits (GAME_BLOCK_BREAK) */
	int spawn_perk;    /* 1: every life starts with one charge of its perk (the
	                      Marksman's landmine); the streak still earns one */
	float dig_delay;   /* seconds between two right button digs (the three
	                      cell or 3 x 3 x 3 one); GAME_DIG_DELAY the usual */
	/* the retail (first Steam Ace of Spades) figures below: what a life of
	   the class plays like, sent on the wire and played by both sides */
	float sprint;      /* replaces the walk speed on the ground while
	                      sprinting (retail CLASS_SPRINT_MULTIPLIER): a
	                      classed player's sprint is not the classic x1.3 on
	                      top of its walk, it IS the walk; <= 0 keeps the
	                      walk speed (no sprint boost) */
	float water_friction; /* the figure of the water friction divisor
	                      (the engine's 6); the larger, the slower in water */
	float headshot;    /* what a shot to the head does to this class's
	                      bodies, times: its victims take that, whatever the
	                      shooter's class. 1.0 the usual */
	int   melee;       /* the damage of its spade (or pickaxe) swing, of the
	                      classic 80; the class's swing paces it */
	float fall_min;    /* falling hurts past this many blocks of fall (the
	                      distance flown down, not the landing speed) */
	float fall_max;    /* at this distance the fall takes its full damage */
	float fall_dmg;    /* the damage at fall_max and beyond */
	float fall_water;  /* a fall into water damages less: the damage times */
	int   blocks_max;  /* what the block wallet holds (the restock fills to
	                      it); blocks above is what a life starts with */
	char title_own[32]; /* a title or line the server set (title, blurb point here) */
	char blurb_own[96];
};

/* the usual spade swing (0.75: 0.2 s); the Miner's is quicker */
#define GAME_SWING 0.2f
/* the usual right button dig (0.75: one a second, the server's
   THREEBLOCK_DELAY); the Miner's is quicker */
#define GAME_DIG_DELAY 1.0f

/* a block's health: the damage that breaks it. On protocol 1.0 the server
   keeps it per cell for the guns (BlockHealth.c) and tells every client
   ("block-damage"); on a 0.75/0.76 server, and for the spade everywhere,
   the client counts it and sends the hit that breaks. A block_damage of
   this is one shot */
#define GAME_BLOCK_BREAK 100
/* a crack nobody touched for this long is forgotten: the client stops
   drawing it (map.c) and the server takes the block as whole again, so
   both sides heal it together */
#define GAME_BLOCK_HEAL_SECONDS 10

/* what one pellet of a gun does to a block, of GAME_BLOCK_BREAK (a class's
   block_damage replaces it): the same table on both sides, so the crack the
   client predicts is the one the server keeps */
static inline int game_weapon_block_damage(int weapon) {
	switch(weapon) {
		case 0: return 50;                   /* rifle: two shots */
		case 1: return 34;                   /* SMG: three */
		case 2: return 20;                   /* shotgun, a pellet */
		case GAME_WEAPON_REVOLVER: return 34; /* like the SMG */
		default: return 0;
	}
}

/* The Miner's drill bit (server Proto10/Drills.c, client rockets.c): fired
   along the look at GAME_DRILL_SPEED blocks a second, spinning; in the air
   it falls (GAME_DRILL_GRAVITY, z grows downwards: the Gravity trajectory),
   in the ground it bores straight on, a 3 x 3 hole round its way, losing
   GAME_DRILL_DRAG of its speed a block, until it has bored GAME_DRILL_DEPTH
   blocks, slowed under GAME_DRILL_MIN_SPEED or flown GAME_DRILL_LIFETIME.
   The speed, the fall and the life are BattleSpades' figures (its weapon
   catalog: DRILL_FLYING_SPEED 40, DRILL_GRAVITY_MULTIPLIER 1.5 on the
   retail projectile gravity of 30, DRILL_LIFESPAN 3); 28, 24 and 4 s left
   the bit a slow, floating thing. Classes are protocol 1.0's alone, so the
   0.75 profile never reads any of these. */
#define GAME_DRILL_SPEED     40.0f
#define GAME_DRILL_GRAVITY   45.0f
#define GAME_DRILL_DEPTH     12
#define GAME_DRILL_DRAG      0.04f
#define GAME_DRILL_MIN_SPEED 4.0f
#define GAME_DRILL_LIFETIME  3.0f

/* the k-th class as it ships (0..GAME_CLASSES_COUNT-1), NULL past the
   end. The first Steam Ace of Spades' class profile (shared/constants.py's
   CLASS_* tables and class_data.py's loadouts, ported 2026-11): the walk is
   the one accel multiplier 0.7 (5.6 blocks a second) and the sprint REPLACES
   it (Commando 1.4, Marksman 1.45, Miner 1.4, Rocketeer 1.1); the jump is
   the retail jump multiplier x 0.36; the health is what the class effectively
   has (the wire scales every hit by 100 / health); the headshot is what its
   bodies take there; falls hurt by the distance flown down, linear between
   fall_min and fall_max, a water landing falled by fall_water; water drags
   by water_friction; the wallet starts with blocks and holds blocks_max
   (the restock fills to it, the dig gives blocks back); the swing is the
   class's melee tool's interval and melee its damage: BattleSpades' retail
   tools, the pickaxe (40, every 0.6 s) for the Commando, Rocketeer and
   Marksman, the Miner's superspade (50, 0.6 s) */
static inline const struct game_class_info* game_class_default(int k) {
	static const struct game_class_info classes[GAME_CLASSES_COUNT] = {
		{.name = "commando", .title = N_("Commando"), .blurb = N_("Tough, slow, a rocket launcher"),
		 .health = 100, .speed = 0.7f, .jump = -0.432f, .weapon = GAME_WEAPON_ROCKET,
		 .blocks = 200, .gear = GAME_GEAR_GRENADES, .gear_count = 4, .jetpack = 0, .dig = 1,
		 .perk = GAME_PERK_MG, .swing = 0.6f, .secondary = GAME_WEAPON_REVOLVER, .enabled = 1,
		 .block_damage = 0, .spawn_perk = 0, .dig_delay = GAME_DIG_DELAY,
		 .sprint = 1.4f, .water_friction = 8.0f, .headshot = 1.0f, .melee = 40,
		 .fall_min = 10, .fall_max = 40, .fall_dmg = 100, .fall_water = 0.5f, .blocks_max = 1000,
		 .title_own = "", .blurb_own = ""},
		{.name = "rocketeer", .title = N_("Rocketeer"), .blurb = N_("Fast, an SMG and a jetpack"),
		 .health = 70, .speed = 0.7f, .jump = -0.36f, .weapon = 1,
		 .blocks = 500, .gear = GAME_GEAR_GRENADES, .gear_count = 2, .jetpack = 1, .dig = 1,
		 .perk = GAME_PERK_SENTRY, .swing = 0.6f, .secondary = -1, .enabled = 1,
		 .block_damage = 0, .spawn_perk = 0, .dig_delay = GAME_DIG_DELAY,
		 .sprint = 1.1f, .water_friction = 12.0f, .headshot = 1.5f, .melee = 40,
		 .fall_min = 10, .fall_max = 10, .fall_dmg = 10, .fall_water = 0.5f, .blocks_max = 1500,
		 .title_own = "", .blurb_own = ""},
		{.name = "marksman", .title = N_("Marksman"), .blurb = N_("The fastest and highest jumper, a rifle, little health"),
		 .health = 70, .speed = 0.7f, .jump = -0.54f, .weapon = 0,
		 .blocks = 400, .gear = GAME_GEAR_GRENADES, .gear_count = 2, .jetpack = 0, .dig = 1,
		 .perk = GAME_PERK_LANDMINE, .swing = 0.6f, .secondary = GAME_WEAPON_REVOLVER, .enabled = 1,
		 .block_damage = 0, .spawn_perk = 1, .dig_delay = GAME_DIG_DELAY,
		 .sprint = 1.45f, .water_friction = 8.0f, .headshot = 1.5f, .melee = 40,
		 .fall_min = 10, .fall_max = 30, .fall_dmg = 100, .fall_water = 0.5f, .blocks_max = 1000,
		 .title_own = "", .blurb_own = ""},
		{.name = "miner", .title = N_("Miner"), .blurb = N_("A shotgun, a 3x3 spade, a drill and many blocks"),
		 .health = 85, .speed = 0.7f, .jump = -0.432f, .weapon = 2,
		 .blocks = 0, .gear = GAME_GEAR_DRILL, .gear_count = 5, .jetpack = 0, .dig = 3,
		 .perk = GAME_PERK_DYNAMITE, .swing = 0.6f, .secondary = -1, .enabled = 1,
		 .block_damage = GAME_BLOCK_BREAK, .spawn_perk = 0, .dig_delay = 0.7f,
		 .sprint = 1.4f, .water_friction = 8.0f, .headshot = 0.5f, .melee = 50,
		 .fall_min = 10, .fall_max = 40, .fall_dmg = 100, .fall_water = 0.5f, .blocks_max = 1000,
		 .title_own = "", .blurb_own = ""},
	};
	return (k >= 0 && k < GAME_CLASSES_COUNT) ? &classes[k] : NULL;
}

/* the program's own table (above) */
struct game_class_info* game_class_table(void);

/* the k-th class as it plays now, NULL past the end */
static inline const struct game_class_info* game_class_get(int k) {
	return (k >= 0 && k < GAME_CLASSES_COUNT) ? game_class_table() + k : NULL;
}

/* a copy whose title and line stay its own */
static inline void game_class_copy(struct game_class_info* dst, const struct game_class_info* src) {
	*dst = *src;
	if(src->title == src->title_own)
		dst->title = dst->title_own;
	if(src->blurb == src->blurb_own)
		dst->blurb = dst->blurb_own;
}

/* every class back to how it ships */
static inline void game_class_reset(void) {
	for(int k = 0; k < GAME_CLASSES_COUNT; k++)
		game_class_copy(game_class_table() + k, game_class_default(k));
}


/* -1 for an unknown name; a number "0".."3" is the class too */
static inline int game_class_find(const char* name) {
	if(!name)
		return -1;
	for(int k = 0; k < GAME_CLASSES_COUNT; k++)
		if(!strcmp(game_class_default(k)->name, name))
			return k;
	if(name[0] >= '0' && name[0] < '0' + GAME_CLASSES_COUNT && name[1] == '\0')
		return name[0] - '0';
	return -1;
}

/* the keys of a class's figures, in the order "class-info" sends them */
#define GAME_CLASS_KEYS 27
static inline const char* game_class_key(int k) {
	static const char* keys[GAME_CLASS_KEYS] = {"health", "speed",  "jump",  "weapon", "blocks", "gear",  "gear_count",
												"jetpack", "dig",   "perk",  "swing",  "title",  "blurb", "enabled", "secondary",
												"block_damage", "spawn_perk", "dig_delay", "sprint",
"water_friction", "headshot", "melee", "fall_min", "fall_max", "fall_dmg",
"fall_water", "blocks_max"};
	return (k >= 0 && k < GAME_CLASS_KEYS) ? keys[k] : NULL;
}

static inline int game_class_word(const char* const* words, int n, const char* value) {
	for(int k = 0; k < n; k++)
		if(!strcmp(words[k], value))
			return k;
	char* end;
	long v = strtol(value, &end, 10);
	return (*value && !*end && v >= 0 && v < n) ? (int)v : -1;
}

static const char* const game_class_weapons[GAME_WEAPONS] = {"rifle", "smg", "shotgun", "rocket", "revolver"};
static const char* const game_class_gears[2] = {"grenades", "drill"};
static const char* const game_class_perks[5] = {"none", "sentry", "landmine", "dynamite", "mg"};

/* Set one figure from its text: 1 when taken, 0 when the key is unknown or
   the value out of its range (then nothing changes). The ranges keep a
   server's or a plugin's figures playable: health 1..1000 (the hits are
   scaled by 100 / health), speed 0.3..3, jump 0.1..1.5 (the upward speed,
   0.36 the usual, a negative one taken as is), blocks 0..65535 (what a life
   starts with), blocks_max 0..65535 (what the wallet holds), melee 0..500
   (the swing's damage), gear_count 0..255, dig 1 or 3, block_damage 0..100,
   swing 0.05..2 s, dig_delay 0.2..3 s, sprint 0..3 (0 keeps the walk speed),
   water_friction 1..30, headshot 0.1..4, fall_min/fall_max 0..63 (the fall's
   distance in blocks; fall_min == fall_max keeps: from fall_min on the full
   damage), fall_dmg 1..200, fall_water 0..1, a title of 31 bytes and a line
   of 95; weapon rifle|smg|shotgun|rocket|revolver, secondary the same or
   none, gear grenades|drill, perk
   none|sentry|landmine|dynamite|mg, jetpack, enabled and spawn_perk
   0|1|true|false */
static inline int game_class_set(struct game_class_info* c, const char* key, const char* value) {
	char* end;
	if(!c || !key || !value)
		return 0;
	if(!strcmp(key, "title") || !strcmp(key, "blurb")) {
		int title = key[0] == 't';
		size_t size = title ? sizeof(c->title_own) : sizeof(c->blurb_own);
		if(!*value || strlen(value) >= size)
			return 0;
		strcpy(title ? c->title_own : c->blurb_own, value);
		if(title)
			c->title = c->title_own;
		else
			c->blurb = c->blurb_own;
		return 1;
	}
	if(!strcmp(key, "jetpack") || !strcmp(key, "enabled") || !strcmp(key, "spawn_perk")) {
		int v = !strcmp(value, "1") || !strcmp(value, "true") ? 1 : (!strcmp(value, "0") || !strcmp(value, "false") ? 0 : -1);
		if(v < 0)
			return 0;
		*(key[0] == 'j' ? &c->jetpack : key[0] == 'e' ? &c->enabled : &c->spawn_perk) = v;
		return 1;
	}
	if(!strcmp(key, "dig_delay")) {
		float v = strtof(value, &end);
		if(!*value || *end || v < 0.2f || v > 3.0f)
			return 0;
		c->dig_delay = v;
		return 1;
	}
	if(!strcmp(key, "secondary")) {
		int v = !strcmp(value, "none") || !strcmp(value, "-1") ? -1 : game_class_word(game_class_weapons, GAME_WEAPONS, value);
		if(v < -1 || (v < 0 && strcmp(value, "none") && strcmp(value, "-1")))
			return 0;
		c->secondary = v;
		return 1;
	}
	if(!strcmp(key, "weapon") || !strcmp(key, "gear") || !strcmp(key, "perk")) {
		int v = key[0] == 'w' ? game_class_word(game_class_weapons, GAME_WEAPONS, value)
			: key[0] == 'g'   ? game_class_word(game_class_gears, 2, value)
							  : game_class_word(game_class_perks, 5, value);
		if(v < 0)
			return 0;
		*(key[0] == 'w' ? &c->weapon : key[0] == 'g' ? &c->gear : &c->perk) = v;
		return 1;
	}
	if(!strcmp(key, "speed") || !strcmp(key, "jump") || !strcmp(key, "swing")) {
		float v = strtof(value, &end);
		if(!*value || *end)
			return 0;
		if(key[0] == 'j' && v > 0.0f)
			v = -v; /* the upward speed, z grows downwards */
		if((key[1] == 'p' && (v < 0.3f || v > 3.0f)) || (key[0] == 'j' && (v < -1.5f || v > -0.1f))
		   || (key[1] == 'w' && (v < 0.05f || v > 2.0f)))
			return 0;
		*(key[1] == 'p' ? &c->speed : key[0] == 'j' ? &c->jump : &c->swing) = v;
		return 1;
	}
	/* the retail figures: the sprint that replaces the walk on the ground, a
	   water drag's figure, the head shot taken, the swing's damage and the
	   fall curve by the distance flown down (fall_min == fall_max keeps:
	   from fall_min on every fall takes all of fall_dmg) */
	if(!strcmp(key, "sprint") || !strcmp(key, "water_friction") || !strcmp(key, "headshot")
	   || !strcmp(key, "fall_min") || !strcmp(key, "fall_max") || !strcmp(key, "fall_dmg")
	   || !strcmp(key, "fall_water")) {
		float v = strtof(value, &end);
		if(!*value || *end)
			return 0;
		float low = 0.0f, high = 3.0f; /* sprint: 0 keeps the walk speed */
		if(!strcmp(key, "water_friction")) {
			low = 1.0f;
			high = 30.0f;
		} else if(!strcmp(key, "headshot")) {
			low  = 0.1f;
			high = 4.0f;
		} else if(!strcmp(key, "fall_min") || !strcmp(key, "fall_max")) {
			high = 63.0f;
		} else if(!strcmp(key, "fall_dmg")) {
			low  = 1.0f;
			high = 200.0f;
		} else if(!strcmp(key, "fall_water")) {
			high = 1.0f;
		}
		if(v < low || v > high)
			return 0;
		*(strcmp(key, "sprint") == 0              ? &c->sprint
		      : strcmp(key, "water_friction") == 0 ? &c->water_friction
		      : strcmp(key, "headshot") == 0       ? &c->headshot
		      : strcmp(key, "fall_min") == 0       ? &c->fall_min
		      : strcmp(key, "fall_max") == 0       ? &c->fall_max
		      : strcmp(key, "fall_dmg") == 0       ? &c->fall_dmg
		                                           : &c->fall_water) = v;
		return 1;
	}
	long v = strtol(value, &end, 10);
	if(!*value || *end)
		return 0;
	if(!strcmp(key, "health") && v >= 1 && v <= 1000)
		c->health = (int)v;
	else if(!strcmp(key, "blocks") && v >= 0 && v <= 65535)
		c->blocks = (int)v;
	else if(!strcmp(key, "blocks_max") && v >= 0 && v <= 65535)
		c->blocks_max = (int)v;
	else if(!strcmp(key, "melee") && v >= 0 && v <= 500)
		c->melee = (int)v;
	else if(!strcmp(key, "gear_count") && v >= 0 && v <= 255)
		c->gear_count = (int)v;
	else if(!strcmp(key, "dig") && (v == 1 || v == 3))
		c->dig = (int)v;
	else if(!strcmp(key, "block_damage") && v >= 0 && v <= GAME_BLOCK_BREAK)
		c->block_damage = (int)v;
	else
		return 0;
	return 1;
}

/* one figure as text (what game_class_set takes back); 0 for an unknown key */
static inline int game_class_value(const struct game_class_info* c, const char* key, char* out, size_t size) {
	if(!c || !key || !out || !size)
		return 0;
	if(!strcmp(key, "health"))
		snprintf(out, size, "%d", c->health);
	else if(!strcmp(key, "speed"))
		snprintf(out, size, "%.3f", c->speed);
	else if(!strcmp(key, "jump"))
		snprintf(out, size, "%.3f", c->jump);
	else if(!strcmp(key, "weapon"))
		snprintf(out, size, "%s", c->weapon >= 0 && c->weapon < GAME_WEAPONS ? game_class_weapons[c->weapon] : "rifle");
	else if(!strcmp(key, "secondary"))
		snprintf(out, size, "%s", c->secondary >= 0 && c->secondary < GAME_WEAPONS ? game_class_weapons[c->secondary] : "none");
	else if(!strcmp(key, "blocks"))
		snprintf(out, size, "%d", c->blocks);
	else if(!strcmp(key, "gear"))
		snprintf(out, size, "%s", c->gear == GAME_GEAR_DRILL ? "drill" : "grenades");
	else if(!strcmp(key, "gear_count"))
		snprintf(out, size, "%d", c->gear_count);
	else if(!strcmp(key, "jetpack"))
		snprintf(out, size, "%d", c->jetpack);
	else if(!strcmp(key, "dig"))
		snprintf(out, size, "%d", c->dig);
	else if(!strcmp(key, "block_damage"))
		snprintf(out, size, "%d", c->block_damage);
	else if(!strcmp(key, "perk"))
		snprintf(out, size, "%s", c->perk >= 0 && c->perk < 5 ? game_class_perks[c->perk] : "none");
	else if(!strcmp(key, "swing"))
		snprintf(out, size, "%.3f", c->swing);
	else if(!strcmp(key, "title"))
		snprintf(out, size, "%s", c->title);
	else if(!strcmp(key, "blurb"))
		snprintf(out, size, "%s", c->blurb);
	else if(!strcmp(key, "enabled"))
		snprintf(out, size, "%d", c->enabled);
	else if(!strcmp(key, "spawn_perk"))
		snprintf(out, size, "%d", c->spawn_perk);
	else if(!strcmp(key, "dig_delay"))
		snprintf(out, size, "%.3f", c->dig_delay);
	else if(!strcmp(key, "sprint"))
		snprintf(out, size, "%.3f", c->sprint);
	else if(!strcmp(key, "water_friction"))
		snprintf(out, size, "%.3f", c->water_friction);
	else if(!strcmp(key, "headshot"))
		snprintf(out, size, "%.3f", c->headshot);
	else if(!strcmp(key, "melee"))
		snprintf(out, size, "%d", c->melee);
	else if(!strcmp(key, "fall_min"))
		snprintf(out, size, "%.3f", c->fall_min);
	else if(!strcmp(key, "fall_max"))
		snprintf(out, size, "%.3f", c->fall_max);
	else if(!strcmp(key, "fall_dmg"))
		snprintf(out, size, "%.3f", c->fall_dmg);
	else if(!strcmp(key, "fall_water"))
		snprintf(out, size, "%.3f", c->fall_water);
	else if(!strcmp(key, "blocks_max"))
		snprintf(out, size, "%d", c->blocks_max);
	else
		return 0;
	return 1;
}

#endif
