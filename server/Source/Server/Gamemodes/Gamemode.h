// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GAMEMODE_H
#define GAMEMODE_H

/*
 * Game modes behind one interface. Each mode is one file in this folder that
 * fills a gamemode_ops_t (Ctf.c: CTF and Babel, Arena.c), registered in
 * Gamemode.c. The rest of the server never asks which mode is running: it
 * calls the gamemode_* functions below, which dispatch to the current mode
 * and fall back to a neutral default when the mode leaves a hook NULL.
 *
 * Adding a mode: a new file with its ops, one line in the registry, and its
 * name and maps in common/modes/modes.h (the client's picker). This is also
 * the layer Lua game modes will plug into: every hook here becomes an event
 * or a query a script can answer.
 *
 * Rule: current_gamemode and GAME_MODE_* are only read in this folder.
 */

#include <Server/BotGoal.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <stdint.h>
#include <tomlc99/toml.h>

/* the protocol 1.0 GameModeState keys a mode adds to the generic ones
   (mode, team names, colours, scores, capture-limit) */
#define GM_KEYS_MAX      24
#define GM_KEY_LEN       24
#define GM_KEY_VALUE_LEN 48

typedef struct
{
    int  count;
    char key[GM_KEYS_MAX][GM_KEY_LEN];
    char value[GM_KEYS_MAX][GM_KEY_VALUE_LEN];
} gm_keys_t;

/* appends key = printf(fmt, ...); an empty value is not sent */
void gm_keys_put(gm_keys_t* out, const char* key, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/* the goal a mode (or a plugin) gives a bot: Server/BotGoal.h */

/* what a player may do in this mode (gamemode_player_rules): the zombies'
   limits */
#define GM_RULE_NO_GUN      0x01 /* no gun: no shot, the gun tool refused */
#define GM_RULE_NO_GRENADE  0x02
#define GM_RULE_NO_BLOCK    0x04 /* no block put (the spade still digs) */
#define GM_RULE_HIGH_JUMP   0x08 /* GAME_ZOMBIE_JUMP_VELOCITY */
#define GM_RULE_NO_FALL     0x10 /* no fall damage */
#define GM_RULE_FAST        0x20 /* runs GAME_ZOMBIE_SPEED times as fast (the client predicts it) */
#define GM_RULE_NO_PERKS    0x40 /* earns and holds no perk (a zombie) */
#define GM_RULE_FLY         0x80 /* flies freely, no gravity (creative: player->flying, flight_free) */
#define GM_RULE_NO_DESTROY  0x100 /* nothing of the map may be dug, shot open or cratered (the arena) */
#define GM_RULE_NO_LIGHT    0x200 /* carries no flashlight (a zombie) */
#define GM_RULE_NIGHT_VISION 0x400 /* sees in the dark as by day (a zombie: its client draws night vision) */

/* the mechanics a mode adds to the 0.75 rules (gamemode_has). Classic CTF
   ("ctf") keeps the 0.75 game as it was and has none of them nor perks;
   ngCTF ("ngctf") is the same CTF with all of them */
#define GM_FEAT_ASSISTS     0x01 /* kill assists (Assists.c) */
#define GM_FEAT_SPOTTING    0x02 /* the ping on an enemy marks it (Bridge.c _spot) */
#define GM_FEAT_SQUAD_SPAWN 0x04 /* respawn next to a squadmate, the rally point */
#define GM_FEAT_CLASSES     0x08 /* Commando, Rocketeer, Marksman, Miner (Server/Classes.c) */
#define GM_FEAT_ONE_TEAM    0x10 /* everyone on team 1: a join or change to team 2 lands there (creative) */
#define GM_FEAT_NO_BOTS     0x20 /* no bot is made, the host's bot commands refused (creative) */
#define GM_FEAT_CLASS_PERKS 0x40 /* a class brings its own streak perk and its jetpack (ngCTF); elsewhere
                                   the mode's own perks apply (the zombie mode's jetpack after two
                                   kills) and a class is only the survivors' kit */

/* a mode's own perk rules over config.toml's [perks], one per P10_PERK_*
   (zombie: only the jetpack, after two kills) */
typedef struct
{
    uint8_t enabled;
    uint8_t points; /* progress needed */
    uint8_t kill;   /* progress per enemy kill */
} gm_perk_rule_t;

/* a mode's own supply crate rule (Proto10/Crates.c) over the default one,
   which only plays where the classes do: 15% of the kills made in a crowd
   of 4, landing within 8 blocks. The zombie mode drops them for the
   humans' kills, away from the killer so it has to go and get them */
