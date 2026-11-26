// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    The bots' shared internals: the per bot state (bot_ai_t), the tuning
    constants and the functions one part of the AI calls in another
    (Bots.c, BotSenses.c, BotNav.c, BotBlocks.c, BotCombat.c, BotRoles.c,
    BotThink.c). Only those files include it; the server sees Bots.h.
*/

#ifndef BOTS_INTERNAL_H
#define BOTS_INTERNAL_H

#include <Server/Gamemodes/Gamemode.h>
#include <Server/BotPlanner.h>
#include <Server/Bots.h>
#include <Server/Perks.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/Sentry.h>
#include <Server/MapMeta/MapMeta.h>
#include <Server/EventLog.h>
#include <Server/Packets/Packets.h>
#include <Util/Checks/PlayerChecks.h>
#include <Util/Checks/PositionChecks.h>
#include <Server/Packets/ReceivePackets.h>
#include <Server/Player.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Alloc.h>
#include <Util/Checks/TimeChecks.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Physics.h>
#include <uthash/uthash.h>
#include <uthash/utlist.h>
#include <Util/Weapon.h>
#include <math.h>
#include <names/names.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tomlc99/toml.h>

#define BOT_MAX_PLAYERS    256 /* any player id: 128 player servers have bots past id 31 */
#define BOT_THINK_DELAY    (NANO_IN_SECOND / 5)   // 5 logic ticks per second
/* acquire range: a player's, the fog's end ([server] fog_distance, 128 by
   default; 160 against a 128 fog saw past it) */
#define BOT_VIEW_DISTANCE  ((float) get_server()->fog_distance)
#define BOT_MAX_SHOT_RANGE 80.0f
#define BOT_ROCKET_RANGE   50.0f /* the rocket flies straight, 40 blocks a second */
#define BOT_REVOLVER_RANGE 40.0f                  // the revolver (a class's second gun) fires this far
#define BOT_ROCKET_MIN     7.0f  /* closer, its own blast would come back: the spade or nothing */
#define BOT_REVOLVER_DRAW  25.0f /* the Commando fights with the revolver this close, the rocket past it */
#define BOT_SIDEARM_DRAW   12.0f /* the Marksman fights with the revolver this close, the rifle past it */
/* the jetpack (_bot_jetpack): the flights a bot takes */
#define BOT_JET_GOAL        1
#define BOT_JET_PERCH       2
#define BOT_JET_BRAKE       3
#define BOT_JET_GOAL_R      20.0f /* it flies to a goal 3+ levels up this close (12 left most unflown) */
#define BOT_JET_TIMEOUT     6.0f  /* seconds a flight may take (in the air it moves slowly) */
#define BOT_PERCH_R         10    /* a roof it flies onto is this close (14 timed out: slow in the air) */
#define BOT_PERCH_EVERY     8.0f  /* seconds between looks for a roof */
#define BOT_PERCH_EVERY_GOAL 4.0f /* a goal with a perch radius (gm_bot_goal_t.perch) looks for one this often: the roof is where its work is */
#define BOT_PERCH_HOLD      12.0f /* a jetpack wearer on a roof on its way watches it this long (a fight from up there
                                     re-arms it: the Rocketeer holds the high ground, not the street) */
#define BOT_PERCH_HOLD_GOAL 60.0f /* and stays on one over the goal this long, renewed while it is still up there */
#define BOT_PERCH_FUEL      0.7f  /* a jetpack wearer takes off on this share of a tank (a climb takes ~1 s of the 1.333,
                                     and it refills in 10 s: at 0.9 it flew once a refill and walked the rest of the way) */