typedef struct
{
    float    chance;    /* of a kill that counts */
    uint8_t  crowd;     /* living players within 12 blocks of the killer, it too */
    uint8_t  team;      /* only kills by this team (TEAM_A, TEAM_B), 255 any */
    float    near_dist, far_dist; /* it lands between these blocks from the killer */
    uint16_t cooldown;  /* seconds after a kill's drop before the next one */
    uint8_t  max_out;   /* the mode's crates out at once (kills only) */
    /* and every hot_every seconds (0: never) one where players died most
       lately (Crates.c's heat map), when a spot has hot_min deaths' heat;
       at most hot_max of those out at once */
    uint16_t hot_every;
    float    hot_min;
    uint8_t  hot_max;
} gm_crate_rule_t;

typedef struct gamemode_ops
{
    uint8_t     id;        /* gamemode_t */
    const char* name;      /* "ctf", "babel", "arena": --gamemode, server list */
    const char* wire_name; /* protocol 1.0 "mode": babel is drawn like "ctf" */
    uint8_t     perks;     /* the jetpack, the sentry and the MG exist */
    uint8_t     features;  /* GM_FEAT_*: the other new mechanics (none in classic CTF) */
    /* NULL: config.toml's [perks]; else P10_PERK_COUNT rules of the mode's own */
    const gm_perk_rule_t* perk_rules;
    /* NULL: the default crates, where the classes play; else the mode's own */
    const gm_crate_rule_t* crate_rule;

    /* the map's toml, before init (the arena's gates) */
    void (*read_map)(server_t* server, toml_table_t* map_toml);
    /* the map is loaded (first start, won match, /reset): place the objectives */
    void (*init)(server_t* server);
    /* the map is loaded: the mode's own time of day (the zombie mode plays
       at night). It goes through Sky.c's setters, so every client is told
       by the sky command and the bots see by it; the weather is left as
       config.toml says. Run after init, every map start, so an admin's
       /sky lasts one match; the next map without the mode's sky starts
       from config.toml's [sky] again (sky_map_start). NULL: that [sky] */
    /* returns 1 when it set the sky: the next map start puts config.toml's
       [sky] back (Sky.c's sky_map_start), so a mode's night never outlives it */
    int (*sky_start)(server_t* server);
    /* every server tick */
    void (*tick)(server_t* server);
    /* /extend: add seconds to a timed match; 1 when the mode did */
    int (*extend)(server_t* server, int seconds);
    /* every server tick for each READY player (intel pickup, capture, tent: a
       player running over the intel is on it for a fraction of a second) */
    void (*player_step)(server_t* server, player_t* player);
    /* once a second for each READY player */
    void (*player_tick)(server_t* server, player_t* player);
    /* the player stops playing for now: death, leaving, team change, bot removal */
    void (*player_out)(server_t* server, player_t* player);
    /* blocks were built (1) or destroyed (0): objectives follow the ground */
    void (*terrain_changed)(server_t* server, int built);

    /* 0 keeps a dead player waiting (an arena round) */
    int (*allows_respawn)(server_t* server, player_t* player);
    /* 1 for a cell nobody may build on or destroy */
    int (*cell_protected)(server_t* server, int x, int y, int z);
    /* the mode's own GameModeState keys */
    void (*state_keys)(server_t* server, gm_keys_t* out);
    /* the match summary's column of the mode's own (gamemode_match_won):
       its name on the wire ("captures", "hill") and each player's figure;
       NULL: none */
    const char* summary_stat;
    uint32_t (*player_stat)(server_t* server, player_t* player);
    /* 1 and a goal when the mode has an objective for this bot right now */
    int (*bot_goal)(server_t* server, player_t* bot, gm_bot_goal_t* out);
    /* where a bot with nothing better to do patrols around */
    void (*patrol_anchor)(server_t* server, player_t* bot, float* x, float* y);
    /* 1 while the player carries what its team must bring home (the CTF's
       intel): a bot carrying it throws nothing, puts nothing down and stops
       for no fight, and the enemy's defenders go for it first. NULL: no
       mode objective is ever carried */
    int (*player_carries)(server_t* server, player_t* player);
    /* 1 when the mode placed the spawning player itself (movement.position) */
    int (*spawn_point)(server_t* server, player_t* player);
    /* 1 and the map column a spawning player should face (the objective:
       the hill, the enemy intel); NULL or 0 faces the patrol anchor. A
       player who spawned facing away from the fight turned round first */
    int (*spawn_facing)(server_t* server, player_t* player, float* x, float* y);
    /* 1 and *out when the mode chooses the team a player joins with; 0 keeps
       the request. The player is still a spectator. */
    int (*join_team)(server_t* server, player_t* player, uint8_t requested, uint8_t* out);
    /* 1 and *out when the mode chooses the team for a change; *out == the
       player's current team refuses the change. */
    int (*team_change)(server_t* server, player_t* player, uint8_t requested, uint8_t* out);
    /* 1 when this player matters for the escape watch's entombed test
       (a zombie survivor, an intel carrier, a VIP); every mode may leave it
       NULL and no player is marked for being sealed away. */
    int (*escape_watch_player)(server_t* server, player_t* player);
    /* a 1.0 client entered the game: send it the mode's own state */
    void (*client_in_game)(server_t* server, player_t* player);
    /* a client's GenericCommand the server does not know ("squad 2"); 1 when
       the mode took it */
    int (*player_command)(server_t* server, player_t* player, const char* command, const char* arg);
    /* GM_RULE_* for this player now (0: everything a player may do) */
    unsigned (*player_rules)(server_t* server, player_t* player);
    /* 1 when a weapon change takes effect at once, without killing and
       respawning the player (the arena's countdown: everyone waits behind
       closed gates and pyspades lets the gun be picked before the round).
       NULL: a change kills and respawns, the 0.75 behaviour */
    int (*weapon_change_instant)(server_t* server, player_t* player);
    /* the damage a hit does (kill_reason: 0 body, 1 head, 2 melee, 3
       grenade, 4 fall or water), damage when NULL */
    long (*hit_damage)(server_t* server, player_t* attacker, player_t* victim, long damage, uint8_t kill_reason);
    /* killer killed victim (before the victim is out of play) */
    void (*player_killed)(server_t* server, player_t* killer, player_t* victim);
    /* 0 when the map does not suit the mode (zombie: only zombie maps); the
       rotation drops it at start-up, and an empty rotation takes the mode's
       catalog maps (common/modes/modes.h). NULL: every map */
    int (*map_fits)(const char* map_name);
    /* the host's choice for the classes (the zombie mode's): the mode turns
       them on or off and tells its clients. 1 when the mode took it, 0 when
       it has no such option */
    int (*set_classes)(server_t* server, int on);
} gamemode_ops_t;

/* bows a far goal out to the bot's own side (a half moon), see Gamemode.c */
void gamemode_flank(server_t* server, player_t* bot, gm_bot_goal_t* out);

/* the default bot goal: the nearest enemy alive (a mode's hook may fall back on it) */
int gamemode_hunt_nearest(server_t* server, player_t* bot, gm_bot_goal_t* out);

/* NULL for an unknown id or name */
const gamemode_ops_t* gamemode_ops_of(uint8_t id);
const gamemode_ops_t* gamemode_find(const char* name);
/* never NULL: the running mode (CTF before the first init) */
const gamemode_ops_t* gamemode_current(server_t* server);

/* lifecycle, called by Server.c */
void gamemode_read_map(server_t* server, uint8_t id, toml_table_t* map_toml);
void gamemode_init(server_t* server, uint8_t id);
void gamemode_tick(server_t* server);
/* 1 when the mode added the seconds to its clock; 0 when its match has no
   time limit (the command says so and changes nothing) */
int  gamemode_extend(server_t* server, int seconds);
/* a team won the match: everyone reloads, the next map starts */
void gamemode_match_won(server_t* server);

/* events */
void gamemode_player_step(server_t* server, player_t* player);
void gamemode_player_tick(server_t* server, player_t* player);
void gamemode_player_out(server_t* server, player_t* player);
void gamemode_terrain_changed(server_t* server, int built);

/* queries */
int     gamemode_allows_respawn(server_t* server, player_t* player);
uint8_t gamemode_cell_editable(server_t* server, int x, int y, int z);
uint8_t gamemode_grenade_can_destroy(server_t* server, vector3f_t pos);
int     gamemode_has_perks(server_t* server);
int     gamemode_has(server_t* server, unsigned feature);
/* the host's choice for the classes (the zombie mode's): 1 when the mode has
   the option and it was applied, 0 when it has none */
int     gamemode_set_classes(server_t* server, int on);
void    gamemode_state_keys(server_t* server, gm_keys_t* out);
int     gamemode_bot_goal(server_t* server, player_t* bot, gm_bot_goal_t* out);
void    gamemode_patrol_anchor(server_t* server, player_t* bot, float* x, float* y);
int     gamemode_player_carries(server_t* server, player_t* player);
int     gamemode_spawn_point(server_t* server, player_t* player);
void    gamemode_spawn_facing(server_t* server, player_t* player, float* x, float* y);
int     gamemode_join_team(server_t* server, player_t* player, uint8_t requested, uint8_t* out);
int     gamemode_team_change(server_t* server, player_t* player, uint8_t requested, uint8_t* out);
int     gamemode_escape_watch_player(server_t* server, player_t* player);
void    gamemode_client_in_game(server_t* server, player_t* player);
int     gamemode_player_command(server_t* server, player_t* player, const char* command, const char* arg);
unsigned gamemode_player_rules(server_t* server, player_t* player);
int      gamemode_weapon_change_instant(server_t* server, player_t* player);
long     gamemode_hit_damage(server_t* server, player_t* attacker, player_t* victim, long damage, uint8_t kill_reason);
void     gamemode_player_killed(server_t* server, player_t* killer, player_t* victim);
/* 1 when the map's toml lists the mode in [map] modes (the maps repository
   tags each map with the modes its bots play, tools/tag_maps.py) */
int      gamemode_map_tagged(const char* map_name, const char* mode);

#endif