#define BOT_PERCH_GOAL      25.0f /* no roof this close to its goal: the goal first */
#define BOT_JET_BRAKE_FALL  0.45f /* falling faster it thrusts (the fall hurts from 0.58) */
#define BOT_FIRE_STRETCH   1.6f                   // shots go this far past a gun's range (rarer)
#define BOT_SNAP_CARRIER   30.0f                  // a carrier (_bot_carries) only shoots back this close
#define BOT_SNAP_SNEAK     35.0f                  // and the infiltrator only this close
#define BOT_WORK_GUN       1.5f                   // seconds after the last block or dig the gun is back
#define BOT_TURN_WALK      200.0f                 // degrees a second a bot turns on its way (540 read as swinging about)
#define BOT_TURN_AIM_MIN   260.0f                 // and onto an enemy, the weakest bot
#define BOT_TURN_AIM_MAX   720.0f                 // and the best
#define BOT_ADS_MIN        12.0f                  // a target farther than this is shot down the sights
#define BOT_ADS_HOLD       1.5f                   // seconds the sights stay up after the last shot asked
#define BOT_FIRE_ALIGN     0.9986f                // cos(3 deg): it fires only once it looks there
#define BOT_ENGAGE_KEEP    1.0f                   // seconds it stays stopped for an enemy out of sight
#define BOT_ENGAGE_MAX     4.0f                   // and at most this long in a row
#define BOT_ENGAGE_COOL    3.0f                   // then it walks on this long before stopping again
#define BOT_PUSH_FIGHT     30.0f                  // an attacker on its way stops to fight only this close
#define BOT_NADE_MIN       8.0f                   // grenades at enemies this far
#define BOT_NADE_MAX       30.0f                  // to this far
#define BOT_NADE_EVERY     20.0f                  // seconds between two grenades of one bot
#define BOT_NADE_SELF      15                     // the most damage a throw may do to the thrower
#define BOT_MELEE_REACH    2.2f                   // an enemy this close gets the spade
#define BOT_BREAK_RANGE    6.0f                   // a hidden enemy this close: dig through its cover
#define BOT_NADE_DAMAGE    40                     // the least damage a throw must promise

/* Aim error in radians, uniform in +-spread/2: a floor even the best bot has,
   plus a part that shrinks with skill, all growing with distance. */
#define BOT_AIM_SPREAD_MIN   0.05f
#define BOT_AIM_SPREAD_SKILL 0.10f
#define BOT_AIM_SPREAD_DIST  40.0f

/* The body a bot's shot must cross, copied from the client's hit boxes
   (the client's player.c, box_head/box_torso/box_leg drawn from eye + 0.85):
   offsets from position.z, which grows downwards and sits at the hip. */
#define BOT_BODY_TOP          -1.45f
#define BOT_BODY_NECK         -0.85f
#define BOT_BODY_CENTER       -0.40f /* where a bot aims: the chest */
#define BOT_BODY_HIP          0.05f
#define BOT_BODY_FEET         1.25f
#define BOT_BODY_FEET_CROUCH  0.35f
#define BOT_BODY_HALF_WIDTH   0.35f
#define BOT_HEAD_HALF_WIDTH   0.30f
#define BOT_TARGET_MEMORY  4.0f                   // seconds an unseen target is kept
#define BOT_WALL_MIN_DIST  6.0f                   // closer than this a wall is pointless
#define BOT_REBUILD_DELAY  8.0f                   // seconds between looks for new cover
#define BOT_WALL_CHANCE    0.15f                  // no cover around: how often it builds one (0.3 filled the map)
#define BOT_CLOSE_FIGHT    10.0f                  // closer than this it attacks, no cover
#define BOT_COVER_RADIUS   4.0f                   // how far it goes for cover that is there
#define BOT_BLOCK_WAIT     (NANO_IN_MILLI * 550)  // just over BLOCK_DELAY
#define BOT_DIG_WAIT       (NANO_IN_MILLI * 1050) // just over THREEBLOCK_DELAY
#define BOT_DODGE_SPAN     1.2f                   // how far a bot strafes from its cover spot
#define BOT_WALL_IN_FRONT  0.7f                   // cosine: further off its wall than ~45 degrees and the wall is beside it
#define BOT_GOAL_STOP      0.35f                  // close enough to the centre of a goal cell
#define BOT_ROUTE_MAX      160                    // water cells of a swim to a climbable shore
#define BOT_ROUTE_RADIUS   60                     // how far it looks for one
#define BOT_PATH_MAX       PLAN_PATH_MAX          // steps of a planned way to the goal
#define BOT_FOV_COS        0.26f                  // cos(75 deg): a 150 degree view cone
#define BOT_NEAR_SENSE     6.0f                   // closer than this an enemy is noticed anyway
#define BOT_WALL_PEEK      1.0f                   // seconds a shot held by a wall before it moves for a clear line
#define BOT_WALL_IGNORE    2.0f                   // no clear line to be had: seconds it leaves that enemy alone
#define BOT_ALERT_RANGE    3.0f                   // a mate hit this close alerts the bot too
#define BOT_ALERT_TIME     4.0f                   // seconds a bot keeps reacting to a hit
#define BOT_HEAR_RANGE     30.0f                  // a spade digging this close is heard
#define BOT_HEAR_TIME      10.0f                  // seconds a heard dig is remembered
#define BOT_GUARD_HUNT     20.0f                  // the guard goes after digs this close to its post
#define BOT_GUARD_LEASH    25.0f                  // and gives up this far from it
#define BOT_GUARD_HUNT_MAX 12.0f                  // or after this many seconds
#define BOT_SNEAK_RANGE    12.0f                  // the infiltrator only fights enemies this close
#define BOT_SNEAK_NEAR     70.0f                  // and sneaks only this close to its objective
#define BOT_MINER_SNEAK    2.5f                   // a Miner from this many times as far (it tunnels)
#define BOT_MINER_CLOSE    14.0f                  // a Miner fights within this, closes in beyond it
#define BOT_MINER_STALK_R  70.0f                  // and goes after one out of sight this far, underground
#define BOT_MINER_STALK    10.0f                  // for this many seconds after it last saw it
#define BOT_MINER_TUNNEL_IN  90.0f                // an attacking Miner tunnels to its goal from this near
#define BOT_MINER_TUNNEL_OUT 160.0f               // and once tunnelling, on out to this far
#define BOT_MINER_THREAT_R   130.0f               // with a far enemy about, it tunnels from this far
#define BOT_MINER_THREAT_S   20.0f                // for this many seconds after it saw one
#define BOT_FLEE_DIST      12.0f                  // how far it runs from its own dynamite
#define BOT_FLEE_SAFE      10.0f                  // and from where on it is safe (a grenade's 41 hp)
#define BOT_COVER_RANGE    20.0f                  // and its bodyguard those this close
#define BOT_COVER_LOST     25.0f                  // the bodyguard this far behind only catches up
#define BOT_COVER_NEAR     4.0f                   // close enough to the infiltrator to hold
#define BOT_LANE_RANGE     60.0f                  // a squad on its lane only takes on enemies this close (40 walked past enemies in plain sight)
#define BOT_CAMP_JUDGE      100.0f               // an overwatch's partial plan bans its spot only this close
#define BOT_DRILL_MATE_NEAR 8.0f                 // nor with a mate this close (the bit bores their ground)
#define BOT_STEPS_WALK     14.0f                  // a camper hears an enemy walk this close
#define BOT_STEPS_RUN      24.0f                  // and run (crouched or sneaking: silent)
#define BOT_STEPS_TIME     3.0f                   // seconds it keeps looking where it heard them
#define BOT_PERSONAL_SPACE 1.2f                   // closer than this to another player it steps aside
#define BOT_PERSONAL_STOP  0.75f                  // and closer than this it does not step into it

/* Combat phases. A bot that spots an enemy within range stops, raises a two
   block wall towards it and then dodges behind that wall: it strafes side to
   side, ducks behind the blocks and stands up to shoot over them. Skill
   decides how well each step is executed (aim, rhythm, exposure). */
typedef enum bot_phase
{
    BOT_PHASE_PATROL = 0,
    BOT_PHASE_BUILD,        // standing still, placing the two wall blocks
    BOT_PHASE_DODGE,        // strafing behind the wall, ducking and shooting
    BOT_PHASE_ASSAULT,      // the goal says assault (a mate carries): push and shoot, no wall, no ducking
} bot_phase_t;

/* Per bot AI memory, indexed by player id. Kept out of player_t on purpose:
   it is only meaningful for bots and would otherwise bloat every player. */
typedef struct bot_ai
{
    uint8_t  active;
    uint8_t  spawn_in_team_rect; /* test hook: use team A's spawn rectangle */
    uint8_t  dummy;              /* the tutorial's target: spawns, never moves or fires */
    uint8_t  target_id;
    uint8_t  has_target;
    uint8_t  strafe_dir;   // +1 or -1
    uint64_t next_think;
    uint64_t next_strafe;
    uint64_t next_hop;
    /* a melee bot's horde (_bot_melee): waiting out of sight since, and
       rushing (no more waiting) until */
    uint64_t horde_wait_at, horde_rush_until;
    uint64_t last_seen;    // last time the target was actually visible
    float    obj_x;        // patrol objective when there is no enemy
    float    obj_y;
    uint8_t  has_objective; // obj_x/obj_y were picked since the last spawn
    const char* mode_goal; // the game mode's goal name, kept to log changes only
    uint16_t    goal_intent; // GM_INTENT_*: what the mode's goal is for, read instead of its name
    uint8_t  escaping;     // 1: walking out of the water to obj_x/obj_y, 2: no dry land around
    int      escape_floor; // floor cell of the dry land it heads for (z grows downwards)
    int16_t  route[BOT_ROUTE_MAX][2]; // water cells to swim through to a shore it can climb
    uint16_t route_len;
    uint16_t route_i;

    /* --- getting through: digging, bridging, steps (_bot_clear_path) --- */
    float    goal_best;    // closest it got to the current goal
    uint64_t goal_progress; // when it last got closer
    uint64_t walk_at;       // the last think that walked toward the goal
    uint64_t clear_until;  // stuck: walk straight and clear the way until then
    uint64_t detour_until; // the way was closed: walk sideways until then
    float    detour_turn;  // +-pi/2
    int      pillar_floor; // jumped from this floor to put a block under itself, 0 none
    int      pillar_x;     // and from this column (the jump can drift off it)
    int      pillar_y;

    /* --- the planned way to the mode's goal (_bot_plan, _bot_follow) --- */
    int16_t  path[BOT_PATH_MAX][3]; // cells (x, y, floor) to step through
    uint8_t  path_act[BOT_PATH_MAX]; // how each one is reached: NAV_*
    uint16_t path_len;
    uint16_t path_i;
    float    path_gx;      // the goal it was planned for
    float    path_gy;
    uint64_t path_at;      // when it was planned
    uint64_t path_retry;   // no new plan before this (a failed search is costly)
    uint32_t plan_ticket;  // a plan asked of the planner thread, 0 none
    uint64_t plan_asked;
    int      plan_goal[3];
    float    plan_gx, plan_gy;
    uint8_t  plan_stealth;
    uint8_t  path_fails;   // refused actions on the current step
    uint64_t path_step_at; // when it reached the current step
    uint64_t path_used_at; // when it last followed the way
    int16_t  nav_ban[3];   // a cell it could not get into: the next plans avoid it
    uint64_t nav_ban_until;

    /* --- a hit on it or a mate next to it (bots_on_damage) --- */
    uint8_t  alert_from;     // the attacker
    float    alert_x;        // where the shot came from (the attacker then)
    float    alert_y;
    float    alert_z;
    uint64_t alert_react_at; // reaction time: nothing changes before it
    uint64_t alert_hide_until; // ducked in cover until then, then it aims there
    uint64_t alert_until;
    uint8_t  alert_spot;     // a cover spot was looked for: 1 found, 2 none
    float    alert_cx;
    float    alert_cy;

    /* --- role (the mode's, gm_bot_goal_t.role) and its state --- */
    uint8_t  role;          // GM_BOT_*
    uint8_t  role_logged;
    uint8_t  pref_weapon;   // an attacker's gun, drawn once: WEAPON_RIFLE or WEAPON_SMG
    uint8_t  pref_set;
    uint8_t  pref_class;    // an attacker's class in a classes mode, drawn once (game_class_id)
    uint8_t  natural_cover; // the cover is the map's (no wall of its own)
    uint8_t  in_cover;      // reached that cover spot
    uint8_t  close_fight;   // an enemy close: attacking, not hiding
    uint8_t  heard_from;    // a dig heard (bots_on_dig)
    int      heard_x, heard_y, heard_z;
    uint64_t heard_until;
    uint64_t heard_logged;
    uint8_t  hunting;       // the guard is after a heard dig
    uint8_t  steps_from;    // a camper heard this enemy's steps
    float    steps_x, steps_y, steps_z;
    uint64_t steps_until;
    uint64_t hunt_started;
    uint8_t  camper;        // a rifle bot that snipes from a spot of its own side
    uint8_t  overwatch;     // the camper the mode seats to watch home (camp 2): its spot sees it
    uint8_t  sneaky;        // an attacker that goes the hidden way (id % 4 == 2)
    float    goal_hold;     // the goal's hold radius (gm_bot_goal_t.hold)
    uint8_t  goal_climb;    // gm_bot_goal_t.climb
    uint8_t  goal_walk;     // gm_bot_goal_t.walk_only: no digging, no sneaking
    uint8_t  goal_keep;     // gm_bot_goal_t.keep_terrain: no spade out of a jam either
    uint64_t stealth_off;   // a sneaking plan came back partial: plain plans until then
    uint64_t drop_ok;       // a planned drop: the rail lets it step off until then (_bot_turn)
    const char* plan_why;   // why the pending plan was asked for (bot_nav action=plan why=)
    uint8_t     path_resumed; // the way was dropped after a pause (a fight)
    uint64_t rail_logged;   // bot_rail logged until then
    const char* look_cause; // what turned the look off the way this think (bot_look)
    const char* move_src;   // what chose this think's step: path, walk, steer (bot_spin)
    float       spin_deg;   // degrees turned on the way, with no fight about (bot_spin)
    float       spin_t;     // over this many seconds
    float       uturn_rad;  // signed turn of the body in the last uturn_t seconds (bot_uturn)
    float       uturn_t;
    float       want_yaw;   // the last think's wanted heading (bot_turnjump)
    uint16_t    want_path;  // and its path_i
    uint8_t     close_stance; // 1 crouched, 2 standing in its last close fight (bot_close)
    uint8_t     shotgun_wait; // waiting for a far enemy to come close (bot_shotgun)
    uint64_t    blind_at;     // the next bot_blind check
    float       aim_px, aim_py; // the point _bot_follow aimed at
    uint64_t    look_logged;
    /* a grenade on its way out (_bot_grenade): the throw, worked out on the
       server's own grenade physics, waits until the look got there */
    uint8_t    nade_ready;
    vector3f_t nade_dir;     // the direction thrown in (unit)
    float      nade_fuse;
    uint8_t    nade_target;
    uint64_t   nade_until;   // given up then
    uint64_t   nade_next;    // no other grenade before then
    uint64_t   melee_next;   // the next spade swing at an enemy (_bot_close_quarters)
    uint64_t   sentry_next;  // the next try to place a sentry (_bot_sentry)
    uint64_t   flee_until;   // running from its own dynamite until then (_think_flee)
    uint64_t   close_in_until; // a Miner closing in on an enemy the hidden way (_bot_shotgun)
    uint8_t    routing_close_in; // _bot_route_to is asked for that way (the enemy), not the goal's
    /* the way to its goal, kept while a Miner closes in on an enemy and
       taken up again after (_bot_route_to): a new plan from the chase's end
       turned it round and back out along its own tunnel */
    uint8_t    closing_in;      // the last way asked for was the enemy's
    uint64_t   miner_threat;    // a far enemy seen: until then the way to its goal is a tunnel
    /* a Miner's tunnel to its goal, measured (bot_miner action=tunnel_end):
       from its first dig on the way there to the goal, a death, another
       goal, the surface, or no progress */
    uint8_t    tun_active;
    float      tun_gx, tun_gy, tun_from, tun_best;
    uint16_t   tun_digs;
    uint64_t   tun_at, tun_progress_at;
    int16_t    kept_path[BOT_PATH_MAX][3];
    uint8_t    kept_act[BOT_PATH_MAX];
    uint16_t   kept_len, kept_i;
    float      kept_gx, kept_gy;
    uint8_t    kept_stealth;
    uint64_t   kept_at;
    float      flee_x, flee_y; // where to, and the charge it runs from
    float      flee_from_x, flee_from_y;
    uint8_t    jet_on;       // holding the jetpack's thrust (_bot_jetpack)
    uint8_t    jet_why;      // the flight: BOT_JET_GOAL, _PERCH, _BRAKE, 0 none
    float      jet_tx, jet_ty; // where it flies to (the goal, a roof)
    int        jet_tfloor;   // and that place's floor cell
    uint64_t   jet_until;    // the flight gives up then
    uint64_t   jet_next;     // no new flight looked for before then
    uint64_t   perch_next;   // no roof looked for before then
    uint64_t   perch_until;  // on a roof it flew to, watching, until then
    uint64_t   perch_logged; // bot_perch action=hold, at most every 2 s
    uint8_t  home_set;      // home_*: what the team protects (the goal's has_home; the CTF's own intel)
    float    home_x, home_y, home_z;
    uint8_t  camp_set;      // camp_* picked for this life
    float    camp_hx, camp_hy; // where home was when the overwatch picked its spot
    uint8_t  work;          // the goal's work at its destination (gm_bot_goal_t.work)
    int      work_x, work_y, work_z;
    uint32_t work_color;    // the work's block colour (gm_bot_goal_t.work_color)
    uint8_t  guard_set;     // guard_*: what else the team defends (Babel's staircase)
    float    guard_x, guard_y;
    uint64_t hunt_logged;   // bot_overwatch action=shoot_carrier, at most every 2 s
    int      camp_x, camp_y, camp_z;
    int      camp_ban[8][3];   // overwatch spots its plan could not reach (a roof with no way up)
    uint8_t  camp_bans;        // how many were banned (the oldest is replaced past 8)
    int      camp_try_x, camp_try_y; // the overwatch spot it has been trying to reach
    float    camp_try;         // seconds alive spent on the way to it
    uint64_t camp_try_at;      // the last think that counted (0: none this life)
    /* a planted shooter (guard, overwatch, camper) does not stay a statue:
       three shots from one cell and it steps sideways; the guard also beats
       (stands and leans out) every few seconds */
    uint8_t  camp_shots;
    uint64_t shift_until;
    int8_t   shift_dir;
    uint64_t beat_next;
    uint8_t  beat_on;
    uint8_t  path_stealth;  // the way was planned sneaking
    uint64_t fire_release_at; // the trigger is let go then (see _bot_trigger)
    uint64_t ads_until;       // wants the sights up until then (_bot_fire, _bot_ads_tick)
    float    ads_dist;        // how far the target was when it asked
    uint8_t  ads_on;          // the sights are up (secondary_fire set by this code)
    uint8_t  aimed;           // this think aimed at someone (_bot_aim_with_error)
    uint16_t snap_logged;     // the last enemy bot_snap logged, id + 1
    uint64_t worked_at;       // the spade or a block was last taken out then
    /* the look turns at a player's speed (_bot_turn): a think sets where
       the bot wants to look, the body gets there tick by tick */
    uint8_t    look_ok;         // look_cur / look_want hold something
    vector3f_t look_cur;        // where it really looks
    vector3f_t look_want;       // where the last think wanted it to look
    float      move_wx, move_wy; // the step the think chose, in the world (0, 0: none)
    uint8_t    turn_combat;     // the last think aimed at an enemy: the fast turn
    uint64_t   turn_at;         // the last turn step
    /* a mate (or an enemy) standing in its cell: it steps aside
       (_bot_keep_space; nothing in the physics keeps players apart) */
    uint64_t   space_step_until; // stepping off a body until then
    uint8_t    wall_target;      // the enemy its shots are held for a wall on
    uint64_t   wall_since;       // since when (0: not held)
    uint64_t   wall_last;        // the last hold
    uint64_t   peek_until;       // stepping aside for a clear line until then
    float      peek_dx, peek_dy; // and which way (unit, in the world)
    uint8_t    wall_ignore_id;   // an enemy behind a wall it gave up on
    uint64_t   wall_ignore_until;
    uint64_t   space_next;       // and no new step before then
    uint64_t   space_logged;     // bot_space logged until then
    float      space_dx, space_dy; // and which way (unit, in the world)
    uint8_t    engage_id;       // the enemy it stopped for (_bot_snap_shot), id + 1
    uint64_t   engage_until;    // stopped for it until then (renewed while seen)
    uint64_t   engage_fire_at;  // its reaction: no shot before then
    uint64_t   engage_since;    // stopped since then (for one enemy or the next)
    uint64_t   engage_cool;     // no stopping before then (it stood still too long)
    float      aim_err[3];      // the aim error now (_bot_aim_with_error), a fraction of the spread
    uint64_t   aim_err_at;      // drawn again then
    /* a post's wall (_bot_defend_hold): the guard's L, the overwatch's back,
       a camper's castle wall */
    uint8_t  dwall_planned;
    int      dwall[32][3];  // a post wall's cells (the castle's 9, the bunker's 21)
    uint8_t  wall_style;    // gm_bot_goal_t.wall_style
    uint8_t  camp_ground;   // gm_bot_goal_t.camp_ground
    int8_t   wall_side;     // gm_bot_goal_t.wall_side
    float    goal_perch;    // gm_bot_goal_t.perch: a roof this close to the goal counts as the goal
    int      dwall_n;
    int      dwall_floor;
    int      dpost_x, dpost_y; // the cell it guards from
    uint8_t  dwall_fails;

    /* --- combat behaviour --- */
    bot_phase_t phase;
    uint64_t    phase_until;   // when the current phase times out
    float       skill;         // 0.0 (weak) .. 1.0 (deadly)
    uint8_t     burst_left;    // shots left in the current burst

    /* the wall: two blocks stacked on the floor of (wall_x, wall_y), and the
       spot behind it the bot dodges around */
    uint8_t  has_cover;
    uint8_t  cover_blocks;     // wall blocks standing (placed or natural)
    uint8_t  build_fails;
    int      wall_x;
    int      wall_y;
    int      wall_floor;
    float    cover_x;
    float    cover_y;
    uint64_t next_rebuild;

    int8_t   dodge_dir;        // +1 / -1 along the axis across the threat
    uint64_t next_dodge_flip;
    uint8_t  ducked;           // crouched behind the wall
    uint64_t next_posture;
} bot_ai_t;


#define BOT_PATH_WALK  0
#define BOT_PATH_ACTED 1
#define BOT_PATH_STOP  2
#define BOT_PATH_WAIT  3 /* in the air (landing on a pillar block): do not step off */


/* Difficulty presets: the range a bot's skill is rolled in, and a factor on
   the aim error (BOT_AIM_SPREAD_*). Normal is the "poor shot" tuning; easy
   bots barely hit beyond 10 blocks, hard ones are still not aimbots. */
typedef struct
{
    const char* name;
    float       skill_min;
    float       skill_max;
    float       aim_error;
} bot_difficulty_t;

extern const bot_difficulty_t bot_difficulties[BOT_DIFFICULTY_COUNT];
extern uint8_t bot_difficulty_level;
bot_ai_t* _bot_ai(uint8_t id);
uint8_t _bot_is_wet(const player_t* bot);
uint8_t _bot_column_usable(server_t* server, uint16_t x, uint16_t y, int* floor_out);
uint8_t _bot_nearest_dry(server_t* server, player_t* bot, float* x_out, float* y_out);
void    _bot_flee_from(server_t* server, player_t* bot, bot_ai_t* ai, float cx, float cy, uint64_t now);
void _bot_pick_objective(server_t* server, player_t* bot, bot_ai_t* ai);
uint8_t _bot_sneaking(const bot_ai_t* ai);
extern uint64_t sight_asked, sight_rays;
uint8_t _bot_can_see(server_t* server, player_t* bot, player_t* target);
uint8_t _bot_line_clear(server_t* server, float x0, float y0, float z0, float x1, float y1, float z1);
player_t* _bot_pick_target_ex(server_t* server, player_t* bot, bot_ai_t* ai, uint8_t any_range);
player_t* _bot_pick_target(server_t* server, player_t* bot, bot_ai_t* ai);
void _bot_face(player_t* bot, float x, float y, float z);
void _bot_clear_inputs(player_t* bot);
void _bot_set_tool(server_t* server, player_t* bot, uint8_t tool);
void _bot_aim_with_error(server_t* server, player_t* bot, float tx, float ty, float tz, float skill);
uint8_t _bot_place_block(server_t* server, player_t* bot, uint32_t X, uint32_t Y, uint32_t Z);
int _bot_floor_cell(server_t* server, player_t* bot);
void _bot_trigger(player_t* bot);
void _bot_ads_tick(server_t* server, player_t* bot, bot_ai_t* ai);
uint8_t _bot_block_ready(server_t* server, const player_t* bot, uint8_t action);
uint8_t _bot_block_action(server_t* server, player_t* bot, uint8_t action, int X, int Y, int Z);
uint8_t _bot_placeable(server_t* server, int x, int y, int z);
uint8_t _bot_harvest(server_t* server, player_t* bot);
uint8_t _bot_dig(server_t* server, player_t* bot, bot_ai_t* ai, int ax, int ay, int centre, uint8_t rising,
                        int goal_floor);
void    _bot_tunnel_dug(player_t* bot, bot_ai_t* ai, uint64_t now);
void    _bot_tunnel_end(player_t* bot, bot_ai_t* ai, const char* why, uint64_t now);
void    _bot_tunnel_watch(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);
uint8_t _bot_pillar_up(server_t* server, player_t* bot, bot_ai_t* ai, int goal_floor);
uint8_t _bot_clear_path(server_t* server, player_t* bot, bot_ai_t* ai, float angle, int goal_floor);
void _bot_start_detour(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);
uint8_t _bot_water_route(server_t* server, player_t* bot, bot_ai_t* ai);
uint8_t _bot_plan_wall(server_t* server, player_t* bot, player_t* threat, bot_ai_t* ai);
uint8_t _bot_wall_standing(server_t* server, bot_ai_t* ai);
uint8_t _bot_build_step(server_t* server, player_t* bot, bot_ai_t* ai);
float _bot_weapon_range(const player_t* bot);
uint8_t _bot_pushes(server_t* server, const bot_ai_t* ai, player_t* bot, float d);
/* the player carries what its team must bring home (the mode says:
   gamemode_player_carries; the CTF's intel). The core never reads the
   intel itself */
static inline uint8_t _bot_carries(server_t* server, player_t* player)
{
    return (uint8_t) gamemode_player_carries(server, player);
}
void _bot_fire(server_t* server, player_t* bot, player_t* target);
void _bot_walk_to(server_t* server, player_t* bot, bot_ai_t* ai, float x, float y);
void _bot_keep_space(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);
void _bot_strafe_world(player_t* bot, float wx, float wy);
const char* _bot_phase_name(bot_phase_t phase);
void _bot_set_phase(player_t* bot, bot_ai_t* ai, bot_phase_t phase, uint64_t until);
uint64_t _bot_secs(float s);
void _bot_engage(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, float hdist, uint64_t now);
uint8_t _nav_solid(map_store_t* map, int x, int y, int z);
uint8_t _nav_room(map_store_t* map, int x, int y, int f);
uint8_t _nav_edge(map_store_t* map, int x, int y, int f);
void _bot_camp_ban(player_t* bot, bot_ai_t* ai, const char* why);
uint8_t _bot_route_to(server_t* server, player_t* bot, bot_ai_t* ai, float gx, float gy, int gz, uint8_t exact,
                             uint8_t rush);
uint8_t _bot_find_cover(server_t* server, player_t* bot, bot_ai_t* ai, float* cx, float* cy);
uint8_t _bot_cover_from(server_t* server, player_t* bot, float sx, float sy, float sz, float radius, float* cx,
                               float* cy);
uint8_t _bot_hear_steps(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);
void _bot_defend_hold(server_t* server, player_t* bot, bot_ai_t* ai, float wx, float wy, uint64_t now,
                             uint8_t sniper);
void _bot_camp_spot(server_t* server, player_t* bot, bot_ai_t* ai);
void _bot_role_weapon(server_t* server, player_t* bot, bot_ai_t* ai);
uint8_t _bot_mode_goal(server_t* server, player_t* bot, bot_ai_t* ai, float* gx, float* gy, uint8_t* exact,
                              uint8_t* rush, int* gfloor, uint8_t* assault, float* wx, float* wy);
uint8_t _bot_sentry(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                           uint64_t now);
uint8_t _bot_jetpack(server_t* server, player_t* bot, bot_ai_t* ai, float gx, float gy, int gfloor,
                            uint64_t now);
uint8_t _bot_perched(server_t* server, player_t* bot, const bot_ai_t* ai, uint64_t now);
void    _bot_jet_brake(server_t* server, player_t* bot, bot_ai_t* ai);
uint8_t _bot_grenade(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                            uint64_t now);
uint8_t _bot_shotgun(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                            uint64_t now);
uint8_t _bot_close_quarters(server_t* server, player_t* bot, bot_ai_t* ai, player_t* target, uint8_t visible,
                                   uint64_t now);
void _bot_think(server_t* server, player_t* bot, bot_ai_t* ai);
void _bot_turn(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);
void _bot_after_think(player_t* bot, bot_ai_t* ai);
void _bot_snap_shot(server_t* server, player_t* bot, bot_ai_t* ai);
void _bot_blind_check(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);

void _bot_leap(player_t* bot, float up);
/* BotMelee.c: a bot with no gun (GM_RULE_NO_GUN) */
void _bot_melee(server_t* server, player_t* bot, bot_ai_t* ai, uint64_t now);
/* when the high jump (GM_RULE_HIGH_JUMP) is back, game clock */
static inline uint64_t _bot_leap_back(const player_t* bot)
{
    return bot->zombie_leap_ready;
}
/* the map's water kills (its damage a second is half a life or more) */
uint8_t _bot_water_kills(server_t* server);
uint8_t _bot_falls_in_water(map_store_t* map, int x, int y, int fz);

#endif /* BOTS_INTERNAL_H */
